#include "ControlServerCommands.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GLCanvas3D.hpp"
#include "MainFrame.hpp"
#include "NotificationManager.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "ObjectDataViewModel.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/calib.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <wx/glcanvas.h>
#include <wx/dcmemory.h>
#include <wx/dcscreen.h>
#include <wx/image.h>
#include <wx/mstream.h>

namespace Slic3r { namespace GUI { namespace ControlCommands {

using json = nlohmann::json;

namespace {

constexpr double DEG = PI / 180.0;

// ---------------------------------------------------------------------------- helpers

Plater& plater()
{
    if (!wxGetApp().plater())
        throw std::runtime_error("the main window is not ready");
    return *wxGetApp().plater();
}

double round2(double v) { return std::round(v * 100.0) / 100.0; }

json vec_json(const Vec3d& v) { return json::array({round2(v.x()), round2(v.y()), round2(v.z())}); }

Vec3d vec3(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3)
        throw std::runtime_error(std::string(what) + " must be [x, y, z]");
    return Vec3d(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
}

std::string lower(std::string s) { return boost::algorithm::to_lower_copy(s); }

// Text form of a JSON value as the config parser expects it.
std::string value_text(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "1" : "0";
    return value.dump();
}

size_t object_index(const json& ref)
{
    const ModelObjectPtrs& objects = plater().model().objects;
    if (ref.is_number_integer()) {
        const long long i = ref.get<long long>();
        if (i < 0 || i >= (long long) objects.size())
            throw std::runtime_error("no object " + std::to_string(i) + "; the plate has " + std::to_string(objects.size()));
        return size_t(i);
    }
    if (ref.is_string()) {
        const std::string name = ref.get<std::string>();
        for (size_t i = 0; i < objects.size(); ++i)
            if (objects[i]->name == name)
                return i;
        throw std::runtime_error("no object named '" + name + "'");
    }
    throw std::runtime_error("an object is given by its index or name");
}

size_t object_arg(const json& body)
{
    if (!body.contains("object"))
        throw std::runtime_error("give 'object' (index or name)");
    return object_index(body["object"]);
}

size_t part_index(const ModelObject& o, const json& ref)
{
    const long long i = ref.get<long long>();
    if (i < 0 || i >= (long long) o.volumes.size())
        throw std::runtime_error("object '" + o.name + "' has no part " + std::to_string(i));
    return size_t(i);
}

size_t instance_index(const ModelObject& o, const json& body)
{
    const long long i = body.value("instance", 0LL);
    if (i < 0 || i >= (long long) o.instances.size())
        throw std::runtime_error("object '" + o.name + "' has no instance " + std::to_string(i));
    return size_t(i);
}

// The instances a transform applies to: the one named by 'instance', or all of them.
std::vector<size_t> instance_targets(const ModelObject& o, const json& body)
{
    std::vector<size_t> out;
    if (!body.contains("instance") || (body["instance"].is_string() && body["instance"] == "all")) {
        for (size_t i = 0; i < o.instances.size(); ++i)
            out.push_back(i);
    } else {
        out.push_back(instance_index(o, body));
    }
    return out;
}

const char* volume_type_name(ModelVolumeType t)
{
    switch (t) {
    case ModelVolumeType::MODEL_PART: return "part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "support_enforcer";
    default: return is_precise_seam(t) ? "seam_modifier" : "other";
    }
}

ModelVolumeType volume_type_from(const std::string& s)
{
    if (s == "part") return ModelVolumeType::MODEL_PART;
    if (s == "negative") return ModelVolumeType::NEGATIVE_VOLUME;
    if (s == "modifier") return ModelVolumeType::PARAMETER_MODIFIER;
    if (s == "support_blocker") return ModelVolumeType::SUPPORT_BLOCKER;
    if (s == "support_enforcer") return ModelVolumeType::SUPPORT_ENFORCER;
    throw std::runtime_error("type must be part, negative, modifier, support_enforcer or support_blocker");
}

json config_json(const DynamicPrintConfig& c)
{
    json j = json::object();
    for (const std::string& k : c.keys())
        j[k] = c.opt_serialize(k);
    return j;
}

const std::set<std::string>& keys_of(Preset::Type type)
{
    static const std::set<std::string> print(Preset::print_options().begin(), Preset::print_options().end());
    static const std::set<std::string> filament(Preset::filament_options().begin(), Preset::filament_options().end());
    static const std::set<std::string> printer(Preset::printer_options().begin(), Preset::printer_options().end());
    return type == Preset::TYPE_PRINT ? print : type == Preset::TYPE_FILAMENT ? filament : printer;
}

std::string scope_of(const std::string& key)
{
    if (keys_of(Preset::TYPE_PRINT).count(key)) return "process";
    if (keys_of(Preset::TYPE_FILAMENT).count(key)) return "filament";
    if (keys_of(Preset::TYPE_PRINTER).count(key)) return "printer";
    return "other";
}

// Sets per-object / per-part / layer-range overrides. A null value removes the override.
void apply_overrides(ModelConfig& target, const json& values, json& applied, json& errors)
{
    if (!values.is_object())
        throw std::runtime_error("'values' must be an object of setting -> value");
    for (const auto& [key, value] : values.items()) {
        if (value.is_null()) {
            if (target.erase(key))
                applied.push_back(key);
            continue;
        }
        if (key != "extruder" && !keys_of(Preset::TYPE_PRINT).count(key)) {
            errors[key] = "only process settings (and 'extruder') can be set per object, part or layer range";
            continue;
        }
        try {
            DynamicPrintConfig parsed;
            parsed.set_deserialize_strict(key, value_text(value));
            target.set_key_value(key, parsed.option(key)->clone());
            applied.push_back(key);
        } catch (const std::exception& e) {
            errors[key] = e.what();
        }
    }
}

// World-space triangles (instance rotation/scale applied, no translation) of the object's model parts.
std::vector<Vec3d> world_triangles(const ModelObject& o, size_t instance, bool with_offset)
{
    const ModelInstance* mi    = o.instances[instance];
    const Transform3d    inst  = with_offset ? mi->get_matrix() : mi->get_matrix_no_offset();
    std::vector<Vec3d>   out;
    for (const ModelVolume* v : o.volumes) {
        if (!v->is_model_part())
            continue;
        const Transform3d           m    = inst * v->get_matrix();
        const bool                  flip = m.linear().determinant() < 0;
        const indexed_triangle_set& its  = v->mesh().its;
        for (const auto& f : its.indices) {
            out.push_back(m * its.vertices[f[0]].cast<double>());
            if (flip) {
                out.push_back(m * its.vertices[f[2]].cast<double>());
                out.push_back(m * its.vertices[f[1]].cast<double>());
            } else {
                out.push_back(m * its.vertices[f[1]].cast<double>());
                out.push_back(m * its.vertices[f[2]].cast<double>());
            }
        }
    }
    return out;
}

// Rotation that turns `down` (a world direction) to point at the bed (-Z).
Matrix3d rotation_to_down(Vec3d down)
{
    if (down.norm() < 1e-9)
        throw std::runtime_error("'down' must be a non-zero vector");
    down.normalize();
    return Eigen::Quaterniond::FromTwoVectors(down, -Vec3d::UnitZ()).toRotationMatrix();
}

double support_threshold_angle()
{
    const DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
    const int                 angle  = config.has("support_threshold_angle") ? config.opt_int("support_threshold_angle") : 0;
    return angle > 0 ? angle : 30.0;
}

json object_json(size_t idx, bool detail)
{
    Plater&            p      = plater();
    const ModelObject* o      = p.model().objects[idx];
    PartPlateList&     plates = p.get_partplate_list();

    json j = {{"index", idx}, {"name", o->name}, {"source_file", o->input_file}, {"printable", o->printable}};

    json instances = json::array();
    for (size_t k = 0; k < o->instances.size(); ++k) {
        const ModelInstance* mi = o->instances[k];
        const BoundingBoxf3  bb = o->instance_bounding_box(k);
        instances.push_back({{"index", k},
                             {"plate", plates.find_instance(int(idx), int(k))},
                             {"position", vec_json(mi->get_offset())},
                             {"rotation_deg", vec_json(mi->get_rotation() / DEG)},
                             {"scale", vec_json(mi->get_scaling_factor())},
                             {"size", vec_json(bb.size())},
                             {"bbox_min", vec_json(bb.min)},
                             {"bbox_max", vec_json(bb.max)},
                             {"printable", mi->printable}});
    }
    j["instances"] = instances;
    if (!detail) {
        j["parts"] = o->volumes.size();
        return j;
    }

    const TriangleMeshStats stats = o->get_object_stl_stats();
    const Vec3d             s     = o->instances.front()->get_scaling_factor();
    size_t                  facets = 0;
    for (const ModelVolume* v : o->volumes)
        if (v->is_model_part())
            facets += v->mesh().its.indices.size();
    j["mesh"] = {{"facets", facets},
                 {"volume_mm3", round2(std::abs(stats.volume * s.x() * s.y() * s.z()))},
                 {"open_edges", stats.open_edges},
                 {"shells", stats.number_of_parts}};

    const Transform3d inst  = o->instances.front()->get_matrix();
    json              parts = json::array();
    for (size_t vi = 0; vi < o->volumes.size(); ++vi) {
        const ModelVolume*  v  = o->volumes[vi];
        const BoundingBoxf3 bb = v->mesh().transformed_bounding_box(inst * v->get_matrix());
        json                painted = json::array();
        if (!v->supported_facets.empty()) painted.push_back("support");
        if (!v->seam_facets.empty()) painted.push_back("seam");
        if (!v->mmu_segmentation_facets.empty()) painted.push_back("color");
        if (!v->fuzzy_skin_facets.empty()) painted.push_back("fuzzy_skin");
        parts.push_back({{"index", vi},
                         {"name", v->name},
                         {"type", volume_type_name(v->type())},
                         {"filament", v->config.has("extruder") ? v->config.opt_int("extruder") : 0},
                         {"settings", config_json(v->config.get())},
                         {"painted", painted},
                         {"facets", v->mesh().its.indices.size()},
                         {"bbox_min", vec_json(bb.min)},
                         {"bbox_max", vec_json(bb.max)}});
    }
    j["parts"]    = parts;
    j["settings"] = config_json(o->config.get());

    json ranges = json::array();
    for (const auto& [range, cfg] : o->layer_config_ranges)
        ranges.push_back({{"from", range.first}, {"to", range.second}, {"settings", config_json(cfg.get())}});
    j["layer_ranges"] = ranges;
    return j;
}

// Replaces the 3D view's selection when the request names objects or parts; otherwise keeps it.
void select_targets(const json& body)
{
    if (!body.contains("objects") && !body.contains("parts"))
        return;
    Plater&    p   = plater();
    Selection& sel = p.get_view3D_canvas3D()->get_selection();
    sel.clear();
    if (body.contains("objects")) {
        sel.set_mode(Selection::Instance);
        for (const auto& ref : body["objects"])
            sel.add_object((unsigned) object_index(ref), false);
    }
    if (body.contains("parts")) {
        sel.set_mode(Selection::Volume);
        for (const auto& item : body["parts"]) {
            if (!item.is_object() || !item.contains("object") || !item.contains("part"))
                throw std::runtime_error("each entry of 'parts' is {\"object\": ..., \"part\": ...}");
            const size_t obj = object_index(item["object"]);
            const size_t vol = part_index(*p.model().objects[obj], item["part"]);
            sel.add_volume((unsigned) obj, (unsigned) vol, 0, false);
        }
    }
    wxGetApp().obj_list()->update_selections();
    p.get_view3D_canvas3D()->set_as_dirty();
}

json selection_json()
{
    json objects = json::array();
    for (const auto& [obj, instances] : plater().get_selection().get_content())
        objects.push_back(obj);
    return objects;
}

PresetCollection& collection(const std::string& type)
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (type == "process" || type == "print") return pb->prints;
    if (type == "filament") return pb->filaments;
    if (type == "printer") return pb->printers;
    throw std::runtime_error("type must be process, filament or printer");
}

Preset::Type preset_type(const std::string& type)
{
    if (type == "process" || type == "print") return Preset::TYPE_PRINT;
    if (type == "filament") return Preset::TYPE_FILAMENT;
    if (type == "printer") return Preset::TYPE_PRINTER;
    throw std::runtime_error("type must be process, filament or printer");
}

const Preset& find_preset(const std::string& type, const std::string& name)
{
    PresetCollection& c = collection(type);
    if (const Preset* pr = c.find_preset(name, false))
        return *pr;
    const std::string& by_alias = wxGetApp().preset_bundle->get_preset_name_by_alias(preset_type(type), name);
    if (const Preset* pr = c.find_preset(by_alias, false))
        return *pr;
    throw std::runtime_error("no " + type + " preset named '" + name + "'; list them with list_presets");
}

// ---------------------------------------------------------------------------- objects

json cmd_objects(const json& body)
{
    Plater& p = plater();
    if (body.contains("object"))
        return object_json(object_arg(body), true);
    json list = json::array();
    for (size_t i = 0; i < p.model().objects.size(); ++i)
        list.push_back(object_json(i, body.value("detail", false)));
    return json{{"objects", list}, {"selection", selection_json()}};
}

json cmd_select(const json& body)
{
    Plater& p = plater();
    if (body.value("clear", false)) {
        p.get_view3D_canvas3D()->get_selection().clear();
        wxGetApp().obj_list()->update_selections();
    }
    select_targets(body);
    return json{{"selection", selection_json()}};
}

json cmd_transform(const json& body)
{
    Plater&                   p   = plater();
    const size_t              idx = object_arg(body);
    ModelObject*              o   = p.model().objects[idx];
    const std::vector<size_t> targets = instance_targets(*o, body);
    if ((body.contains("position")) && targets.size() > 1)
        throw std::runtime_error("this object has several instances; give 'instance' to set a position");

    Plater::TakeSnapshot snapshot(&p, "Claude: transform " + o->name);
    for (size_t k : targets) {
        ModelInstance* mi = o->instances[k];
        if (body.contains("scale")) {
            const json& s = body["scale"];
            const Vec3d f = s.is_number() ? Vec3d::Constant(s.get<double>()) : vec3(s, "scale");
            if (f.minCoeff() <= 0)
                throw std::runtime_error("scale factors must be positive (1 = 100%)");
            mi->set_scaling_factor(f);
        }
        if (body.contains("size")) {
            const json& s = body["size"];
            if (!s.is_array() || s.size() != 3)
                throw std::runtime_error("size is [x, y, z] in mm; use null for an axis to leave");
            const Vec3d current = o->instance_bounding_box(k, true).size();
            Vec3d       factor  = mi->get_scaling_factor();
            if (body.value("uniform", true)) {
                double ratio = 0;
                for (int a = 0; a < 3 && ratio == 0; ++a)
                    if (!s[a].is_null())
                        ratio = s[a].get<double>() / current[a];
                if (ratio <= 0)
                    throw std::runtime_error("give at least one positive size");
                factor *= ratio;
            } else {
                for (int a = 0; a < 3; ++a)
                    if (!s[a].is_null())
                        factor[a] *= s[a].get<double>() / current[a];
            }
            mi->set_scaling_factor(factor);
        }
        if (body.contains("rotation"))
            mi->set_rotation(vec3(body["rotation"], "rotation") * DEG);
        if (body.contains("rotate_by")) {
            const json& r = body["rotate_by"];
            Vec3d       axis;
            if (r.contains("axis") && r["axis"].is_string()) {
                const std::string a = r["axis"].get<std::string>();
                axis = a == "x" ? Vec3d(Vec3d::UnitX()) : a == "y" ? Vec3d(Vec3d::UnitY()) : a == "z" ? Vec3d(Vec3d::UnitZ()) : Vec3d(Vec3d::Zero());
            } else if (r.contains("axis")) {
                axis = vec3(r["axis"], "rotate_by.axis");
            }
            if (axis.norm() < 1e-9 || !r.contains("degrees"))
                throw std::runtime_error("rotate_by is {\"axis\": \"x\"|\"y\"|\"z\"|[x,y,z], \"degrees\": n}");
            mi->rotate(Eigen::AngleAxisd(r["degrees"].get<double>() * DEG, axis.normalized()).toRotationMatrix());
        }
        if (body.contains("down"))
            mi->rotate(rotation_to_down(vec3(body["down"], "down")));
        if (body.contains("position")) {
            const json& pos = body["position"];
            if (!pos.is_array() || (pos.size() != 2 && pos.size() != 3))
                throw std::runtime_error("position is [x, y] or [x, y, z] in mm");
            Vec3d off = mi->get_offset();
            off.x()   = pos[0].get<double>();
            off.y()   = pos[1].get<double>();
            if (pos.size() == 3)
                off.z() = pos[2].get<double>();
            mi->set_offset(off);
        }
        if (body.contains("move_by"))
            mi->set_offset(mi->get_offset() + vec3(body["move_by"], "move_by"));
    }
    o->invalidate_bounding_box();
    o->ensure_on_bed();
    for (size_t k : targets)
        p.get_partplate_list().notify_instance_update(int(idx), int(k));
    p.changed_object(int(idx));
    p.update();
    return object_json(idx, false);
}

json cmd_analyze_orientation(const json& body)
{
    Plater&            p     = plater();
    const size_t       idx   = object_arg(body);
    const ModelObject* o     = p.model().objects[idx];
    const size_t       inst  = instance_index(*o, body);
    const double       limit = body.value("overhang_angle", support_threshold_angle());

    const std::vector<Vec3d> tris = world_triangles(*o, inst, false);
    if (tris.empty())
        throw std::runtime_error("the object has no model parts");

    struct Candidate { Vec3d down; std::string label; };
    std::vector<Candidate> candidates;
    auto add = [&candidates](Vec3d d, const std::string& label) {
        d.normalize();
        for (const Candidate& c : candidates)
            if (c.down.dot(d) > 0.9995)
                return;
        candidates.push_back({d, label});
    };
    add(-Vec3d::UnitZ(), "current");
    if (body.contains("candidates"))
        for (const auto& c : body["candidates"])
            add(vec3(c, "candidate"), "given");

    // Flat faces of the convex hull are the stable resting positions.
    std::vector<Vec3f> points;
    points.reserve(tris.size());
    for (const Vec3d& v : tris)
        points.push_back(v.cast<float>());
    const indexed_triangle_set hull = its_convex_hull(points);
    struct Face { Vec3d normal; double area; };
    std::vector<Face> faces;
    for (const auto& f : hull.indices) {
        const Vec3d  a = hull.vertices[f[0]].cast<double>(), b = hull.vertices[f[1]].cast<double>(), c = hull.vertices[f[2]].cast<double>();
        Vec3d        n = (b - a).cross(c - a);
        const double len = n.norm();
        if (len < 1e-12)
            continue;
        n /= len;
        auto it = std::find_if(faces.begin(), faces.end(), [&n](const Face& face) { return face.normal.dot(n) > 0.9995; });
        if (it == faces.end())
            faces.push_back({n, 0.5 * len});
        else
            it->area += 0.5 * len;
    }
    std::sort(faces.begin(), faces.end(), [](const Face& l, const Face& r) { return l.area > r.area; });
    for (size_t i = 0; i < faces.size() && i < 10; ++i)
        add(faces[i].normal, "flat face, " + std::to_string(int(std::round(faces[i].area))) + " mm2");
    add(Vec3d::UnitZ(), "upside down (+Z down)");
    add(Vec3d::UnitX(), "+X down");
    add(-Vec3d::UnitX(), "-X down");
    add(Vec3d::UnitY(), "+Y down");
    add(-Vec3d::UnitY(), "-Y down");

    json results = json::array();
    for (const Candidate& cand : candidates) {
        const Matrix3d R    = rotation_to_down(cand.down);
        double         minz = DBL_MAX, maxz = -DBL_MAX;
        Vec2d          lo(DBL_MAX, DBL_MAX), hi(-DBL_MAX, -DBL_MAX);
        std::vector<Vec3d> r(tris.size());
        for (size_t i = 0; i < tris.size(); ++i) {
            r[i] = R * tris[i];
            minz = std::min(minz, r[i].z());
            maxz = std::max(maxz, r[i].z());
            lo   = lo.cwiseMin(r[i].head<2>());
            hi   = hi.cwiseMax(r[i].head<2>());
        }
        double contact = 0, overhang = 0, projected = 0, support = 0;
        for (size_t i = 0; i + 2 < r.size(); i += 3) {
            const Vec3d &a = r[i], &b = r[i + 1], &c = r[i + 2];
            Vec3d        n   = (b - a).cross(c - a);
            const double len = n.norm();
            if (len < 1e-12)
                continue;
            n /= len;
            const double area   = 0.5 * len;
            const double top    = std::max({a.z(), b.z(), c.z()});
            const double center = (a.z() + b.z() + c.z()) / 3.0;
            if (top < minz + 0.05) {
                if (n.z() < -0.99)
                    contact += area;
            } else if (n.z() < 0) {
                const double slope = std::acos(std::min(1.0, -n.z())) / DEG; // 0 = flat ceiling, 90 = wall
                if (slope < limit && center > minz + 0.1) {
                    overhang += area;
                    projected += area * -n.z();
                    support += area * -n.z() * (center - minz);
                }
            }
        }
        results.push_back({{"down", vec_json(cand.down)},
                           {"label", cand.label},
                           {"height_mm", round2(maxz - minz)},
                           {"footprint_mm", json::array({round2(hi.x() - lo.x()), round2(hi.y() - lo.y())})},
                           {"bed_contact_mm2", round2(contact)},
                           {"overhang_mm2", round2(overhang)},
                           {"overhang_projected_mm2", round2(projected)},
                           {"support_volume_estimate_mm3", round2(support)}});
    }
    std::stable_sort(results.begin(), results.end(), [](const json& l, const json& r) {
        return l["support_volume_estimate_mm3"].get<double>() < r["support_volume_estimate_mm3"].get<double>();
    });
    return json{{"object", idx},
                {"overhang_angle_deg", limit},
                {"note", "down vectors are in the current world frame; apply one with transform(down=...). The support estimate "
                         "ignores self-shadowing, so it ranks orientations rather than predicting grams."},
                {"candidates", results}};
}

// Connected regions of downward faces flatter than the support threshold: what would need support.
json cmd_detect_overhangs(const json& body)
{
    Plater&              p        = plater();
    const size_t         idx      = object_arg(body);
    const ModelObject*   o        = p.model().objects[idx];
    const size_t         inst     = instance_index(*o, body);
    const ModelInstance* mi       = o->instances[inst];
    const double         limit    = body.value("overhang_angle", support_threshold_angle());
    const double         min_area = body.value("min_area", 1.0);
    const size_t         max_out  = body.value("limit", 30);
    const double         bed_z    = o->instance_bounding_box(inst).min.z();
    const std::vector<Vec3d> world = world_triangles(*o, inst, true);

    struct Region { size_t part = 0, facets = 0; double area = 0, projected = 0, flattest = 90; BoundingBoxf3 bb; Vec3d center = Vec3d::Zero(); };
    std::vector<Region> regions;

    for (size_t vi = 0; vi < o->volumes.size(); ++vi) {
        const ModelVolume* v = o->volumes[vi];
        if (!v->is_model_part())
            continue;
        const Transform3d           m    = mi->get_matrix() * v->get_matrix();
        const bool                  flip = m.linear().determinant() < 0;
        const indexed_triangle_set& its  = v->mesh().its;
        const size_t                F    = its.indices.size();
        std::vector<int>            parent(F, -1);
        std::function<int(int)>     find = [&parent, &find](int i) { return parent[i] == i ? i : parent[i] = find(parent[i]); };
        std::vector<double>         area(F, 0), slope(F, 90), down(F, 0);
        std::vector<int>            owner(its.vertices.size(), -1);
        for (size_t f = 0; f < F; ++f) {
            const auto&  t = its.indices[f];
            const Vec3d  a = m * its.vertices[t[0]].cast<double>(), b = m * its.vertices[t[1]].cast<double>(),
                         c = m * its.vertices[t[2]].cast<double>();
            Vec3d        n   = (b - a).cross(c - a);
            const double len = n.norm();
            if (len < 1e-12 || std::max({a.z(), b.z(), c.z()}) < bed_z + 0.1)
                continue;
            n /= flip ? -len : len;
            if (n.z() >= 0)
                continue;
            const double s = std::acos(std::min(1.0, -n.z())) / DEG;
            if (s >= limit)
                continue;
            parent[f] = int(f);
            area[f]   = 0.5 * len;
            slope[f]  = s;
            down[f]   = -n.z();
            for (int k = 0; k < 3; ++k) {
                int& o_f = owner[t[k]];
                if (o_f < 0)
                    o_f = int(f);
                else
                    parent[find(int(f))] = find(o_f);
            }
        }
        std::map<int, size_t> root_to_region;
        for (size_t f = 0; f < F; ++f) {
            if (parent[f] < 0)
                continue;
            const int root = find(int(f));
            auto      it   = root_to_region.find(root);
            if (it == root_to_region.end()) {
                it = root_to_region.emplace(root, regions.size()).first;
                regions.push_back(Region());
                regions.back().part = vi;
            }
            Region&     r = regions[it->second];
            const auto& t = its.indices[f];
            Vec3d       centroid = Vec3d::Zero();
            for (int k = 0; k < 3; ++k) {
                const Vec3d pt = m * its.vertices[t[k]].cast<double>();
                r.bb.merge(pt);
                centroid += pt / 3.0;
            }
            r.facets += 1;
            r.area += area[f];
            r.projected += area[f] * down[f];
            r.flattest = std::min(r.flattest, slope[f]);
            r.center += centroid * area[f];
        }
    }

    // Where a support column under the region's centre would stand: the bed, or an upward face of the model.
    auto landing = [&world, bed_z](const Vec3d& pt) {
        double best = bed_z;
        bool   on_model = false;
        for (size_t i = 0; i + 2 < world.size(); i += 3) {
            const Vec3d &a = world[i], &b = world[i + 1], &c = world[i + 2];
            const Vec3d  n = (b - a).cross(c - a);
            if (n.z() <= 0)
                continue;
            const double d = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
            if (std::abs(d) < 1e-12)
                continue;
            const double u = ((pt.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (pt.y() - a.y())) / d;
            const double w = ((b.x() - a.x()) * (pt.y() - a.y()) - (pt.x() - a.x()) * (b.y() - a.y())) / d;
            if (u < 0 || w < 0 || u + w > 1)
                continue;
            const double z = a.z() + u * (b.z() - a.z()) + w * (c.z() - a.z());
            if (z < pt.z() - 0.05 && z > best) {
                best     = z;
                on_model = true;
            }
        }
        return std::make_pair(on_model, best);
    };

    std::sort(regions.begin(), regions.end(), [](const Region& l, const Region& r) { return l.projected > r.projected; });
    json   out = json::array();
    double total = 0;
    size_t shown = 0;
    for (Region& r : regions) {
        if (r.area < min_area)
            continue;
        total += r.area;
        if (shown++ >= max_out)
            continue;
        const Vec3d center       = r.center / r.area;
        const auto [on_model, z] = landing(Vec3d(center.x(), center.y(), r.bb.min.z()));
        out.push_back({{"part", r.part},
                       {"facets", r.facets},
                       {"area_mm2", round2(r.area)},
                       {"projected_mm2", round2(r.projected)},
                       {"flattest_slope_deg", round2(r.flattest)},
                       {"center", vec_json(center)},
                       {"bbox_min", vec_json(r.bb.min)},
                       {"bbox_max", vec_json(r.bb.max)},
                       {"height_above_bed_mm", round2(r.bb.min.z() - bed_z)},
                       {"support_lands_on", on_model ? "model" : "bed"},
                       {"support_drop_mm", round2(r.bb.min.z() - z)}});
    }
    return json{{"object", idx},
                {"overhang_angle_deg", limit},
                {"regions", out},
                {"region_count", shown},
                {"total_area_mm2", round2(total)},
                {"note", "slope 0 = flat ceiling, 90 = vertical wall. Short drops onto the model or small flat spans can often "
                         "bridge or be left unsupported; to support one region only, paint(kind=support, overhang_angle, "
                         "box_min/box_max around it) or add_shape(type=support_enforcer)."}};
}

// Everything the slicer would complain about: placement (off plate, outside the bed, too tall, floating,
// overlapping), mesh errors, unsupported overhangs, the plate validation, slice-time conflicts and the
// error/warning notifications on screen.
json cmd_check_errors(const json& body)
{
    Plater&                   p      = plater();
    Model&                    model  = p.model();
    PartPlateList&            plates = p.get_partplate_list();
    const DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
    json errors = json::array(), warnings = json::array(), info = json::array();
    auto issue = [](json& list, const std::string& kind, const std::string& message, json extra = json::object()) {
        extra["kind"]    = kind;
        extra["message"] = message;
        list.push_back(extra);
    };

    // The slicer's own check of the current plate; it also refreshes the notifications read below.
    bool model_fits = true, validate_error = false;
    p.validate_current_plate(model_fits, validate_error);

    struct Placed { size_t obj, inst; int plate; Polygon hull; BoundingBoxf3 bb; std::string name; };
    std::vector<Placed> placed;
    const double eps = 0.05;
    for (size_t i = 0; i < model.objects.size(); ++i) {
        ModelObject* o = model.objects[i];
        for (size_t k = 0; k < o->instances.size(); ++k) {
            const ModelInstance* mi    = o->instances[k];
            const BoundingBoxf3  bb    = o->instance_bounding_box(k);
            const int            plate = plates.find_instance(int(i), int(k));
            const std::string    name  = o->name + (o->instances.size() > 1 ? " (instance " + std::to_string(k) + ")" : "");
            json                 where = {{"object", i}, {"instance", k}, {"plate", plate}};
            if (plate < 0) {
                issue(errors, "off_plate", name + " is not on any plate", where);
            } else if (PartPlate* pp = plates.get_plate(plate)) {
                const BoundingBoxf3 vol = pp->get_build_volume(true);
                if (bb.min.x() < vol.min.x() - eps || bb.min.y() < vol.min.y() - eps || bb.max.x() > vol.max.x() + eps ||
                    bb.max.y() > vol.max.y() + eps) {
                    where["object_bbox"] = {vec_json(bb.min), vec_json(bb.max)};
                    where["bed_area"]    = {vec_json(vol.min), vec_json(vol.max)};
                    issue(errors, "outside_bed", name + " extends past the edge of plate " + std::to_string(plate + 1), where);
                }
                if (vol.max.z() > vol.min.z() + 1 && bb.max.z() > vol.max.z() + eps)
                    issue(errors, "too_tall", name + " is " + std::to_string(int(std::round(bb.max.z()))) + " mm tall; the printer reaches " +
                                                  std::to_string(int(std::round(vol.max.z()))) + " mm", where);
            }
            if (bb.min.z() > eps)
                issue(warnings, "floating", name + " floats " + std::to_string(round2(bb.min.z())).substr(0, 5) + " mm above the bed", where);
            else if (bb.min.z() < -eps)
                issue(info, "sinking", name + " is sunk " + std::to_string(round2(-bb.min.z())).substr(0, 5) + " mm into the bed; that part is not printed", where);
            if (!mi->printable || !o->printable)
                issue(info, "not_printable", name + " is marked not printable and will be skipped", where);
            placed.push_back({i, k, plate, o->convex_hull_2d(mi->get_matrix()), bb, name});
        }

        // First layer: the object's cross-section half a first layer above its lowest point. Nothing there is
        // the slicer's "empty first layer" error; very little means poor adhesion.
        if (!o->instances.empty()) {
            const double first_layer = config.has("initial_layer_print_height") ? config.opt_float("initial_layer_print_height") : 0.2;
            const BoundingBoxf3 bb   = o->instance_bounding_box(0);
            const float         z    = float(std::max(0.0, bb.min.z()) + first_layer * 0.5);
            Polygons            section;
            for (const ModelVolume* v : o->volumes) {
                if (!v->is_model_part())
                    continue;
                MeshSlicingParams params;
                params.trafo = o->instances.front()->get_matrix() * v->get_matrix();
                append(section, slice_mesh(v->mesh().its, z, params));
            }
            // Slivers narrower than an extrusion line print nothing, so erode by half a nozzle width first.
            const auto*  nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
            const double nozzle  = nozzles && !nozzles->values.empty() ? nozzles->values.front() : 0.4;
            const Polygons merged = union_(section);
            double contact = 0, printable = 0;
            for (const Polygon& pg : merged)
                contact += pg.area() * SCALING_FACTOR * SCALING_FACTOR;
            for (const Polygon& pg : offset(merged, -float(scale_(nozzle * 0.5))))
                printable += pg.area() * SCALING_FACTOR * SCALING_FACTOR;
            const json where = {{"object", i}, {"first_layer_area_mm2", round2(contact)}, {"printable_first_layer_mm2", round2(printable)}};
            if (printable < 0.05)
                issue(errors, "empty_first_layer",
                      o->name + " has almost nothing on the first layer (" + std::to_string(round2(contact)).substr(0, 4) +
                          " mm2), so the slicer will refuse it. Lay a flat face down (analyze_orientation), cut the bottom flat, or enable supports.",
                      where);
            else if (contact < 25 && bb.size().z() > 4 * std::sqrt(contact))
                issue(warnings, "small_bed_contact",
                      o->name + " stands " + std::to_string(int(std::round(bb.size().z()))) + " mm tall on only " +
                          std::to_string(int(std::round(contact))) + " mm2 of first layer; it may come loose. Consider a brim or another orientation.",
                      where);
        }

        const TriangleMeshStats stats = o->get_object_stl_stats();
        if (stats.open_edges > 0)
            issue(warnings, "mesh_errors", o->name + " has " + std::to_string(stats.open_edges) + " open edges (not watertight); try model_action repair",
                  {{"object", i}});

        // Overhangs, against whether this object gets supports.
        const bool supports = o->config.has("enable_support") ? o->config.get().opt_bool("enable_support") : config.opt_bool("enable_support");
        const json oh       = cmd_detect_overhangs(json{{"object", i}, {"limit", 3}, {"min_area", 2.0}});
        const double area   = oh["total_area_mm2"].get<double>();
        if (area > 0) {
            json extra = {{"object", i}, {"regions", oh["region_count"]}, {"total_area_mm2", area}, {"largest", oh["regions"]}};
            if (!supports)
                issue(warnings, "unsupported_overhangs",
                      o->name + " has " + std::to_string(oh["region_count"].get<size_t>()) + " overhang region(s), " +
                          std::to_string(int(std::round(area))) + " mm2, flatter than " + std::to_string(int(oh["overhang_angle_deg"].get<double>())) +
                          " deg and supports are off. Small or bridged spans may print fine; otherwise enable supports, paint them, or reorient.",
                      extra);
            else
                issue(info, "supported_overhangs", o->name + " has overhangs; supports are on", extra);
        }
    }

    // Outlines that overlap on the same plate.
    for (size_t a = 0; a < placed.size(); ++a)
        for (size_t b = a + 1; b < placed.size(); ++b) {
            const Placed &A = placed[a], &B = placed[b];
            if (A.plate < 0 || A.plate != B.plate || A.bb.max.x() < B.bb.min.x() || B.bb.max.x() < A.bb.min.x() ||
                A.bb.max.y() < B.bb.min.y() || B.bb.max.y() < A.bb.min.y())
                continue;
            double overlap = 0;
            for (const Polygon& pg : intersection(Polygons{A.hull}, Polygons{B.hull}))
                overlap += std::abs(pg.area()) * SCALING_FACTOR * SCALING_FACTOR;
            if (overlap > 0.5)
                issue(errors, "overlap", A.name + " and " + B.name + " overlap by about " + std::to_string(int(std::round(overlap))) +
                                             " mm2 on the bed (convex outlines; nested shapes can be a false alarm). Use arrange or move one.",
                      {{"objects", json::array({A.obj, B.obj})}, {"instances", json::array({A.inst, B.inst})}, {"plate", A.plate}});
        }

    // Results of the last slice of each plate.
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate* plate = plates.get_plate(i);
        if (!plate || plate->empty())
            continue;
        GCodeProcessorResult* r = plate->get_slice_result();
        if (!plate->is_slice_result_valid() || !r) {
            issue(info, "not_sliced", "plate " + std::to_string(i + 1) + " is not sliced; slice it to check toolpaths too", {{"plate", i}});
            continue;
        }
        if (r->conflict_result)
            issue(errors, "toolpath_conflict",
                  "toolpaths of " + r->conflict_result->_objName1 + " and " + r->conflict_result->_objName2 + " collide at " +
                      std::to_string(round2(r->conflict_result->_height)).substr(0, 6) + " mm",
                  {{"plate", i}, {"height_mm", r->conflict_result->_height}});
        if (r->toolpath_outside)
            issue(errors, "toolpath_outside", "toolpaths on plate " + std::to_string(i + 1) + " go outside the printable area", {{"plate", i}});
        if (r->gcode_check_result.error_code != 0)
            issue(errors, "printable_area", "plate " + std::to_string(i + 1) + " has extrusions outside the printable area or height (code " +
                                                std::to_string(r->gcode_check_result.error_code) + ")", {{"plate", i}});
        if (r->exclusion_volume_path_conflict || r->exclusion_volume_extrusion_conflict || r->exclusion_volume_travel_conflict)
            issue(errors, "exclusion_area", "plate " + std::to_string(i + 1) + " moves through an excluded area of the bed", {{"plate", i}});
    }

    // What the slicer itself is showing.
    using Level = NotificationManager::NotificationLevel;
    for (const auto& n : p.get_notification_manager()->active_notifications()) {
        if (n.text.empty() || n.text.rfind("Object name:", 0) == 0) // skip the selected object's size/volume panel
            continue;
        json extra = {{"source", "slicer notification"}, {"outdated", n.outdated}};
        if (n.level == Level::ErrorNotificationLevel)
            issue(errors, "slicer_error", n.text, extra);
        else if (n.level == Level::WarningNotificationLevel || n.level == Level::SeriousWarningNotificationLevel)
            issue(warnings, "slicer_warning", n.text, extra);
        else if (n.level == Level::ImportantNotificationLevel || n.level == Level::PrintInfoNotificationLevel)
            issue(info, "slicer_info", n.text, extra);
    }

    return json{{"ok", errors.empty()},
                {"model_fits", model_fits},
                {"validate_error", validate_error},
                {"errors", errors},
                {"warnings", warnings},
                {"info", info}};
}

json cmd_paint(const json& body)
{
    Plater&      p    = plater();
    const size_t idx  = object_arg(body);
    ModelObject* o    = p.model().objects[idx];
    const std::string kind = body.value("kind", std::string("support"));
    const std::string mode = body.value("mode", std::string("enforce"));

    EnforcerBlockerType state = EnforcerBlockerType::NONE;
    if (kind == "color") {
        const int filament = body.value("filament", -1);
        if (filament < 0 || filament > int(EnforcerBlockerType::ExtruderMax))
            throw std::runtime_error("color painting needs 'filament' (1-based; 0 clears)");
        state = EnforcerBlockerType(filament);
    } else if (mode == "enforce") {
        state = EnforcerBlockerType::ENFORCER;
    } else if (mode == "block") {
        if (kind == "fuzzy_skin")
            throw std::runtime_error("fuzzy skin has only enforce and clear");
        state = EnforcerBlockerType::BLOCKER;
    } else if (mode != "clear") {
        throw std::runtime_error("mode must be enforce, block or clear");
    }
    if (kind != "support" && kind != "seam" && kind != "color" && kind != "fuzzy_skin")
        throw std::runtime_error("kind must be support, seam, color or fuzzy_skin");

    const json  where = body.value("where", json::object());
    const bool  all   = where.value("all", false);
    const bool  has_overhang = where.contains("overhang_angle");
    const double overhang    = has_overhang ? where["overhang_angle"].get<double>() : 0;
    const bool  has_facing   = where.contains("facing");
    const Vec3d facing       = has_facing ? vec3(where["facing"], "facing").normalized() : Vec3d::Zero();
    const double facing_cos  = std::cos(where.value("facing_tolerance_deg", 10.0) * DEG);
    const bool  has_box      = where.contains("box_min") || where.contains("box_max");
    const Vec3d box_min      = where.contains("box_min") ? vec3(where["box_min"], "box_min") : Vec3d::Constant(-DBL_MAX);
    const Vec3d box_max      = where.contains("box_max") ? vec3(where["box_max"], "box_max") : Vec3d::Constant(DBL_MAX);
    const double z_min       = where.value("z_min", -DBL_MAX);
    const double z_max       = where.value("z_max", DBL_MAX);
    const bool  exclude_bed  = where.value("exclude_bed", true);
    if (!all && !has_overhang && !has_facing && !has_box && !where.contains("z_min") && !where.contains("z_max"))
        throw std::runtime_error("'where' needs all, overhang_angle, facing, box_min/box_max or z_min/z_max");

    p.exit_gizmo(); // an open paint gizmo would write its own state back over this
    Plater::TakeSnapshot snapshot(&p, "Claude: paint " + kind);

    const ModelInstance* mi    = o->instances.front();
    const double         bed_z = o->instance_bounding_box(0).min.z();
    json                 per_part = json::array();
    size_t               total    = 0;
    bool                 changed  = false;
    for (size_t vi = 0; vi < o->volumes.size(); ++vi) {
        ModelVolume* v = o->volumes[vi];
        if (!v->is_model_part())
            continue;
        FacetsAnnotation& facets = kind == "support" ? v->supported_facets :
                                   kind == "seam"    ? v->seam_facets :
                                   kind == "color"   ? v->mmu_segmentation_facets :
                                                       v->fuzzy_skin_facets;
        TriangleSelector selector(v->mesh());
        if (!body.value("replace", false))
            selector.deserialize(facets.get_data());

        const Transform3d           m    = mi->get_matrix() * v->get_matrix();
        const bool                  flip = m.linear().determinant() < 0;
        const indexed_triangle_set& its  = v->mesh().its;
        size_t                      count = 0;
        for (size_t f = 0; f < its.indices.size(); ++f) {
            const auto&  t = its.indices[f];
            const Vec3d  a = m * its.vertices[t[0]].cast<double>(), b = m * its.vertices[t[1]].cast<double>(),
                         c = m * its.vertices[t[2]].cast<double>();
            Vec3d        n   = (b - a).cross(c - a);
            const double len = n.norm();
            if (len < 1e-12)
                continue;
            n /= flip ? -len : len;
            const Vec3d center = (a + b + c) / 3.0;
            if (exclude_bed && std::max({a.z(), b.z(), c.z()}) < bed_z + 0.1)
                continue;
            if (has_overhang && !(n.z() < 0 && std::acos(std::min(1.0, -n.z())) / DEG < overhang))
                continue;
            if (has_facing && n.dot(facing) < facing_cos)
                continue;
            if (has_box && !((center.array() >= box_min.array()).all() && (center.array() <= box_max.array()).all()))
                continue;
            if (center.z() < z_min || center.z() > z_max)
                continue;
            selector.set_facet(int(f), state);
            ++count;
        }
        changed |= facets.set(selector);
        total += count;
        per_part.push_back({{"part", vi}, {"facets", count}});
    }
    if (changed) {
        wxGetApp().obj_list()->update_info_items(idx);
        p.changed_object(int(idx));
    }

    json r = {{"object", idx}, {"kind", kind}, {"facets_painted", total}, {"parts", per_part}};
    if (kind == "support") {
        const DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
        if (!config.opt_bool("enable_support"))
            r["note"] = "enable_support is off, so painted enforcers have no effect until it is on (support_type "
                        "normal(manual)/tree(manual) uses only painted and modifier enforcers)";
    }
    return r;
}

json cmd_add_shape(const json& body)
{
    Plater&               p     = plater();
    const size_t          idx   = object_arg(body);
    ModelObject*          o     = p.model().objects[idx];
    const ModelVolumeType type  = volume_type_from(body.value("type", std::string("modifier")));
    const std::string     shape = body.value("shape", std::string("box"));
    const Vec3d           size  = body.contains("size") ? vec3(body["size"], "size") : Vec3d(10, 10, 10);
    if (size.minCoeff() <= 0)
        throw std::runtime_error("size must be positive");

    TriangleMesh mesh;
    if (shape == "box")
        mesh = TriangleMesh(its_make_cube(size.x(), size.y(), size.z()));
    else if (shape == "cylinder")
        mesh = TriangleMesh(its_make_cylinder(size.x() / 2, size.z()));
    else if (shape == "sphere")
        mesh = TriangleMesh(its_make_sphere(size.x() / 2, PI / 60));
    else
        throw std::runtime_error("shape must be box, cylinder (size = [diameter, -, height]) or sphere (size = [diameter, -, -])");

    const ModelInstance* mi     = o->instances.front();
    const Vec3d          center = body.contains("center") ? vec3(body["center"], "center") : o->instance_bounding_box(0).center();

    Plater::TakeSnapshot snapshot(&p, "Claude: add " + shape);
    ModelVolume*         v = o->add_volume(std::move(mesh), type);
    // Aligned with the bed like the GUI's added shapes, then placed at `center` (world coordinates).
    const Transform3d inst = mi->get_matrix_no_offset();
    v->set_transformation(Transform3d(inst.inverse()));
    v->set_offset(inst.inverse() * (center - mi->get_offset()));
    v->name = body.value("name", std::string(volume_type_name(type)) + "-" + shape);
    if (type == ModelVolumeType::MODEL_PART)
        v->config.set_key_value("extruder", new ConfigOptionInt(o->config.has("extruder") ? o->config.opt_int("extruder") : 0));
    v->source.is_from_builtin_objects = true;

    json applied = json::array(), errors = json::object();
    if (body.contains("settings"))
        apply_overrides(v->config, body["settings"], applied, errors);

    ObjectList* list = wxGetApp().obj_list();
    list->reorder_volumes_and_get_selection(int(idx));
    if (!applied.empty())
        list->object_config_options_changed({o, v});

    const auto it = std::find(o->volumes.begin(), o->volumes.end(), v);
    return json{{"object", idx}, {"part", it - o->volumes.begin()}, {"applied", applied}, {"errors", errors}};
}

json cmd_remove_part(const json& body)
{
    Plater&      p    = plater();
    const size_t idx  = object_arg(body);
    ModelObject* o    = p.model().objects[idx];
    const size_t part = part_index(*o, body["part"]);
    if (o->volumes[part]->is_model_part() &&
        std::count_if(o->volumes.begin(), o->volumes.end(), [](const ModelVolume* v) { return v->is_model_part(); }) == 1)
        throw std::runtime_error("this is the object's only part; delete the object instead");
    Plater::TakeSnapshot snapshot(&p, "Claude: remove part");
    wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itVolume, int(idx), int(part));
    return object_json(idx, true);
}

json cmd_edit_part(const json& body)
{
    Plater&      p    = plater();
    const size_t idx  = object_arg(body);
    ModelObject* o    = p.model().objects[idx];
    ObjectList*  list = wxGetApp().obj_list();
    if (!body.contains("part")) {
        if (body.contains("name")) {
            o->name = body["name"].get<std::string>();
            list->update_name_for_items();
        }
        if (body.contains("filament")) {
            select_targets(json{{"objects", json::array({idx})}});
            list->set_extruder_for_selected_items(body["filament"].get<int>());
        }
        return object_json(idx, true);
    }
    const size_t part = part_index(*o, body["part"]);
    ModelVolume* v    = o->volumes[part];
    if (body.contains("name")) {
        v->name = body["name"].get<std::string>();
        list->update_name_for_items();
    }
    if (body.contains("type") || body.contains("filament")) {
        select_targets(json{{"parts", json::array({json{{"object", idx}, {"part", part}}})}});
        if (body.contains("type")) {
            const ModelVolumeType type = volume_type_from(body["type"].get<std::string>());
            if (v->is_model_part() && type != ModelVolumeType::MODEL_PART &&
                std::count_if(o->volumes.begin(), o->volumes.end(), [](const ModelVolume* m) { return m->is_model_part(); }) == 1)
                throw std::runtime_error("an object needs at least one model part");
            list->set_volume_type(type, false);
        }
        if (body.contains("filament"))
            list->set_extruder_for_selected_items(body["filament"].get<int>());
    }
    return object_json(idx, true);
}

json cmd_object_settings(const json& body)
{
    Plater&      p    = plater();
    const size_t idx  = object_arg(body);
    ModelObject* o    = p.model().objects[idx];
    ObjectList*  list = wxGetApp().obj_list();
    const json   values = body.value("values", json::object());
    json         applied = json::array(), errors = json::object();

    Plater::TakeSnapshot snapshot(&p, "Claude: object settings");
    if (body.contains("layer_range")) {
        const json& r = body["layer_range"];
        if (!r.is_array() || r.size() != 2 || r[0].get<double>() >= r[1].get<double>())
            throw std::runtime_error("layer_range is [from_z, to_z] in mm with from < to");
        const t_layer_height_range range(r[0].get<double>(), r[1].get<double>());
        if (body.value("remove", false)) {
            o->layer_config_ranges.erase(range);
        } else {
            const bool   is_new = !o->layer_config_ranges.count(range);
            ModelConfig& cfg    = o->layer_config_ranges[range];
            if (is_new && !values.contains("layer_height"))
                cfg.set_key_value("layer_height", new ConfigOptionFloat(wxGetApp().preset_bundle->full_config().opt_float("layer_height")));
            apply_overrides(cfg, values, applied, errors);
        }
        list->update_after_undo_redo();
    } else if (body.contains("part")) {
        ModelVolume* v = o->volumes[part_index(*o, body["part"])];
        apply_overrides(v->config, values, applied, errors);
        list->object_config_options_changed({o, v});
    } else {
        apply_overrides(o->config, values, applied, errors);
        list->object_config_options_changed({o, nullptr});
    }
    p.changed_object(int(idx));
    return json{{"applied", applied}, {"errors", errors}, {"object", object_json(idx, true)}};
}

json cmd_cut(const json& body)
{
    Plater&      p    = plater();
    const size_t idx  = object_arg(body);
    ModelObject* o    = p.model().objects[idx];
    const size_t inst = instance_index(*o, body);
    if (!body.contains("z"))
        throw std::runtime_error("give 'z', the world height of the horizontal cut");
    const double        z  = body["z"].get<double>();
    const BoundingBoxf3 bb = o->instance_bounding_box(inst);
    if (z <= bb.min.z() || z >= bb.max.z())
        throw std::runtime_error("z must be inside the object (" + std::to_string(bb.min.z()) + " .. " + std::to_string(bb.max.z()) + ")");

    const std::string keep     = body.value("keep", std::string("both"));
    const bool        as_parts = body.value("as_parts", false);
    const ModelObjectCutAttributes attributes =
        only_if(keep != "lower", ModelObjectCutAttribute::KeepUpper) | only_if(keep != "upper", ModelObjectCutAttribute::KeepLower) |
        only_if(as_parts, ModelObjectCutAttribute::KeepAsParts) |
        only_if(body.value("flip_upper", false), ModelObjectCutAttribute::FlipUpper) |
        only_if(body.value("place_lower_on_cut", false), ModelObjectCutAttribute::PlaceOnCutLower) |
        only_if(o->cut_id.id().invalid(), ModelObjectCutAttribute::InvalidateCutInfo) | ModelObjectCutAttribute::KeepPaint;

    Plater::TakeSnapshot snapshot(&p, "Claude: cut");
    // Cutting an object that has height-range settings hangs the slicer (the pieces inherit ranges that no
    // longer match their heights), so the ranges are dropped first and reported; undo brings them back.
    json removed_ranges = json::array();
    if (!o->layer_config_ranges.empty()) {
        for (const auto& [range, cfg] : o->layer_config_ranges)
            removed_ranges.push_back({{"from", range.first}, {"to", range.second}, {"settings", config_json(cfg.get())}});
        o->layer_config_ranges.clear();
        wxGetApp().obj_list()->update_after_undo_redo();
    }
    const Vec3d offset = o->instances[inst]->get_offset();
    Cut         cut(o, int(inst), Geometry::translation_transform(z * Vec3d::UnitZ() - offset), attributes);
    p.apply_cut_object_to_model(idx, cut.perform_with_plane());
    json r = {{"object_count", p.model().objects.size()},
              {"note_pieces", "the pieces stay where they were cut and may overlap; run model_action arrange, then check_errors"}};
    if (!removed_ranges.empty()) {
        r["removed_layer_ranges"] = removed_ranges;
        r["note"] = "height-range settings were removed before cutting; re-add them to the pieces with object_settings(layer_range=...)";
    }
    return r;
}

// Actions on the selection (or the whole plate), the same ones as the GUI's menus and toolbar.
json cmd_action(const json& body)
{
    Plater&           p    = plater();
    ObjectList*       list = wxGetApp().obj_list();
    const std::string name = body.value("action", std::string());
    auto done = [&p, &name]() {
        return json{{"action", name}, {"busy", !p.get_ui_job_worker().is_idle()}, {"selection", selection_json()}};
    };

    if (name == "undo") { p.undo(); return done(); }
    if (name == "redo") { p.redo(); return done(); }
    if (name == "select_all") { p.select_all(); return done(); }
    if (name == "deselect_all") { p.deselect_all(); return done(); }
    if (name == "add_plate") { p.add_plate(); return done(); }
    if (name == "delete_plate") { p.delete_plate(body.value("plate", -1)); return done(); }
    if (name == "select_plate") { p.select_plate(body.value("plate", 0)); return done(); }
    if (name == "paste") { p.paste_from_clipboard(); return done(); }
    if (name == "arrange" || name == "auto_orient") {
        if (body.contains("objects") || body.contains("parts"))
            select_targets(body);
        else
            p.deselect_all(); // nothing selected = the whole plate
        name == "arrange" ? p.arrange() : p.orient();
        return done();
    }

    select_targets(body);
    if (p.is_selection_empty())
        throw std::runtime_error("nothing is selected; pass objects=[...] or parts=[...]");

    if (name == "delete") p.remove_selected();
    else if (name == "center") p.center_selection();
    else if (name == "drop_to_bed") p.drop_selection();
    else if (name == "mirror_x") p.mirror(X);
    else if (name == "mirror_y") p.mirror(Y);
    else if (name == "mirror_z") p.mirror(Z);
    else if (name == "split_to_objects") p.split_object();
    else if (name == "split_to_parts") p.split_volume();
    else if (name == "assemble") {
        if (!list->can_merge_to_multipart_object())
            throw std::runtime_error("select two or more objects to assemble");
        list->merge(true);
    } else if (name == "merge_parts") {
        if (!list->can_merge_to_single_object())
            throw std::runtime_error("select one multi-part object, or several objects, to merge into a single part");
        list->merge(false);
    }
    else if (name == "add_instances") p.increase_instances(size_t(std::max(1, body.value("count", 1))));
    else if (name == "remove_instances") p.decrease_instances(size_t(std::max(1, body.value("count", 1))));
    else if (name == "instances_to_objects") list->split_instances();
    else if (name == "fill_bed") p.fill_bed_with_instances();
    else if (name == "scale_to_fit") p.scale_selection_to_fit_print_volume();
    else if (name == "toggle_printable") list->toggle_printable_state();
    else if (name == "repair") list->fix_through_cgal();
    else if (name == "copy") p.copy_selection_to_clipboard();
    else if (name == "reload_from_disk") p.reload_from_disk();
    else if (name == "set_filament") list->set_extruder_for_selected_items(body.value("filament", 1));
    else
        throw std::runtime_error("unknown action '" + name + "'");
    return done();
}

// ---------------------------------------------------------------------------- view

json cmd_view(const json& body)
{
    Plater& p = plater();
    if (body.contains("tab")) {
        const std::string tab = body["tab"].get<std::string>();
        if (tab == "prepare")
            wxGetApp().mainframe->select_tab(wxString(TAB_ID_PREPARE));
        else if (tab == "preview")
            wxGetApp().mainframe->select_tab(wxString(TAB_ID_PREVIEW));
        else
            throw std::runtime_error("tab must be prepare or preview");
    }
    GLCanvas3D* canvas = p.get_current_canvas3D();
    if (!canvas)
        throw std::runtime_error("no 3D view");
    if (body.contains("camera"))
        canvas->select_view(body["camera"].get<std::string>()); // iso, top, bottom, front, rear, left, right
    const std::string zoom = body.value("zoom", std::string());
    if (zoom == "bed") canvas->zoom_to_bed();
    else if (zoom == "objects") canvas->zoom_to_volumes();
    else if (zoom == "selection") canvas->zoom_to_selection();
    else if (!zoom.empty()) throw std::runtime_error("zoom must be bed, objects or selection");
    if (body.contains("show_overhangs")) {
        GLCanvas3D* view3d = p.get_view3D_canvas3D();
        view3d->show_overhang(body["show_overhangs"].get<bool>());
        view3d->set_as_dirty();
    }
    canvas->set_as_dirty();
    canvas->request_extra_frame();
    return json{{"preview", p.is_preview_shown()}, {"overhangs_shown", p.get_view3D_canvas3D()->is_overhang_shown()}};
}

json cmd_screenshot(const json& body)
{
    Plater&     p      = plater();
    GLCanvas3D* canvas = p.get_current_canvas3D();
    wxGLCanvas* gl     = canvas ? canvas->get_wxglcanvas() : nullptr;
    if (!gl || !gl->IsShownOnScreen())
        throw std::runtime_error("the 3D view is not on screen; switch to the Prepare or Preview tab first (view)");

    canvas->render();
    const Size size = canvas->get_canvas_size();
    const int  w = size.get_width(), h = size.get_height();
    if (w <= 0 || h <= 0)
        throw std::runtime_error("the 3D view has no size");

    // The front buffer holds the frame just rendered and swapped.
    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4);
    GLint read_buffer = GL_BACK;
    glFinish();
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glReadBuffer(GLenum(read_buffer));

