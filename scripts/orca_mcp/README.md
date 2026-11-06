# OrcaSlicer MCP server

An [MCP](https://modelcontextprotocol.io) server that lets an AI assistant such as Claude Code drive a running OrcaSlicer from this fork. With it, the assistant can:

- load and reload models
- move, rotate, scale, mirror, cut, split, assemble, arrange and auto-orient them
- rank orientations by how much support they need
- paint supports, seams, filament colors and fuzzy skin, and add modifiers, support enforcers and blockers
- set settings per object, per part and per height range
- read what each setting means, its unit, range and allowed values
- list, compare, select and save presets
- set up OrcaSlicer's calibration tests for tuning a filament
- slice, read the slice results, and take screenshots of the 3D view
- watch and control Bambu Lab printers connected with the **Bambu Lab (LAN, open)** printer agent

You keep using the slicer window as usual and see every change happen.

## How it connects

At startup, the fork starts a control API on `127.0.0.1` (default port 13618). It writes the port and a random bearer token to `control_server.json` in OrcaSlicer's data folder (`%APPDATA%\OrcaSlicer` on Windows). This server reads that file, so it works only while OrcaSlicer is running, and only for the user running it.

## Register it with Claude Code

Add this to `.mcp.json` in your project, or use `claude mcp add`:

```json
{
  "mcpServers": {
    "orcaslicer": {
      "command": "uv",
      "args": ["run", "--quiet", "--script", "<path to>/scripts/orca_mcp/server.py"]
    }
  }
}
```

[uv](https://docs.astral.sh/uv/) installs the one dependency, the `mcp` package, on first run.

## Tools

| Tool | What it does |
| --- | --- |
| `slicer_status` | Project, presets and unsaved edits, objects and their source files, plates and slice validity |
| `load_models` | Load files, optionally replacing everything on the plate |
| `reload_models_from_disk` | Reload every object from its source file, for example after a CAD re-export |
| `get_settings` / `set_settings` | Read settings, or change them in the active process, filament or printer preset (unsaved, as if edited in the tab) |
| `slice` / `slice_result` | Slice a plate and wait; returns time, filament length and weight |
| `screenshot` | Image of the 3D view (Prepare: models and paint; Preview: toolpaths and supports), optionally setting the camera first |
| `describe_settings` | Label, tooltip, unit, range, allowed values, default and current value of settings, by key or search words |
| `list_presets` / `preset_settings` / `preset_changes` | Find presets, read or compare one without selecting it, list unsaved edits and differences from the system parent |
| `select_preset` / `save_preset` / `discard_preset_changes` | Pick a preset (per filament slot), save the edited one under a name, or throw edits away |
| `calibrate` | Set up a temperature, flow rate, pressure advance, max volumetric speed, retraction or VFA test |
| `list_objects` / `object_info` | Objects, instances (position, rotation, scale, plate), parts, mesh stats, painted data, per-object and per-range settings |
| `select_objects` | Select objects or parts as clicking them does |
| `transform_object` | Position, move, rotate (absolute, relative, or "lay this direction down"), scale or resize |
| `check_errors` | Everything the slicer would object to: off plate, outside the bed, too tall, overlapping, floating, mesh errors, unsupported overhangs, toolpath conflicts, and the error/warning notifications on screen |
| `detect_overhangs` | Overhang regions with area, slope, location, and whether support under them lands on the bed or the model |
| `analyze_orientation` | Rank resting orientations by estimated support, contact area and height |
| `model_action` | Undo/redo, arrange, auto-orient, delete, center, drop, mirror, split, assemble, merge parts, instances, fill bed, scale to fit, printable, repair, filament assignment, plates, copy/paste |
| `cut_object` | Horizontal cut, keeping either or both pieces, optionally as parts or flipped |
| `paint` | Paint supports, seams, colors or fuzzy skin by overhang angle, facing direction, box or height band |
| `add_shape` / `remove_part` / `edit_part` | Add modifiers, enforcers, blockers, negative parts or parts; rename, retype, set filament |
| `object_settings` | Per-object, per-part and per-height-range process overrides |
| `printers` | Connected printers with a status summary |
| `camera_snapshot` | Latest camera frame as an image |
| `printer_light`, `printer_pause`, `printer_resume`, `printer_stop`, `printer_speed`, `printer_gcode` | Printer controls |

Model edits take an undo snapshot named "Claude: …", so Ctrl+Z in the slicer reverts them.

The `tune_filament` prompt walks through dialing in a new filament with the calibration tests.

Claude Code asks before running each tool unless you allow it. Consider allowing only the read-only tools: `slicer_status`, `get_settings`, `slice_result`, `describe_settings`, `list_presets`, `preset_settings`, `preset_changes`, `list_objects`, `object_info`, `analyze_orientation`, `screenshot`, `printers` and `camera_snapshot`.
