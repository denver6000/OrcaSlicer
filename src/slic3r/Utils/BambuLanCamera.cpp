#include "BambuLanCamera.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <istream>
#include <stdexcept>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/log/trivial.hpp>
#include <openssl/ssl.h>

namespace Slic3r {

namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
using tcp      = boost::asio::ip::tcp;

namespace {

constexpr int  CAMERA_PORT     = 6000;
constexpr auto IDLE_TIMEOUT    = std::chrono::seconds(30); // close a stream nobody has looked at for this long
constexpr auto IO_TIMEOUT      = std::chrono::seconds(10);
constexpr auto FIRST_FRAME_MAX = std::chrono::seconds(5);

// Runs one asynchronous operation to completion or until the timeout, then cancels it.
// why: synchronous Asio reads have no portable timeout.
template<class Stream, class StartOp>
void run_with_timeout(asio::io_context& io, Stream& stream, StartOp start_op)
{
    boost::system::error_code result;
    bool                      done = false;
    io.restart();
    start_op([&](const boost::system::error_code& ec, auto&&...) {
        result = ec;
        done   = true;
    });
    io.run_for(IO_TIMEOUT);
    if (!done) {
        boost::system::error_code ignored;
        stream.lowest_layer().cancel(ignored);
        io.restart();
        io.run();
        throw std::runtime_error("camera timed out");
    }
    if (result)
        throw boost::system::system_error(result);
}

void put_u32le(std::string& buf, size_t offset, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        buf[offset + i] = char((v >> (8 * i)) & 0xFF);
}

} // namespace

struct BambuLanCamera::Feed
{
    std::mutex                            mutex; // guards everything up to `frame`
    std::string                           serial;
    std::string                           ip;
    std::string                           access_code;
    std::string                           ca_file;
    std::chrono::steady_clock::time_point last_request;
    std::shared_ptr<const std::string>    frame;
    std::condition_variable               frame_cv;

    std::thread       thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
};

struct BambuLanCamera::Server
{
    asio::io_context io;
    tcp::acceptor    acceptor{io};
};

BambuLanCamera::BambuLanCamera() = default;

BambuLanCamera::~BambuLanCamera()
{
    if (m_server)
        m_server->io.stop();
    if (m_server_thread.joinable())
        m_server_thread.join();

    std::map<std::string, std::shared_ptr<Feed>> feeds;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        feeds.swap(m_feeds);
    }
    for (auto& [serial, feed] : feeds)
        feed->stop = true;
    for (auto& [serial, feed] : feeds)
        if (feed->thread.joinable())
            feed->thread.join();
}

void BambuLanCamera::set_printer(const std::string& serial, const std::string& ip, const std::string& access_code, const std::string& ca_file)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& feed = m_feeds[serial];
    if (!feed)
        feed = std::make_shared<Feed>();
    std::lock_guard<std::mutex> feed_lock(feed->mutex);
    feed->serial      = serial;
    feed->ip          = ip;
    feed->access_code = access_code;
    feed->ca_file     = ca_file;
}

void BambuLanCamera::remove_printer(const std::string& serial)
{
    std::shared_ptr<Feed> feed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_feeds.find(serial);
        if (it == m_feeds.end())
            return;
        feed = it->second;
        m_feeds.erase(it);
    }
    feed->stop = true;
    if (feed->thread.joinable())
        feed->thread.join();
}

std::shared_ptr<BambuLanCamera::Feed> BambuLanCamera::touch_feed(const std::string& serial)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_feeds.find(serial);
    if (it == m_feeds.end())
        return nullptr;
    std::shared_ptr<Feed> feed = it->second;
    {
        std::lock_guard<std::mutex> feed_lock(feed->mutex);
        feed->last_request = std::chrono::steady_clock::now();
    }
    if (!feed->running) {
        if (feed->thread.joinable())
            feed->thread.join(); // a feed that went idle has already returned
        feed->stop    = false;
        feed->running = true;
        feed->thread  = std::thread(&BambuLanCamera::run_feed, feed);
    }
    return feed;
}

std::shared_ptr<const std::string> BambuLanCamera::latest_frame(const std::string& serial)
{
    std::shared_ptr<Feed> feed = touch_feed(serial);
    if (!feed)
        return nullptr;
    std::unique_lock<std::mutex> lock(feed->mutex);
    feed->frame_cv.wait_for(lock, FIRST_FRAME_MAX, [&] { return feed->frame != nullptr || feed->stop; });
    return feed->frame;
}

