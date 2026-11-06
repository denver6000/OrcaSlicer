#include "BambuLanPrinterAgent.hpp"

#include "BambuLanCamera.hpp"
#include "BambuLanMqtt.hpp"
#include "NetworkAgentFactory.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace Slic3r {

namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
using tcp      = boost::asio::ip::tcp;
using udp      = boost::asio::ip::udp;
using json     = nlohmann::json;

namespace {

constexpr int FTPS_PORT = 990;
constexpr int MQTT_PORT = 8883;
constexpr int SSDP_PORT = 2021;

// Serial-number prefixes of Bambu Lab models, used when SSDP has not announced the model.
// why: the IP dialog identifies a printer from its TLS certificate alone, which carries only
// the serial. HMS lookups key on the same three-character prefix.
const std::map<std::string, std::string>& serial_prefix_models()
{
    static const std::map<std::string, std::string> models = {
        {"00M", "BL-P001"}, // X1 Carbon
        {"03W", "C13"},     // X1E
        {"01S", "C11"},     // P1P
        {"01P", "C12"},     // P1S
        {"030", "N1"},      // A1 mini
        {"039", "N2S"},     // A1
    };
    return models;
}

std::string random_client_id()
{
    std::random_device rd;
    return "orca-lan-" + std::to_string(rd() % 1000000);
}

// File name safe for the printer's FAT storage and the MQTT url.
std::string sanitize_remote_name(std::string name)
{
    for (char& c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || std::string("\\/:*?\"<>|#%").find(c) != std::string::npos)
            c = '_';
    }
    boost::algorithm::trim(name);
    return name.empty() ? std::string("print") : name;
}

struct UploadProgress
{
    const OnUpdateStatusFn* update_fn = nullptr;
    const WasCancelledFn*   cancel_fn = nullptr;
    int                     last      = -1;
};

int upload_progress_cb(void* user, curl_off_t, curl_off_t, curl_off_t ultotal, curl_off_t ulnow)
{
    auto* p = static_cast<UploadProgress*>(user);
    if (p->cancel_fn && *p->cancel_fn && (*p->cancel_fn)())
        return 1; // aborts the transfer
    if (ultotal > 0 && p->update_fn && *p->update_fn) {
        const int percent = int(ulnow * 100 / ultotal);
        if (percent != p->last) {
            p->last = percent;
            (*p->update_fn)(PrintingStageUpload, percent, "");
        }
    }
    return 0;
}

} // namespace

BambuLanPrinterAgent::BambuLanPrinterAgent(std::string log_dir)
    : m_log_dir(std::move(log_dir)), m_camera(std::make_unique<BambuLanCamera>())
{}

BambuLanPrinterAgent::~BambuLanPrinterAgent()
{
    start_discovery(false, false);
    disconnect_printer();
}

AgentInfo BambuLanPrinterAgent::get_agent_info_static()
{
    return AgentInfo{BAMBU_LAN_PRINTER_AGENT_ID, "Bambu Lab (LAN, open)", "1.0.0",
                     "Bambu Lab printers in LAN Only + Developer Mode, without the Bambu network plug-in"};
}

void BambuLanPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cloud_agent = std::move(cloud);
}

std::string BambuLanPrinterAgent::ca_file() const
{
    const boost::filesystem::path path = boost::filesystem::path(resources_dir()) / "cert" / "printer.cer";
    return boost::filesystem::exists(path) ? path.string() : std::string();
}

std::string BambuLanPrinterAgent::model_for(const std::string& dev_id) const
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_ssdp_seen.find(dev_id);
        if (it != m_ssdp_seen.end() && it->second.contains("dev_type"))
            return it->second["dev_type"].get<std::string>();
    }
    auto it = serial_prefix_models().find(dev_id.substr(0, 3));
    return it != serial_prefix_models().end() ? it->second : std::string();
}

// ---------------------------------------------------------------------------- connection

int BambuLanPrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    if (params.dev_id.empty() || params.host.empty())
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;

    BambuLanMqtt::Options options;
    options.host        = params.host;
    options.serial      = params.dev_id;
    options.access_code = params.password;
    options.ca_file     = ca_file();
    options.client_id   = random_client_id();
    if (options.ca_file.empty())
        BOOST_LOG_TRIVIAL(warning) << "BambuLanPrinterAgent: printer.cer not found, connecting without certificate verification";

    const std::string dev_id = params.dev_id;
    auto mqtt = std::make_unique<BambuLanMqtt>(
        options,
        [this, dev_id](BambuLanMqtt::State state, const std::string& detail) {
            OnLocalConnectedFn fn;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                fn = m_on_local_connect_fn;
                if (state == BambuLanMqtt::State::failed)
                    m_connect_errors[dev_id] = detail.empty() ? "-1" : detail;
                else
                    m_connect_errors.erase(dev_id);
            }
            if (!fn)
                return;
            switch (state) {
            case BambuLanMqtt::State::connected: fn(ConnectStatusOk, dev_id, ""); break;
            case BambuLanMqtt::State::failed: fn(ConnectStatusFailed, dev_id, detail); break;
            case BambuLanMqtt::State::lost: fn(ConnectStatusLost, dev_id, ""); break;
            }
        },
        [this, dev_id](const std::string& payload) { on_mqtt_message(dev_id, payload); });

    std::unique_ptr<BambuLanMqtt> old;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Connection& c = m_connections[dev_id];
        old           = std::move(c.mqtt);
        c.ip          = params.host;
        c.access_code = params.password;
        c.mqtt        = std::move(mqtt);
        m_connect_errors.erase(dev_id);
        c.mqtt->start();
        m_selected_dev_id = dev_id;
    }
    m_camera->set_printer(dev_id, params.host, params.password, options.ca_file);
    // why: stop() joins the client thread, whose callbacks take m_mutex; never join under it
    old.reset();
    BOOST_LOG_TRIVIAL(info) << "BambuLanPrinterAgent: connecting to " << dev_id << " at " << params.host;
    return BAMBU_NETWORK_SUCCESS;
}

int BambuLanPrinterAgent::disconnect_printer()
{
    std::map<std::string, Connection> closing;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        closing.swap(m_connections);
        m_selected_dev_id.clear();
    }
    closing.clear(); // stops and joins each client outside the lock
    return BAMBU_NETWORK_SUCCESS;
}

int BambuLanPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_connections.find(dev_id);
    if (it == m_connections.end() || !it->second.mqtt)
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    return it->second.mqtt->publish_request(json_str) ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

int BambuLanPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    return send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
}

std::string BambuLanPrinterAgent::get_camera_url() const
{
    std::string dev_id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        dev_id = m_selected_dev_id;
    }
    return dev_id.empty() ? std::string() : m_camera->snapshot_url(dev_id);
}

std::vector<std::string> BambuLanPrinterAgent::printer_ids() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string>    ids;
    for (const auto& [id, c] : m_connections)
        ids.push_back(id);
    return ids;
}

nlohmann::json BambuLanPrinterAgent::printer_status(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_status.find(dev_id);
    return it != m_status.end() ? it->second : nlohmann::json::object();
}

bool BambuLanPrinterAgent::is_printer_connected(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_connections.find(dev_id);
    return it != m_connections.end() && it->second.mqtt && it->second.mqtt->is_connected();
}

std::string BambuLanPrinterAgent::connection_error(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_connect_errors.find(dev_id);
    return it != m_connect_errors.end() ? it->second : std::string();
}

std::string BambuLanPrinterAgent::camera_snapshot_url(const std::string& dev_id) const
{
    return m_camera->snapshot_url(dev_id);
}

void BambuLanPrinterAgent::on_mqtt_message(const std::string& dev_id, const std::string& payload)
{
    // Keep a merged copy of the status. P1-series printers send only the fields that changed,
    // so each report is merged into the last full state.
    try {
        json j = json::parse(payload);
        if (j.contains("print") && j["print"].is_object()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            json& status = m_status[dev_id];
            if (!status.is_object())
                status = json::object();
            status.merge_patch(j["print"]);
        }
    } catch (const std::exception&) {
    }

    // Answers to print commands carry their command name; status reports are "push_status".
    if (payload.find("\"project_file\"") != std::string::npos) {
        try {
            json j = json::parse(payload);
            if (j.contains("print") && j["print"].value("command", "") == "project_file" && j["print"].contains("result")) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_reply_dev_id == dev_id) {
                    m_reply = j["print"];
                    m_reply_cv.notify_all();
                }
            }
        } catch (const std::exception&) {
        }
    }

    OnMessageFn fn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fn = m_on_local_message_fn;
    }
    if (fn)
        fn(dev_id, payload);
}

// ---------------------------------------------------------------------------- discovery

bool BambuLanPrinterAgent::start_discovery(bool start, bool /*sending*/)
{
    if (start) {
        if (m_ssdp_thread.joinable())
            return true;
        m_ssdp_stop   = false;
        m_ssdp_thread = std::thread([this] { ssdp_loop(); });
        return true;
    }
    m_ssdp_stop = true;
    if (m_ssdp_thread.joinable())
        m_ssdp_thread.join();
    return true;
}

