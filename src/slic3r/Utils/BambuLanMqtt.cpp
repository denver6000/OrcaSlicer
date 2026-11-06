#include "BambuLanMqtt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <exception>
#include <utility>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/log/trivial.hpp>
#include <openssl/ssl.h>

namespace Slic3r {

namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
using tcp      = boost::asio::ip::tcp;

namespace {

void put_u16(std::string& out, size_t v)
{
    out.push_back(char((v >> 8) & 0xFF));
    out.push_back(char(v & 0xFF));
}

void put_str(std::string& out, const std::string& s)
{
    put_u16(out, s.size());
    out += s;
}

// Fixed header byte + remaining length (MQTT variable-length integer) + body.
std::string make_packet(uint8_t type_and_flags, const std::string& body)
{
    std::string out(1, char(type_and_flags));
    size_t      n = body.size();
    do {
        uint8_t byte = n % 128;
        n /= 128;
        if (n > 0)
            byte |= 0x80;
        out.push_back(char(byte));
    } while (n > 0);
    return out + body;
}

std::string make_connect(const BambuLanMqtt::Options& o)
{
    std::string body;
    put_str(body, "MQTT");
    body.push_back(char(4));    // protocol level 3.1.1
    body.push_back(char(0xC2)); // username + password + clean session
    put_u16(body, size_t(o.keepalive));
    put_str(body, o.client_id);
    put_str(body, "bblp");
    put_str(body, o.access_code);
    return make_packet(0x10, body);
}

std::string make_subscribe(const std::string& topic)
{
    std::string body;
    put_u16(body, 1); // packet id
    put_str(body, topic);
    body.push_back(char(0)); // QoS 0
    return make_packet(0x82, body);
}

std::string make_publish(const std::string& topic, const std::string& payload)
{
    std::string body;
    put_str(body, topic);
    body += payload;
    return make_packet(0x30, body);
}

} // namespace

struct BambuLanMqtt::Session
{
    asio::io_context                               io;
    ssl::context                                   ctx{ssl::context::tls_client};
    std::unique_ptr<ssl::stream<tcp::socket>>      stream;
    asio::steady_timer                             deadline{io};
    asio::steady_timer                             ping_timer{io};
    std::deque<std::string>                        write_queue;
    std::vector<uint8_t>                           rx;
    std::array<uint8_t, 16384>                     rbuf{};
    bool                                           closed = false;
    bool                                           connack_ok = false;
    std::string                                    detail = "-1"; // reported if the session never connects
    std::function<void()>                          write_next; // writes write_queue.front(), one at a time

    void send(std::string packet)
    {
        if (closed)
            return;
        const bool idle = write_queue.empty();
        write_queue.push_back(std::move(packet));
        if (idle && write_next)
            write_next();
    }

    void close()
    {
        if (closed)
            return;
        closed = true;
        boost::system::error_code ec;
        deadline.cancel();
        ping_timer.cancel();
        if (stream)
            stream->lowest_layer().close(ec);
    }
};

BambuLanMqtt::BambuLanMqtt(Options options, StateFn on_state, MessageFn on_message)
    : m_options(std::move(options)), m_on_state(std::move(on_state)), m_on_message(std::move(on_message))
{}

BambuLanMqtt::~BambuLanMqtt() { stop(); }

void BambuLanMqtt::start()
{
    if (m_thread.joinable())
        return;
    m_stop = false;
    m_thread = std::thread([this] { run(); });
}

void BambuLanMqtt::stop()
{
    m_stop = true;
    {
        std::lock_guard<std::mutex> lock(m_session_mutex);
        if (m_session)
            m_session->io.stop();
    }
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
        m_thread.join();
    m_connected = false;
}

bool BambuLanMqtt::publish_request(const std::string& payload)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(m_session_mutex);
        s = m_session;
    }
    if (!s || !m_connected)
        return false;
    std::string packet = make_publish("device/" + m_options.serial + "/request", payload);
    std::weak_ptr<Session> weak = s;
    asio::post(s->io, [weak, packet = std::move(packet)]() mutable {
        if (auto session = weak.lock())
            session->send(std::move(packet));
    });
    return true;
}