    bool uniform = true;
    for (size_t i = 4; i < rgba.size() && uniform; i += 4 * 97)
        uniform = std::equal(rgba.begin(), rgba.begin() + 3, rgba.begin() + i);

    wxImage image;
    if (!uniform) {
        unsigned char* rgb = (unsigned char*) malloc(size_t(w) * size_t(h) * 3); // wxImage takes ownership
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const unsigned char* src = &rgba[(size_t(h - 1 - y) * w + x) * 4];
                unsigned char*       dst = rgb + (size_t(y) * w + x) * 3;
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            }
        image = wxImage(w, h, rgb);
    } else {
        // Some drivers do not expose the front buffer; copy the window from the screen instead.
        const wxRect rect = gl->GetScreenRect();
        wxBitmap     bitmap(rect.width, rect.height);
        {
            wxMemoryDC mem(bitmap);
            wxScreenDC screen;
            mem.Blit(0, 0, rect.width, rect.height, &screen, rect.x, rect.y);
        }
        image = bitmap.ConvertToImage();
    }

    const int max_width = body.value("max_width", 1280);
    if (max_width > 0 && image.GetWidth() > max_width)
        image.Rescale(max_width, std::max(1, image.GetHeight() * max_width / image.GetWidth()), wxIMAGE_QUALITY_HIGH);

