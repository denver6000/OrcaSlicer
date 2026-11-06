#include "ControlServer.hpp"
#include "ControlServerCommands.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Selection.hpp"
#include "NotificationManager.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevStorage.h"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/NotificationProgressIndicator.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "Jobs/PrintJob.hpp"
#include "Jobs/Worker.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/BambuLanPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp      = boost::asio::ip::tcp;
using json     = nlohmann::json;

namespace {

constexpr unsigned short DEFAULT_PORT = 13618;

std::atomic<bool> s_stopping{false};

boost::filesystem::path info_file() { return boost::filesystem::path(data_dir()) / "control_server.json"; }

std::string random_token()
{
    std::random_device rd;
    static const char  hex[] = "0123456789abcdef";
    std::string        token;
    for (int i = 0; i < 40; ++i)
        token.push_back(hex[rd() % 16]);
    return token;
}

boost::filesystem::path path_from_utf8(const std::string& s)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(s));
#else
    return boost::filesystem::path(s);
#endif
}

// Runs `fn` on the GUI thread and waits for its result.
json run_on_gui(std::function<json()> fn, std::chrono::seconds timeout = std::chrono::seconds(60))
{
    auto promise = std::make_shared<std::promise<json>>();
    auto future  = promise->get_future();
    wxGetApp().CallAfter([promise, fn]() {
        try {
            promise->set_value(fn());
        } catch (const std::exception& e) {
            promise->set_value(json{{"error", e.what()}});
        }
    });
    const auto until = std::chrono::steady_clock::now() + timeout;
    // Waits in slices so shutdown, which blocks the GUI thread, is never held up.
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (s_stopping || std::chrono::steady_clock::now() > until)
            return json{{"error", "timed out waiting for the OrcaSlicer window"}};
    }
    return future.get();
}

// ---------------------------------------------------------------------------- commands (GUI thread)

Plater& plater()
{
    if (!wxGetApp().plater())
        throw std::runtime_error("the main window is not ready");
    return *wxGetApp().plater();
}

json cmd_status(const json&)
{
    Plater&       p  = plater();
    PresetBundle* pb = wxGetApp().preset_bundle;
    json          r;
    r["project_name"] = into_u8(p.get_project_name());
    r["presets"]      = {
        {"printer", pb->printers.get_edited_preset().name},
        {"printer_dirty", pb->printers.get_edited_preset().is_dirty},
        {"process", pb->prints.get_edited_preset().name},
        {"process_dirty", pb->prints.get_edited_preset().is_dirty},
        {"filaments", pb->filament_presets},
    };

    json objects = json::array();
    const auto& model = p.model();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const ModelObject* o = model.objects[i];
        objects.push_back({{"index", i},
                           {"name", o->name},
                           {"source_file", o->input_file},
                           {"instances", o->instances.size()},
                           {"parts", o->volumes.size()}});
    }
    r["objects"] = objects;

    PartPlateList& plates = p.get_partplate_list();
    json           plate_list = json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate* plate = plates.get_plate(i);
        plate_list.push_back({{"index", i}, {"slice_valid", plate && plate->is_slice_result_valid()}});
    }
    r["plates"]        = plate_list;
    r["current_plate"] = plates.get_curr_plate_index();
    r["filament_tab_preset"] = pb->filaments.get_edited_preset().name;
    r["busy"]          = !p.get_ui_job_worker().is_idle(); // arrange / orient / other jobs running
    r["preview_shown"] = p.is_preview_shown();
    json selection     = json::array();
    for (const auto& [obj, instances] : p.get_selection().get_content())
        selection.push_back(obj);
    r["selection"] = selection;
    return r;
}