void BambuLanCamera::run_feed(std::shared_ptr<Feed> feed)
{
    auto idle = [&] {
        std::lock_guard<std::mutex> lock(feed->mutex);
        return std::chrono::steady_clock::now() - feed->last_request > IDLE_TIMEOUT;
    };

    while (!feed->stop && !idle()) {
        std::string serial, ip, access_code, ca_file;
        {
            std::lock_guard<std::mutex> lock(feed->mutex);
            serial      = feed->serial;
            ip          = feed->ip;
            access_code = feed->access_code;
            ca_file     = feed->ca_file;
        }
        try {
            asio::io_context io;
            ssl::context     ctx(ssl::context::tls_client);
            if (!ca_file.empty()) {
                ctx.load_verify_file(ca_file);
                ctx.set_verify_mode(ssl::verify_peer);
            } else {
                ctx.set_verify_mode(ssl::verify_none);
            }
            ssl::stream<tcp::socket> stream(io, ctx);
            if (!ca_file.empty())
                stream.set_verify_callback(ssl::host_name_verification(serial));
            SSL_set_tlsext_host_name(stream.native_handle(), serial.c_str());

            tcp::resolver resolver(io);
            auto          endpoints = resolver.resolve(ip, std::to_string(CAMERA_PORT));
            run_with_timeout(io, stream, [&](auto handler) { asio::async_connect(stream.lowest_layer(), endpoints, handler); });
            run_with_timeout(io, stream, [&](auto handler) { stream.async_handshake(ssl::stream_base::client, handler); });

            // 80-byte login: two magic words, two zero words, then user and access code, each
            // zero-padded to 32 bytes. All integers little-endian.
            std::string auth(80, '\0');
            put_u32le(auth, 0, 0x40);
            put_u32le(auth, 4, 0x3000);
            std::memcpy(&auth[16], "bblp", 4);
            std::memcpy(&auth[48], access_code.data(), std::min<size_t>(access_code.size(), 32));
            run_with_timeout(io, stream, [&](auto handler) { asio::async_write(stream, asio::buffer(auth), handler); });

            // Each frame: a 16-byte header whose first word is the JPEG length, then the JPEG.
            while (!feed->stop && !idle()) {
                std::array<uint8_t, 16> header{};
                run_with_timeout(io, stream, [&](auto handler) { asio::async_read(stream, asio::buffer(header), handler); });
                const uint32_t size = uint32_t(header[0]) | uint32_t(header[1]) << 8 | uint32_t(header[2]) << 16 | uint32_t(header[3]) << 24;
                if (size == 0 || size > 8 * 1024 * 1024)
                    throw std::runtime_error("unexpected camera frame header");
                auto jpeg = std::make_shared<std::string>(size, '\0');
                run_with_timeout(io, stream, [&](auto handler) { asio::async_read(stream, asio::buffer(&(*jpeg)[0], size), handler); });
                if (size < 4 || uint8_t((*jpeg)[0]) != 0xFF || uint8_t((*jpeg)[1]) != 0xD8)
                    continue; // not a JPEG; skip it
                {
                    std::lock_guard<std::mutex> lock(feed->mutex);
                    feed->frame = std::move(jpeg);
                }
                feed->frame_cv.notify_all();
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "BambuLanCamera: " << ip << ": " << e.what();
        }

        // Retry after a pause, unless stopped or nobody is watching any more.
        for (int i = 0; i < 20 && !feed->stop && !idle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    feed->running = false;
    feed->frame_cv.notify_all();
}

bool BambuLanCamera::ensure_server()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_server)
        return m_port != 0;
    try {
        auto server = std::make_unique<Server>();
        tcp::endpoint endpoint(asio::ip::address_v4::loopback(), 0);
        server->acceptor.open(endpoint.protocol());
        server->acceptor.bind(endpoint);
        server->acceptor.listen();
        m_port   = server->acceptor.local_endpoint().port();
        m_server = std::move(server);
        m_server_thread = std::thread([this] { serve(); });
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "BambuLanCamera: cannot start the local camera server: " << e.what();
        return false;
    }
}

void BambuLanCamera::serve()
{
    std::function<void()> accept = [this, &accept]() {
        m_server->acceptor.async_accept([this, &accept](const boost::system::error_code& ec, tcp::socket socket) {
            if (ec)
                return;
            try {
                asio::streambuf request;
                asio::read_until(socket, request, "\r\n\r\n");
                std::istream in(&request);
                std::string  method, target;
                in >> method >> target;

                // Target: /<serial>/snapshot.jpg[?query]
                target = target.substr(0, target.find('?'));
                std::string serial;
                if (target.size() > 1) {
                    const auto slash = target.find('/', 1);
                    if (slash != std::string::npos && target.substr(slash) == "/snapshot.jpg")
                        serial = target.substr(1, slash - 1);
                }

                std::shared_ptr<const std::string> frame = serial.empty() ? nullptr : latest_frame(serial);
                std::string response;
                if (frame) {
                    response = "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(frame->size()) +
                               "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
                    asio::write(socket, asio::buffer(response));
                    asio::write(socket, asio::buffer(*frame));
                } else {
                    response = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    asio::write(socket, asio::buffer(response));
                }
            } catch (const std::exception&) {
                // the web view went away mid-request
            }
            accept();
        });
    };
    accept();
    m_server->io.run();
}

std::string BambuLanCamera::snapshot_url(const std::string& serial)
{
    if (!ensure_server())
        return {};
    return "http://127.0.0.1:" + std::to_string(m_port.load()) + "/" + serial + "/snapshot.jpg";
}

} // namespace Slic3r