void BambuLanPrinterAgent::ssdp_loop()
{
    asio::io_context          io;
    udp::socket               socket(io);
    boost::system::error_code ec;
    socket.open(udp::v4(), ec);
    if (!ec)
        socket.set_option(asio::socket_base::reuse_address(true), ec);
    if (!ec)
        socket.bind(udp::endpoint(asio::ip::address_v4::any(), SSDP_PORT), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << "BambuLanPrinterAgent: SSDP discovery unavailable on UDP " << SSDP_PORT << ": " << ec.message();
        return;
    }

    std::array<char, 4096> buf{};
    udp::endpoint          from;

    // Printers broadcast NOTIFY messages with Bambu-specific headers every few seconds.
    auto handle = [this](const std::string& text, const std::string& sender_ip) {
        if (!boost::algorithm::istarts_with(text, "NOTIFY"))
            return;
        std::vector<std::string> lines;
        boost::algorithm::split(lines, text, [](char c) { return c == '\n'; });
        std::map<std::string, std::string> headers;
        for (std::string& line : lines) {
            boost::algorithm::trim(line);
            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = boost::algorithm::to_lower_copy(line.substr(0, colon));
            std::string val = line.substr(colon + 1);
            boost::algorithm::trim(val);
            headers[key] = val;
        }
        const std::string dev_id = headers["usn"];
        if (dev_id.empty() || headers["devmodel.bambu.com"].empty())
            return;

        json j;
        j["dev_id"]       = dev_id;
        j["dev_ip"]       = headers["location"].empty() ? sender_ip : headers["location"];
        j["dev_name"]     = headers["devname.bambu.com"].empty() ? dev_id : headers["devname.bambu.com"];
        j["dev_type"]     = headers["devmodel.bambu.com"];
        j["dev_signal"]   = headers["devsignal.bambu.com"];
        j["connect_type"] = headers["devconnect.bambu.com"].empty() ? "lan" : headers["devconnect.bambu.com"];
        j["bind_state"]   = headers["devbind.bambu.com"].empty() ? "free" : headers["devbind.bambu.com"];
        j["sec_link"]     = headers["devseclink.bambu.com"];
        j["ssdp_version"] = headers["devversion.bambu.com"];

        OnMsgArrivedFn fn;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ssdp_seen[dev_id] = j;
            fn                  = m_on_ssdp_msg_fn;
        }
        if (fn)
            fn(j.dump());
    };

    std::function<void()> receive = [&]() {
        socket.async_receive_from(asio::buffer(buf), from, [&](const boost::system::error_code& ec, size_t n) {
            if (ec)
                return;
            handle(std::string(buf.data(), n), from.address().to_string());
            receive();
        });
    };
    receive();

    while (!m_ssdp_stop) {
        io.run_for(std::chrono::milliseconds(250));
        if (io.stopped())
            io.restart();
    }
}

int BambuLanPrinterAgent::bind_detect(std::string dev_ip, std::string /*sec_link*/, detectResult& detect)
{
    // The printer's TLS certificate names it: CN = serial. No login is needed to read it, and
    // the MQTT connection that follows verifies the certificate against the Bambu CA.
    std::string serial;
    try {
        asio::io_context         io;
        ssl::context             ctx(ssl::context::tls_client);
        ctx.set_verify_mode(ssl::verify_none);
        ssl::stream<tcp::socket> stream(io, ctx);
        tcp::resolver            resolver(io);
        auto                     endpoints = resolver.resolve(dev_ip, std::to_string(MQTT_PORT));
        bool                     done      = false;
        asio::async_connect(stream.lowest_layer(), endpoints, [&](const boost::system::error_code& ec, const tcp::endpoint&) {
            if (ec)
                return;
            stream.async_handshake(ssl::stream_base::client, [&](const boost::system::error_code& ec) { done = !ec; });
        });
        io.run_for(std::chrono::seconds(6));
        if (done) {
            // why: the bundled OpenSSL is 1.1.1; SSL_get_peer_certificate returns a new reference there
            // and is an alias of SSL_get1_peer_certificate in 3.x
            if (X509* cert = SSL_get_peer_certificate(stream.native_handle())) {
                std::array<char, 256> cn{};
                if (X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn.data(), int(cn.size())) > 0)
                    serial = cn.data();
                X509_free(cert);
            }
        }
        boost::system::error_code ec;
        stream.lowest_layer().close(ec);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "BambuLanPrinterAgent: bind_detect " << dev_ip << ": " << e.what();
    }
    if (serial.empty())
        return -1; // "Failed to connect to printer."

    detect.dev_id       = serial;
    detect.model_id     = model_for(serial);
    detect.connect_type = "lan";
    detect.bind_state   = "free";
    detect.dev_name     = serial;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_ssdp_seen.find(serial);
        if (it != m_ssdp_seen.end()) {
            detect.dev_name = it->second.value("dev_name", serial);
            detect.version  = it->second.value("ssdp_version", "");
        }
    }
    BOOST_LOG_TRIVIAL(info) << "BambuLanPrinterAgent: " << dev_ip << " is " << serial << " (model " << detect.model_id << ")";
    return 0;
}

