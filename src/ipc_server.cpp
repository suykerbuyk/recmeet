// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "ipc_server.h"
#include "log.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>

namespace recmeet {

namespace {

// Constant-time string comparison. Avoids early-exit timing leaks on the
// PSK match path. Strings of differing length still take O(max(len)) time
// (length difference is itself non-secret — the attacker controls their
// own input length).
bool ct_equals(const std::string& a, const std::string& b) {
    const size_t na = a.size();
    const size_t nb = b.size();
    const size_t n  = na > nb ? na : nb;
    unsigned char diff = static_cast<unsigned char>(na ^ nb);
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = i < na ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < nb ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

// Format peer address for log lines from getpeername(). Falls back to
// "unknown" if the syscall fails. Never logs anything sensitive.
std::string format_peer_tcp(int fd) {
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0)
        return "unknown";
    char host[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
    return std::string(host) + ":" + std::to_string(ntohs(addr.sin_port));
}

// Send a small auth-error JSON frame. Hand-rolled rather than going through
// the `IpcError` enum because the auth surface is its own protocol shape:
// it is `{"type":"auth.error","reason":"..."}`, not a request/response pair.
std::string make_auth_error_frame(const std::string& reason) {
    return std::string("{\"type\":\"auth.error\",\"reason\":\"") + reason + "\"}\n";
}

const std::string AUTH_OK_FRAME = "{\"type\":\"auth.ok\"}\n";

// Lightweight extractor for `{"type":"auth.token","token":"..."}`. Avoids
// pulling the full IpcMessage parser into the auth path — auth frames are
// not requests/responses/events and do not carry an id. Returns true when
// `line` looks like a well-formed auth.token frame and `out` is populated.
bool try_parse_auth_token(const std::string& line, std::string& out) {
    // Find "type":"auth.token"
    if (line.find("\"type\":\"auth.token\"") == std::string::npos
        && line.find("\"type\": \"auth.token\"") == std::string::npos)
        return false;

    // Find "token": followed by a quoted string. Tolerate optional whitespace
    // and conservative escape handling (\\, \"). Daemon-side validation only
    // accepts the value if extraction succeeds; on any malformed shape the
    // caller treats this as a reject and closes.
    size_t key = line.find("\"token\"");
    if (key == std::string::npos) return false;
    size_t colon = line.find(':', key);
    if (colon == std::string::npos) return false;
    size_t open = line.find('"', colon);
    if (open == std::string::npos) return false;
    out.clear();
    for (size_t i = open + 1; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            char nc = line[i + 1];
            if (nc == '"' || nc == '\\') { out += nc; ++i; continue; }
            // Other escape sequences are not expected for token values; pass through.
            out += c;
            continue;
        }
        if (c == '"') return true;
        out += c;
    }
    return false;
}

} // anonymous namespace

IpcServer::IpcServer(const std::string& socket_path) {
    if (!parse_ipc_address(socket_path, addr_)) {
        addr_ = default_ipc_address();
    }
}

IpcServer::~IpcServer() {
    stop();
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    if (wakeup_read_ >= 0) { close(wakeup_read_); wakeup_read_ = -1; }
    if (wakeup_write_ >= 0) { close(wakeup_write_); wakeup_write_ = -1; }
    for (auto& [fd, _] : clients_) close(fd);
    clients_.clear();
    if (addr_.transport == IpcTransport::Unix)
        unlink(addr_.socket_path.c_str());
}

void IpcServer::on(const std::string& method, MethodHandler handler) {
    handlers_[method] = std::move(handler);
}

bool IpcServer::start() {
    // Phase A.1 PSK gate: refuse to bring up an unauthenticated TCP listener.
    // Unix-socket listeners do not require a PSK because kernel peer
    // credentials provide local trust.
    if (addr_.transport == IpcTransport::Tcp && psk_.empty()) {
        log_error("ipc_server: TCP listener requires a pre-shared key. "
                  "Set RECMEET_AUTH_TOKEN or use a Unix socket.");
        return false;
    }

    // Create self-pipe for cross-thread wakeup
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        log_error("ipc_server: pipe() failed: %s", strerror(errno));
        return false;
    }
    wakeup_read_ = pipefd[0];
    wakeup_write_ = pipefd[1];
    fcntl(wakeup_read_, F_SETFL, O_NONBLOCK);
    fcntl(wakeup_write_, F_SETFL, O_NONBLOCK);