json cmd_load_files(const json& body)
{
    if (!body.contains("paths") || !body["paths"].is_array() || body["paths"].empty())
        throw std::runtime_error("'paths' must be a non-empty array of file paths");
    std::vector<boost::filesystem::path> paths;
    for (const auto& item : body["paths"]) {
        boost::filesystem::path path = path_from_utf8(item.get<std::string>());
        if (!boost::filesystem::exists(path))
            throw std::runtime_error("file not found: " + item.get<std::string>());
        paths.push_back(path);
    }
    Plater& p = plater();
    if (body.value("replace", false))
        p.delete_all_objects_from_model();
    const std::vector<size_t> loaded = p.load_files(paths, LoadStrategy::LoadModel);
    return json{{"loaded_object_indices", loaded}, {"object_count", p.model().objects.size()}};
}

json cmd_reload_from_disk(const json&)
{
    Plater& p = plater();
    p.reload_all_from_disk();
    return json{{"object_count", p.model().objects.size()}};
}

json cmd_get_settings(const json& body)
{
    const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    json                     values = json::object();
    json                     missing = json::array();
    if (body.contains("keys")) {
        for (const auto& k : body["keys"]) {
            const std::string key = k.get<std::string>();
            if (config.has(key))
                values[key] = config.opt_serialize(key);
            else
                missing.push_back(key);
        }
    } else if (body.contains("search")) {
        // Every setting whose key contains the text, capped so a short search stays readable.
        const std::string text = body["search"].get<std::string>();
        for (const std::string& key : config.keys())
            if (key.find(text) != std::string::npos && values.size() < 100)
                values[key] = config.opt_serialize(key);
    } else {
        throw std::runtime_error("give 'keys' (a list) or 'search' (text)");
    }
    return json{{"values", values}, {"missing", missing}};
}

json cmd_slice(const json& body)
{
    Plater& p = plater();
    if (body.contains("plate"))
        p.select_plate(body["plate"].get<int>());
    p.reslice();
    return json{{"started", true}, {"plate", p.get_partplate_list().get_curr_plate_index()}};
}

json cmd_slice_result(const json& body)
{
    PartPlateList& plates = plater().get_partplate_list();
    const int      index  = body.value("plate", plates.get_curr_plate_index());
    PartPlate*     plate  = plates.get_plate(index);
    if (!plate)
        throw std::runtime_error("no plate " + std::to_string(index));

    json r;
    r["plate"] = index;
    r["valid"] = plate->is_slice_result_valid();

    PrintBase*  print_base = nullptr;
    GCodeResult* result     = nullptr;
    int          print_idx  = 0;
    plate->get_print(&print_base, &result, &print_idx);
    if (const Print* print = dynamic_cast<const Print*>(print_base)) {
        const PrintStatistics& st = print->print_statistics();
        r["estimated_time"] = st.estimated_normal_print_time;
        r["filament_m"]     = st.total_used_filament / 1000.0;
        r["filament_g"]     = st.total_weight;
        r["cost"]           = st.total_cost;
    }
    if (GCodeProcessorResult* gcode = plate->get_slice_result())
        r["time_s"] = gcode->print_statistics.modes[0].time;
    // Errors the slicer is showing (e.g. "empty first layer"): slicing will not finish until they are fixed.
    json errors = json::array();
    for (const auto& n : plater().get_notification_manager()->active_notifications())
        if (n.level == NotificationManager::NotificationLevel::ErrorNotificationLevel && !n.outdated && !n.text.empty())
            errors.push_back(n.text);
    r["errors"] = errors;
    return r;
}

std::shared_ptr<IPrinterAgent> printer_agent()
{
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent ? agent->get_printer_agent() : nullptr;
}

std::string selected_dev_id()
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    MachineObject* m  = dm ? dm->get_selected_machine() : nullptr;
    return m ? m->get_dev_id() : std::string();
}

json status_summary(const json& status)
{
    static const char* summary_keys[] = {"gcode_state",     "mc_percent",        "mc_remaining_time", "layer_num",
                                         "total_layer_num", "subtask_name",      "nozzle_temper",     "nozzle_target_temper",
                                         "bed_temper",      "bed_target_temper", "spd_lvl",           "print_error",
                                         "hms",             "lights_report",     "wifi_signal"};
    json summary = json::object();
    for (const char* k : summary_keys)
        if (status.contains(k))
            summary[k] = status[k];
    return summary;
}