// ---------------------------------------------------------------------------- printing

int BambuLanPrinterAgent::upload_file(const PrintParams& params, const std::string& local_path, const std::string& remote_name,
                                      const OnUpdateStatusFn& update_fn, const WasCancelledFn& cancel_fn)
{
    std::string ip, access_code;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_connections.find(params.dev_id);
        if (it != m_connections.end()) {
            ip          = it->second.ip;
            access_code = it->second.access_code;
        }
    }
    if (!params.dev_ip.empty())
        ip = params.dev_ip;
    if (!params.password.empty())
        access_code = params.password;
    if (ip.empty() || access_code.empty())
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;

    FILE* file = boost::nowide::fopen(local_path.c_str(), "rb");
    if (!file)
        return BAMBU_NETWORK_ERR_PRINT_WR_FILE_NOT_EXIST;
    const auto size = boost::filesystem::file_size(boost::filesystem::path(local_path));

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    // why: the certificate names the printer by serial, so reach the IP under the serial's name
    // to keep both certificate and host-name verification on.
    const std::string serial  = params.dev_id;
    char*             escaped = curl_easy_escape(curl, remote_name.c_str(), int(remote_name.size()));
    const std::string url     = "ftps://" + serial + ":" + std::to_string(FTPS_PORT) + "/" + (escaped ? escaped : remote_name.c_str());
    curl_free(escaped);
    curl_slist* resolve = curl_slist_append(nullptr, (serial + ":" + std::to_string(FTPS_PORT) + ":" + ip).c_str());

    UploadProgress progress{&update_fn, &cancel_fn};
    const std::string ca = ca_file();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(curl, CURLOPT_USERNAME, "bblp");
    curl_easy_setopt(curl, CURLOPT_PASSWORD, access_code.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, long(CURLUSESSL_ALL));
    if (!ca.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, file);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, curl_off_t(size));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, upload_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "");
    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(resolve);
    curl_easy_cleanup(curl);
    fclose(file);

    if (res == CURLE_ABORTED_BY_CALLBACK)
        return BAMBU_NETWORK_ERR_CANCELED;
    if (res != CURLE_OK) {
        BOOST_LOG_TRIVIAL(error) << "BambuLanPrinterAgent: FTPS upload of " << remote_name << " to " << ip
                                 << " failed: " << curl_easy_strerror(res);
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }
    BOOST_LOG_TRIVIAL(info) << "BambuLanPrinterAgent: uploaded " << remote_name << " (" << size << " bytes) to " << serial;
    return BAMBU_NETWORK_SUCCESS;
}