    wxMemoryOutputStream out;
    if (!image.SaveFile(out, wxBITMAP_TYPE_PNG))
        throw std::runtime_error("could not encode the screenshot");
    std::string bytes(out.GetLength(), '\0');
    out.CopyTo(bytes.data(), bytes.size());
    std::string b64(boost::beast::detail::base64::encoded_size(bytes.size()), '\0');
    b64.resize(boost::beast::detail::base64::encode(b64.data(), bytes.data(), bytes.size()));
    return json{{"png_base64", b64}, {"width", image.GetWidth()}, {"height", image.GetHeight()}, {"preview", p.is_preview_shown()}};
}

// ---------------------------------------------------------------------------- settings knowledge

const char* type_name(ConfigOptionType t)
{
    switch (t) {
    case coFloat: return "float";
    case coFloats: return "float per extruder/filament";
    case coInt: return "int";
    case coInts: return "int per extruder/filament";
    case coString: return "string";
    case coStrings: return "strings";
    case coPercent: return "percent";
    case coPercents: return "percent per extruder/filament";
    case coFloatOrPercent: return "float or percent";
    case coFloatsOrPercents: return "float or percent per extruder/filament";
    case coPoint: return "point";
    case coPoints: return "points";
    case coBool: return "bool";
    case coBools: return "bool per extruder/filament";
    case coEnum: return "enum";
    case coEnums: return "enum per extruder/filament";
    default: return "other";
    }
}