std::string connection_error_text(const std::string& code)
{
    if (code == "5")
        return "the printer refused the access code. Read it again from the printer's screen (Settings > "
               "Network); it changes whenever LAN Only mode is switched on or the code is refreshed";
    return "could not open a connection to the printer (code " + code + "). Check that it is on, on the same "
           "network as this PC, and in LAN Only mode";
}

json cmd_printers(const json& body)
{
    auto pa = printer_agent();
    json r;
    r["agent"]    = pa ? pa->get_agent_info().id : std::string();
    r["selected"] = selected_dev_id();
    json list     = json::array();
    if (auto* lan = dynamic_cast<BambuLanPrinterAgent*>(pa.get())) {
        for (const std::string& id : lan->printer_ids()) {
            json status = lan->printer_status(id);
            json item   = {{"dev_id", id}, {"connected", lan->is_printer_connected(id)}, {"camera_url", lan->camera_snapshot_url(id)}};
            if (const std::string err = lan->connection_error(id); !err.empty())
                item["error"] = connection_error_text(err);
            item["status"] = body.value("full", false) ? status : status_summary(status);
            list.push_back(item);
        }
    }
    r["printers"] = list;
    return r;
}

// Adds a Bambu printer by IP address and LAN access code, as the Devices tab's "connect via IP"
// dialog does, selects it and waits for its first status report. Runs on the server thread: the
// TLS probe and the wait take seconds, and the window stays usable meanwhile.
json cmd_connect_printer(const json& body)
{
    const std::string ip     = body.value("ip", std::string());
    const std::string code   = body.value("access_code", std::string());
    const int         wait_s = std::clamp(body.value("timeout_s", 20), 0, 120);

    boost::system::error_code ec;
    asio::ip::make_address_v4(ip, ec);
    if (ip.empty() || ec)
        return json{{"error", "give the printer's IPv4 address as 'ip' (printer screen: Settings > Network)"}};
    if (code.empty() || code.size() > 32 || code.find_first_of(" \t\r\n") != std::string::npos)
        return json{{"error", "give the printer's LAN access code as 'access_code' (8 characters, shown under its IP)"}};

    // why: the holder outlives a run_on_gui timeout, after which the GUI lambda may still write to it
    auto agent_holder = std::make_shared<std::shared_ptr<IPrinterAgent>>();
    json pre          = run_on_gui([agent_holder]() {
        *agent_holder = printer_agent();
        return json{{"agent", *agent_holder ? (*agent_holder)->get_agent_info().id : std::string()}};
    });
    if (pre.contains("error"))
        return pre;
    auto* lan = dynamic_cast<BambuLanPrinterAgent*>(agent_holder->get());
    if (!lan)
        return json{{"error", "the active printer preset uses the '" + pre.value("agent", std::string()) +
                                  "' printer connection, not the open LAN one. Set its printer_agent to '" +
                                  BAMBU_LAN_PRINTER_AGENT_ID + "' (set_settings, printer scope) and connect again"}};

    // The printer's TLS certificate names its serial; no login is needed for that.
    detectResult detect;
    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent || agent->bind_detect(ip, "secure", detect) < 0 || detect.dev_id.empty())
        return json{{"error", "no Bambu printer answered on " + ip + ":8883. Check that it is on, on the same network "
                              "as this PC, and in LAN Only mode"}};
    const std::string dev_id = detect.dev_id;
    const std::string name   = body.value("name", detect.dev_name.empty() ? dev_id : detect.dev_name);

    json added = run_on_gui([detect, ip, code, name]() {
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm)
            throw std::runtime_error("the device manager is not ready");
        BBLocalMachine machine;
        machine.dev_id       = detect.dev_id;
        machine.dev_name     = name;
        machine.dev_ip       = ip;
        machine.printer_type = detect.model_id;
        MachineObject* obj   = dm->insert_local_device(machine, detect.connect_type.empty() ? "lan" : detect.connect_type,
                                                       detect.bind_state.empty() ? "free" : detect.bind_state, detect.version, code);
        if (!obj)
            throw std::runtime_error("the printer could not be added");
        obj->set_access_code(code); // also saves it with the printer, as the dialog does
        if (!dm->set_selected_machine(detect.dev_id))
            throw std::runtime_error("the printer was added but could not be selected");
        wxGetApp().app_config->save();
        return json{{"ok", true}};
    });
    if (added.contains("error"))
        return added;

    json r = {{"dev_id", dev_id}, {"name", name}, {"model", detect.model_id}, {"ip", ip}, {"connected", false}};
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(wait_s);
    while (!s_stopping) {
        if (const std::string err = lan->connection_error(dev_id); !err.empty())
            return json{{"error", connection_error_text(err)}, {"dev_id", dev_id}};
        if (lan->is_printer_connected(dev_id)) {
            const json status = lan->printer_status(dev_id);
            if (!status.empty()) {
                r["connected"]  = true;
                r["status"]     = status_summary(status);
                r["camera_url"] = lan->camera_snapshot_url(dev_id);
                return r;
            }
        }
        if (std::chrono::steady_clock::now() > until)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    r["connected"] = lan->is_printer_connected(dev_id);
    r["note"]      = r["connected"].get<bool>() ? "connected, but no status report arrived yet; call printers in a moment" :
                                                  "still connecting; call printers in a moment to see whether it succeeded";
    return r;
}