int BambuLanPrinterAgent::send_project_file(const PrintParams& params, const std::string& remote_name, const OnUpdateStatusFn& update_fn)
{
    json ams_mapping = json::array();
    if (!params.ams_mapping.empty()) {
        try {
            ams_mapping = json::parse(params.ams_mapping);
        } catch (const std::exception&) {
        }
    }

    json print;
    print["sequence_id"]    = "0";
    print["command"]        = "project_file";
    print["param"]          = "Metadata/plate_" + std::to_string(std::max(1, params.plate_index)) + ".gcode";
    print["project_id"]     = "0";
    print["profile_id"]     = "0";
    print["task_id"]        = "0";
    print["subtask_id"]     = "0";
    print["subtask_name"]   = params.project_name.empty() ? remote_name : params.project_name;
    print["file"]           = "";
    print["url"]            = "ftp:///" + remote_name;
    print["md5"]            = "";
    print["timelapse"]      = params.task_record_timelapse;
    print["bed_type"]       = "auto";
    print["bed_levelling"]  = params.task_bed_leveling;
    print["flow_cali"]      = params.task_flow_cali;
    print["vibration_cali"] = params.task_vibration_cali;
    print["layer_inspect"]  = params.task_layer_inspect;
    print["ams_mapping"]    = ams_mapping;
    print["use_ams"]        = params.task_use_ams;
    json request;
    request["print"] = print;

    if (update_fn)
        update_fn(PrintingStageSending, 0, "");

    std::unique_lock<std::mutex> lock(m_mutex);
    auto it = m_connections.find(params.dev_id);
    if (it == m_connections.end() || !it->second.mqtt)
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    m_reply_dev_id = params.dev_id;
    m_reply        = json();
    if (!it->second.mqtt->publish_request(request.dump())) {
        m_reply_dev_id.clear();
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    // The printer answers within a second or two. A missing answer is not an error: the
    // print's progress arrives in the status reports either way.
    m_reply_cv.wait_for(lock, std::chrono::seconds(10), [this] { return !m_reply.is_null(); });
    json reply = m_reply;
    m_reply_dev_id.clear();
    lock.unlock();

    if (!reply.is_null()) {
        const std::string result = boost::algorithm::to_lower_copy(reply.value("result", std::string()));
        if (result != "success") {
            const std::string reason = reply.value("reason", std::string());
            BOOST_LOG_TRIVIAL(error) << "BambuLanPrinterAgent: printer refused the print: " << reply.dump();
            if (update_fn)
                update_fn(PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED,
                          reason.find("verify") != std::string::npos ?
                              _u8L("The printer refused the print command. Turn on Developer Mode in the printer's LAN Only settings.") :
                              reason);
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
    } else {
        BOOST_LOG_TRIVIAL(info) << "BambuLanPrinterAgent: no answer to project_file within 10 s, assuming it started";
    }
    if (update_fn)
        update_fn(PrintingStageFinished, 0, "");
    return BAMBU_NETWORK_SUCCESS;
}

int BambuLanPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    std::string name = params.project_name;
    if (name.empty())
        name = boost::filesystem::path(params.filename).stem().string();
    name = sanitize_remote_name(name);
    if (!boost::algorithm::iends_with(name, ".3mf"))
        name += ".3mf";

    if (update_fn)
        update_fn(PrintingStageCreate, 0, "");
    const int uploaded = upload_file(params, params.filename, name, update_fn, cancel_fn);
    if (uploaded != BAMBU_NETWORK_SUCCESS)
        return uploaded;
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;
    return send_project_file(params, name, update_fn);
}

int BambuLanPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int BambuLanPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int BambuLanPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    std::string name = !params.dst_file.empty() ? boost::filesystem::path(params.dst_file).filename().string() : params.project_name;
    if (name.empty())
        name = boost::filesystem::path(params.filename).filename().string();
    name = sanitize_remote_name(name);
    const int result = upload_file(params, params.filename, name, update_fn, cancel_fn);
    if (result == BAMBU_NETWORK_SUCCESS && update_fn)
        update_fn(PrintingStageFinished, 0, "");
    return result;
}

int BambuLanPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn)
{
    std::string name = !params.dst_file.empty() ? params.dst_file : params.ftp_file;
    if (name.empty())
        name = boost::filesystem::path(params.filename).filename().string();
    if (!name.empty() && name.front() == '/')
        name.erase(0, 1);
    return send_project_file(params, name, update_fn);
}

// ---------------------------------------------------------------------------- selection and callbacks

std::string BambuLanPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected_dev_id;
}

int BambuLanPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_selected_dev_id = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

int BambuLanPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_server_err_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

#define BAMBU_LAN_SET_CALLBACK(setter, member, type)  \
    int BambuLanPrinterAgent::setter(type fn)         \
    {                                                 \
        std::lock_guard<std::mutex> lock(m_mutex);    \
        member = std::move(fn);                       \
        return BAMBU_NETWORK_SUCCESS;                 \
    }

BAMBU_LAN_SET_CALLBACK(set_on_ssdp_msg_fn, m_on_ssdp_msg_fn, OnMsgArrivedFn)
BAMBU_LAN_SET_CALLBACK(set_on_printer_connected_fn, m_on_printer_connected_fn, OnPrinterConnectedFn)
BAMBU_LAN_SET_CALLBACK(set_on_subscribe_failure_fn, m_on_subscribe_failure_fn, GetSubscribeFailureFn)
BAMBU_LAN_SET_CALLBACK(set_on_message_fn, m_on_message_fn, OnMessageFn)
BAMBU_LAN_SET_CALLBACK(set_on_user_message_fn, m_on_user_message_fn, OnMessageFn)
BAMBU_LAN_SET_CALLBACK(set_on_local_connect_fn, m_on_local_connect_fn, OnLocalConnectedFn)
BAMBU_LAN_SET_CALLBACK(set_on_local_message_fn, m_on_local_message_fn, OnMessageFn)
BAMBU_LAN_SET_CALLBACK(set_queue_on_main_fn, m_queue_on_main_fn, QueueOnMainFn)

#undef BAMBU_LAN_SET_CALLBACK

} // namespace Slic3r