    if (addr_.transport == IpcTransport::Tcp)
        return start_tcp();
    return start_unix();
}

bool IpcServer::start_unix() {
    // Create socket directory
    std::string dir = addr_.socket_path.substr(0, addr_.socket_path.rfind('/'));
    if (!dir.empty()) {
        mkdir(dir.c_str(), 0700);
    }

    // Remove stale socket file
    unlink(addr_.socket_path.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        log_error("ipc_server: socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (addr_.socket_path.size() >= sizeof(addr.sun_path)) {
        log_error("ipc_server: socket path too long");
        close(listen_fd_); listen_fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, addr_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_error("ipc_server: bind(%s) failed: %s", addr_.socket_path.c_str(), strerror(errno));
        close(listen_fd_); listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 8) < 0) {
        log_error("ipc_server: listen() failed: %s", strerror(errno));
        close(listen_fd_); listen_fd_ = -1;
        return false;
    }

    fcntl(listen_fd_, F_SETFL, O_NONBLOCK);
    running_ = true;
    return true;
}

bool IpcServer::start_tcp() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        log_error("ipc_server: socket(TCP) failed: %s", strerror(errno));
        return false;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(addr_.port);
    if (addr_.host == "0.0.0.0" || addr_.host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, addr_.host.c_str(), &addr.sin_addr) != 1) {
            // Try resolving hostname
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(addr_.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                log_error("ipc_server: cannot resolve host: %s", addr_.host.c_str());
                close(listen_fd_); listen_fd_ = -1;
                return false;
            }
            addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
    }

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_error("ipc_server: bind(%s:%d) failed: %s",
                  addr_.host.c_str(), addr_.port, strerror(errno));
        close(listen_fd_); listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 8) < 0) {
        log_error("ipc_server: listen(TCP) failed: %s", strerror(errno));
        close(listen_fd_); listen_fd_ = -1;
        return false;
    }

    fcntl(listen_fd_, F_SETFL, O_NONBLOCK);
    running_ = true;
    return true;
}

void IpcServer::run() {
    log_debug("ipc: event loop ENTER (tid=%d)", (int)syscall(SYS_gettid));
    while (running_) {
        // Build pollfd array: [wakeup_read, listen_fd, ...clients]
        // Each client fd watches POLLIN always; POLLOUT is added only when
        // the per-fd outbound queue is back-pressured (Phase A.2). This
        // replaces the iter-139 C-1 busy-spin on EAGAIN — the poll loop
        // now sleeps until either there is data to read OR the kernel
        // socket buffer drains enough to accept more bytes.
        std::vector<struct pollfd> fds;
        fds.push_back({wakeup_read_, POLLIN, 0});
        fds.push_back({listen_fd_, POLLIN, 0});
        for (auto& [fd, cs] : clients_) {
            short events = POLLIN;
            if (cs.want_pollout) events |= POLLOUT;
            fds.push_back({fd, events, 0});
        }

        int ret = poll(fds.data(), fds.size(), -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log_error("ipc_server: poll() error: %s", strerror(errno));
            break;
        }

        // Check wakeup pipe
        if (fds[0].revents & POLLIN) {
            drain_wakeup();
            run_posted();
            if (!running_) break;
        }

        // Check listen socket
        if (fds[1].revents & POLLIN)
            accept_client();

        // Check client sockets. POLLOUT path runs BEFORE the read path so
        // that an outstanding response goes out even if the client is
        // simultaneously sending more data — and so that fds flagged
        // pending_close on a prior enqueue overflow are reaped before
        // they are read again. Hangup/error wins outright.
        for (size_t i = 2; i < fds.size(); ++i) {
            int fd = fds[i].fd;
            short rev = fds[i].revents;
            if (rev & (POLLHUP | POLLERR | POLLNVAL)) {
                remove_client(fd);
                continue;
            }
            if (rev & POLLOUT) {
                drain_outbound(fd);
                if (clients_.find(fd) == clients_.end()) continue;
            }
            if (rev & POLLIN)
                handle_client_data(fd);
        }
    }
    log_debug("ipc: event loop EXIT");
}

void IpcServer::stop() {
    running_ = false;
    // Wake the poll loop
    if (wakeup_write_ >= 0) {
        char c = 'X';
        (void)write(wakeup_write_, &c, 1);
    }
}