const char* mode_name(ConfigOptionMode m)
{
    switch (m) {
    case comSimple: return "simple";
    case comAdvanced: return "advanced";
    case comExpert: return "expert";
    default: return "develop";
    }
}

json def_json(const std::string& key, const ConfigOptionDef& d, const DynamicPrintConfig& config)
{
    json j = {{"key", key},
              {"label", d.full_label.empty() ? d.label : d.full_label},
              {"category", d.category},
              {"scope", scope_of(key)},
              {"per_object", keys_of(Preset::TYPE_PRINT).count(key) > 0},
              {"type", type_name(d.type)},
              {"mode", mode_name(d.mode)},
              {"tooltip", d.tooltip}};
    if (!d.sidetext.empty())
        j["unit"] = d.sidetext;
    if (d.min > -FLT_MAX)
        j["min"] = d.min;
    if (d.max < FLT_MAX)
        j["max"] = d.max;
    if (!d.enum_values.empty()) {
        j["values"] = d.enum_values;
        if (d.enum_labels.size() == d.enum_values.size())
            j["value_labels"] = d.enum_labels;
    }
    if (d.default_value.get() != nullptr)
        j["default"] = d.default_value->serialize();
    if (config.has(key))
        j["current"] = config.opt_serialize(key);
    return j;
}

