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
#include "GeometryImportDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/RadioGroup.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>
#include <boost/algorithm/string.hpp>
#include <atomic>
#include <cstdint>
#include <sstream>
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

// ---------------------------------------------------------------------------- dialogs (GUI thread)
//
// A modal dialog runs its own event loop, which still runs the control API's CallAfter work. So
// while one is open the API can read it and answer it as a click would: buttons are pressed by
// sending their wxEVT_BUTTON, toggles and choices are set and then notify like a click, and the
// dialog's own handlers run (saving "remember my choice", closing, continuing the interrupted
// command).

std::string dialog_id(const wxWindow* w) { return std::to_string(reinterpret_cast<std::uintptr_t>(w)); }

std::string clean_label(const wxString& s)
{
    std::string out = into_u8(wxStripMenuCodes(s));
    boost::algorithm::trim(out);
    return out;
}

bool same_label(const std::string& a, const std::string& b)
{
    std::string x = a, y = b;
    for (std::string* s : {&x, &y}) {
        boost::algorithm::trim(*s);
        boost::algorithm::to_lower(*s);
        while (!s->empty() && (s->back() == '.' || s->back() == ':'))
            s->pop_back();
    }
    return x == y;
}

// Dialogs on screen, topmost (last created) first.
std::vector<wxDialog*> open_dialogs()
{
    std::vector<wxDialog*> out;
    for (wxWindowList::compatibility_iterator n = wxTopLevelWindows.GetFirst(); n; n = n->GetNext())
        if (auto* d = dynamic_cast<wxDialog*>(n->GetData()); d && d->IsShown())
            out.push_back(d);
    std::reverse(out.begin(), out.end());
    return out;
}

struct DialogControls
{
    std::vector<std::string>                       message;
    std::vector<std::pair<wxWindow*, std::string>> buttons;    // window, label
    std::vector<std::pair<wxWindow*, std::string>> checkboxes; // toggles: wxCheckBox, Orca CheckBox/SwitchButton, wxToggleButton
    std::vector<wxWindow*>                         radios;     // Orca RadioGroup, or a wxRadioButton
    std::vector<wxWindow*>                         choices;    // wxChoice, wxComboBox, Orca ComboBox
    std::vector<wxTextCtrl*>                       texts;
};

void collect_controls(wxWindow* parent, DialogControls& c)
{
    const wxWindowList& children = parent->GetChildren();
    for (auto it = children.begin(); it != children.end(); ++it) {
        wxWindow* w = *it;
        if (!w->IsShown())
            continue;
        if (dynamic_cast<RadioGroup*>(w) || dynamic_cast<wxRadioButton*>(w)) {
            c.radios.push_back(w);
            continue;
        }
        if (auto* cb = dynamic_cast<wxCheckBox*>(w)) {
            c.checkboxes.emplace_back(cb, clean_label(cb->GetLabel()));
            continue;
        }
        if (dynamic_cast<wxToggleButton*>(w) || dynamic_cast<wxBitmapToggleButton*>(w)) {
            // Orca's check boxes carry no text; their label is the static text created next to them.
            std::string label = clean_label(w->GetLabel());
            for (auto next = std::next(it); label.empty() && next != children.end(); ++next) {
                if (auto* st = dynamic_cast<wxStaticText*>(*next)) {
                    label = clean_label(st->GetLabel());
                    break;
                }
                if ((*next)->IsShown() && !dynamic_cast<wxStaticText*>(*next))
                    break;
            }
            c.checkboxes.emplace_back(w, label);
            continue;
        }
        if (dynamic_cast<wxItemContainer*>(w) && (dynamic_cast<wxChoice*>(w) || dynamic_cast<wxComboBox*>(w) || dynamic_cast<::ComboBox*>(w))) {
            c.choices.push_back(w);
            continue;
        }
        if (dynamic_cast<::Button*>(w) || dynamic_cast<wxButton*>(w)) {
            c.buttons.emplace_back(w, clean_label(w->GetLabel()));
            continue;
        }
        if (auto* tc = dynamic_cast<wxTextCtrl*>(w)) {
            c.texts.push_back(tc);
            continue;
        }
        if (auto* st = dynamic_cast<wxStaticText*>(w)) {
            const std::string text = clean_label(st->GetLabel());
            if (!text.empty())
                c.message.push_back(text);
            continue;
        }
        collect_controls(w, c);
    }
}