json cmd_printer_command(const json& body)
{
    auto pa = printer_agent();
    if (!pa)
        throw std::runtime_error("no printer agent");
    const std::string dev_id = body.value("dev_id", selected_dev_id());
    if (dev_id.empty())
        throw std::runtime_error("no printer selected; pass dev_id");
    if (!body.contains("command") || !body["command"].is_object())
        throw std::runtime_error("'command' must be a printer request object, e.g. {\"print\": {...}}");
    const int rc = pa->send_message_to_printer(dev_id, body["command"].dump(), 0, 0);
    return json{{"dev_id", dev_id}, {"result", rc}, {"ok", rc == 0}};
}

// ---------------------------------------------------------------------------- sending prints

// Progress and outcome of the print job send_print queued, written by the job's worker thread
// and read by the server thread.
struct PrintSend
{
    std::mutex  mutex;
    bool        done    = false;
    bool        ok      = false;
    int         percent = 0;
    std::string message;
    std::string error;

    void finish(bool success, const std::string& why = {})
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (done)
            return;
        done = true;
        ok   = success;
        if (!success && error.empty())
            error = why.empty() ? (message.empty() ? std::string("the print job stopped") : message) : why;
    }
};

// Runs a PrintJob, the same job the Print dialog runs, and records what it reports. The window
// shows its progress as for a print sent from the dialog.
class AgentPrintJob : public Job
{
public:
    AgentPrintJob(std::unique_ptr<PrintJob> job, std::shared_ptr<PrintSend> state)
        : m_job(std::move(job)), m_state(std::move(state))
    {}

    void process(Ctl& ctl) override
    {
        struct Recorder : Ctl
        {
            Ctl&       inner;
            PrintSend& st;
            Recorder(Ctl& inner, PrintSend& st) : inner(inner), st(st) {}
            void update_status(int s, const std::string& msg) override
            {
                {
                    std::lock_guard<std::mutex> lock(st.mutex);
                    st.percent = s;
                    if (!msg.empty())
                        st.message = msg;
                }
                inner.update_status(s, msg);
            }
            bool was_canceled() const override { return inner.was_canceled(); }
            void clear_percent() override { inner.clear_percent(); }
            void show_error_info(const std::string& msg, int code, const std::string& description, const std::string& extra) override
            {
                {
                    std::lock_guard<std::mutex> lock(st.mutex);
                    st.error = msg.empty() ? description : msg;
                }
                inner.show_error_info(msg, code, description, extra);
            }
            std::future<void> call_on_main_thread(std::function<void()> fn) override { return inner.call_on_main_thread(std::move(fn)); }
        } recorder(ctl, *m_state);

        m_job->process(recorder);
        bool failed;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            failed = !m_state->error.empty();
        }
        // why: is_finished() is also set when the upload check fails; that path records an error first
        m_state->finish(m_job->is_finished() && !failed);
    }

    void finalize(bool canceled, std::exception_ptr& e) override
    {
        m_job->finalize(canceled, e);
        if (canceled)
            m_state->finish(false, "the print job was cancelled");
        else if (e)
            m_state->finish(false, "the print job failed with an internal error (see the OrcaSlicer log)");
        else
            m_state->finish(false); // no-op once process() has finished
    }

