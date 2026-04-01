// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "ipc_protocol.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace recmeet {

// Method handler: receives a request, returns a response or error.
// On success, populate resp and return true.
// On failure, populate err and return false.
using MethodHandler = std::function<bool(const IpcRequest& req,
                                         IpcResponse& resp,
                                         IpcError& err)>;

class IpcServer {
public:
    explicit IpcServer(const std::string& socket_path);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    // Register a method handler. Must be called before start().
    void on(const std::string& method, MethodHandler handler);

    // Start listening. Returns false on bind/listen failure.
    bool start();

    // Run the poll() loop. Blocks until stop() is called.
    void run();

    // Signal the poll loop to exit (thread-safe via self-pipe).
    void stop();

    // Broadcast an event to all connected clients.
    void broadcast(const IpcEvent& ev);

    // Wake the poll loop from a worker thread (e.g., after job completion).
    // The callback will be invoked on the poll thread.
    void post(std::function<void()> fn);

    int listen_fd() const { return listen_fd_; }

private:
    void accept_client();
    void handle_client_data(int fd);
    void remove_client(int fd);
    void send_to(int fd, const std::string& msg);
    void drain_wakeup();
    void run_posted();
    bool start_unix();
    bool start_tcp();

    IpcAddress addr_;
    int listen_fd_ = -1;
    int wakeup_read_ = -1;   // self-pipe read end
    int wakeup_write_ = -1;  // self-pipe write end
    bool running_ = false;

    struct ClientState {
        std::string read_buf;  // accumulates data until \n
    };
    std::unordered_map<int, ClientState> clients_;

    std::unordered_map<std::string, MethodHandler> handlers_;

    std::mutex post_mu_;
    std::vector<std::function<void()>> posted_;
};

} // namespace recmeet
