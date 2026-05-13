// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "ipc_client.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace recmeet {

namespace {

// Phase A.4: extract a `client_id` value from an auth.ok NDJSON frame.
// The frame shape is `{"type":"auth.ok","client_id":"..."}`. We do not
// route through `parse_ipc_message()` because auth frames are not the
// request/response/event/error shape that parser recognizes — auth is
// its own micro-protocol layered atop the same NDJSON transport.
// Returns the extracted id (without quotes) or empty string if the
// frame does not carry the field. The function is intentionally
// tolerant: an auth.ok frame from a pre-A.4 daemon (which omits
// `client_id`) round-trips cleanly with an empty result.
std::string extract_client_id_from_auth_ok(const std::string& line) {
    size_t key = line.find("\"client_id\"");
    if (key == std::string::npos) return "";
    size_t colon = line.find(':', key);
    if (colon == std::string::npos) return "";
    size_t open = line.find('"', colon);
    if (open == std::string::npos) return "";
    std::string out;
    for (size_t i = open + 1; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            char nc = line[i + 1];
            if (nc == '"' || nc == '\\') { out += nc; ++i; continue; }
            out += c;
            continue;
        }
        if (c == '"') return out;
        out += c;
    }
    return "";
}

// JSON-escape a token value. Tokens are unlikely to contain special chars
// in practice, but we never want to emit invalid JSON if an operator
// chose a token with quotes or backslashes.
std::string json_escape_token(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Read one NDJSON line from a blocking-mode fd with a deadline. Returns
// true when a complete line is in `out` (without the trailing \n); false
// on timeout/disconnect/error.
bool read_one_line_blocking(int fd, std::string& out, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    out.clear();
    char buf[256];
    while (true) {
        // Check if line already complete from a previous read attempt.
        // (We never accumulate beyond the first \n in this helper.)
        struct pollfd pfd = {fd, POLLIN, 0};
        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining <= 0) return false;
        int r = poll(&pfd, 1, static_cast<int>(remaining));
        if (r <= 0) return false;
        if (pfd.revents & (POLLHUP | POLLERR)) {
            // POLLHUP can come with a final readable buffer; try one read.
        }
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return false;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                // We don't currently support the server sending more than
                // one frame in this window — auth.ok is the only valid
                // response. Anything trailing is a protocol violation, but
                // we ignore it because connection state matters more.
                return true;
            }
            out += buf[i];
        }
    }
}

} // anonymous namespace

IpcClient::IpcClient(const std::string& socket_path) {
    if (!parse_ipc_address(socket_path, addr_))
        addr_ = default_ipc_address();
}

IpcClient::~IpcClient() {
    close_connection();
}

bool IpcClient::connect() {
    if (fd_ >= 0) return true;
    if (addr_.transport == IpcTransport::Tcp)
        return connect_tcp();
    return connect_unix();
}

bool IpcClient::connect_unix() {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (addr_.socket_path.size() >= sizeof(addr.sun_path)) {
        close(fd_); fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, addr_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_); fd_ = -1;
        return false;
    }

    // Phase A.4: the daemon enqueues a synthetic `auth.ok` frame for Unix
    // clients at accept time so the handshake-completion event shape
    // matches TCP. Read it now, mirror of the TCP path. The blocking
    // read uses the same 5 s budget as TCP's auth wait; in practice the
    // frame is in the socket buffer well before connect() returns, so
    // this is a fast-path read.
    //
    // Tolerance: a pre-A.4 daemon does NOT emit this frame. To stay
    // backward-compatible we poll the fd briefly and only consume the
    // frame if it is actually there; on timeout we proceed with an
    // empty client_id_ rather than failing the connect outright. The
    // poll is cheap (100 ms upper bound) because the local daemon
    // either has the frame queued already or never will.
    struct pollfd pfd = {fd_, POLLIN, 0};
    int pr = poll(&pfd, 1, 100);
    if (pr > 0 && (pfd.revents & POLLIN)) {
        std::string reply;
        if (read_one_line_blocking(fd_, reply, 500)) {
            if (reply.find("\"auth.ok\"") != std::string::npos) {
                client_id_ = extract_client_id_from_auth_ok(reply);
            } else {
                // Unexpected first frame on Unix — close. The protocol
                // contract says the only daemon-initiated unsolicited
                // frame on a fresh Unix connection is auth.ok.
                close(fd_); fd_ = -1;
                return false;
            }
        }
        // poll() lied or timed out under the inner deadline → tolerate.
    }

    return true;
}

