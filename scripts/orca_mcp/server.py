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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import guide  # noqa: E402  (plain-language explanations and the settings review)

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
- Printers: connect_printer(ip, access_code) adds a Bambu printer in LAN Only mode; OrcaSlicer
  then remembers it. printers shows status; camera_snapshot shows the chamber. send_print starts
  a sliced plate on the printer: only when the user asked to print it.
- Before slicing for a print, read back the settings that matter (get_settings: printer and
  nozzle, filament preset and type, temperatures, plate_type, brim, supports) and compare them
  with the printer (printers full=True: nozzle_diameter, nozzle_type) and the camera (which
  plate is on the bed). Ask the user which nozzle and filament are loaded when unsure.
- Dialogs: a tool that returns blocked_by_dialog is waiting on a dialog; dialogs and
  answer_dialog read and answer it (see answer_dialog for which ones to answer yourself).

The user is not a tuning expert. When settings come up:
- explain(topic) gives a plain-language background (pressure advance, flow ratio, max volumetric
  speed, cooling, retraction, seams...). Pass it on in your own words, briefly, before changing
  anything advanced.
- browse_settings shows the settings the way the tabs lay them out; review_settings flags values
  worth a look for the active filament, plate and printer.
- Say what you change and why, as old -> new (set_settings reports both). Change one thing at a
  time when chasing a print problem, so the effect can be seen.
- show_setting opens the setting in the slicer window so the user can see where it lives.
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
    object first. Paths must be absolute. Import dialogs are skipped (their remembered choices
    apply, e.g. original names kept). Loading a plain model can make OrcaSlicer reselect the
    presets saved for the printer; the selected presets and unsaved edits are then put back and
    listed in presets_restored. A project 3MF's own presets apply as usual."""
    return await _acall("load_files", {"paths": paths, "replace": replace})


@mcp.tool()
async def plate_type(type: str | None = None) -> dict[str, Any]:
    """Read or set the build plate type (the sidebar's plate drop-down), e.g. "Textured PEI Plate",
    "High Temp Plate", "Cool Plate", "Engineering Plate". It must match the plate on the printer:
    it selects the filament's bed temperatures for that plate and, for Textured PEI, a first-layer
    nozzle offset. Without type, returns the current one and the options. Slice again after."""
    return await _acall("plate_type", {"type": type} if type else {})


@mcp.tool()
async def dialogs() -> dict[str, Any]:
    """Dialogs open in OrcaSlicer: title, message, buttons, check boxes, radio groups, choices and
    text fields with their values. A command that opens a dialog waiting for an answer returns
    blocked_by_dialog with the same description instead of timing out."""
    return await _acall("dialogs")


@mcp.tool()
async def answer_dialog(button: str | None = None, set: dict[str, Any] | None = None,
                        dialog: str | None = None) -> dict[str, Any]:
    """Answer an open dialog as a user would: `set` changes controls first, then `button` is
    pressed (both optional). dialog is the id or title (default: the one waiting for an answer).
    set: {"<check box label>": true/false, "<radio or choice option>": true,
    "radio:0": "<option>", "choice:0": "<option>", "text:0": "<value>"}.

    Answer on your own only dialogs that ask HOW to do what was asked (import naming, mesh quality,
    which plate...). Ask the user first for anything that discards, overwrites or deletes their
    work or presets, or affects the printer. Say what you answered. The interrupted command then
    finishes; check its effect with the usual tools."""
    body: dict[str, Any] = {}
    if button:
        body["button"] = button
    if set:
        body["set"] = set
    if dialog:
        body["dialog"] = dialog
    return await _acall("answer_dialog", body)


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
async def set_settings(values: dict[str, Any], scope: Literal["auto", "process", "filament", "printer"] = "auto",
                       slot: int = 0) -> dict[str, Any]:
    """Change settings in the active presets, exactly as editing them in the settings tabs would
    (the presets show as modified; nothing is saved until save_preset). Keys from different presets
    can be mixed: each goes to the preset that holds it (scope "auto"). Values use the slicer's
    text forms, e.g. {"layer_height": 0.16, "sparse_infill_density": "15%", "enable_support": true,
    "nozzle_temperature": 215, "pressure_advance": 0.035}. Filament settings go to the filament in
    `slot` (0 = first); that fails while another filament has unsaved edits.
    Values outside a setting's min/max are refused. Returns each change as from -> to, plus notes
    when a setting is hidden at the user's current ui_mode.
    For a single object instead of the whole plate, use object_settings."""
    return await _acall("set_settings", {"scope": scope, "values": values, "slot": slot})


