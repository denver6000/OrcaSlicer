#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace Slic3r { namespace GUI {

// Local control API for automation, such as an MCP server driving the running slicer.
//
// It serves JSON over HTTP on 127.0.0.1 only. Every request must carry the bearer token that
// the server writes, with its port, to <data_dir>/control_server.json; that file is readable
// only by the user who runs OrcaSlicer. Requests run on the GUI thread one at a time, so they
// see and change exactly what the user sees in the window.
//
// POST /api/<command> with a JSON body; commands are listed in ControlServer.cpp.
class ControlServer
{
public:
    static ControlServer& instance();

    void start(); // idempotent
    void stop();

    int port() const { return m_port; }

private:
    ControlServer() = default;
    ~ControlServer();

    struct Impl;
    void serve();

    std::unique_ptr<Impl> m_impl;
    std::thread           m_thread;
    std::atomic<int>      m_port{0};
    std::string           m_token;
};

}} // namespace Slic3r::GUI