bool IpcClient::connect_tcp() {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(addr_.port);
    if (getaddrinfo(addr_.host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
        return false;

    fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) { freeaddrinfo(res); return false; }

    // Non-blocking connect with 5s timeout (avoids freezing GTK main loop)
    fcntl(fd_, F_SETFL, O_NONBLOCK);
    int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd_); fd_ = -1;
        return false;
    }
    if (rc < 0) {
        // EINPROGRESS — wait for completion
        struct pollfd pfd = {fd_, POLLOUT, 0};
        rc = poll(&pfd, 1, 5000);
        if (rc <= 0) { close(fd_); fd_ = -1; return false; }
        int err = 0; socklen_t len = sizeof(err);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) { close(fd_); fd_ = -1; return false; }
    }

    // Restore blocking mode for read/write
    fcntl(fd_, F_SETFL, 0);

    // TCP_NODELAY for low-latency NDJSON
    int yes = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    // TCP keepalive: detect dead connections in ~60s
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int idle = 30;
    setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    int intvl = 10;
    setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    int cnt = 3;
    setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

    // Phase A.1 PSK gate: TCP clients must send `auth.token` as the first
    // frame and wait for `auth.ok` before any other traffic. The token is
    // read from RECMEET_AUTH_TOKEN at connect time so operators can rotate
    // by relaunching. Per-client config arrives in A.6 (session.init);
    // until then, the env var is the v1 source of truth.
    const char* token_env = std::getenv("RECMEET_AUTH_TOKEN");
    std::string token = token_env ? token_env : "";

    std::string auth_frame = "{\"type\":\"auth.token\",\"token\":\""
                           + json_escape_token(token) + "\"}\n";
    ssize_t aw = write(fd_, auth_frame.data(), auth_frame.size());
    if (aw < 0) { close(fd_); fd_ = -1; return false; }

    std::string reply;
    if (!read_one_line_blocking(fd_, reply, 5000)) {
        close(fd_); fd_ = -1; return false;
    }

    // We accept any reply containing the literal `"auth.ok"` type marker.
    // Anything else (auth.error or unexpected) → close. We don't surface a
    // structured error here because v1 callers handle connection failure
    // identically; operators see the daemon-side log line which already
    // names the rejection reason.
    if (reply.find("\"auth.ok\"") == std::string::npos) {
        close(fd_); fd_ = -1; return false;
    }

    // Phase A.4: capture the server-issued client_id. A pre-A.4 daemon
    // emits `{"type":"auth.ok"}` with no client_id field — leave the
    // value empty in that case for backward compatibility (no callers
    // require a non-empty id in v1; the field is observability today
    // and will become load-bearing in Phase C.7 routing).
    client_id_ = extract_client_id_from_auth_ok(reply);

    return true;
}

void IpcClient::set_address(const std::string& addr) {
    assert(fd_ == -1 && "set_address() requires disconnected client");
    if (!parse_ipc_address(addr, addr_))
        addr_ = default_ipc_address();
}

void IpcClient::close_connection() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    read_buf_.clear();
    // Phase A.4: drop the cached server-issued client_id so a subsequent
    // reconnect cannot surface a stale value. The daemon mints a fresh
    // id on every accept; carrying the old one across a reconnect would
    // confuse Phase C.7 routing.
    client_id_.clear();
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
    if (fd_ < 0) return false;  // callback closed connection

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
