# Bambu LAN agent and control API

This fork adds two things: an open printer agent for Bambu Lab printers in LAN Only mode, and a local control API that lets automation, such as an MCP server, drive the running slicer.

## Bambu LAN agent (`bambu_lan`)

`BambuLanPrinterAgent` implements `IPrinterAgent` for Bambu Lab printers without Bambu's closed network plug-in. Users select it per printer preset with the Printer Agent option ("Bambu Lab (LAN, open)"). The stock `bbl` agent is unchanged.

The printer has to be in **LAN Only mode with Developer Mode on**. Developer Mode is Bambu's supported way to accept print commands that the Bambu plug-in did not sign. Without it, the printer reports status but refuses `print.*` commands with "mqtt message verify failed". The agent reports that refusal to the user rather than working around it.

It speaks the printer's own protocols:

| Channel | Port | Use |
| --- | --- | --- |
| MQTT over TLS | 8883 | Status reports and commands. `BambuLanMqtt` is a minimal MQTT 3.1.1 client (QoS 0) on Boost.Asio and OpenSSL. |
| FTPS, implicit TLS | 990 | Uploading print files, through libcurl. |
| Camera, TLS | 6000 | The JPEG stream of P1/A1-series printers, through `BambuLanCamera`. |
| SSDP, UDP | 2021 | Discovery of printers on the local network. |

**TLS:** every connection is verified against the Bambu Lab CAs in `resources/cert/printer.cer`. A printer's certificate names it by serial, not by IP, so the expected host name is the serial. libcurl reaches the IP under the serial's name through `CURLOPT_RESOLVE`.

**Status:** reports are forwarded unchanged to `DeviceManager`, because `MachineObject` already parses Bambu's JSON. The agent also keeps a merged copy per printer. P1-series printers send only the fields that changed, so each report is merged into the previous one with a JSON merge patch. The control API reads that copy.

**Camera:** frames are served at `http://127.0.0.1:<port>/<serial>/snapshot.jpg`, which the existing `http_snapshot` camera mode polls. A printer's stream opens on the first request and closes after 30 s without one.

**Printing:** a print uploads the sliced 3MF over FTPS, then sends `print.project_file` with the plate's G-code path and the job options. The agent waits briefly for the printer's answer, so a refusal reaches the user as an error.

**Bambu workflow:** `is_bambu_protocol_agent()` marks agents that use the Bambu printer workflow (saved LAN printers, access codes, restoring the last printer) rather than a print-host preset. `GUI_App` uses it where that workflow used to be tied to the `bbl` id.

## Control API

`ControlServer` serves JSON over HTTP on `127.0.0.1` only. At startup it writes its port and a random bearer token to `<data_dir>/control_server.json`, and every request must present that token. Commands run on the GUI thread through `CallAfter`, one at a time. They therefore act on exactly what the user sees, and the window shows each change.

Settings changes go through `Tab::load_config` and `Plater::on_config_change`, the same path as editing a value in a settings tab. The preset shows as modified, and nothing is saved.

`scripts/orca_mcp/server.py` wraps the API as MCP tools for assistants such as Claude Code.
