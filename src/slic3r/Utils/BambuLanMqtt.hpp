#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

// A minimal MQTT 3.1.1 client (QoS 0 only) for the broker built into Bambu Lab printers in
// LAN Only mode. Each instance owns one TLS connection on its own thread and reconnects with
// backoff until stop() is called.
//
// why: OrcaSlicer bundles no MQTT library, and a printer broker needs only CONNECT, SUBSCRIBE,
// PUBLISH and PING. Boost.Asio and OpenSSL are already dependencies, so this avoids adding one.
class BambuLanMqtt
{
public:
    struct Options
    {
        std::string host;        // printer IP or hostname
        std::string serial;      // printer serial: the topic prefix and the certificate's CN
        std::string access_code; // the printer's LAN access code (MQTT password)
        std::string ca_file;     // PEM bundle of Bambu Lab CAs; empty skips verification
        std::string client_id;
        int         port      = 8883;
        int         keepalive = 30; // seconds
    };

    enum class State { connected, failed, lost };

    // Called on the client thread. `detail` is "5" when the printer refused the access code.
    using StateFn   = std::function<void(State state, const std::string& detail)>;
    using MessageFn = std::function<void(const std::string& payload)>;

    BambuLanMqtt(Options options, StateFn on_state, MessageFn on_message);
    ~BambuLanMqtt();

    BambuLanMqtt(const BambuLanMqtt&)            = delete;
    BambuLanMqtt& operator=(const BambuLanMqtt&) = delete;

    void start();
    void stop();

    // Publishes to device/<serial>/request. Thread-safe. False when not connected.
    bool publish_request(const std::string& payload);
    bool is_connected() const { return m_connected.load(); }
    const Options& options() const { return m_options; }

private:
    struct Session;

    void run();

    Options                  m_options;
    StateFn                  m_on_state;
    MessageFn                m_on_message;
    std::thread              m_thread;
    std::atomic<bool>        m_stop{false};
    std::atomic<bool>        m_connected{false};
    std::mutex               m_session_mutex; // guards m_session
    std::shared_ptr<Session> m_session;
};

} // namespace Slic3r
