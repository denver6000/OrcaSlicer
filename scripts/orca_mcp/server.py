# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.12,<2"]
# ///
"""MCP server that lets an AI assistant drive a running OrcaSlicer (this fork).

It talks to the slicer's local control API (127.0.0.1, bearer token), which the app writes to
<data_dir>/control_server.json at startup. Through it the assistant can do what a user does in the
window: load and reload models; move, rotate, scale, mirror, cut, split, assemble and arrange them;
paint supports, seams, colors and fuzzy skin; add modifiers and support enforcers/blockers; set
per-object, per-part and per-layer-range settings; read setting descriptions; pick, compare and
save presets; run calibration tests; slice; look at the 3D view; and watch and control Bambu Lab
printers connected with the "Bambu Lab (LAN, open)" printer agent.

Run with:  uv run --script scripts/orca_mcp/server.py
"""

from __future__ import annotations

import asyncio
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Literal

from mcp.server.fastmcp import FastMCP, Image

INSTRUCTIONS = """\
Drives the user's running OrcaSlicer. Every change shows up live in their window and can be undone
there (or with model_action("undo")).

Coordinates are world millimetres: X/Y on the bed, Z up, the bed surface at Z=0. Objects are named
by index or name (list_objects). Parts are an object's volumes (model parts, modifiers, negative
volumes, support enforcers/blockers) by index (object_info).

Good habits:
- Look before and after: object_info for numbers, screenshot for the picture.
- Run check_errors after loading, after edits and after slicing; fix errors before suggesting
  a print (overlaps, off-bed, too tall, toolpath conflicts, unsupported overhangs...).
- Supports: detect_overhangs shows where and how bad; then supports on, painted, enforced or a
  better orientation.
- Use describe_settings to learn what a setting does, its unit, range and current value before
  changing it, rather than guessing.
- Orientation: analyze_orientation ranks resting positions by estimated support; apply one with
  transform_object(down=...). Then decide supports (enable_support, support_type, paint, enforcers).
- Settings edits (set_settings) change the active presets without saving. save_preset keeps them.
- Never start a print or send G-code unless the user asked for it.
"""

mcp = FastMCP("orcaslicer", instructions=INSTRUCTIONS)


# --------------------------------------------------------------------------- control API client


def _info_file() -> Path:
    if os.environ.get("ORCA_CONTROL_INFO"):
        return Path(os.environ["ORCA_CONTROL_INFO"])
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", "~")).expanduser()
    elif sys.platform == "darwin":
        base = Path("~/Library/Application Support").expanduser()
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", "~/.config")).expanduser()
    return base / "OrcaSlicer" / "control_server.json"