private:
    std::unique_ptr<PrintJob>  m_job;
    std::shared_ptr<PrintSend> m_state;
};

Worker& print_worker()
{
    static std::unique_ptr<Worker> worker;
    if (!worker)
        worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(
            &plater(), std::make_shared<NotificationProgressIndicator>(plater().get_notification_manager()), "agent_print_worker");
    return *worker;
}

std::string project_name_for_print(const json& body)
{
    if (body.contains("name") && body["name"].is_string() && !body["name"].get<std::string>().empty())
        return body["name"].get<std::string>();
    const std::string project = into_u8(plater().get_project_name());
    if (!project.empty() && project != "Untitled")
        return project;
    const Model& model = wxGetApp().model();
    if (!model.objects.empty())
        return boost::filesystem::path(model.objects.front()->name).stem().string();
    return "print";
}

// Checks the printer and plate and queues the print job (GUI thread). Throws with the reason a
// print cannot be sent.
json start_print_send(const json& body, const std::shared_ptr<PrintSend>& state)
{
    Plater&        p   = plater();
    DeviceManager* dm  = wxGetApp().getDeviceManager();
    MachineObject* obj = dm ? dm->get_selected_machine() : nullptr;
    if (!obj)
        throw std::runtime_error("no printer is selected; connect_printer first");
    const std::string printer = obj->get_dev_name().empty() ? obj->get_dev_id() : obj->get_dev_name();
    if (!obj->is_connected())
        throw std::runtime_error(printer + " is not connected right now; check printers");
    if (obj->is_in_upgrading())
        throw std::runtime_error(printer + " is updating its firmware");
    if (obj->is_system_printing())
        throw std::runtime_error(printer + " is busy with a system task (e.g. calibration); try again when it ends");
    if (obj->is_in_printing())
        throw std::runtime_error(printer + " is already printing; it must finish or be stopped first");
    if (!print_worker().is_idle())
        throw std::runtime_error("a print is already being sent");

    const auto sd = obj->GetStorage()->get_sdcard_state();
    if (sd == DevStorage::SdcardState::NO_SDCARD)
        throw std::runtime_error(printer + " has no storage card; printing over LAN needs one");
    if (sd == DevStorage::SdcardState::HAS_SDCARD_READONLY)
        throw std::runtime_error(printer + "'s storage card is read-only");

    PartPlateList& plates = p.get_partplate_list();
    const int      idx    = body.value("plate", plates.get_curr_plate_index());
    if (!plates.get_plate(idx))
        throw std::runtime_error("no plate " + std::to_string(idx));
    if (idx != plates.get_curr_plate_index())
        p.select_plate(idx);
    PartPlate* plate = plates.get_plate(idx);
    if (!plate->is_slice_result_valid())
        throw std::runtime_error("plate " + std::to_string(idx) + " is not sliced, or changed since it was; slice it first");
    if (!plate->is_slice_result_ready_for_print())
        throw std::runtime_error("plate " + std::to_string(idx) + "'s slice has errors (toolpaths outside the bed or a "
                                 "G-code check failure); see check_errors");

    // The preset the plate was sliced with must be for this printer model (P1P and P1S share one).
    PresetBundle*     bundle = wxGetApp().preset_bundle;
    const std::string sliced = bundle->printers.get_edited_preset().get_printer_type(bundle);
    const std::string target = obj->printer_type;
    const bool        p1     = (sliced == "C11" || sliced == "C12") && (target == "C11" || target == "C12");
    if (!sliced.empty() && !target.empty() && sliced != target && !p1 && !body.value("allow_other_model", false))
        throw std::runtime_error("the plate was sliced for printer model " + sliced + " but " + printer + " is " + target +
                                 "; select a matching printer preset and slice again");

    // Filaments: which printer slot feeds each filament the plate uses.
    const std::vector<int> used        = plate->get_extruders(true); // 1-based filament numbers
    const size_t           n_filaments = bundle->filament_presets.size();
    std::string            ams_mapping;
    bool                   use_ams = false;
    json                   feeds   = json::object();
    if (obj->HasAms()) {
        if (!body.contains("ams_slots") || !body["ams_slots"].is_array() || body["ams_slots"].size() != used.size()) {
            json list = json::array();
            for (int f : used)
                list.push_back(f);
            throw std::runtime_error("this printer has an AMS: pass ams_slots with one tray per filament the plate uses "
                                     "(filaments " + list.dump() + ", in that order): 0-3 = AMS 1 slots 1-4, 4-7 = AMS 2, "
                                     "and so on; -1 = the external spool");
        }
        json v0 = json::array();
        for (size_t i = 0; i < n_filaments; ++i)
            v0.push_back(-1);
        bool has_ams = false, has_ext = false;
        for (size_t k = 0; k < used.size(); ++k) {
            const int tray = body["ams_slots"][k].get<int>();
            if (tray < -1 || tray > 15)
                throw std::runtime_error("ams_slots values are 0-15 (AMS trays) or -1 (external spool)");
            if (used[k] >= 1 && size_t(used[k]) <= n_filaments)
                v0[used[k] - 1] = tray;
            (tray >= 0 ? has_ams : has_ext) = true;
            feeds[std::to_string(used[k])] = tray >= 0 ? "AMS " + std::to_string(tray / 4 + 1) + " slot " + std::to_string(tray % 4 + 1) :
                                                         std::string("external spool");
        }
        ams_mapping = v0.dump();
        use_ams     = has_ams;
    } else {
        if (used.size() > 1)
            throw std::runtime_error("the plate uses " + std::to_string(used.size()) + " filaments but " + printer +
                                     " has no AMS, so it can only print one; assign one filament to every object");
        for (int f : used)
            feeds[std::to_string(f)] = "external spool";
    }

    // Package the sliced plate, as the Print dialog does before it sends.
    if (p.send_gcode(idx, [](int, int, int, bool&) {}) < 0)
        throw std::runtime_error("the sliced plate could not be packaged; slice it again");

    const std::string name       = project_name_for_print(body);
    const bool        leveling   = body.value("bed_leveling", true);
    const bool        flow_cali  = body.value("flow_calibration", false);
    const bool        timelapse  = body.value("timelapse", false);

    auto job            = std::make_unique<PrintJob>(obj->get_dev_id());
    job->m_dev_ip       = obj->get_dev_ip();
    job->m_ftp_folder   = obj->get_ftp_folder();
    job->m_access_code  = obj->get_access_code();
#if !BBL_RELEASE_TO_PUBLIC
    job->m_local_use_ssl_for_ftp = wxGetApp().app_config->get("enable_ssl_for_ftp") == "true";
    job->m_local_use_ssl         = wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true";
#else
    job->m_local_use_ssl_for_ftp = obj->local_use_ssl_for_ftp;
    job->m_local_use_ssl         = obj->local_use_ssl;
#endif
    job->connection_type  = obj->connection_type();
    job->cloud_print_only = obj->is_support_cloud_print_only;
    job->m_print_type     = "from_normal";
    job->set_project_name(name);
    job->task_ams_mapping = ams_mapping;
    job->task_use_ams     = use_ams;
    job->sdcard_state     = sd;
    job->has_sdcard       = wxGetApp().app_config->get("allow_abnormal_storage") == "true" ?
                                (sd == DevStorage::SdcardState::HAS_SDCARD_NORMAL || sd == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL) :
                                sd == DevStorage::SdcardState::HAS_SDCARD_NORMAL;
    job->could_emmc_print = obj->can_use_emmc_print();
    job->set_print_config("auto", leveling, flow_cali, false, timelapse, true, false, leveling ? 1 : 0, flow_cali ? 1 : 0, 0);
    job->on_check_ip_address_fail([state]() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->error = "the file could not be uploaded to the printer (FTPS, port 990); check its storage card and access code";
    });

    replace_job(print_worker(), std::make_shared<AgentPrintJob>(std::move(job), state));
    BOOST_LOG_TRIVIAL(info) << "ControlServer: sending plate " << idx << " (" << name << ") to " << obj->get_dev_id();

    return json{{"dev_id", obj->get_dev_id()},
                {"printer", printer},
                {"plate", idx},
                {"name", name},
                {"filaments", feeds},
                {"use_ams", use_ams},
                {"bed_leveling", leveling},
                {"flow_calibration", flow_cali},
                {"timelapse", timelapse}};
}

