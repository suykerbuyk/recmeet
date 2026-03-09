// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "ipc_client.h"

#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

namespace recmeet {

IpcClient::IpcClient(const std::string& socket_path)
    : socket_path_(socket_path.empty() ? default_socket_path() : socket_path) {}

IpcClient::~IpcClient() {
    close_connection();
}

bool IpcClient::connect() {
    if (fd_ >= 0) return true;

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        close(fd_); fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_); fd_ = -1;
        return false;
    }

    return true;
}

void IpcClient::close_connection() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    read_buf_.clear();
}

bool IpcClient::call(const std::string& method, const JsonMap& params,
                     IpcResponse& resp, IpcError& err, int timeout_ms) {
    if (fd_ < 0) {
        err.code = static_cast<int>(IpcErrorCode::InternalError);
        err.message = "Not connected";
        return false;
    }

    IpcRequest req;
    req.id = next_id_++;
    req.method = method;
    req.params = params;

    std::string wire = serialize(req) + "\n";
    ssize_t n = write(fd_, wire.data(), wire.size());
    if (n < 0) {
        err.code = static_cast<int>(IpcErrorCode::InternalError);
        err.message = "Write failed";
        close_connection();
        return false;
    }

    // Wait for matching response
    pending_id_ = req.id;
    pending_done_ = false;

    auto deadline = std::chrono::steady_clock::now();
    if (timeout_ms > 0)
        deadline += std::chrono::milliseconds(timeout_ms);

    while (!pending_done_) {
        int remaining = -1;
        if (timeout_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (remaining <= 0) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "Timeout waiting for response";
                pending_id_ = 0;
                return false;
            }
        }
        if (!read_and_dispatch(remaining)) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "Connection lost";
            pending_id_ = 0;
            return false;
        }
    }

    pending_id_ = 0;
    pending_done_ = false;

    if (pending_result_.type == IpcMessageType::Response) {
        resp = pending_result_.response;
        return true;
    } else if (pending_result_.type == IpcMessageType::Error) {
        err = pending_result_.error;
        return false;
    }

    err.code = static_cast<int>(IpcErrorCode::InternalError);
    err.message = "Unexpected response type";
    return false;
}

bool IpcClient::call(const std::string& method, IpcResponse& resp, IpcError& err,
                     int timeout_ms) {
    return call(method, {}, resp, err, timeout_ms);
}

bool IpcClient::read_events(const std::string& until_event, int timeout_ms) {
    until_event_ = until_event;
    event_matched_ = false;

    auto deadline = std::chrono::steady_clock::now();
    if (timeout_ms > 0)
        deadline += std::chrono::milliseconds(timeout_ms);

    while (true) {
        int remaining = timeout_ms;
        if (timeout_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (remaining <= 0) { until_event_.clear(); return false; }
        }

        if (!read_and_dispatch(remaining)) {
            until_event_.clear();
            return event_matched_;
        }

        if (event_matched_) {
            until_event_.clear();
            return true;
        }
    }
}

bool IpcClient::read_and_dispatch(int timeout_ms) {
    // First, process ALL complete lines already in the buffer
    size_t nl;
    while ((nl = read_buf_.find('\n')) != std::string::npos) {
        std::string line = read_buf_.substr(0, nl);
        read_buf_.erase(0, nl + 1);
        if (!line.empty()) process_line(line);
    }
    if (pending_done_) return true;

    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return ret == 0;  // timeout → true, error → false
    if (pfd.revents & (POLLHUP | POLLERR)) return false;

    char buf[4096];
    ssize_t n = read(fd_, buf, sizeof(buf));
    if (n <= 0) {
        close_connection();
        return false;
    }

    read_buf_.append(buf, n);

    while ((nl = read_buf_.find('\n')) != std::string::npos) {
        std::string line = read_buf_.substr(0, nl);
        read_buf_.erase(0, nl + 1);
        if (!line.empty()) process_line(line);
    }
    if (pending_done_) return true;

    return true;
}

void IpcClient::process_line(const std::string& line) {
    IpcMessage msg;
    if (!parse_ipc_message(line, msg)) return;

    if (msg.type == IpcMessageType::Event) {
        if (!until_event_.empty() && msg.event.event == until_event_)
            event_matched_ = true;
        if (event_cb_) event_cb_(msg.event);
        return;
    }

    // Check if this is the response to our pending call
    if (pending_id_ > 0) {
        if ((msg.type == IpcMessageType::Response && msg.response.id == pending_id_) ||
            (msg.type == IpcMessageType::Error && msg.error.id == pending_id_)) {
            pending_result_ = msg;
            pending_done_ = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Free function
// ---------------------------------------------------------------------------

bool daemon_running(const std::string& socket_path) {
    IpcClient client(socket_path);
    if (!client.connect()) return false;

    IpcResponse resp;
    IpcError err;
    if (!client.call("status.get", resp, err, 2000)) return false;
    return true;
}

} // namespace recmeet