void IpcServer::broadcast(const IpcEvent& ev) {
    std::string wire = serialize(ev) + "\n";
    // Copy client fds in case send_to removes one (Response-class overflow
    // closes the client mid-loop). Events themselves use drop-oldest, but
    // an unrelated queued response on the same fd could trigger close;
    // the copy makes the iteration safe either way.
    std::vector<int> fds;
    fds.reserve(clients_.size());
    for (auto& [fd, _] : clients_) fds.push_back(fd);
    for (int fd : fds)
        send_to(fd, wire, MessageClass::Event);
}

void IpcServer::post(std::function<void()> fn) {
    log_debug("ipc: post() from tid=%d", (int)syscall(SYS_gettid));
    {
        std::lock_guard<std::mutex> lock(post_mu_);
        posted_.push_back(std::move(fn));
    }
    if (wakeup_write_ >= 0) {
        char c = 'P';
        (void)write(wakeup_write_, &c, 1);
    }
}

void IpcServer::accept_client() {
    int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, O_NONBLOCK);

    ClientState cs;
    if (addr_.transport == IpcTransport::Tcp) {
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
        int idle = 30;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        int intvl = 10;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        int cnt = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

        // Phase A.1: TCP clients must complete the PSK handshake before any
        // request handler is dispatched. Capture the peer address up front
        // so refusal log lines have it even if the client closes early.
        cs.auth_state = AuthState::PendingPsk;
        cs.peer_addr  = format_peer_tcp(fd);
    } else {
        // Unix-socket peer credentials make local trust authoritative.
        cs.auth_state = AuthState::Authed;
        cs.peer_addr  = "unix";
    }

    clients_[fd] = std::move(cs);
    log_info("ipc_server: client connected (fd=%d, peer=%s, total=%zu)",
             fd, clients_[fd].peer_addr.c_str(), clients_.size());
}

bool IpcServer::handle_pending_psk(int fd, ClientState& cs, const std::string& line) {
    std::string token;
    if (!try_parse_auth_token(line, token)) {
        // First TCP frame was not auth.token — protocol violation.
        log_warn("ipc_server: TCP auth refused (peer=%s, reason=auth_required)",
                 cs.peer_addr.c_str());
        cs.auth_state = AuthState::Rejected;
        send_to(fd, make_auth_error_frame("auth_required"), MessageClass::Response);
        // send_to() never closes synchronously on EAGAIN any more (Phase A.2);
        // it queues + arms POLLOUT. But Response-class overflow may close.
        // Drop the fd if it is still alive.
        if (clients_.find(fd) != clients_.end())
            remove_client(fd);
        return false;
    }

    if (!ct_equals(token, psk_)) {
        // Mismatched PSK. Log the peer + reason but never the token itself.
        log_warn("ipc_server: TCP auth refused (peer=%s, reason=invalid_token)",
                 cs.peer_addr.c_str());
        cs.auth_state = AuthState::Rejected;
        send_to(fd, make_auth_error_frame("invalid_token"), MessageClass::Response);
        if (clients_.find(fd) != clients_.end())
            remove_client(fd);
        return false;
    }

    cs.auth_state = AuthState::Authed;
    log_info("ipc_server: TCP auth ok (peer=%s)", cs.peer_addr.c_str());
    send_to(fd, AUTH_OK_FRAME, MessageClass::Response);
    return true;
}