json cmd_describe_settings(const json& body)
{
    const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    json                     out    = json::array();
    if (body.contains("keys")) {
        json missing = json::array();
        for (const auto& k : body["keys"]) {
            const std::string key = k.get<std::string>();
            if (const ConfigOptionDef* d = print_config_def.get(key))
                out.push_back(def_json(key, *d, config));
            else
                missing.push_back(key);
        }
        return json{{"settings", out}, {"missing", missing}};
    }
    if (!body.contains("search"))
        throw std::runtime_error("give 'keys' (a list) or 'search' (words to look for in keys, labels and tooltips)");

    std::vector<std::string> words;
    const std::string        text = lower(body["search"].get<std::string>());
    {
        std::stringstream ss(text);
        for (std::string w; ss >> w;)
            words.push_back(w);
    }
    std::string as_key; // "pressure advance" -> "pressure_advance"
    for (const std::string& w : words)
        as_key += (as_key.empty() ? "" : "_") + w;
    const std::string scope = body.value("scope", std::string());
    const size_t      limit = body.value("limit", 25);
    std::vector<std::pair<int, std::string>> hits;
    for (const auto& [key, d] : print_config_def.options) {
        if (!config.has(key) || (!scope.empty() && scope_of(key) != scope))
            continue;
        const std::string k = lower(key), label = lower(d.full_label.empty() ? d.label : d.full_label), rest = lower(d.tooltip + " " + d.category);
        int score = 0;
        bool all  = true;
        for (const std::string& w : words) {
            const int s = (k.find(w) != std::string::npos ? 4 : 0) + (label.find(w) != std::string::npos ? 3 : 0) +
                          (rest.find(w) != std::string::npos ? 1 : 0);
            all &= s > 0;
            score += s;
        }
        if (!all)
            continue;
        if (k == as_key || label == text)
            score += 20; // exact name first
        else if (k.rfind(as_key, 0) == 0)
            score += 5; // then settings that start with it
        hits.emplace_back(score, key);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& l, const auto& r) {
        return l.first != r.first ? l.first > r.first : l.second.size() < r.second.size();
    });
    for (size_t i = 0; i < hits.size() && i < limit; ++i)
        out.push_back(def_json(hits[i].second, *print_config_def.get(hits[i].second), config));
    return json{{"settings", out}, {"matches", hits.size()}};
}

