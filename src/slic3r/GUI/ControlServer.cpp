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
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/BambuLanPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <map>
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

json cmd_set_settings(const json& body)
{
    static const std::map<std::string, Preset::Type> scopes = {
        {"process", Preset::TYPE_PRINT}, {"print", Preset::TYPE_PRINT}, {"filament", Preset::TYPE_FILAMENT}, {"printer", Preset::TYPE_PRINTER}};
    const std::string scope = body.value("scope", std::string("process"));
    auto              it    = scopes.find(scope);
    if (it == scopes.end())
        throw std::runtime_error("scope must be process, filament or printer");
    if (!body.contains("values") || !body["values"].is_object())
        throw std::runtime_error("'values' must be an object of setting -> value");

    Tab* tab = wxGetApp().get_tab(it->second);
    if (!tab || !tab->get_config())
        throw std::runtime_error("settings tab not available");
    DynamicPrintConfig config = *tab->get_config();

    json applied = json::array();
    json errors  = json::object();
    for (const auto& [key, value] : body["values"].items()) {
        if (!config.has(key)) {
            errors[key] = "not a " + scope + " setting";
            continue;
        }
        std::string text = value.is_string() ? value.get<std::string>() : value.is_boolean() ? (value.get<bool>() ? "1" : "0") : value.dump();
        try {
            config.set_deserialize_strict(key, text);
            applied.push_back(key);
        } catch (const std::exception& e) {
            errors[key] = e.what();
        }
    }
    if (!applied.empty()) {
        // The same path as editing in the settings tab: the tab marks the preset modified and
        // refreshes its fields, and the plater invalidates and re-slices what changed.
        tab->load_config(config);
        plater().on_config_change(config);
    }
    return json{{"applied", applied}, {"errors", errors}};
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

json cmd_printers(const json& body)
{
    auto pa = printer_agent();
    json r;
    r["agent"]    = pa ? pa->get_agent_info().id : std::string();
    r["selected"] = selected_dev_id();
    json list     = json::array();
    if (auto* lan = dynamic_cast<BambuLanPrinterAgent*>(pa.get())) {
        static const char* summary_keys[] = {"gcode_state",     "mc_percent",    "mc_remaining_time", "layer_num",
                                             "total_layer_num", "subtask_name",  "nozzle_temper",     "nozzle_target_temper",
                                             "bed_temper",      "bed_target_temper", "spd_lvl",       "print_error",
                                             "hms",             "lights_report", "wifi_signal"};
        for (const std::string& id : lan->printer_ids()) {
            json status = lan->printer_status(id);
            json item   = {{"dev_id", id}, {"connected", lan->is_printer_connected(id)}, {"camera_url", lan->camera_snapshot_url(id)}};
            if (body.value("full", false)) {
                item["status"] = status;
            } else {
                json summary = json::object();
                for (const char* k : summary_keys)
                    if (status.contains(k))
                        summary[k] = status[k];
                item["status"] = summary;
            }
            list.push_back(item);
        }
    }
    r["printers"] = list;
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

using Command = ControlCommands::Handler;
const std::map<std::string, Command>& commands()
{
    static const std::map<std::string, Command> table = [] {
        std::map<std::string, Command> t = {
            {"status", cmd_status},
            {"load_files", cmd_load_files},
            {"reload_from_disk", cmd_reload_from_disk},
            {"get_settings", cmd_get_settings},
            {"set_settings", cmd_set_settings},
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
                } else if (target.rfind("/api/", 0) != 0 || !commands().count(target.substr(5))) {
                    json names = json::array();
                    for (const auto& [name, fn] : commands())
                        names.push_back(name);
                    res.result(http::status::not_found);
                    res.body() = json{{"error", "unknown command"}, {"commands", names}}.dump();
                } else {
                    json body = json::object();
                    if (!req.body().empty())
                        body = json::parse(req.body(), nullptr, false);
                    Command fn  = commands().at(target.substr(5));
                    json    out = body.is_discarded() ? json{{"error", "the request body is not valid JSON"}} :
                                                        run_on_gui([fn, body]() { return fn(body); });
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