// Sends the sliced plate to the selected printer and starts it, as the Print dialog's Send does,
// then waits for the upload and for the printer to take the job. Server thread.
json cmd_send_print(const json& body)
{
    const int wait_s = std::clamp(body.value("timeout_s", 180), 10, 900);
    auto      state  = std::make_shared<PrintSend>();
    json      r      = run_on_gui([body, state]() { return start_print_send(body, state); }, std::chrono::seconds(120));
    if (r.contains("error"))
        return r;

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(wait_s);
    while (!s_stopping) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->done)
                break;
        }
        if (std::chrono::steady_clock::now() > until) {
            std::lock_guard<std::mutex> lock(state->mutex);
            r["sent"] = false;
            r["note"] = "still sending (" + std::to_string(state->percent) + "%: " + state->message +
                        "); check printers in a moment";
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->ok)
            return json{{"error", "the print was not sent: " + state->error}, {"printer", r["printer"]}, {"plate", r["plate"]}};
    }
    r["sent"] = true;

    // The printer answers with PREPARE (heating, levelling) and then RUNNING.
    auto  agent_holder = std::make_shared<std::shared_ptr<IPrinterAgent>>();
    run_on_gui([agent_holder]() {
        *agent_holder = printer_agent();
        return json::object();
    });
    if (auto* lan = dynamic_cast<BambuLanPrinterAgent*>(agent_holder->get())) {
        const std::string dev_id  = r["dev_id"].get<std::string>();
        const auto        started = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::string       gstate;
        while (!s_stopping && std::chrono::steady_clock::now() < started) {
            const json status = lan->printer_status(dev_id);
            gstate            = status.value("gcode_state", std::string());
            if (gstate == "PREPARE" || gstate == "RUNNING" || gstate == "SLICING")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        r["printer_state"] = gstate;
        r["status"]        = status_summary(lan->printer_status(dev_id));
    }
    return r;
}