// ---------------------------------------------------------------------------- presets

json cmd_list_presets(const json& body)
{
    const std::string type            = body.value("type", std::string("filament"));
    PresetCollection& c               = collection(type);
    const std::string filter          = lower(body.value("filter", std::string()));
    const bool        compatible_only = body.value("compatible_only", true);
    const bool        user_only       = body.value("user_only", false);
    const bool        include_hidden  = body.value("include_hidden", false);
    const size_t      limit           = body.value("limit", 100);
    const std::string selected        = c.get_selected_preset_name();

    json   list  = json::array();
    size_t total = 0;
    for (const Preset& pr : c) {
        if ((!pr.is_visible && !include_hidden) || (compatible_only && !pr.is_compatible) || (user_only && !pr.is_user()))
            continue;
        if (!filter.empty() && lower(pr.name).find(filter) == std::string::npos && lower(pr.alias).find(filter) == std::string::npos)
            continue;
        if (++total > limit)
            continue;
        json item = {{"name", pr.name}, {"system", pr.is_system}, {"user", pr.is_user()}, {"selected", pr.name == selected}};
        if (!pr.inherits().empty())
            item["inherits"] = pr.inherits();
        if (!pr.alias.empty() && pr.alias != pr.name)
            item["alias"] = pr.alias;
        if (type == "filament") {
            for (const char* k : {"filament_type", "filament_vendor"})
                if (pr.config.has(k))
                    item[k] = pr.config.opt_serialize(k);
        }
        list.push_back(item);
    }
    return json{{"type", type}, {"presets", list}, {"total", total}, {"selected", selected}};
}