void IpcServer::handle_client_data(int fd) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
        remove_client(fd);
        return;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    it->second.read_buf.append(buf, n);

    // Phase A.2 NDJSON line cap. Apply BEFORE the per-line consume loop so
    // a malicious peer that never sends `\n` (slowloris) cannot grow
    // `read_buf` unboundedly. A buffer past the cap with no `\n` proves
    // there is no complete line to dispatch; treat as protocol abuse and
    // drop. The check is post-append intentionally — if the cap is e.g.
    // 8 MB and a single read brought in 4 KB, we want to admit the bytes
    // and only drop when the cap is genuinely exceeded.
    {
        const std::string& rbuf_c = it->second.read_buf;
        if (rbuf_c.size() > max_message_bytes_
            && rbuf_c.find('\n') == std::string::npos) {
            log_warn("ipc_server: NDJSON line cap exceeded "
                     "(fd=%d, peer=%s, buffered=%zu, cap=%zu); closing client",
                     fd, it->second.peer_addr.c_str(),
                     rbuf_c.size(), max_message_bytes_);
            remove_client(fd);
            return;
        }
    }

    // Process complete lines (NDJSON)
    std::string& rbuf = it->second.read_buf;
    size_t pos;
    while ((pos = rbuf.find('\n')) != std::string::npos) {
        // Phase A.2: even a line terminated by `\n` is rejected if its
        // length exceeds the cap. This guards against the case where a
        // client streams `max_message_bytes + 1` bytes and only then
        // sends a newline — the slowloris cap above caught the
        // never-newline path; this catches the eventually-newline path.
        if (pos > max_message_bytes_) {
            log_warn("ipc_server: NDJSON line cap exceeded "
                     "(fd=%d, peer=%s, line_len=%zu, cap=%zu); closing client",
                     fd, it->second.peer_addr.c_str(),
                     pos, max_message_bytes_);
            remove_client(fd);
            return;
        }
        std::string line = rbuf.substr(0, pos);
        rbuf.erase(0, pos + 1);

        if (line.empty()) continue;

        // Phase A.1 PSK gate: TCP clients must clear PendingPsk before any
        // other dispatch. handle_pending_psk() consumes the line — on
        // success it flips auth_state to Authed and continues; on failure
        // it sends auth.error, schedules removal, and we break out of the
        // per-line loop because the iterator may now be stale. Unix
        // clients are pre-marked Authed in accept_client() and skip this
        // branch entirely.
        if (it->second.auth_state == AuthState::PendingPsk) {
            if (!handle_pending_psk(fd, it->second, line)) {
                // Connection dropped; the iterator/clients_ entry is gone.
                return;
            }
            // Either we're now Authed and continue dispatching subsequent
            // lines, or we have already returned. Re-look-up the iterator
            // because clients_ map may have rehashed.
            it = clients_.find(fd);
            if (it == clients_.end()) return;
            continue;
        }

        // Defensive: a Rejected client should already be gone, but guard
        // anyway so a residual buffered frame after rejection cannot
        // surface to a request handler.
        if (it->second.auth_state != AuthState::Authed) {
            remove_client(fd);
            return;
        }

        IpcMessage msg;
        if (!parse_ipc_message(line, msg) || msg.type != IpcMessageType::Request) {
            IpcError err;
            err.id = 0;
            err.code = static_cast<int>(IpcErrorCode::InvalidRequest);
            err.message = "Invalid request";
            send_to(fd, serialize(err) + "\n", MessageClass::Response);
            // Response-class overflow may have closed the client; re-look
            // up to keep the loop safe.
            it = clients_.find(fd);
            if (it == clients_.end()) return;
            continue;
        }

        log_debug("ipc: handling '%s' from fd=%d", msg.request.method.c_str(), fd);
        auto handler_it = handlers_.find(msg.request.method);
        if (handler_it == handlers_.end()) {
            IpcError err;
            err.id = msg.request.id;
            err.code = static_cast<int>(IpcErrorCode::MethodNotFound);
            err.message = "Method not found: " + msg.request.method;
            send_to(fd, serialize(err) + "\n", MessageClass::Response);
            it = clients_.find(fd);
            if (it == clients_.end()) return;
            continue;
        }

        IpcResponse resp;
        IpcError err;
        resp.id = msg.request.id;
        err.id = msg.request.id;

        if (handler_it->second(msg.request, resp, err))
            send_to(fd, serialize(resp) + "\n", MessageClass::Response);
        else
            send_to(fd, serialize(err) + "\n", MessageClass::Response);

        // Re-look-up the iterator: send_to() with Response-class can close
        // the fd on overflow, invalidating any cached reference.
        it = clients_.find(fd);
        if (it == clients_.end()) return;
    }
}

void IpcServer::remove_client(int fd) {
    log_info("ipc_server: client disconnected (fd=%d)", fd);
    close(fd);
    clients_.erase(fd);
}