using Command = ControlCommands::Handler;
const std::map<std::string, Command>& commands()
{
    static const std::map<std::string, Command> table = [] {
        std::map<std::string, Command> t = {
            {"status", cmd_status},
            {"load_files", cmd_load_files},
            {"reload_from_disk", cmd_reload_from_disk},
            {"get_settings", cmd_get_settings},
            {"slice", cmd_slice},
            {"slice_result", cmd_slice_result},
            {"printers", cmd_printers},
            {"printer_command", cmd_printer_command},
        };
        ControlCommands::add_model_commands(t);
        return t;
    }();
    return table;
}

// Commands that run on the server thread because they wait on the network; they reach the window
// through run_on_gui themselves.
const std::map<std::string, Command>& background_commands()
{
    static const std::map<std::string, Command> table = {
        {"connect_printer", cmd_connect_printer},
        {"send_print", cmd_send_print},
    };
    return table;
}

bool has_command(const std::string& name) { return commands().count(name) || background_commands().count(name); }

} // namespace

struct ControlServer::Impl
{
    asio::io_context io;
    tcp::acceptor    acceptor{io};
};

ControlServer& ControlServer::instance()
{
    static ControlServer server;
    return server;
}

ControlServer::~ControlServer() { stop(); }

void ControlServer::start()
{
    if (m_impl)
        return;
    s_stopping = false;
    m_token    = random_token();
    auto impl  = std::make_unique<Impl>();
    try {
        boost::system::error_code ec;
        impl->acceptor.open(tcp::v4());
        impl->acceptor.bind(tcp::endpoint(asio::ip::address_v4::loopback(), DEFAULT_PORT), ec);
        if (ec) // another instance has the usual port; take any free one, the info file says which
            impl->acceptor.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        impl->acceptor.listen();
        m_port = impl->acceptor.local_endpoint().port();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ControlServer: cannot listen: " << e.what();
        return;
    }

    try {
        boost::nowide::ofstream out(info_file().string(), std::ios::trunc);
        out << json{{"port", m_port.load()}, {"token", m_token}}.dump(2);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ControlServer: cannot write " << info_file().string() << ": " << e.what();
        return;
    }

    m_impl   = std::move(impl);
    m_thread = std::thread([this] { serve(); });
    BOOST_LOG_TRIVIAL(info) << "ControlServer: listening on 127.0.0.1:" << m_port;
}