def _call(command: str, body: dict[str, Any] | None = None, timeout: float = 90) -> dict[str, Any]:
    info_path = _info_file()
    if not info_path.exists():
        raise RuntimeError(f"OrcaSlicer is not running, or is not this fork ({info_path} not found).")
    info = json.loads(info_path.read_text(encoding="utf-8"))
    req = urllib.request.Request(
        f"http://127.0.0.1:{info['port']}/api/{command}",
        data=json.dumps(body or {}).encode("utf-8"),
        headers={"Authorization": f"Bearer {info['token']}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            detail = json.loads(e.read().decode("utf-8")).get("error", str(e))
        except Exception:
            detail = str(e)
        raise RuntimeError(f"{command}: {detail}") from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"Cannot reach OrcaSlicer's control API: {e.reason}. Is OrcaSlicer running?") from None


async def _acall(command: str, body: dict[str, Any] | None = None, timeout: float = 90) -> dict[str, Any]:
    """_call on a worker thread, so a slow slicer never blocks the other tools."""
    return await asyncio.to_thread(_call, command, body, timeout)


async def _printer_request(command: dict[str, Any], dev_id: str | None) -> dict[str, Any]:
    body: dict[str, Any] = {"command": command}
    if dev_id:
        body["dev_id"] = dev_id
    return await _acall("printer_command", body)


# --------------------------------------------------------------------------- slicer tools


@mcp.tool()
async def slicer_status() -> dict[str, Any]:
    """What the slicer has open: project, presets (and whether they have unsaved edits), objects on
    the plate with their source files, and which plates have a valid slice."""
    return await _acall("status")


@mcp.tool()
async def load_models(paths: list[str], replace: bool = False) -> dict[str, Any]:
    """Load model files (STL, 3MF, STEP, OBJ...) into the slicer. With replace=True, removes every
    object first. Paths must be absolute."""
    return await _acall("load_files", {"paths": paths, "replace": replace})


@mcp.tool()
async def reload_models_from_disk() -> dict[str, Any]:
    """Reload every object from its source file, keeping its position and per-object settings.
    Use this after re-exporting a model from CAD to the same path."""
    return await _acall("reload_from_disk")


@mcp.tool()
async def get_settings(keys: list[str] | None = None, search: str | None = None) -> dict[str, Any]:
    """Read slicer settings from the active printer, process and filament presets. Give exact
    setting keys (e.g. ["layer_height", "sparse_infill_density"]) or a search text (e.g. "infill")."""
    body: dict[str, Any] = {}
    if keys:
        body["keys"] = keys
    elif search:
        body["search"] = search
    else:
        raise ValueError("give keys or search")
    return await _acall("get_settings", body)


@mcp.tool()
async def set_settings(values: dict[str, Any], scope: str = "process") -> dict[str, Any]:
    """Change settings in the active preset, exactly as editing them in the settings tab would
    (the preset shows as modified; nothing is saved). scope is "process", "filament" or
    "printer". Values use the slicer's own text forms, e.g. {"layer_height": 0.16,
    "sparse_infill_density": "15%", "enable_support": true}. The filament scope edits the
    preset shown in the Filament settings tab (slicer_status: filament_tab_preset); per-filament
    values are lists, one entry per filament slot, e.g. "nozzle_temperature": "215".
    Use describe_settings first to see what a setting means and its allowed values."""
    return await _acall("set_settings", {"scope": scope, "values": values})


@mcp.tool()
async def slice(plate: int | None = None, wait: bool = True, timeout_s: int = 180) -> dict[str, Any]:
    """Slice a plate (default: the current one) and, unless wait=False, wait for the result:
    print time, filament length and weight. If the slicer refuses (e.g. "empty first layer",
    objects outside the bed), returns valid=False with its errors right away; run check_errors
    for details and how to fix them."""
    body = {"plate": plate} if plate is not None else {}
    started = await _acall("slice", body)
    if not wait:
        return started
    plate_index = started.get("plate", 0)
    deadline = time.time() + timeout_s
    seen_errors = 0
    await asyncio.sleep(1.0)
    while time.time() < deadline:
        result = await _acall("slice_result", {"plate": plate_index})
        if result.get("valid"):
            return result
        # An error that is still there a second later is what stops the slice, not a stale one.
        seen_errors = seen_errors + 1 if result.get("errors") else 0
        if seen_errors >= 2:
            result["error"] = "the slicer refused to slice; fix the errors listed (check_errors explains them)"
            return result
        await asyncio.sleep(1.0)
    return {"plate": plate_index, "valid": False, "error": f"no valid slice after {timeout_s} s; run check_errors"}


@mcp.tool()
async def slice_result(plate: int | None = None) -> dict[str, Any]:
    """The last slice result of a plate (default: current): whether it is valid, estimated time,
    filament length (m) and weight (g)."""
    return await _acall("slice_result", {"plate": plate} if plate is not None else {})


# --------------------------------------------------------------------------- setting knowledge and presets


@mcp.tool()
async def describe_settings(
    keys: list[str] | None = None,
    search: str | None = None,
    scope: Literal["process", "filament", "printer"] | None = None,
    limit: int = 25,
) -> dict[str, Any]:
    """Explain slicer settings: label, category, tooltip (what it does and trade-offs), unit,
    min/max, allowed values for enums, default, current value, which preset holds it (scope) and
    whether it can be set per object. Give exact keys, or search words matched against keys,
    labels and tooltips (e.g. search="pressure advance", search="bridge flow", scope="filament")."""
    body: dict[str, Any] = {"limit": limit}
    if keys:
        body["keys"] = keys
    elif search:
        body["search"] = search
    else:
        raise ValueError("give keys or search")
    if scope:
        body["scope"] = scope
    return await _acall("describe_settings", body)


@mcp.tool()
async def list_presets(
    type: Literal["filament", "process", "printer"] = "filament",
    filter: str | None = None,
    compatible_only: bool = True,
    user_only: bool = False,
    include_hidden: bool = False,
    limit: int = 100,
) -> dict[str, Any]:
    """List presets (system and the user's own), which one is selected, and for filaments their
    type and vendor. By default only presets compatible with the current printer and visible in the
    combo boxes. filter matches part of the name, e.g. "PLA", "Bambu", "PETG"."""
    body: dict[str, Any] = {"type": type, "compatible_only": compatible_only, "user_only": user_only,
                            "include_hidden": include_hidden, "limit": limit}
    if filter:
        body["filter"] = filter
    return await _acall("list_presets", body)


@mcp.tool()
async def preset_settings(
    name: str,
    type: Literal["filament", "process", "printer"] = "filament",
    keys: list[str] | None = None,
    compare_to: str | None = None,
) -> dict[str, Any]:
    """Read a preset's saved values without selecting it. With keys, just those; with
    compare_to (another preset name, or "current" for the active edited preset), only the
    settings that differ, e.g. to see how "Bambu PETG HF" differs from "Generic PETG"."""
    body: dict[str, Any] = {"type": type, "name": name}
    if keys:
        body["keys"] = keys
    if compare_to:
        body["compare_to"] = compare_to
    return await _acall("preset_settings", body)


@mcp.tool()
async def preset_changes() -> dict[str, Any]:
    """For the active printer, process and filament presets: unsaved edits (now vs saved) and
    every setting that differs from the system preset it inherits from."""
    return await _acall("preset_changes")


@mcp.tool()
async def select_preset(
    name: str,
    type: Literal["filament", "process", "printer"] = "filament",
    slot: int = 0,
    discard_changes: bool = False,
) -> dict[str, Any]:
    """Select a preset, as picking it in the sidebar does. For filaments, slot is the filament
    slot (0 = first). Fails if the current preset of that type has unsaved edits, unless
    discard_changes=True (or save them first with save_preset)."""
    return await _acall("select_preset", {"type": type, "name": name, "slot": slot, "discard_changes": discard_changes})


@mcp.tool()
async def save_preset(name: str, type: Literal["filament", "process", "printer"] = "filament", detach: bool = False) -> dict[str, Any]:
    """Save the active preset of a type, with its current edits, as a user preset named `name`
    (existing user presets of that name are overwritten; system presets cannot be). It stays
    linked to its system parent unless detach=True. Ask the user before overwriting their presets."""
    return await _acall("save_preset", {"type": type, "name": name, "detach": detach})


@mcp.tool()
async def discard_preset_changes(type: Literal["filament", "process", "printer"] | None = None) -> dict[str, Any]:
    """Throw away unsaved edits of the active presets (one type, or all)."""
    return await _acall("discard_preset_changes", {"type": type} if type else {})


@mcp.tool()
async def calibrate(
    test: Literal["temperature", "flow_rate", "pressure_advance", "max_volumetric_speed", "retraction", "vfa"],
    start: float | None = None,
    end: float | None = None,
    step: float | None = None,
    method: Literal["line", "pattern", "tower"] = "line",
    pass_number: int = 1,
    discard_project: bool = False,
) -> dict[str, Any]:
    """Set up one of OrcaSlicer's calibration prints in a new project, using the active printer,
    filament and process presets (the Calibration menu does the same). The slicer adjusts the
    settings the test needs; slice it, print it (only with the user's go-ahead), read the result
    from the part, then set the found value on the filament preset and save it.
    - temperature: start = hottest, end = coolest (degC), 5 degC per block.
    - flow_rate: pass_number 1 (coarse, -20..+20 %) then 2 (fine); new filament_flow_ratio =
      old * (100 + chosen block) / 100.
    - pressure_advance: method line (default), pattern or tower; start/end/step in PA units
      (defaults 0 / 0.1 / 0.002). Result goes in pressure_advance with enable_pressure_advance on.
    - max_volumetric_speed: start/end/step in mm3/s (defaults 5 / 20 / 0.5) -> filament_max_volumetric_speed.
    - retraction: start/end/step in mm (defaults 0 / 2 / 0.1) -> filament_retraction_length.
    - vfa: start/end/step in mm/s (defaults 40 / 200 / 10), to find speeds without vertical fine artifacts.
    The active presets must have no unsaved edits. Afterwards the presets show the test's edits;
    discard them with discard_preset_changes before normal printing."""
    body: dict[str, Any] = {"test": test, "method": method, "pass": pass_number, "discard_project": discard_project}
    for key, value in (("start", start), ("end", end), ("step", step)):
        if value is not None:
            body[key] = value
    return await _acall("calibrate", body)


# --------------------------------------------------------------------------- model tools


@mcp.tool()
async def list_objects(detail: bool = False) -> dict[str, Any]:
    """Objects on the plates with each instance's plate, position, rotation (deg), scale, size and
    bounding box, and the current selection. detail=True adds what object_info gives."""
    return await _acall("objects", {"detail": detail})


@mcp.tool()
async def object_info(object: int | str) -> dict[str, Any]:
    """Everything about one object: instances (position, rotation, scale, size, plate), mesh
    stats (volume, facets, open edges = errors), parts with type, filament, per-part settings,
    painted data (support/seam/color/fuzzy_skin) and bounding boxes, per-object settings and
    layer-range settings."""
    return await _acall("objects", {"object": object})


@mcp.tool()
async def select_objects(objects: list[int | str] | None = None, parts: list[dict[str, Any]] | None = None,
                   clear: bool = False) -> dict[str, Any]:
    """Select objects (or parts, as [{"object": 0, "part": 1}]) in the 3D view and object list, as
    clicking them does. clear=True deselects everything first."""
    body: dict[str, Any] = {"clear": clear}
    if objects is not None:
        body["objects"] = objects
    if parts is not None:
        body["parts"] = parts
    return await _acall("select", body)


@mcp.tool()
async def transform_object(
    object: int | str,
    position: list[float] | None = None,
    move_by: list[float] | None = None,
    rotation: list[float] | None = None,
    rotate_axis: str | list[float] | None = None,
    rotate_degrees: float | None = None,
    down: list[float] | None = None,
    scale: float | list[float] | None = None,
    size: list[float | None] | None = None,
    uniform: bool = True,
    instance: int | None = None,
) -> dict[str, Any]:
    """Move, rotate or scale an object (all its instances unless `instance` is given). Applied in
    this order: scale/size, rotation, rotate_axis+rotate_degrees, down, position/move_by; then the
    object drops onto the bed.
    - position: [x, y] or [x, y, z] of the object's origin on the bed (mm); move_by: [dx, dy, dz].
    - rotation: absolute [rx, ry, rz] in degrees, like the rotation fields in the sidebar.
    - rotate_axis ("x", "y", "z" or a vector) with rotate_degrees: rotate relative to now.
    - down: a direction in the current world frame to turn toward the bed, e.g. a face normal or
      a candidate from analyze_orientation ("lay on face").
    - scale: a factor (1 = 100%) or [sx, sy, sz]; size: target [x, y, z] mm with null for free
      axes; uniform=True keeps proportions from the first given axis."""
    body: dict[str, Any] = {"object": object}
    if instance is not None:
        body["instance"] = instance
    for key, value in (("position", position), ("move_by", move_by), ("rotation", rotation), ("down", down),
                       ("scale", scale), ("size", size)):
        if value is not None:
            body[key] = value
    if size is not None:
        body["uniform"] = uniform
    if rotate_axis is not None or rotate_degrees is not None:
        if rotate_axis is None or rotate_degrees is None:
            raise ValueError("give both rotate_axis and rotate_degrees")
        body["rotate_by"] = {"axis": rotate_axis, "degrees": rotate_degrees}
    return await _acall("transform", body)


@mcp.tool()
async def analyze_orientation(object: int | str, overhang_angle: float | None = None,
                        candidates: list[list[float]] | None = None) -> dict[str, Any]:
    """Rank resting orientations of an object: the current one, its largest flat (convex hull)
    faces, the six axis directions, and any `candidates` (down vectors) you add. For each: height,
    footprint, bed contact area, overhang area below the support threshold angle (default: the
    process's support_threshold_angle, else 30 deg) and a support volume estimate. Sorted by
    least support. Apply with transform_object(down=...). Weigh this against strength (layer
    lines vs load direction), surface finish of visible faces, and bed adhesion."""
    body: dict[str, Any] = {"object": object}
    if overhang_angle is not None:
        body["overhang_angle"] = overhang_angle
    if candidates:
        body["candidates"] = candidates
    return await _acall("analyze_orientation", body)


@mcp.tool()
async def check_errors() -> dict[str, Any]:
    """Check the plate the way the slicer does, and more. Run it after loading, after edits and
    after slicing, and fix errors before suggesting a print. Returns ok (no errors), plus
    errors / warnings / info, each with a kind, a message and the objects involved:
    - placement: off_plate, outside_bed, too_tall, overlap (objects' outlines intersect),
      floating, sinking, not_printable
    - geometry: mesh_errors (open edges), unsupported_overhangs (overhangs with supports off),
      supported_overhangs
    - slicing: toolpath_conflict (toolpaths of two objects collide), toolpath_outside,
      printable_area, exclusion_area, not_sliced
    - slicer_error / slicer_warning / slicer_info: the notifications shown in the window
      (validation errors, slicing warnings such as empty layers or floating regions, etc.)."""
    return await _acall("check_errors")


@mcp.tool()
async def detect_overhangs(object: int | str, overhang_angle: float | None = None, min_area: float = 1.0,
                     limit: int = 30, instance: int = 0) -> dict[str, Any]:
    """Find the overhang regions of an object in its current orientation: connected downward
    faces flatter than the support threshold (default: the process's support_threshold_angle, else
    30 deg from horizontal). For each region: area, flattest slope, centre and bounding box (world
    mm), height above the bed, and where a support under it would stand (bed or the model) and how
    far it drops. Use it to decide supports: leave short drops and small spans, paint or enforce
    the rest (paint with box_min/box_max around a region, or add_shape support_enforcer), or
    reorient (analyze_orientation)."""
    body: dict[str, Any] = {"object": object, "min_area": min_area, "limit": limit, "instance": instance}
    if overhang_angle is not None:
        body["overhang_angle"] = overhang_angle
    return await _acall("detect_overhangs", body)


ACTIONS = Literal[
    "undo", "redo", "select_all", "deselect_all", "arrange", "auto_orient", "add_plate", "delete_plate",
    "select_plate", "copy", "paste", "delete", "center", "drop_to_bed", "mirror_x", "mirror_y", "mirror_z",
    "split_to_objects", "split_to_parts", "assemble", "merge_parts", "add_instances", "remove_instances",
    "instances_to_objects", "fill_bed", "scale_to_fit", "toggle_printable", "repair", "reload_from_disk",
    "set_filament",
]


@mcp.tool()
async def model_action(action: ACTIONS, objects: list[int | str] | None = None, parts: list[dict[str, Any]] | None = None,
                 count: int = 1, filament: int | None = None, plate: int | None = None,
                 wait: bool = True) -> dict[str, Any]:
    """Run one of the slicer's menu / toolbar / right-click actions. objects or parts
    ([{"object": 0, "part": 1}]) set the selection first; without them the current selection is
    used. arrange and auto_orient act on the given objects, or on the whole plate when none are given.
    - assemble: selected objects -> one object with several parts. merge_parts: -> one single part.
    - split_to_objects / split_to_parts: separate disconnected shells.
    - add_instances / remove_instances: `count` copies. instances_to_objects: copies -> objects.
    - set_filament: assign `filament` (1-based slot) to the selected objects or parts.
    - repair: fix mesh errors. reload_from_disk: reload the selection from its files.
    - select_plate / delete_plate: `plate` index. Objects are moved onto plates by position or arrange.
    wait=True waits for background jobs (arrange, orient) to finish."""
    body: dict[str, Any] = {"action": action, "count": count}
    if objects is not None:
        body["objects"] = objects
    if parts is not None:
        body["parts"] = parts
    if filament is not None:
        body["filament"] = filament
    if plate is not None:
        body["plate"] = plate
    result = await _acall("action", body)
    if wait and result.get("busy"):
        result["busy"] = await _wait_idle()
    return result


@mcp.tool()
async def cut_object(object: int | str, z: float, keep: Literal["both", "upper", "lower"] = "both",
               as_parts: bool = False, flip_upper: bool = False, place_lower_on_cut: bool = False,
               instance: int = 0) -> dict[str, Any]:
    """Cut an object with a horizontal plane at world height z (mm). keep chooses the pieces;
    as_parts keeps them as parts of one object; flip_upper turns the upper piece over;
    place_lower_on_cut lays the lower piece on its cut face. To cut at an angle, rotate first."""
    return await _acall("cut", {"object": object, "z": z, "keep": keep, "as_parts": as_parts, "flip_upper": flip_upper,
                         "place_lower_on_cut": place_lower_on_cut, "instance": instance})


@mcp.tool()
async def paint(
    object: int | str,
    kind: Literal["support", "seam", "color", "fuzzy_skin"] = "support",
    mode: Literal["enforce", "block", "clear"] = "enforce",
    filament: int | None = None,
    all: bool = False,
    overhang_angle: float | None = None,
    facing: list[float] | None = None,
    facing_tolerance_deg: float = 10.0,
    box_min: list[float] | None = None,
    box_max: list[float] | None = None,
    z_min: float | None = None,
    z_max: float | None = None,
    exclude_bed: bool = True,
    replace: bool = False,
) -> dict[str, Any]:
    """Paint facets of an object like the paint-on gizmos: support enforcers/blockers, seam
    enforcers/blockers, filament colors (kind="color" with filament = 1-based slot, 0 clears) or
    fuzzy skin. Facets are chosen by ALL the given criteria (world coordinates):
    - all: every facet; overhang_angle: downward faces flatter than this many degrees from
      horizontal (what needs support); facing + facing_tolerance_deg: normals near a direction
      (e.g. [0, -1, 0] = the front faces, good for hiding/forcing seams);
    - box_min/box_max: facet centres inside a box; z_min/z_max: a height band.
    exclude_bed skips faces lying on the bed. replace=True clears that kind of paint first.
    Painted enforcers only act with enable_support on; support_type "normal(manual)" or
    "tree(manual)" supports only painted/enforced areas."""
    where: dict[str, Any] = {"exclude_bed": exclude_bed}
    if all:
        where["all"] = True
    if overhang_angle is not None:
        where["overhang_angle"] = overhang_angle
    if facing is not None:
        where["facing"] = facing
        where["facing_tolerance_deg"] = facing_tolerance_deg
    for key, value in (("box_min", box_min), ("box_max", box_max), ("z_min", z_min), ("z_max", z_max)):
        if value is not None:
            where[key] = value
    body: dict[str, Any] = {"object": object, "kind": kind, "mode": mode, "where": where, "replace": replace}
    if filament is not None:
        body["filament"] = filament
    return await _acall("paint", body)


@mcp.tool()
async def add_shape(
    object: int | str,
    type: Literal["modifier", "support_enforcer", "support_blocker", "negative", "part"] = "modifier",
    shape: Literal["box", "cylinder", "sphere"] = "box",
    size: list[float] | None = None,
    center: list[float] | None = None,
    settings: dict[str, Any] | None = None,
    name: str | None = None,
) -> dict[str, Any]:
    """Add a shape to an object, as Add Part / Add Modifier / Support Enforcer / Support Blocker /
    Negative Part do. size is [x, y, z] mm (cylinder: [diameter, unused, height]; sphere:
    [diameter, unused, unused]); center is the world position (default: the object's centre).
    For a modifier, settings are the process settings that apply inside it, e.g.
    {"sparse_infill_density": "60%", "wall_loops": 5}. A negative part subtracts (holes)."""
    body: dict[str, Any] = {"object": object, "type": type, "shape": shape}
    for key, value in (("size", size), ("center", center), ("settings", settings), ("name", name)):
        if value is not None:
            body[key] = value
    return await _acall("add_shape", body)


@mcp.tool()
async def remove_part(object: int | str, part: int) -> dict[str, Any]:
    """Delete one part (modifier, enforcer, blocker, negative or model part) of an object."""
    return await _acall("remove_part", {"object": object, "part": part})


@mcp.tool()
async def edit_part(object: int | str, part: int | None = None, name: str | None = None,
              type: Literal["part", "negative", "modifier", "support_enforcer", "support_blocker"] | None = None,
              filament: int | None = None) -> dict[str, Any]:
    """Rename an object or part, change a part's type, or assign a filament (1-based slot) to an
    object or part. Leave part out to edit the object itself."""
    body: dict[str, Any] = {"object": object}
    for key, value in (("part", part), ("name", name), ("type", type), ("filament", filament)):
        if value is not None:
            body[key] = value
    return await _acall("edit_part", body)


@mcp.tool()
async def object_settings(object: int | str, values: dict[str, Any], part: int | None = None,
                    layer_range: list[float] | None = None, remove: bool = False) -> dict[str, Any]:
    """Per-object overrides of process settings (the object list's settings items). With part,
    the override applies to that part or modifier; with layer_range [from_z, to_z], to that height
    range (remove=True deletes the range). A null value removes an override, e.g.
    {"wall_loops": 4, "sparse_infill_density": "30%", "enable_support": null}."""
    body: dict[str, Any] = {"object": object, "values": values, "remove": remove}
    if part is not None:
        body["part"] = part
    if layer_range is not None:
        body["layer_range"] = layer_range
    return await _acall("object_settings", body)


# --------------------------------------------------------------------------- view


@mcp.tool()
async def screenshot(
    tab: Literal["prepare", "preview"] | None = None,
    camera: Literal["iso", "top", "bottom", "front", "rear", "left", "right"] | None = None,
    zoom: Literal["bed", "objects", "selection"] | None = None,
    show_overhangs: bool | None = None,
    max_width: int = 1280,
) -> Image:
    """A picture of the slicer's 3D view, as the user sees it. tab "prepare" shows the models
    (with paint and modifiers), "preview" the sliced toolpaths and supports. camera and zoom set
    the view first; show_overhangs toggles the Prepare view's red overhang highlight. These
    change the user's view too."""
    body: dict[str, Any] = {}
    if tab:
        body["tab"] = tab
    if camera:
        body["camera"] = camera
    if zoom:
        body["zoom"] = zoom
    if show_overhangs is not None:
        body["show_overhangs"] = show_overhangs
    if body:
        await _acall("view", body)
        await asyncio.sleep(0.8 if tab else 0.3)
    result = await _acall("screenshot", {"max_width": max_width})
    return Image(data=base64.b64decode(result["png_base64"]), format="png")


async def _wait_idle(timeout_s: float = 120) -> bool:
    """Waits for the slicer's background jobs; returns True if still busy at the timeout."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        await asyncio.sleep(0.5)
        if not await _acall("status").get("busy"):
            return False
    return True


# --------------------------------------------------------------------------- prompts


@mcp.prompt()
def tune_filament(filament: str, printer_notes: str = "") -> str:
    """Workflow for dialing in a new filament with OrcaSlicer's calibration tests."""
    return f"""Help me dial in the filament "{filament}" on my printer. {printer_notes}

1. list_presets(type="filament", filter=...) to find the closest system preset; preset_settings
   to read it and compare_to a sibling. select_preset it, then save_preset under a new name
   (e.g. "{filament} - tuned") so the edits have a home.
2. Read the maker's ranges (nozzle and bed temperature, max speed) from me or the spool label;
   set them with set_settings(scope="filament").
3. Calibrate in this order, one test at a time:
   temperature -> flow_rate pass 1 -> flow_rate pass 2 -> pressure_advance -> retraction ->
   max_volumetric_speed. For each: calibrate, slice, screenshot(tab="preview") to check it,
   then I print it and tell you what I see. Then discard_preset_changes (the test's own edits),
   set the value I found with set_settings(scope="filament") and save_preset. Use
   describe_settings to name the exact keys before setting them.
4. Finish with a summary of the final values next to the starting ones (preset_changes).
Never start a print yourself."""


# --------------------------------------------------------------------------- printer tools


@mcp.tool()
async def printers(full: bool = False) -> dict[str, Any]:
    """Bambu Lab printers connected through the "Bambu Lab (LAN, open)" agent, with a status
    summary (state, progress, layer, time left, temperatures, errors). full=True returns the whole
    status report."""
    return await _acall("printers", {"full": full})


@mcp.tool()
async def camera_snapshot(dev_id: str | None = None) -> Image:
    """The latest camera frame of a printer (default: the selected one), as a JPEG image."""
    info = await _acall("printers")
    targets = [p for p in info.get("printers", []) if not dev_id or p["dev_id"] == dev_id]
    if not dev_id and info.get("selected"):
        targets = [p for p in targets if p["dev_id"] == info["selected"]] or targets
    if not targets:
        raise RuntimeError("no connected printer; select one in the Device tab")
    def fetch() -> bytes:
        with urllib.request.urlopen(targets[0]["camera_url"], timeout=15) as resp:
            return resp.read()

    return Image(data=await asyncio.to_thread(fetch), format="jpeg")


@mcp.tool()
async def printer_light(on: bool, dev_id: str | None = None) -> dict[str, Any]:
    """Turn the chamber light on or off."""
    return await _printer_request({"system": {"sequence_id": "0", "command": "ledctrl", "led_node": "chamber_light",
                                        "led_mode": "on" if on else "off", "led_on_time": 500, "led_off_time": 500,
                                        "loop_times": 0, "interval_time": 0}}, dev_id)


@mcp.tool()
async def printer_pause(dev_id: str | None = None) -> dict[str, Any]:
    """Pause the current print."""
    return await _printer_request({"print": {"sequence_id": "0", "command": "pause"}}, dev_id)


@mcp.tool()
async def printer_resume(dev_id: str | None = None) -> dict[str, Any]:
    """Resume a paused print."""
    return await _printer_request({"print": {"sequence_id": "0", "command": "resume"}}, dev_id)


@mcp.tool()
async def printer_stop(dev_id: str | None = None) -> dict[str, Any]:
    """Stop (cancel) the current print. This cannot be undone."""
    return await _printer_request({"print": {"sequence_id": "0", "command": "stop"}}, dev_id)


@mcp.tool()
async def printer_speed(level: int, dev_id: str | None = None) -> dict[str, Any]:
    """Set the print speed profile: 1 silent, 2 standard, 3 sport, 4 ludicrous."""
    if level not in (1, 2, 3, 4):
        raise ValueError("level must be 1, 2, 3 or 4")
    return await _printer_request({"print": {"sequence_id": "0", "command": "print_speed", "param": str(level)}}, dev_id)


@mcp.tool()
async def printer_gcode(gcode: str, dev_id: str | None = None) -> dict[str, Any]:
    """Send raw G-code lines to the printer (e.g. "M104 S0" to cool the nozzle). This can move
    axes and heat parts: only use what the user asked for."""
    line = gcode if gcode.endswith("\n") else gcode + "\n"
    return await _printer_request({"print": {"sequence_id": "0", "command": "gcode_line", "param": line}}, dev_id)


if __name__ == "__main__":
    mcp.run()