wxItemContainer* items_of(wxWindow* w) { return dynamic_cast<wxItemContainer*>(w); }

json describe_dialog(wxDialog* d)
{
    DialogControls c;
    collect_controls(d, c);
    json r = {{"id", dialog_id(d)}, {"title", clean_label(d->GetTitle())}, {"modal", d->IsModal()}};
    std::string message;
    for (const std::string& line : c.message)
        message += (message.empty() ? "" : "\n") + line;
    r["message"] = message;
    json buttons = json::array();
    for (const auto& [w, label] : c.buttons)
        buttons.push_back({{"label", label}, {"enabled", w->IsEnabled()}});
    r["buttons"] = buttons;
    json checks = json::array();
    for (const auto& [w, label] : c.checkboxes) {
        bool value = false;
        if (auto* cb = dynamic_cast<wxCheckBox*>(w)) value = cb->GetValue();
        else if (auto* tb = dynamic_cast<wxToggleButton*>(w)) value = tb->GetValue();
        else if (auto* bb = dynamic_cast<wxBitmapToggleButton*>(w)) value = bb->GetValue();
        checks.push_back({{"label", label}, {"value", value}, {"enabled", w->IsEnabled()}});
    }
    r["checkboxes"] = checks;
    json radios = json::array();
    for (wxWindow* w : c.radios) {
        if (auto* rg = dynamic_cast<RadioGroup*>(w)) {
            DialogControls inner;
            collect_controls(rg, inner);
            json options = json::array();
            for (const auto& [b, label] : inner.buttons)
                options.push_back(label);
            const int sel = rg->GetSelection();
            radios.push_back({{"options", options}, {"selected", sel >= 0 && sel < int(options.size()) ? options[sel] : json()}});
        } else if (auto* rb = dynamic_cast<wxRadioButton*>(w)) {
            radios.push_back({{"options", json::array({clean_label(rb->GetLabel())})}, {"selected", rb->GetValue() ? json(clean_label(rb->GetLabel())) : json()}});
        }
    }
    r["radios"] = radios;
    json choices = json::array();
    for (wxWindow* w : c.choices) {
        wxItemContainer* items = items_of(w);
        json options = json::array();
        for (unsigned i = 0; i < items->GetCount(); ++i)
            options.push_back(into_u8(items->GetString(i)));
        const int sel = items->GetSelection();
        choices.push_back({{"options", options}, {"selected", sel >= 0 ? json(into_u8(items->GetString(sel))) : json()}});
    }
    r["choices"] = choices;
    json texts = json::array();
    for (wxTextCtrl* t : c.texts)
        texts.push_back({{"value", into_u8(t->GetValue())}, {"editable", t->IsEditable()}});
    r["texts"] = texts;
    return r;
}

json modal_dialogs()
{
    json list = json::array();
    for (wxDialog* d : open_dialogs())
        if (d->IsModal())
            list.push_back(describe_dialog(d));
    return list;
}

