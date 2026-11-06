#ifndef slic3r_GUI_ControlServerCommands_hpp_
#define slic3r_GUI_ControlServerCommands_hpp_

#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace ControlCommands {

// A control API command. It runs on the GUI thread and returns a JSON object; throwing reports an error.
using Handler = nlohmann::json (*)(const nlohmann::json&);

// Adds the commands that inspect and edit the model (objects, parts, transforms, painting, modifiers, cuts,
// per-object settings), the presets, the view (camera, screenshots), setting descriptions and calibrations.
void add_model_commands(std::map<std::string, Handler>& table);

}}} // namespace Slic3r::GUI::ControlCommands

#endif