@mcp.tool()
async def browse_settings(
    scope: Literal["process", "filament", "printer"] = "process",
    page: str | None = None,
    group: str | None = None,
    changed_only: bool = False,
    max_mode: Literal["simple", "advanced", "expert", "develop"] | None = None,
) -> dict[str, Any]:
    """The settings as the slicer's tabs lay them out: pages (e.g. Quality, Strength, Speed,
    Support, Others for process; Filament, Cooling, Setting Overrides, Advanced for filament)
    with their option groups. Without page/group: an outline with counts of settings and of
    changed ones. With page or group (part of the title): every setting with label, value, unit,
    mode (simple/advanced/expert) and whether it has unsaved edits or differs from the system
    preset. changed_only=True lists only changed settings across all pages. max_mode="simple"
    shows only what the Simple view shows."""
    body: dict[str, Any] = {"scope": scope, "changed_only": changed_only}
    if page:
        body["page"] = page
    if group:
        body["group"] = group
    if max_mode:
        body["max_mode"] = max_mode
    return await _acall("settings_layout", body)


@mcp.tool()
async def show_setting(key: str, slot: int | None = None, raise_mode: bool = True) -> dict[str, Any]:
    """Open the settings tab on a setting and focus its field in the slicer window, so the user
    sees where it lives (for example after explaining or changing it). If the setting is hidden
    at the current ui_mode, the mode is raised (raise_mode=False to leave it). slot picks the
    filament for filament settings."""
    body: dict[str, Any] = {"key": key, "raise_mode": raise_mode}
    if slot is not None:
        body["slot"] = slot
    return await _acall("show_setting", body)


@mcp.tool()
async def ui_mode(mode: Literal["simple", "advanced", "expert"] | None = None) -> dict[str, Any]:
    """Read or set how many settings the slicer's tabs show: simple (the basics), advanced, or
    expert (everything). Without mode, returns the current one."""
    return await _acall("ui_mode", {"mode": mode} if mode else {})


@mcp.tool()
async def explain(topic: str | None = None, include_current: bool = True) -> dict[str, Any]:
    """Plain-language background on a printing concept or advanced feature: what it is, signs it
    is set too high or too low, typical values, how to set or calibrate it, the setting keys
    involved (with their current values) and notes for Bambu enclosed printers. topic can be a
    word or phrase ("pressure advance", "stringing", "flow ratio", "seams", "heat creep", "PA").
    Without a topic, lists the topics. For any single setting's official description, use
    describe_settings."""
    if not topic:
        return {"topics": guide.topic_list()}
    matches = guide.find_topic(topic)
    if not matches:
        return {"error": f"no topic matches {topic!r}; try describe_settings(search=...)", "topics": guide.topic_list()}
    entry = {k: v for k, v in guide.TOPICS[matches[0]].items() if k != "aliases"}
    out: dict[str, Any] = {"topic": matches[0], **entry}
    if include_current and entry.get("keys"):
        try:
            got = await _acall("get_settings", {"keys": entry["keys"]})
            out["current_values"] = got.get("values", {})
        except RuntimeError as e:
            out["current_values_error"] = str(e)
    if len(matches) > 1:
        out["related_topics"] = matches[1:4]
    return out


@mcp.tool()
async def review_settings(slot: int = 0) -> dict[str, Any]:
    """Look over the active printer, process and filament presets (for one filament slot) the
    way an experienced user would: nozzle and bed temperature for the material and plate, flow
    ratio, pressure advance, max volumetric speed and which speeds it actually caps, cooling,
    retraction, layer and first-layer settings, walls and top layers, brim for warping
    materials, enclosure door advice and abrasive filament on a soft nozzle. Each finding has a
    level (error/warn/info), the explain() topic for background, current values and a
    suggestion. These are rules of thumb, not calibration: say so to the user."""
    got = await _acall("get_settings", {"keys": guide.REVIEW_KEYS})
    return guide.review(got.get("values", {}), slot)


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
    slot (0 = first). Fails if a switch would lose unsaved edits (in this preset type, and for a
    process also the filament, for a printer also process and filament), unless
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
        if not (await _acall("status")).get("busy"):
            return False
    return True


