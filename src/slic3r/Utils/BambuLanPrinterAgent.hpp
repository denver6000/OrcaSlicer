#pragma once

#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class BambuLanCamera;
class BambuLanMqtt;
class ICloudServiceAgent;

// Open printer agent for Bambu Lab printers in LAN Only mode with Developer Mode on.
//
// It talks to the printer directly, without Bambu's network plugin:
//   - MQTT over TLS on port 8883 for status and commands. Reports are forwarded unchanged,
//     because MachineObject already parses Bambu's JSON.
//   - FTPS (implicit TLS) on port 990 to upload print files.
//   - SSDP on UDP port 2021 to discover printers on the local network.
// Unsigned print commands are only accepted with Developer Mode on; without it the printer
// answers them with "mqtt message verify failed".
class BambuLanPrinterAgent : public IPrinterAgent
{
public:
    explicit BambuLanPrinterAgent(std::string log_dir);
    ~BambuLanPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication. There is no cloud relay, so send_message() also goes over the LAN.
    int         send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    std::string default_lan_username() const override { return "bblp"; }
    int         connect_printer(const PrinterConnectionParams& params) override;
    int         disconnect_printer() override;
    int         send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Discovery and binding
    bool start_discovery(bool start, bool sending) override;
    int  bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int  set_server_callback(OnServerErrFn fn) override;

    // Machine selection
    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    // Print jobs. The cloud variants fall back to the LAN path.
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }

    // Camera: P1/A1 printers stream JPEGs on port 6000, served to the camera view as snapshots.
    CameraStreamMode get_camera_stream_mode() const override { return CameraStreamMode::http_snapshot; }
    std::string      get_camera_url() const override;

    // For the control API and dashboards: every printer this agent has connected, its status
    // merged from the printer's reports (the "print" object of push_status), and its camera.
    std::vector<std::string> printer_ids() const;
    nlohmann::json           printer_status(const std::string& dev_id) const;
    bool                     is_printer_connected(const std::string& dev_id) const;
    std::string              camera_snapshot_url(const std::string& dev_id) const;
    // Why the last connection attempt failed: "5" when the printer refused the access code,
    // another code when it could not be reached; empty while connecting or connected.
    std::string              connection_error(const std::string& dev_id) const;

private:
    struct Connection
    {
        std::string                   ip;
        std::string                   access_code;
        std::unique_ptr<BambuLanMqtt> mqtt;
    };

    // Model id ("C12" for a P1S) from SSDP, or from the serial prefix as a fallback.
    std::string model_for(const std::string& dev_id) const;
    std::string ca_file() const;

    // Uploads a local file to the printer's storage over FTPS. Returns 0 or a BAMBU_NETWORK_ERR_* code.
    int upload_file(const PrintParams& params, const std::string& local_path, const std::string& remote_name,
                    const OnUpdateStatusFn& update_fn, const WasCancelledFn& cancel_fn);
    // Sends print.project_file for a file on the printer's storage and waits for the printer's answer.
    int send_project_file(const PrintParams& params, const std::string& remote_name, const OnUpdateStatusFn& update_fn);

    void on_mqtt_message(const std::string& dev_id, const std::string& payload);
    void ssdp_loop();

    std::string m_log_dir;

    mutable std::mutex                          m_mutex; // guards everything below except the callbacks' own copies
    std::map<std::string, Connection>           m_connections; // dev_id -> live MQTT connection
    std::string                                 m_selected_dev_id;
    std::map<std::string, nlohmann::json>       m_ssdp_seen;   // dev_id -> last SSDP announcement
    std::map<std::string, nlohmann::json>       m_status;      // dev_id -> merged "print" report
    // dev_id -> why its last connect failed. Kept apart from m_connections because a failed
    // connect makes the GUI deselect the printer, which drops its connection.
    std::map<std::string, std::string>          m_connect_errors;
    std::shared_ptr<ICloudServiceAgent>         m_cloud_agent;
    std::unique_ptr<BambuLanCamera>             m_camera;

    // Waiting for the printer's answer to a print command.
    std::condition_variable m_reply_cv;
    std::string             m_reply_dev_id;
    nlohmann::json          m_reply;

    std::atomic<bool> m_ssdp_stop{false};
    std::thread       m_ssdp_thread;

    OnMsgArrivedFn        m_on_ssdp_msg_fn;
    OnPrinterConnectedFn  m_on_printer_connected_fn;
    GetSubscribeFailureFn m_on_subscribe_failure_fn;
    OnMessageFn           m_on_message_fn;
    OnMessageFn           m_on_user_message_fn;
    OnLocalConnectedFn    m_on_local_connect_fn;
    OnMessageFn           m_on_local_message_fn;
    QueueOnMainFn         m_queue_on_main_fn;
    OnServerErrFn         m_on_server_err_fn;
};

} // namespace Slic3r
