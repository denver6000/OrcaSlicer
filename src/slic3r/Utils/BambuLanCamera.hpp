#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

// Live view for Bambu Lab P1/A1-series printers, whose camera is a JPEG stream over TLS on
// port 6000 rather than RTSP.
//
// Frames are served on a loopback HTTP endpoint, http://127.0.0.1:<port>/<serial>/snapshot.jpg,
// which OrcaSlicer's existing http_snapshot camera mode polls. A printer's stream is opened on
// the first request and closed once nothing has asked for a frame for a while, so an unwatched
// printer costs nothing.
class BambuLanCamera
{
public:
    BambuLanCamera();
    ~BambuLanCamera();

    BambuLanCamera(const BambuLanCamera&)            = delete;
    BambuLanCamera& operator=(const BambuLanCamera&) = delete;

    // Adds or updates a printer. `serial` is also the name its certificate is verified against.
    void set_printer(const std::string& serial, const std::string& ip, const std::string& access_code, const std::string& ca_file);
    void remove_printer(const std::string& serial);

    // URL of the printer's latest frame; starts the local server on first use. Empty on failure.
    std::string snapshot_url(const std::string& serial);

    // Latest JPEG frame of a printer, or null when none has arrived. Opens the stream if needed.
    std::shared_ptr<const std::string> latest_frame(const std::string& serial);

private:
    struct Feed;

    std::shared_ptr<Feed> touch_feed(const std::string& serial);
    static void           run_feed(std::shared_ptr<Feed> feed);
    bool                  ensure_server();
    void                  serve();

    std::mutex                                   m_mutex;
    std::map<std::string, std::shared_ptr<Feed>> m_feeds;

    struct Server;
    std::unique_ptr<Server> m_server;
    std::thread             m_server_thread;
    std::atomic<int>        m_port{0};
};

} // namespace Slic3r