# --------------------------------------------------------------------------- prompts


@mcp.prompt()
def tune_filament(filament: str, printer_notes: str = "") -> str:
    """Workflow for dialing in a new filament with OrcaSlicer's calibration tests, explained for
    someone new to tuning."""
    return f"""Help me dial in the filament "{filament}" on my printer. {printer_notes}
I am new to tuning: before each test, tell me in two or three sentences what it measures and why
(explain() has the background), and afterwards what the result changed.

1. list_presets(type="filament", filter=...) to find the closest system preset; preset_settings
   to read it, compare_to a sibling if useful. select_preset it, then save_preset under a new
   name (e.g. "{filament} - tuned") so the results have a home.
2. Ask me for the spool label's ranges (nozzle and bed temperature, max speed); set them with
   set_settings. Run review_settings and go through anything it flags.
3. Calibrate in this order, one test at a time (explain("calibration_order") says why):
   temperature -> flow_rate pass 1 -> flow_rate pass 2 -> pressure_advance -> retraction ->
   max_volumetric_speed (-> vfa if walls show fine vertical lines).
   For each: calibrate, slice, screenshot(tab="preview") to check it, then I print it and tell
   you what I see (or send a photo). Then discard_preset_changes (the test's own edits), set the
   value I found with set_settings, save_preset, and show_setting so I see where it lives.
4. Finish with a table of the final values next to the starting ones (preset_changes).
Never start a print yourself."""


@mcp.prompt()
def settings_tour(focus: str = "", level: str = "beginner") -> str:
    """A guided look through the active settings at the user's level, with explanations."""
    about = f" for {focus}" if focus else ""
    return f"""Walk me through my current slicer settings{about}. My level: {level}.

1. slicer_status for the printer, filament and process presets; review_settings for anything
   worth fixing first. Tell me the findings in plain words, most important first.
2. browse_settings(scope="process") for the outline, then go page by page (Quality, Strength,
   Speed, Support, Others), then the filament (Filament, Cooling, Setting Overrides), then the
   printer only if it matters. For each page, explain only the settings that matter for my
   prints, what the current value does, and when I would change it. Use explain() for the
   background of advanced features (pressure advance, flow ratio, max volumetric speed, seams...)
   and describe_settings for exact meanings, units and ranges.
3. Do not change anything unless I ask. When I do: set_settings (report old -> new), then
   show_setting so I see it in the window. Offer save_preset at the end.
Keep each step short and ask before moving to the next page."""


@mcp.prompt()
def fix_print_problem(problem: str) -> str:
    """Track down a print problem (stringing, warping, poor surface, weak parts...) one change at a time."""
    return f"""My print has a problem: {problem}

1. explain() the problem's topic to find the likely causes; ask me the questions that tell them
   apart (where on the part, from the first layer or later, which filament, how old or dry the
   spool is, door open or closed). A photo helps: ask for one, or camera_snapshot if a print is
   running.
2. review_settings and get_settings for the settings involved; check_errors and object_info if
   the model or orientation could be the cause.
3. Suggest the most likely cause first, with one change (old -> new) and why. Prefer free fixes
   (clean plate, dry filament, door open or closed, orientation) before settings, and a
   calibration test over a guess when the value is unknown.
4. Apply it only when I agree (set_settings or object_settings), show_setting it, and suggest a
   small test print (cut_object can keep just the problem height) before a full reprint.
Never start a print yourself."""


# --------------------------------------------------------------------------- printer tools


@mcp.tool()
async def printers(full: bool = False) -> dict[str, Any]:
    """Bambu Lab printers connected through the "Bambu Lab (LAN, open)" agent, with a status
    summary (state, progress, layer, time left, temperatures, errors). full=True returns the whole
    status report."""
    return await _acall("printers", {"full": full})