void IpcServer::send_to(int fd, std::string msg, MessageClass cls) {
    // Phase A.2 send path. Replaces the iter-139 C-1 EAGAIN spin loop.
    //
    // Flow:
    //   1. Look up the client. If gone, drop silently — the caller may
    //      legitimately call send_to() from a posted callback after the
    //      client has disconnected.
    //   2. Enqueue the frame on the per-fd outbound queue, applying the
    //      overflow policy by `cls`.
    //   3. Try to drain the queue immediately (non-blocking). On
    //      `EAGAIN`/`EWOULDBLOCK` set `want_pollout` and return; the poll
    //      loop will arm POLLOUT for this fd and resume the drain when
    //      the kernel socket buffer has room.
    //
    // The path under EAGAIN is bounded — write() returns immediately and
    // the loop yields to poll() — so a slow TCP consumer cannot stall the
    // poll thread under any event-rate pattern.

    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    ClientState& cs = it->second;

    // Apply overflow policy at enqueue time so the queue invariants hold.
    // Two caps (entries OR bytes); either tripping triggers overflow.
    auto would_overflow = [&]() {
        return cs.outbound.size() >= kOutboundMaxEntries
            || (cs.outbound_bytes + msg.size()) > kOutboundMaxBytes;
    };

    if (would_overflow()) {
        if (cls == MessageClass::Response) {
            // Response-class overflow is fatal — closing immediately frees
            // the slot for a healthy client. Logging surfaces the event
            // once per fd; per-frame logging would amplify a flood.
            log_warn("ipc_server: outbound queue overflow on response "
                     "(fd=%d, peer=%s, queued_bytes=%zu, queued_entries=%zu); "
                     "closing client",
                     fd, cs.peer_addr.c_str(),
                     cs.outbound_bytes, cs.outbound.size());
            remove_client(fd);
            return;
        }
        // Event-class: drop-oldest until we have room. Bound the loop to
        // the queue size — the new frame itself fits in `kOutboundMaxBytes`
        // because broadcast events are small (caption/progress/phase
        // ticks are well under a kilobyte).
        while (!cs.outbound.empty() && would_overflow()) {
            const OutboundFrame& front = cs.outbound.front();
            // Account for partial send: only the unsent tail still counts.
            size_t unsent = front.payload.size() - front.bytes_sent;
            cs.outbound_bytes -= unsent;
            cs.outbound.pop_front();
            ++cs.dropped_events;
        }
        // If a single payload exceeds the byte cap, accept it anyway
        // (defensive — better to deliver one oversized event than to
        // silently lose it AND every queued sibling). The byte cap is
        // sized for caption/progress traffic; a payload past it indicates
        // a different bug worth surfacing.
    }

    OutboundFrame frame;
    frame.payload    = std::move(msg);
    frame.bytes_sent = 0;
    frame.cls        = cls;
    cs.outbound_bytes += frame.payload.size();
    cs.outbound.push_back(std::move(frame));

    // Try an immediate drain so the steady-state happy path stays a
    // single write() syscall. drain_outbound() arms `want_pollout` if
    // it hits EAGAIN — no busy spin.
    drain_outbound(fd);
}

void IpcServer::drain_outbound(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    ClientState& cs = it->second;

    while (!cs.outbound.empty()) {
        OutboundFrame& frame = cs.outbound.front();
        const char* p = frame.payload.data() + frame.bytes_sent;
        size_t remaining = frame.payload.size() - frame.bytes_sent;
        ssize_t n = write(fd, p, remaining);
        if (n > 0) {
            frame.bytes_sent += static_cast<size_t>(n);
            cs.outbound_bytes -= static_cast<size_t>(n);
            if (frame.bytes_sent == frame.payload.size()) {
                cs.outbound.pop_front();
                continue;
            }
            // Short write — kernel buffer is full, fall through to the
            // EAGAIN handling below by trying once more.
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Phase A.2 acceptance: this is the path that used to spin.
            // Now we set the POLLOUT-arm flag and return; the poll loop
            // sleeps until the socket is writable again. No CPU burn.
            cs.want_pollout = true;
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // Hard error (ECONNRESET / EPIPE / etc.) — drop the client.
        remove_client(fd);
        return;
    }

    // Queue fully drained. Clear the POLLOUT-arm flag so the next poll()
    // does not wake on writable for no reason.
    cs.want_pollout = false;
}

void IpcServer::drain_wakeup() {
    char buf[64];
    while (read(wakeup_read_, buf, sizeof(buf)) > 0) {}
}

void IpcServer::run_posted() {
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> lock(post_mu_);
        fns.swap(posted_);
    }
    if (!fns.empty())
        log_debug("ipc: executing %zu posted callbacks", fns.size());
    for (auto& fn : fns) fn();
}

} // namespace recmeet
