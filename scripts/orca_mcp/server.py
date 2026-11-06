# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.12,<2"]
# ///
"""MCP server that lets an AI assistant drive a running OrcaSlicer (this fork).

It talks to the slicer's local control API (127.0.0.1, bearer token), which the app writes to
<data_dir>/control_server.json at startup. Through it the assistant can load and reload models,
read and change settings, slice, read slice results, and watch and control Bambu Lab printers
connected with the "Bambu Lab (LAN, open)" printer agent.

Run with:  uv run --script scripts/orca_mcp/server.py
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP, Image

mcp = FastMCP("orcaslicer")


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


def _printer_request(command: dict[str, Any], dev_id: str | None) -> dict[str, Any]:
    body: dict[str, Any] = {"command": command}
    if dev_id:
        body["dev_id"] = dev_id
    return _call("printer_command", body)


# --------------------------------------------------------------------------- slicer tools


@mcp.tool()
def slicer_status() -> dict[str, Any]:
    """What the slicer has open: project, presets (and whether they have unsaved edits), objects on
    the plate with their source files, and which plates have a valid slice."""
    return _call("status")


@mcp.tool()
def load_models(paths: list[str], replace: bool = False) -> dict[str, Any]:
    """Load model files (STL, 3MF, STEP, OBJ...) into the slicer. With replace=True, removes every
    object first. Paths must be absolute."""
    return _call("load_files", {"paths": paths, "replace": replace})


@mcp.tool()
def reload_models_from_disk() -> dict[str, Any]:
    """Reload every object from its source file, keeping its position and per-object settings.
    Use this after re-exporting a model from CAD to the same path."""
    return _call("reload_from_disk")


@mcp.tool()
def get_settings(keys: list[str] | None = None, search: str | None = None) -> dict[str, Any]:
    """Read slicer settings from the active printer, process and filament presets. Give exact
    setting keys (e.g. ["layer_height", "sparse_infill_density"]) or a search text (e.g. "infill")."""
    body: dict[str, Any] = {}
    if keys:
        body["keys"] = keys
    elif search:
        body["search"] = search
    else:
        raise ValueError("give keys or search")
    return _call("get_settings", body)


@mcp.tool()
def set_settings(values: dict[str, Any], scope: str = "process") -> dict[str, Any]:
    """Change settings in the active preset, exactly as editing them in the settings tab would
    (the preset shows as modified; nothing is saved). scope is "process", "filament" or
    "printer". Values use the slicer's own text forms, e.g. {"layer_height": 0.16,
    "sparse_infill_density": "15%", "enable_support": true}."""
    return _call("set_settings", {"scope": scope, "values": values})


@mcp.tool()
def slice(plate: int | None = None, wait: bool = True, timeout_s: int = 300) -> dict[str, Any]:
    """Slice a plate (default: the current one) and, unless wait=False, wait for the result:
    print time, filament length and weight."""
    body = {"plate": plate} if plate is not None else {}
    started = _call("slice", body)
    if not wait:
        return started
    plate_index = started.get("plate", 0)
    deadline = time.time() + timeout_s
    time.sleep(1.0)
    while time.time() < deadline:
        result = _call("slice_result", {"plate": plate_index})
        if result.get("valid"):
            return result
        time.sleep(1.0)
    return {"plate": plate_index, "valid": False, "error": f"no valid slice after {timeout_s} s; check the slicer window"}


@mcp.tool()
def slice_result(plate: int | None = None) -> dict[str, Any]:
    """The last slice result of a plate (default: current): whether it is valid, estimated time,
    filament length (m) and weight (g)."""
    return _call("slice_result", {"plate": plate} if plate is not None else {})


# --------------------------------------------------------------------------- printer tools


@mcp.tool()
def printers(full: bool = False) -> dict[str, Any]:
    """Bambu Lab printers connected through the "Bambu Lab (LAN, open)" agent, with a status
    summary (state, progress, layer, time left, temperatures, errors). full=True returns the whole
    status report."""
    return _call("printers", {"full": full})


@mcp.tool()
def camera_snapshot(dev_id: str | None = None) -> Image:
    """The latest camera frame of a printer (default: the selected one), as a JPEG image."""
    info = _call("printers")
    targets = [p for p in info.get("printers", []) if not dev_id or p["dev_id"] == dev_id]
    if not dev_id and info.get("selected"):
        targets = [p for p in targets if p["dev_id"] == info["selected"]] or targets
    if not targets:
        raise RuntimeError("no connected printer; select one in the Device tab")
    with urllib.request.urlopen(targets[0]["camera_url"], timeout=15) as resp:
        return Image(data=resp.read(), format="jpeg")


@mcp.tool()
def printer_light(on: bool, dev_id: str | None = None) -> dict[str, Any]:
    """Turn the chamber light on or off."""
    return _printer_request({"system": {"sequence_id": "0", "command": "ledctrl", "led_node": "chamber_light",
                                        "led_mode": "on" if on else "off", "led_on_time": 500, "led_off_time": 500,
                                        "loop_times": 0, "interval_time": 0}}, dev_id)


@mcp.tool()
def printer_pause(dev_id: str | None = None) -> dict[str, Any]:
    """Pause the current print."""
    return _printer_request({"print": {"sequence_id": "0", "command": "pause"}}, dev_id)


@mcp.tool()
def printer_resume(dev_id: str | None = None) -> dict[str, Any]:
    """Resume a paused print."""
    return _printer_request({"print": {"sequence_id": "0", "command": "resume"}}, dev_id)


@mcp.tool()
def printer_stop(dev_id: str | None = None) -> dict[str, Any]:
    """Stop (cancel) the current print. This cannot be undone."""
    return _printer_request({"print": {"sequence_id": "0", "command": "stop"}}, dev_id)


@mcp.tool()
def printer_speed(level: int, dev_id: str | None = None) -> dict[str, Any]:
    """Set the print speed profile: 1 silent, 2 standard, 3 sport, 4 ludicrous."""
    if level not in (1, 2, 3, 4):
        raise ValueError("level must be 1, 2, 3 or 4")
    return _printer_request({"print": {"sequence_id": "0", "command": "print_speed", "param": str(level)}}, dev_id)


@mcp.tool()
def printer_gcode(gcode: str, dev_id: str | None = None) -> dict[str, Any]:
    """Send raw G-code lines to the printer (e.g. "M104 S0" to cool the nozzle). This can move
    axes and heat parts: only use what the user asked for."""
    line = gcode if gcode.endswith("\n") else gcode + "\n"
    return _printer_request({"print": {"sequence_id": "0", "command": "gcode_line", "param": line}}, dev_id)


if __name__ == "__main__":
    mcp.run()