void BambuLanMqtt::run()
{
    int  failures       = 0;
    bool ever_connected = false;

    while (!m_stop) {
        auto s = std::make_shared<Session>();
        {
            std::lock_guard<std::mutex> lock(m_session_mutex);
            if (m_stop)
                break;
            m_session = s;
        }

        const auto watchdog = std::chrono::seconds(std::max(60, m_options.keepalive * 2));

        try {
            Session* raw = s.get(); // handlers only run inside s->io.run(), while s is alive

            if (!m_options.ca_file.empty()) {
                raw->ctx.load_verify_file(m_options.ca_file);
                raw->ctx.set_verify_mode(ssl::verify_peer);
            } else {
                raw->ctx.set_verify_mode(ssl::verify_none);
            }
            raw->stream = std::make_unique<ssl::stream<tcp::socket>>(raw->io, raw->ctx);
            if (!m_options.ca_file.empty())
                // why: the printer certificate's CN is its serial, not its IP, so verify the serial
                raw->stream->set_verify_callback(ssl::host_name_verification(m_options.serial));
            SSL_set_tlsext_host_name(raw->stream->native_handle(), m_options.serial.c_str());

            tcp::resolver resolver(raw->io);
            auto          endpoints = resolver.resolve(m_options.host, std::to_string(m_options.port));

            // Deadline for connect + TLS + CONNACK, then reused as the silence watchdog.
            auto arm = [raw](std::chrono::steady_clock::duration d) {
                raw->deadline.expires_after(d);
                raw->deadline.async_wait([raw](const boost::system::error_code& ec) {
                    if (!ec)
                        raw->close();
                });
            };
            arm(std::chrono::seconds(10));

            raw->write_next = [raw]() {
                if (raw->write_queue.empty() || raw->closed)
                    return;
                asio::async_write(*raw->stream, asio::buffer(raw->write_queue.front()),
                                  [raw](const boost::system::error_code& ec, size_t) {
                                      if (ec) {
                                          raw->close();
                                          return;
                                      }
                                      raw->write_queue.pop_front();
                                      raw->write_next();
                                  });
            };

            std::function<void()> schedule_ping = [this, raw, &schedule_ping]() {
                raw->ping_timer.expires_after(std::chrono::seconds(std::max(5, m_options.keepalive / 2)));
                raw->ping_timer.async_wait([raw, &schedule_ping](const boost::system::error_code& ec) {
                    if (ec || raw->closed)
                        return;
                    raw->send(make_packet(0xC0, {}));
                    schedule_ping();
                });
            };

            // Parses every complete packet in raw->rx. Returns false when the session must end.
            auto handle_packets = [this, raw, &schedule_ping, arm, watchdog]() -> bool {
                for (;;) {
                    if (raw->rx.size() < 2)
                        return true;
                    size_t len = 0, mult = 1, pos = 1;
                    for (;;) {
                        if (pos >= raw->rx.size())
                            return true; // length not complete yet
                        const uint8_t b = raw->rx[pos++];
                        len += size_t(b & 0x7F) * mult;
                        if (!(b & 0x80))
                            break;
                        mult *= 128;
                        if (pos > 4)
                            return false; // malformed
                    }
                    if (raw->rx.size() < pos + len)
                        return true;
                    const uint8_t  type = raw->rx[0] >> 4;
                    const uint8_t* body = raw->rx.data() + pos;

                    if (type == 2) { // CONNACK
                        const int rc = len >= 2 ? body[1] : -1;
                        if (rc != 0) {
                            raw->detail = (rc == 4 || rc == 5) ? "5" : std::to_string(rc);
                            return false;
                        }
                        raw->connack_ok = true;
                        m_connected     = true;
                        raw->send(make_subscribe("device/" + m_options.serial + "/report"));
                        schedule_ping();
                        if (m_on_state)
                            m_on_state(State::connected, {});
                    } else if (type == 3 && len >= 2) { // PUBLISH
                        const uint8_t qos  = (raw->rx[0] >> 1) & 0x03;
                        const size_t  tlen = (size_t(body[0]) << 8) | body[1];
                        const size_t  off  = 2 + tlen + (qos ? 2 : 0);
                        if (off <= len && m_on_message)
                            m_on_message(std::string(reinterpret_cast<const char*>(body) + off, len - off));
                    }
                    // SUBACK (9) and PINGRESP (13) need no handling.
                    raw->rx.erase(raw->rx.begin(), raw->rx.begin() + pos + len);
                    if (raw->connack_ok)
                        arm(watchdog);
                }
            };

            std::function<void()> read_loop = [raw, &handle_packets, &read_loop]() {
                raw->stream->async_read_some(asio::buffer(raw->rbuf), [raw, &handle_packets, &read_loop](const boost::system::error_code& ec, size_t n) {
                    if (ec || raw->closed) {
                        raw->close();
                        return;
                    }
                    raw->rx.insert(raw->rx.end(), raw->rbuf.begin(), raw->rbuf.begin() + n);
                    if (!handle_packets()) {
                        raw->close();
                        return;
                    }
                    read_loop();
                });
            };

            asio::async_connect(raw->stream->lowest_layer(), endpoints,
                                [this, raw, &read_loop](const boost::system::error_code& ec, const tcp::endpoint&) {
                                    if (ec) {
                                        raw->close();
                                        return;
                                    }
                                    raw->stream->async_handshake(ssl::stream_base::client, [this, raw, &read_loop](const boost::system::error_code& ec) {
                                        if (ec) {
                                            BOOST_LOG_TRIVIAL(warning) << "BambuLanMqtt: TLS handshake with " << m_options.host
                                                                       << " failed: " << ec.message();
                                            raw->close();
                                            return;
                                        }
                                        raw->send(make_connect(m_options));
                                        read_loop();
                                    });
                                });

            // why: the handlers above reference these locals, which live until run() returns
            raw->io.run();
            raw->write_next = nullptr;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "BambuLanMqtt: " << m_options.host << ": " << e.what();
        }

        {
            std::lock_guard<std::mutex> lock(m_session_mutex);
            m_session.reset();
        }
        const bool was_connected = m_connected.exchange(false);
        if (m_stop)
            break;

        if (was_connected) {
            ever_connected = true;
            failures       = 0;
            BOOST_LOG_TRIVIAL(info) << "BambuLanMqtt: lost connection to " << m_options.host << ", reconnecting";
            if (m_on_state)
                m_on_state(State::lost, s->detail);
        } else if (!ever_connected || s->detail == "5") {
            // why: a first connect that fails, or a refused access code, will not fix itself by
            // retrying; report it once and let the caller decide.
            BOOST_LOG_TRIVIAL(warning) << "BambuLanMqtt: connecting to " << m_options.host << " failed (" << s->detail << ")";
            if (m_on_state)
                m_on_state(State::failed, s->detail);
            break;
        } else {
            ++failures;
        }

        // Backoff 1, 2, 4 ... 30 s, waking early on stop().
        const auto delay = std::chrono::seconds(std::min(30, 1 << std::min(failures, 5)));
        const auto until = std::chrono::steady_clock::now() + delay;
        while (!m_stop && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace Slic3r