// Runs `fn` on the GUI thread and waits for its result. If a modal dialog opens while `fn` runs
// (it waits for an answer), returns that dialog instead of timing out: `fn` carries on once the
// dialog is answered (answer_dialog), and its result then shows in the slicer's state.
json run_on_gui(std::function<json()> fn, std::chrono::seconds timeout = std::chrono::seconds(60))
{
    auto promise = std::make_shared<std::promise<json>>();
    auto future  = promise->get_future();
    auto done    = std::make_shared<std::atomic<bool>>(false);
    wxGetApp().CallAfter([promise, fn, done]() {
        try {
            json r = fn();
            done->store(true);
            promise->set_value(std::move(r));
        } catch (const std::exception& e) {
            done->store(true);
            promise->set_value(json{{"error", e.what()}});
        }
    });
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + timeout;
    // A probe queued behind `fn` runs only while `fn` is still on the stack, i.e. inside a nested
    // event loop such as a modal dialog's.
    auto       probe        = std::make_shared<std::mutex>();
    auto       found        = std::make_shared<json>();
    auto       probe_queued = std::make_shared<std::atomic<bool>>(false);
    auto       next_probe   = start + std::chrono::milliseconds(1500);
    // Waits in slices so shutdown, which blocks the GUI thread, is never held up.
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        const auto now = std::chrono::steady_clock::now();
        if (s_stopping || now > until)
            return json{{"error", "timed out waiting for the OrcaSlicer window"}};
        {
            std::lock_guard<std::mutex> lock(*probe);
            if (found->is_array() && !found->empty())
                return json{{"blocked_by_dialog", *found},
                            {"note", "OrcaSlicer is waiting for an answer to this dialog. answer_dialog answers it; the "
                                     "command then finishes on its own (check the result with the usual read tools)."}};
        }
        if (now > next_probe && !probe_queued->exchange(true)) {
            next_probe = now + std::chrono::seconds(2);
            wxGetApp().CallAfter([probe, found, probe_queued, done]() {
                json dialogs = done->load() ? json::array() : modal_dialogs();
                std::lock_guard<std::mutex> lock(*probe);
                *found = std::move(dialogs);
                probe_queued->store(false);
            });
        }
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

json cmd_dialogs(const json&)
{
    json list = json::array();
    for (wxDialog* d : open_dialogs())
        list.push_back(describe_dialog(d));
    return json{{"dialogs", list}};
}

// Answers an open dialog: sets its check boxes, radio groups, choices and text fields, then
// presses a button, each as a click would.
json cmd_answer_dialog(const json& body)
{
    wxDialog* dialog = nullptr;
    const std::vector<wxDialog*> dialogs = open_dialogs();
    const std::string            want    = body.value("dialog", std::string());
    for (wxDialog* d : dialogs)
        if (want.empty() ? d->IsModal() : (dialog_id(d) == want || same_label(clean_label(d->GetTitle()), want))) {
            dialog = d;
            break;
        }
    if (!dialog)
        throw std::runtime_error(want.empty() ? "no dialog is waiting for an answer" : "no open dialog '" + want + "'; see dialogs");

    DialogControls c;
    collect_controls(dialog, c);
    json changed = json::object();

    if (body.contains("set") && body["set"].is_object()) {
        for (const auto& [key, value] : body["set"].items()) {
            bool done = false;
            // A check box, by its label.
            for (const auto& [w, label] : c.checkboxes) {
                if (!same_label(label, key))
                    continue;
                const bool on = value.is_boolean() ? value.get<bool>() : (value.is_string() && (value == "true" || value == "on"));
                wxEventType type = wxEVT_TOGGLEBUTTON;
                if (auto* cb = dynamic_cast<wxCheckBox*>(w)) {
                    cb->SetValue(on);
                    type = wxEVT_CHECKBOX;
                } else if (auto* tb = dynamic_cast<wxToggleButton*>(w)) {
                    tb->SetValue(on);
                } else if (auto* bb = dynamic_cast<wxBitmapToggleButton*>(w)) {
                    bb->SetValue(on);
                }
                wxCommandEvent evt(type, w->GetId());
                evt.SetInt(on ? 1 : 0);
                evt.SetEventObject(w);
                w->GetEventHandler()->ProcessEvent(evt);
                changed[label] = on;
                done = true;
                break;
            }
            // A radio group or choice, by one of its options ("set": {"<option>": true}), or
            // by index ("set": {"radio:0": "<option>"}, {"choice:1": "<option>"}).
            auto pick_radio = [&](RadioGroup* rg, const std::string& option) {
                DialogControls inner;
                collect_controls(rg, inner);
                for (size_t i = 0; i < inner.buttons.size(); ++i)
                    if (same_label(inner.buttons[i].second, option)) {
                        rg->SetSelection(int(i), true);
                        changed["radio"] = inner.buttons[i].second;
                        return true;
                    }
                return false;
            };
            auto pick_choice = [&](wxWindow* w, const std::string& option) {
                wxItemContainer* items = items_of(w);
                for (unsigned i = 0; i < items->GetCount(); ++i)
                    if (same_label(into_u8(items->GetString(i)), option)) {
                        items->SetSelection(int(i));
                        wxCommandEvent evt(dynamic_cast<wxChoice*>(w) ? wxEVT_CHOICE : wxEVT_COMBOBOX, w->GetId());
                        evt.SetInt(int(i));
                        evt.SetString(items->GetString(i));
                        evt.SetEventObject(w);
                        w->GetEventHandler()->ProcessEvent(evt);
                        changed["choice"] = into_u8(items->GetString(i));
                        return true;
                    }
                return false;
            };
            if (!done && key.rfind("radio:", 0) == 0) {
                const size_t idx = std::stoul(key.substr(6));
                if (idx < c.radios.size())
                    if (auto* rg = dynamic_cast<RadioGroup*>(c.radios[idx]))
                        done = pick_radio(rg, value.get<std::string>());
            } else if (!done && key.rfind("choice:", 0) == 0) {
                const size_t idx = std::stoul(key.substr(7));
                if (idx < c.choices.size())
                    done = pick_choice(c.choices[idx], value.get<std::string>());
            } else if (!done && key.rfind("text:", 0) == 0) {
                const size_t idx = std::stoul(key.substr(5));
                if (idx < c.texts.size()) {
                    c.texts[idx]->SetValue(wxString::FromUTF8(value.is_string() ? value.get<std::string>() : value.dump()));
                    changed[key] = value;
                    done = true;
                }
            }
            if (!done && value.is_boolean() && value.get<bool>()) {
                for (wxWindow* w : c.radios)
                    if (auto* rg = dynamic_cast<RadioGroup*>(w); rg && pick_radio(rg, key)) { done = true; break; }
                for (size_t i = 0; !done && i < c.choices.size(); ++i)
                    done = pick_choice(c.choices[i], key);
            }
            if (!done)
                throw std::runtime_error("'" + key + "' matches no check box, radio option, choice or text field of this dialog "
                                         "(nothing was pressed); see dialogs");
        }
    }

    std::string pressed;
    if (body.contains("button")) {
        const std::string label = body["button"].get<std::string>();
        wxWindow*         btn   = nullptr;
        for (const auto& [w, l] : c.buttons)
            if (same_label(l, label)) {
                btn     = w;
                pressed = l;
                break;
            }
        if (!btn) {
            json labels = json::array();
            for (const auto& [w, l] : c.buttons)
                labels.push_back(l);
            throw std::runtime_error("no button '" + label + "' in this dialog; its buttons are " + labels.dump());
        }
        if (!btn->IsEnabled())
            throw std::runtime_error("the '" + pressed + "' button is disabled");
        wxCommandEvent evt(wxEVT_BUTTON, btn->GetId());
        evt.SetEventObject(btn);
        btn->GetEventHandler()->ProcessEvent(evt);
        // A standard button whose handler did not close the dialog closes it as wxWidgets would.
        const int id = btn->GetId();
        if (dialog->IsShown() && dialog->IsModal() &&
            (id == wxID_OK || id == wxID_CANCEL || id == wxID_YES || id == wxID_NO || id == wxID_CLOSE || id == wxID_APPLY))
            dialog->EndModal(id);
    }
    return json{{"dialog", clean_label(dialog->GetTitle())}, {"set", changed}, {"pressed", pressed},
                {"closed", !dialog->IsShown()}};
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

const std::map<std::string, ControlCommands::Handler>& commands();

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

    // The import dialogs (3MF/AMF naming, name cleanup, STEP mesh quality) are modal: nobody answers
    // them for this request, which then times out. Load with their remembered choices instead, and
    // put the user's settings back afterwards.
    AppConfig*                                        cfg = wxGetApp().app_config;
    std::vector<std::pair<std::string, std::string>> saved;
    for (const char* key : {"import_names_3mf_show_dialog", "import_names_amf_show_dialog", "import_names_cleanup_show_dialog",
                            "enable_step_mesh_setting"}) {
        saved.emplace_back(key, cfg->has(key) ? cfg->get(key) : std::string());
        cfg->set_bool(key, false);
    }
    ScopeGuard restore([cfg, &saved]() {
        for (const auto& [key, value] : saved) {
            if (value.empty())
                cfg->erase("app", key);
            else
                cfg->set(key, value);
        }
    });

    // Loading a model into an empty project can start a new project, which reselects the presets
    // last saved for this printer and drops unsaved edits (it turned a selected ABS back into PLA).
    // A model file carries no presets, so put the user's back afterwards; a project file's own
    // presets are meant to apply.
    PresetBundle* pb      = wxGetApp().preset_bundle;
    bool          plain   = true;
    for (const auto& path : paths)
        if (boost::algorithm::iends_with(path.string(), ".3mf") && !is_geometry_only_3mf(path.string()))
            plain = false;
    struct Snapshot
    {
        std::string                        printer, process;
        std::vector<std::string>           filaments;
        std::map<std::string, json>        dirty; // type -> {key: serialized value}
    } before;
    before.printer   = pb->printers.get_selected_preset_name();
    before.process   = pb->prints.get_selected_preset_name();
    before.filaments = pb->filament_presets;
    for (const auto& [type, coll] : {std::pair<std::string, PresetCollection*>{"printer", &pb->printers},
                                     {"process", &pb->prints}, {"filament", &pb->filaments}}) {
        json values = json::object();
        for (const std::string& key : coll->current_dirty_options())
            values[key] = coll->get_edited_preset().config.opt_serialize(key);
        before.dirty[type] = values;
    }

    const std::vector<size_t> loaded = p.load_files(paths, LoadStrategy::LoadModel);
    json r = {{"loaded_object_indices", loaded}, {"object_count", p.model().objects.size()}};

    if (plain) {
        json restored = json::object();
        const auto& cmds = commands();
        auto select = [&](const std::string& type, const std::string& name, int slot) {
            json b = {{"type", type}, {"name", name}, {"discard_changes", true}};
            if (slot >= 0)
                b["slot"] = slot;
            cmds.at("select_preset")(b);
        };
        if (pb->printers.get_selected_preset_name() != before.printer) {
            select("printer", before.printer, -1);
            restored["printer"] = before.printer;
        }
        if (pb->prints.get_selected_preset_name() != before.process) {
            select("process", before.process, -1);
            restored["process"] = before.process;
        }
        for (size_t i = 0; i < before.filaments.size() && i < pb->filament_presets.size(); ++i)
            if (pb->filament_presets[i] != before.filaments[i]) {
                select("filament", before.filaments[i], int(i));
                restored["filament_" + std::to_string(i)] = before.filaments[i];
            }
        for (const auto& [type, values] : before.dirty) {
            PresetCollection& coll = type == "printer" ? pb->printers : type == "process" ? pb->prints : pb->filaments;
            json lost = json::object();
            for (const auto& [key, value] : values.items())
                if (coll.get_edited_preset().config.opt_serialize(key) != value.get<std::string>())
                    lost[key] = value;
            if (!lost.empty()) {
                cmds.at("set_settings")(json{{"values", lost}, {"scope", type}});
                restored[type + "_edits"] = lost;
            }
        }
        if (!restored.empty())
            r["presets_restored"] = restored;
    }
    return r;
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

    // The nozzle the plate was sliced for must be the one the printer reports; a P1S pauses a
    // mismatched job before the first layer with error 0500-803C.
    const DynamicPrintConfig full = bundle->full_config();
    json printer_status = json::object();
    if (auto* lan = dynamic_cast<BambuLanPrinterAgent*>(printer_agent().get()))
        printer_status = lan->printer_status(obj->get_dev_id());
    const double sliced_nozzle = full.option<ConfigOptionFloats>("nozzle_diameter")->values.front();
    std::string  sliced_nozzle_type = full.opt_serialize("nozzle_type");
    sliced_nozzle_type = sliced_nozzle_type.substr(0, sliced_nozzle_type.find(','));
    if (printer_status.contains("nozzle_diameter")) {
        double printer_nozzle = 0;
        try {
            printer_nozzle = printer_status["nozzle_diameter"].is_string() ? std::stod(printer_status["nozzle_diameter"].get<std::string>()) :
                                                                             printer_status["nozzle_diameter"].get<double>();
        } catch (...) {}
        if (printer_nozzle > 0 && std::abs(printer_nozzle - sliced_nozzle) > 0.01) {
            std::ostringstream msg;
            msg << printer << " has a " << printer_nozzle << " mm nozzle but the plate was sliced for " << sliced_nozzle
                << " mm; select the printer preset for that nozzle and slice again";
            throw std::runtime_error(msg.str());
        }
    }
    json notes = json::array();
    const std::string printer_nozzle_type = printer_status.value("nozzle_type", std::string());
    if (!printer_nozzle_type.empty() && sliced_nozzle_type != "undefine" && printer_nozzle_type != sliced_nozzle_type)
        notes.push_back("the printer reports a " + printer_nozzle_type + " nozzle; the preset says " + sliced_nozzle_type);

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
        // The spool's type, when the printer knows it (set on its screen or by RFID).
        const std::string spool = printer_status.contains("vt_tray") ? printer_status["vt_tray"].value("tray_type", std::string()) : std::string();
        const auto*       types = full.option<ConfigOptionStrings>("filament_type");
        const std::string sliced_type = !used.empty() && types && size_t(used.front()) <= types->values.size() ? types->values[used.front() - 1] : std::string();
        if (!spool.empty() && !sliced_type.empty() && !boost::algorithm::iequals(spool, sliced_type) && !body.value("allow_filament_mismatch", false))
            throw std::runtime_error("the external spool is set to " + spool + " on the printer but the plate was sliced for " + sliced_type +
                                     "; select the matching filament and slice again (or pass allow_filament_mismatch if the "
                                     "printer's spool setting is out of date)");
    }

    // What the plate was sliced with, for the record (a valid slice matches the current settings).
    json sliced_with;
    {
        const std::string bed      = full.opt_serialize("curr_bed_type");
        static const std::map<std::string, std::string> bed_keys = {
            {"Cool Plate", "cool_plate_temp"},          {"Engineering Plate", "eng_plate_temp"},
            {"High Temp Plate", "hot_plate_temp"},      {"Textured PEI Plate", "textured_plate_temp"},
            {"Supertack Plate", "supertack_plate_temp"}, {"Textured Cool Plate", "textured_cool_plate_temp"}};
        const int   f   = used.empty() ? 0 : used.front() - 1;
        auto        nth = [&](const std::string& key) {
            if (!full.has(key))
                return std::string();
            std::vector<std::string> parts;
            const std::string        all = full.opt_serialize(key);
            boost::algorithm::split(parts, all, boost::is_any_of(","));
            return size_t(f) < parts.size() ? parts[f] : all;
        };
        sliced_with["printer_preset"]  = bundle->printers.get_selected_preset_name();
        sliced_with["process_preset"]  = bundle->prints.get_selected_preset_name();
        sliced_with["filament_preset"] = size_t(f) < bundle->filament_presets.size() ? bundle->filament_presets[f] : std::string();
        sliced_with["filament_type"]   = nth("filament_type");
        sliced_with["nozzle"]          = std::to_string(sliced_nozzle).substr(0, 4) + " mm " + sliced_nozzle_type;
        sliced_with["nozzle_temp"]     = nth("nozzle_temperature_initial_layer") + " first layer, " + nth("nozzle_temperature");
        sliced_with["plate"]           = bed;
        if (auto it = bed_keys.find(bed); it != bed_keys.end())
            sliced_with["bed_temp"] = nth(it->second + "_initial_layer") + " first layer, " + nth(it->second);
        sliced_with["layer_height"]    = full.opt_serialize("layer_height");
        sliced_with["brim"]            = full.opt_serialize("brim_type") + " " + full.opt_serialize("brim_width") + " mm";
        sliced_with["supports"]        = full.opt_bool("enable_support") ? full.opt_serialize("support_type") : "off";
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
                {"timelapse", timelapse},
                {"sliced_with", sliced_with},
                {"notes", notes}};
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
            {"dialogs", cmd_dialogs},
            {"answer_dialog", cmd_answer_dialog},
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
