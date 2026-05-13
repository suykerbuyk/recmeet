// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "ipc_client.h"
#include "log.h"

#include <cassert>
#include <cctype>
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

// Phase A.5: extract the `protocol_version` integer from an auth.ok
// NDJSON frame. Frame shape after A.5 is
// `{"type":"auth.ok","client_id":"...","protocol_version":N}`. The
// daemon writes the version with `std::to_string(int)` so the value is
// always a base-10 signed integer with no whitespace, no JSON escapes,
// and no quotes around it. We scan for `"protocol_version"`, walk past
// the colon and any surrounding whitespace, then parse the digits.
// Returns `present=false` when the field is absent (pre-A.5 daemon) —
// the caller treats absence as a mismatch since A.5 is the floor.
struct AuthOkProtocolVersion {
    bool present = false;
    int  value   = 0;
};
AuthOkProtocolVersion extract_protocol_version_from_auth_ok(const std::string& line) {
    AuthOkProtocolVersion r;
    size_t key = line.find("\"protocol_version\"");
    if (key == std::string::npos) return r;
    size_t colon = line.find(':', key);
    if (colon == std::string::npos) return r;
    // Skip whitespace after the colon, then accept an optional leading
    // sign, then digits. Stop at the first non-digit (typically `,` or `}`).
    size_t i = colon + 1;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;
    int sign = 1;
    if (i < line.size() && (line[i] == '+' || line[i] == '-')) {
        if (line[i] == '-') sign = -1;
        ++i;
    }
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i])))
        return r;
    int v = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        v = v * 10 + (line[i] - '0');
        ++i;
    }
    r.present = true;
    r.value   = sign * v;
    return r;
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
    // Phase A.5: clear the latched mismatch flag at the START of a new
    // connect attempt so a successful retry surfaces a clean state. On
    // failure paths below the flag stays valid through the function's
    // return and remains true until the next connect() is attempted —
    // tests inspect it on the failed-connect return value.
    protocol_mismatch_ = false;
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
    // Phase A.5: the frame now also carries `protocol_version`. Mismatch
    // (or absent — treated as v0) fails the connect and sets the
    // `protocol_mismatch_` flag. The peek-before-read idiom from A.4 is
    // retained for the "no auth.ok at all" case (e.g. a pre-A.4 daemon
    // that never wrote anything), but as soon as the frame IS present we
    // enforce A.5 — A.5 explicitly drops backward compatibility with
    // pre-A.5 daemons that omit `protocol_version`.
    struct pollfd pfd = {fd_, POLLIN, 0};
    int pr = poll(&pfd, 1, 100);
    if (pr > 0 && (pfd.revents & POLLIN)) {
        std::string reply;
        if (read_one_line_blocking(fd_, reply, 500)) {
            if (reply.find("\"auth.ok\"") != std::string::npos) {
                if (!verify_auth_ok_and_capture(reply)) {
                    close(fd_); fd_ = -1;
                    return false;
                }
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

bool IpcClient::verify_auth_ok_and_capture(const std::string& reply) {
    // Phase A.5: parse client_id + protocol_version out of the auth.ok
    // frame and enforce the version invariant. Caller owns closing the
    // fd on a `false` return — this function only mutates the per-client
    // state (`client_id_`, `protocol_version_`, `protocol_mismatch_`).
    const std::string parsed_id = extract_client_id_from_auth_ok(reply);
    const auto pv = extract_protocol_version_from_auth_ok(reply);
    if (!pv.present || pv.value != IPC_PROTOCOL_VERSION) {
        // Missing field → treat as v0 for logging. The plan body is
        // explicit: A.5 ships with the daemon as a unit so a pre-A.5
        // daemon on the wire is an operational error worth surfacing
        // loudly, not a "tolerate silently" condition.
        const int seen = pv.present ? pv.value : 0;
        log_error("ipc_client: protocol_version mismatch "
                  "(expected=%d, seen=%d, present=%s) — closing connection",
                  IPC_PROTOCOL_VERSION, seen, pv.present ? "true" : "false");
        protocol_mismatch_ = true;
        protocol_version_  = pv.present ? pv.value : 0;
        // Do NOT populate client_id_ on a rejected handshake — leaving
        // it empty makes "did this connection succeed" trivially testable
        // via `client_id().empty()` alongside `protocol_mismatch()`.
        client_id_.clear();
        return false;
    }
    client_id_         = parsed_id;
    protocol_version_  = pv.value;
    protocol_mismatch_ = false;
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

    // Phase A.4 + A.5: capture client_id and enforce protocol_version.
    // Mismatch flips `protocol_mismatch_` and fails the connect — the
    // server-side has no notion of "rejected by client" but the disconnect
    // is observable as a clean EOF on its end.
    if (!verify_auth_ok_and_capture(reply)) {
        close(fd_); fd_ = -1; return false;
    }

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
    // Phase A.5: drop the cached server-reported protocol_version for
    // symmetry. The latched `protocol_mismatch_` is intentionally NOT
    // cleared here — it documents the reason this connection was torn
    // down, and the next `connect()` call clears it before retrying.
    protocol_version_ = 0;
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