json cmd_preset_settings(const json& body)
{
    const std::string type   = body.value("type", std::string("filament"));
    const Preset&     preset = find_preset(type, body.value("name", std::string()));
    const DynamicPrintConfig& cfg = preset.config;

    json r = {{"type", type}, {"name", preset.name}, {"system", preset.is_system}, {"inherits", preset.inherits()}};
    if (body.contains("keys")) {
        json values = json::object();
        for (const auto& k : body["keys"]) {
            const std::string key = k.get<std::string>();
            values[key] = cfg.has(key) ? json(cfg.opt_serialize(key)) : json(nullptr);
        }
        r["values"] = values;
    } else if (body.contains("compare_to")) {
        const std::string other_name = body["compare_to"].get<std::string>();
        const Preset&     other = other_name == "current" ? collection(type).get_edited_preset() : find_preset(type, other_name);
        json              diff  = json::object();
        for (const std::string& key : keys_of(preset_type(type))) {
            if (!cfg.has(key) || !other.config.has(key) || key == "inherits" || key.find("settings_id") != std::string::npos)
                continue;
            const std::string a = cfg.opt_serialize(key), b = other.config.opt_serialize(key);
            if (a != b)
                diff[key] = {{"this", a}, {"other", b}};
        }
        r["compared_to"] = other.name;
        r["differences"] = diff;
    } else {
        r["values"] = config_json(cfg);
    }
    return r;
}