@mcp.tool()
async def connect_printer(ip: str, access_code: str, name: str | None = None, timeout_s: int = 20,
                          switch_connection: bool = True) -> dict[str, Any]:
    """Connect a Bambu Lab printer in LAN Only mode (Developer Mode on for printing) by its IP
    address and access code, both shown on the printer's screen (Settings > Network). Works like
    the Devices tab's connect-by-IP: it reads the printer's serial, saves the printer with its code
    so OrcaSlicer reconnects on its own next time, selects it and waits up to timeout_s for its
    first status report. Reports a refused access code or an unreachable printer as an error.

    The printer preset must use the open LAN connection (printer_agent "bambu_lan"). With
    switch_connection=True the active printer preset is switched to it when needed; that is an
    unsaved preset edit, so save_preset(type="printer") keeps it (ask the user first)."""
    notes: list[str] = []
    if switch_connection:
        agent = (await _acall("printers")).get("agent")
        if agent != "bambu_lan":
            await _acall("set_settings", {"values": {"printer_agent": "bambu_lan"}, "scope": "printer"})
            notes.append(f"switched the active printer preset from the '{agent or 'bbl'}' connection to the open "
                         "LAN one (unsaved; save_preset(type='printer') keeps it)")
    body: dict[str, Any] = {"ip": ip, "access_code": access_code, "timeout_s": timeout_s}
    if name:
        body["name"] = name
    result = await _acall("connect_printer", body, timeout=timeout_s + 30)
    if notes:
        result["notes"] = notes
    return result


@mcp.tool()
async def send_print(plate: int | None = None, name: str | None = None, bed_leveling: bool = True,
                     flow_calibration: bool = False, timelapse: bool = False, ams_slots: list[int] | None = None,
                     timeout_s: int = 180, allow_filament_mismatch: bool = False) -> dict[str, Any]:
    """Send a sliced plate to the selected printer and START PRINTING it, as the Print dialog's
    Send button does. This makes the real printer heat up and move: only call it when the user
    has asked to print this plate now, and say what will print (object, filament, time) first.

    Before calling: the plate is sliced (slice), check_errors is clean, and printers shows the
    printer connected and idle with a clear bed (camera_snapshot helps). The plate must be sliced
    with a preset for this printer model and nozzle (the printer reports its nozzle; a mismatch is
    refused), the loaded filament, and plate_type matching the plate on the bed. The answer's
    sliced_with lists filament, temperatures, plate, layers, brim and supports: check it. A printer without an AMS prints one filament from the
    external spool; with an AMS, ams_slots gives one tray per filament the plate uses (0-3 = AMS 1
    slots 1-4, 4-7 = AMS 2..., -1 = external spool; the error lists the filaments when missing).

    Waits for the upload and for the printer to take the job (printer_state PREPARE or RUNNING).
    Stop a print with printer_stop."""
    body: dict[str, Any] = {"bed_leveling": bed_leveling, "flow_calibration": flow_calibration,
                            "timelapse": timelapse, "timeout_s": timeout_s}
    if plate is not None:
        body["plate"] = plate
    if name:
        body["name"] = name
    if ams_slots is not None:
        body["ams_slots"] = ams_slots
    if allow_filament_mismatch:
        body["allow_filament_mismatch"] = True
    return await _acall("send_print", body, timeout=timeout_s + 150)


@mcp.tool()
async def camera_snapshot(dev_id: str | None = None) -> Image:
    """The latest camera frame of a printer (default: the selected one), as a JPEG image."""
    info = await _acall("printers")
    targets = [p for p in info.get("printers", []) if not dev_id or p["dev_id"] == dev_id]
    if not dev_id and info.get("selected"):
        targets = [p for p in targets if p["dev_id"] == info["selected"]] or targets
    if not targets:
        raise RuntimeError("no connected printer; connect_printer adds one")
    def fetch() -> bytes:
        with urllib.request.urlopen(targets[0]["camera_url"], timeout=15) as resp:
            return resp.read()

    return Image(data=await asyncio.to_thread(fetch), format="jpeg")


@mcp.tool()
async def printer_light(on: bool, dev_id: str | None = None) -> dict[str, Any]:
    """Turn the chamber light on or off."""
    result = await _printer_request({"system": {"sequence_id": "0", "command": "ledctrl", "led_node": "chamber_light",
                                          "led_mode": "on" if on else "off", "led_on_time": 500, "led_off_time": 500,
                                          "loop_times": 0, "interval_time": 0}}, dev_id)
    # why: P1-series printers leave lights_report out of their partial updates; a full report refreshes it
    await _printer_request({"pushing": {"sequence_id": "0", "command": "pushall"}}, dev_id)
    return result


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