void ControlServer::stop()
{
    if (!m_impl)
        return;
    s_stopping = true;
    m_impl->io.stop();
    boost::system::error_code ec;
    m_impl->acceptor.close(ec);
    if (m_thread.joinable())
        m_thread.join();
    m_impl.reset();
    boost::filesystem::remove(info_file(), ec);
}

void ControlServer::serve()
{
    std::function<void()> accept = [this, &accept]() {
        m_impl->acceptor.async_accept([this, &accept](const boost::system::error_code& ec, tcp::socket socket) {
            if (ec)
                return;
            try {
                boost::beast::flat_buffer                buffer;
                http::request<http::string_body>         req;
                http::read(socket, buffer, req);
                http::response<http::string_body>        res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                res.keep_alive(false);

                const std::string target(req.target());
                const std::string auth(req[http::field::authorization]);
                if (auth != "Bearer " + m_token) {
                    res.result(http::status::unauthorized);
                    res.body() = json{{"error", "missing or wrong token; read it from control_server.json"}}.dump();
                } else if (target.rfind("/api/", 0) != 0 || !has_command(target.substr(5))) {
                    json names = json::array();
                    for (const auto& [name, fn] : commands())
                        names.push_back(name);
                    for (const auto& [name, fn] : background_commands())
                        names.push_back(name);
                    res.result(http::status::not_found);
                    res.body() = json{{"error", "unknown command"}, {"commands", names}}.dump();
                } else {
                    json body = json::object();
                    if (!req.body().empty())
                        body = json::parse(req.body(), nullptr, false);
                    const std::string name = target.substr(5);
                    json              out;
                    if (body.is_discarded()) {
                        out = json{{"error", "the request body is not valid JSON"}};
                    } else if (auto bg = background_commands().find(name); bg != background_commands().end()) {
                        try {
                            out = bg->second(body);
                        } catch (const std::exception& e) {
                            out = json{{"error", e.what()}};
                        }
                    } else {
                        Command fn = commands().at(name);
                        out        = run_on_gui([fn, body]() { return fn(body); });
                    }
                    if (out.contains("error"))
                        res.result(http::status::bad_request);
                    res.body() = out.dump();
                }
                res.prepare_payload();
                http::write(socket, res);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "ControlServer: " << e.what();
            }
            accept();
        });
    };
    accept();
    m_impl->io.run();
}

}} // namespace Slic3r::GUI
