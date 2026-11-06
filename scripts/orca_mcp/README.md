# OrcaSlicer MCP server

An [MCP](https://modelcontextprotocol.io) server that lets an AI assistant such as Claude Code drive a running OrcaSlicer from this fork. With it, the assistant can:

- load and reload models
- read and change settings
- slice and read the slice results
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
| `printers` | Connected printers with a status summary |
| `camera_snapshot` | Latest camera frame as an image |
| `printer_light`, `printer_pause`, `printer_resume`, `printer_stop`, `printer_speed`, `printer_gcode` | Printer controls |

Claude Code asks before running each tool unless you allow it. Consider allowing only the read-only tools: `slicer_status`, `get_settings`, `slice_result`, `printers` and `camera_snapshot`.