json cmd_preset_changes(const json&)
{
    json out = json::object();
    for (const std::string type : {"printer", "process", "filament"}) {
        PresetCollection& c      = collection(type);
        const Preset&     edited = c.get_edited_preset();
        const Preset&     saved  = c.get_selected_preset();
        const Preset*     parent = c.get_selected_preset_parent();
        json              unsaved = json::object(), customized = json::object();
        for (const std::string& key : c.current_dirty_options())
            unsaved[key] = {{"now", edited.config.has(key) ? edited.config.opt_serialize(key) : ""},
                            {"saved", saved.config.has(key) ? saved.config.opt_serialize(key) : ""}};
        if (parent)
            for (const std::string& key : c.current_different_from_parent_options())
                customized[key] = {{"now", edited.config.has(key) ? edited.config.opt_serialize(key) : ""},
                                   {"system", parent->config.has(key) ? parent->config.opt_serialize(key) : ""}};
        out[type] = {{"preset", edited.name},
                     {"parent", parent ? parent->name : std::string()},
                     {"unsaved_changes", unsaved},
                     {"differs_from_system_parent", customized}};
    }
    return out;
}

json cmd_select_preset(const json& body)
{
    Plater&           p    = plater();
    PresetBundle*     pb   = wxGetApp().preset_bundle;
    const std::string type = body.value("type", std::string("filament"));
    PresetCollection& c    = collection(type);
    Tab*              tab  = wxGetApp().get_tab(preset_type(type));
    const Preset&     target = find_preset(type, body.value("name", std::string()));
    const std::string name   = target.name;

    if (c.get_edited_preset().is_dirty) {
        if (!body.value("discard_changes", false))
            throw std::runtime_error("the current " + type + " preset has unsaved changes; save_preset them, or pass discard_changes=true");
        c.discard_current_changes();
    }
    if (type == "filament") {
        const size_t slot = body.value("slot", 0);
        if (slot >= pb->filament_presets.size())
            throw std::runtime_error("there are " + std::to_string(pb->filament_presets.size()) + " filament slots");
        if (pb->filament_presets.size() == 1 || slot == 0)
            tab->select_preset(name, false, "", true);
        pb->set_filament_preset(slot, name);
        p.on_filament_change(slot);
        wxGetApp().sidebar().update_presets(Preset::TYPE_FILAMENT);
        p.update_project_dirty_from_presets();
    } else {
        tab->select_preset(name, false, "", true);
    }
    p.on_config_change(pb->full_config());
    return json{{"type", type}, {"selected", c.get_selected_preset_name()}, {"filaments", pb->filament_presets}};
}

json cmd_discard_preset_changes(const json& body)
{
    Plater&     p     = plater();
    json        reset = json::array();
    for (const std::string type : {"printer", "process", "filament"}) {
        if (body.contains("type") && body["type"].get<std::string>() != type)
            continue;
        PresetCollection& c = collection(type);
        if (!c.get_edited_preset().is_dirty)
            continue;
        c.discard_current_changes();
        wxGetApp().get_tab(preset_type(type))->select_preset(c.get_selected_preset_name(), false, "", true);
        reset.push_back(type);
    }
    p.on_config_change(wxGetApp().preset_bundle->full_config());
    return json{{"discarded", reset}};
}

json cmd_save_preset(const json& body)
{
    const std::string type = body.value("type", std::string("filament"));
    const std::string name = body.value("name", std::string());
    if (name.empty())
        throw std::runtime_error("give the preset a 'name'");
    PresetCollection& c = collection(type);
    if (const Preset* existing = c.find_preset(name, false); existing && (existing->is_system || existing->is_default))
        throw std::runtime_error("'" + name + "' is a system preset; pick a new name");
    Tab* tab = wxGetApp().get_tab(preset_type(type));
    if (!tab->save_preset(name, body.value("detach", false), false, false, "", true))
        throw std::runtime_error("saving was refused (see the slicer window)");
    return json{{"type", type}, {"saved", name}, {"selected", c.get_selected_preset_name()}};
}

// ---------------------------------------------------------------------------- calibration

json cmd_calibrate(const json& body)
{
    Plater&       p  = plater();
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb->prints.get_edited_preset().is_dirty || pb->filaments.get_edited_preset().is_dirty || pb->printers.get_edited_preset().is_dirty)
        throw std::runtime_error("a calibration starts a new project; save_preset or discard_preset_changes the unsaved preset edits first");
    if (p.is_project_dirty() && !p.model().objects.empty() && !body.value("discard_project", false))
        throw std::runtime_error("the project has unsaved changes; save it in the slicer, or pass discard_project=true");
    if (p.is_project_dirty())
        p.new_project(true, true);

    const std::string test = body.value("test", std::string());
    Calib_Params      params;
    params.start = body.value("start", 0.0);
    params.end   = body.value("end", 0.0);
    params.step  = body.value("step", 0.0);

    if (test == "temperature") {
        if (params.start > 500 || params.end < 155 || params.start < params.end + 5)
            throw std::runtime_error("temperature tower: start is the hottest (<= 500), end the coolest (>= 155), start >= end + 5");
        params.mode                = CalibMode::Calib_Temp_Tower;
        params.nozzle_based_resize = body.value("scale_to_nozzle", true);
        p.calib_temp(params);
    } else if (test == "flow_rate") {
        const int pass = body.value("pass", 1);
        if (pass != 1 && pass != 2)
            throw std::runtime_error("flow_rate pass is 1 (coarse) or 2 (fine)");
        p.calib_flowrate(body.value("linear", false), pass);
    } else if (test == "pressure_advance") {
        const std::string method = body.value("method", std::string("line"));
        params.mode = method == "tower" ? CalibMode::Calib_PA_Tower : method == "pattern" ? CalibMode::Calib_PA_Pattern : CalibMode::Calib_PA_Line;
        if (!body.contains("end")) params.end = 0.1;
        if (!body.contains("step")) params.step = method == "pattern" ? 0.005 : 0.002;
        if (params.start < 0 || params.step < 0.001 || params.end < params.start + params.step)
            throw std::runtime_error("pressure advance: start >= 0, step >= 0.001, end > start + step");
        params.print_numbers = method == "line";
        p.calib_pa(params);
    } else if (test == "max_volumetric_speed") {
        if (!body.contains("start")) params.start = 5;
        if (!body.contains("end")) params.end = 20;
        if (!body.contains("step")) params.step = 0.5;
        if (params.start <= 0 || params.step <= 0 || params.end < params.start + params.step)
            throw std::runtime_error("max volumetric speed (mm3/s): start > 0, step > 0, end > start + step");
        params.mode = CalibMode::Calib_Vol_speed_Tower;
        p.calib_max_vol_speed(params);
    } else if (test == "retraction") {
        if (!body.contains("end")) params.end = 2;
        if (!body.contains("step")) params.step = 0.1;
        if (params.start < 0 || params.step <= 0 || params.end < params.start + params.step)
            throw std::runtime_error("retraction (mm): start >= 0, step > 0, end > start + step");
        params.mode = CalibMode::Calib_Retraction_tower;
        p.calib_retraction(params);
    } else if (test == "vfa") {
        if (!body.contains("start")) params.start = 40;
        if (!body.contains("end")) params.end = 200;
        if (!body.contains("step")) params.step = 10;
        params.mode = CalibMode::Calib_VFA_Tower;
        p.calib_VFA(params);
    } else {
        throw std::runtime_error("test must be temperature, flow_rate, pressure_advance, max_volumetric_speed, retraction or vfa");
    }
    return json{{"test", test}, {"objects", p.model().objects.size()}, {"project", into_u8(p.get_project_name())}};
}

} // namespace

void add_model_commands(std::map<std::string, Handler>& table)
{
    table["objects"]               = cmd_objects;
    table["select"]                = cmd_select;
    table["transform"]             = cmd_transform;
    table["analyze_orientation"]   = cmd_analyze_orientation;
    table["detect_overhangs"]      = cmd_detect_overhangs;
    table["check_errors"]          = cmd_check_errors;
    table["paint"]                 = cmd_paint;
    table["add_shape"]             = cmd_add_shape;
    table["remove_part"]           = cmd_remove_part;
    table["edit_part"]             = cmd_edit_part;
    table["object_settings"]       = cmd_object_settings;
    table["cut"]                   = cmd_cut;
    table["action"]                = cmd_action;
    table["view"]                  = cmd_view;
    table["screenshot"]            = cmd_screenshot;
    table["describe_settings"]     = cmd_describe_settings;
    table["list_presets"]          = cmd_list_presets;
    table["preset_settings"]       = cmd_preset_settings;
    table["preset_changes"]        = cmd_preset_changes;
    table["select_preset"]         = cmd_select_preset;
    table["discard_preset_changes"] = cmd_discard_preset_changes;
    table["save_preset"]           = cmd_save_preset;
    table["calibrate"]             = cmd_calibrate;
}

}}} // namespace Slic3r::GUI::ControlCommands
