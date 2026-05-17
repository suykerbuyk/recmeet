// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "ipc_server.h"
#include "json_util.h"
#include "log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
//
// Phase C.1: the returned bytes are a complete `0x00` NDJSON wire frame
// (discriminator + JSON + '\n') via `frame_ndjson()` — auth frames ride
// the same framed transport as every other NDJSON message.
std::string make_auth_error_frame(const std::string& reason) {
    return frame_ndjson(std::string("{\"type\":\"auth.error\",\"reason\":\"")
                        + reason + "\"}");
}

// Phase A.4: the auth.ok frame is now per-client because it embeds the
// server-issued `client_id` ("`{"type":"auth.ok","client_id":"..."}`").
// Built on-demand at the moment auth completes — for TCP this is the
// `handle_pending_psk()` success path; for Unix it is the synthetic
// post-`accept_client()` event. The `client_id` is escaped through the
// shared `json_escape()` helper so a future id format change does not
// silently produce an invalid frame.
//
// Phase A.5: the frame also carries `protocol_version` so the client
// can verify wire compatibility at the moment auth completes. A negative
// version (only used by test seams) suppresses the field entirely so the
// "missing field → mismatch" path can be exercised without forging raw
// bytes; production callers always pass `IPC_PROTOCOL_VERSION`.
//
// Phase C.1: emitted as a complete `0x00` NDJSON wire frame via
// `frame_ndjson()`. The client's auth-handshake reader (which runs before
// the steady-state FrameReader path) strips the same `0x00` prefix.
std::string make_auth_ok_frame(const std::string& client_id, int protocol_version,
                               const std::string& resume_token = "") {
    std::string json;
    json.reserve(64 + client_id.size() + resume_token.size());
    json += "{\"type\":\"auth.ok\",\"client_id\":\"";
    json += json_escape(client_id);
    json += "\"";
    if (protocol_version >= 0) {
        json += ",\"protocol_version\":";
        json += std::to_string(protocol_version);
    }
    // C.13 — additive field per L-2 (no IPC_PROTOCOL_VERSION bump; v1 clients
    // ignore unknown keys). Emitted only when the resolver hook returned a
    // non-empty token (resume succeeded OR fresh mint produced one); empty
    // means "no resume_token mechanism wired" (legacy / test path) — field
    // omitted entirely to preserve byte-equivalence with the pre-C.13 frame
    // on that fallback path.
    if (!resume_token.empty()) {
        json += ",\"resume_token\":\"";
        json += json_escape(resume_token);
        json += "\"";
    }
    json += "}";
    return frame_ndjson(json);
}

// Lightweight extractor for a quoted JSON string value following a key.
// Tolerates optional whitespace and conservative escape handling (\\, \").
// Returns true on a clean extraction; `out` cleared and populated.
bool try_extract_json_string(const std::string& line, const std::string& key,
                             std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t k = line.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = line.find(':', k);
    if (colon == std::string::npos) return false;
    size_t open = line.find('"', colon);
    if (open == std::string::npos) return false;
    out.clear();
    for (size_t i = open + 1; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            char nc = line[i + 1];
            if (nc == '"' || nc == '\\') { out += nc; ++i; continue; }
            out += c;
            continue;
        }
        if (c == '"') return true;
        out += c;
    }
    return false;
}

// Lightweight extractor for `{"type":"auth.token","token":"...","resume_token":"..."}`.
// `resume_token` is C.13's additive optional field — extraction returns true
// when the frame is well-formed (regardless of resume_token presence); the
// out-param is cleared when the field is absent or unextractable. Daemon-side
// validation only accepts `token` if extraction succeeds; on any malformed
// `token` shape the caller treats this as a reject and closes.
bool try_parse_auth_token(const std::string& line, std::string& out_token,
                          std::string& out_resume_token) {
    out_resume_token.clear();
    if (line.find("\"type\":\"auth.token\"") == std::string::npos
        && line.find("\"type\": \"auth.token\"") == std::string::npos)
        return false;
    if (!try_extract_json_string(line, "token", out_token)) return false;
    // resume_token is optional — silently succeed when absent or malformed.
    (void)try_extract_json_string(line, "resume_token", out_resume_token);
    return true;
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

std::string IpcServer::mint_client_id() {
    // Phase A.4: `c-<counter>-<6 hex>`. The counter alone is enough to be
    // unique within a process; the random suffix makes the id harder to
    // forge from outside (a future C.7 routing bug should not let an
    // attacker name a client_id deterministically from connection order
    // alone). `rand()` is seeded once at first use — this is a tag, not
    // a crypto primitive.
    static bool seeded = false;
    if (!seeded) {
        // Mix in the process id so two daemons on the same host do not
        // produce a colliding id sequence if both seed at the same epoch.
        std::srand(static_cast<unsigned>(std::time(nullptr))
                 ^ static_cast<unsigned>(getpid()));
        seeded = true;
    }
    char hex[7];
    std::snprintf(hex, sizeof(hex), "%06x",
                  static_cast<unsigned>(std::rand()) & 0xFFFFFFu);
    std::string id = "c-" + std::to_string(next_client_id_++) + "-" + hex;
    return id;
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

    // Phase A.3: backlog tracks `max_clients * 2` (default 32) so the
    // kernel listen queue does not bottleneck before the daemon-side cap
    // engages. listen_backlog() floors at 8 to preserve historical
    // behavior when the cap is very small.
    if (listen(listen_fd_, listen_backlog()) < 0) {
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

    // Phase A.3: backlog tracks `max_clients * 2` (default 32). See
    // start_unix() / listen_backlog() for rationale.
    if (listen(listen_fd_, listen_backlog()) < 0) {
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
    // Phase C.1: every outbound event is a `0x00` NDJSON wire frame.
    std::string wire = frame_ndjson(serialize(ev));
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

void IpcServer::send_to_client(const std::string& client_id, IpcEvent ev) {
    // Phase A.4 routing primitive. The `client_id → fd` reverse map is
    // the authoritative lookup; we never iterate clients_ to find a
    // matching id (would defeat the whole point of the reverse map).
    //
    // Best-effort delivery: a missing client_id is a normal case under
    // the v1 contract — a Phase C.7 routed event can race a client
    // disconnect, and the caller MUST be tolerant of that. We log at
    // debug-level only so a misbehaving job pumping events at a gone
    // client cannot flood the warning channel.
    auto rit = client_id_to_fd_.find(client_id);
    if (rit == client_id_to_fd_.end()) {
        log_debug("ipc_server: send_to_client dropping event for unknown "
                  "client_id=%s (event=%s)",
                  client_id.c_str(), ev.event.c_str());
        return;
    }
    int fd = rit->second;
    // Stamp the routing target on the wire payload itself so a downstream
    // observer (Phase C.7 test harness, future replay log) can confirm
    // the intended recipient from the bytes alone. The caller may pass
    // an `IpcEvent` with `client_id` already populated; we overwrite to
    // guarantee the wire matches the lookup key.
    ev.client_id = client_id;
    std::string wire = frame_ndjson(serialize(ev));  // Phase C.1: 0x00 frame
    send_to(fd, std::move(wire), MessageClass::Event);
}

void IpcServer::send_binary_to_client(const std::string& client_id,
                                      FrameType type,
                                      std::string payload,
                                      MessageClass cls) {
    // Phase C.4 routing primitive for `0x01`/`0x02`/`0x03` frames. Mirrors
    // `send_to_client()` for NDJSON events: the `client_id → fd` reverse
    // map is the authoritative lookup; a missing entry means the client
    // disconnected between handler return and this call, and we drop the
    // frame with a debug trace rather than logging at warn.
    auto rit = client_id_to_fd_.find(client_id);
    if (rit == client_id_to_fd_.end()) {
        log_debug("ipc_server: send_binary_to_client dropping frame for "
                  "unknown client_id=%s (type=0x%02x, len=%zu)",
                  client_id.c_str(), static_cast<unsigned>(type),
                  payload.size());
        return;
    }
    int fd = rit->second;
    // `frame_binary` produces a complete length-prefixed binary wire frame.
    // The caller has already populated `type` with one of the binary
    // discriminators (0x01/0x02/0x03); `frame_binary` ignores `FrameType::Ndjson`
    // defensively and degrades to `BinaryUpload` if misused.
    std::string wire = frame_binary(type, payload);
    send_to(fd, std::move(wire), cls);
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

    // Phase A.3 connection cap. We MUST drain the kernel accept queue
    // (the `accept()` above) even when over cap, otherwise the listening
    // socket keeps waking poll(). Refusal happens AFTER the syscall: we
    // write a single-line JSON `server_full` error frame and close the fd
    // without ever registering it in `clients_`.
    //
    // The cap is checked here rather than inside the per-transport setup
    // below so a client over the cap pays no PSK / sockopt cost on the
    // daemon's side — the refusal is symmetric for Unix and TCP.
    if (clients_.size() >= max_clients_) {
        reject_full(fd);
        return;
    }

    ClientState cs;
    // Phase C.1: stamp the server's binary-frame cap onto this
    // connection's FrameReader. The reader is default-constructed with
    // `kDefaultMaxBinaryFrameBytes`; this honors a post-construction
    // `set_max_binary_frame_bytes()` override (e.g. from daemon.yaml once
    // C.2 / C.10a wire the config key).
    cs.reader.set_max_binary_frame_bytes(max_binary_frame_bytes_);
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
        //
        // Phase A.4: client_id is NOT minted here for TCP — assignment
        // happens at the moment `handle_pending_psk()` flips auth_state
        // to Authed. Until then the client is pre-auth and has no id;
        // anyone reading `cs.client_id` before then sees the default "".
        cs.auth_state = AuthState::PendingPsk;
        cs.peer_addr  = format_peer_tcp(fd);
    } else {
        // Unix-socket peer credentials make local trust authoritative.
        //
        // Phase A.4: Unix clients bypass PSK entirely, so client_id is
        // minted right here at accept time. A synthetic `auth.ok` event
        // (built per-client to include the id) is queued for the same
        // tick so the client sees the same handshake-completion event
        // shape as a TCP client. This is the only IPC frame the daemon
        // ever sends unprompted on the Unix surface — everything else
        // is request-response or broadcast.
        cs.auth_state = AuthState::Authed;
        cs.peer_addr  = "unix";
        cs.client_id  = mint_client_id();
    }

    std::string assigned_id = cs.client_id;  // empty for TCP, populated for Unix
    bool        is_unix     = (addr_.transport != IpcTransport::Tcp);

    clients_[fd] = std::move(cs);
    if (!assigned_id.empty())
        client_id_to_fd_[assigned_id] = fd;
    log_info("ipc_server: client connected (fd=%d, peer=%s, total=%zu, client_id=%s)",
             fd, clients_[fd].peer_addr.c_str(), clients_.size(),
             assigned_id.empty() ? "<pending>" : assigned_id.c_str());

    // Phase A.4: synthetic auth.ok for the Unix bypass path. Queued via
    // the standard send_to() so it observes the same outbound-queue
    // back-pressure semantics as any other Response-class frame. Doing
    // this AFTER `clients_[fd] = ...` is mandatory; send_to() looks up
    // the fd in `clients_` and silently drops if missing.
    if (is_unix) {
        send_to(fd, make_auth_ok_frame(assigned_id, protocol_version_),
                MessageClass::Response);
    }
}

void IpcServer::reject_full(int fd) {
    // Phase A.3: doomed fd. Take the peer address for the log line, then
    // write the refusal frame synchronously and close. We deliberately do
    // NOT enqueue or arm POLLOUT — the fd is about to be closed and a
    // partial write is acceptable (TCP RST will reach the client either
    // way). Best-effort delivery; the client cap is the contract, the
    // JSON frame is the courtesy.
    //
    // Frame shape parallels Phase A.1's `make_auth_error_frame` style:
    // small, hand-rolled NDJSON, no dependency on the IpcEvent serializer
    // to keep the rejection path free of allocation surprises.
    std::string peer = (addr_.transport == IpcTransport::Tcp)
                           ? format_peer_tcp(fd)
                           : std::string("unix");
    log_warn("ipc_server: connection refused — server_full "
             "(peer=%s, current_clients=%zu, cap=%zu)",
             peer.c_str(), clients_.size(), max_clients_);

    // Phase C.1: the refusal frame is a complete `0x00` NDJSON wire frame
    // (discriminator + JSON + '\n') so a peer that already speaks v2 reads
    // it through the normal FrameReader path. Built once per call rather
    // than as a static literal because `frame_ndjson()` is not constexpr.
    const std::string server_full_frame = frame_ndjson(
        "{\"event\":\"error\",\"kind\":\"server_full\","
        "\"reason\":\"connection cap reached\"}");
    const size_t len = server_full_frame.size();

    // Single best-effort write. The fd was switched to O_NONBLOCK in
    // accept_client() before we got here, so write() returns immediately
    // — either with the full ~80 bytes (loopback / healthy peer), with a
    // short count (rare for a payload this small), or with EAGAIN if the
    // peer's kernel send buffer is wedged. Either way we close
    // immediately after; we don't burn cycles looping on a doomed fd.
    // The plan body explicitly specifies plain `write()` synchronously
    // for the refusal frame, not the queue path.
    ssize_t n = write(fd, server_full_frame.data(), len);
    (void)n;  // intentionally ignored — fd is doomed regardless
    close(fd);
}

bool IpcServer::handle_pending_psk(int fd, ClientState& cs, const std::string& line) {
    std::string psk_token, resume_token;
    if (!try_parse_auth_token(line, psk_token, resume_token)) {
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

    if (!ct_equals(psk_token, psk_)) {
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
    // C.13: resume_token resolver. When the daemon has wired the resolver
    // (set_resume_token_resolver), it owns the "resume an existing session
    // OR mint fresh" decision and the matching `client_id` + `resume_token`
    // pair. Pre-C.13 callers (tests with no resolver) fall back to the
    // legacy fresh-mint path with no token echoed on auth.ok.
    std::string client_id;
    std::string echo_token;
    if (resume_resolver_) {
        auto pr = resume_resolver_(resume_token);
        client_id  = std::move(pr.first);
        echo_token = std::move(pr.second);
    } else {
        client_id = mint_client_id();
    }
    cs.client_id = client_id;
    // Phase A.4: the reverse-map insert MUST happen before send_to() so
    // that a Phase C.7 routed event posted in the same tick can resolve
    // the id immediately. Today's call sites all run on the poll thread
    // so the ordering is enforced by the thread, but lock-free design
    // means the assignment+insert pair must read atomically from the
    // caller's perspective.
    client_id_to_fd_[cs.client_id] = fd;
    log_info("ipc_server: TCP auth ok (peer=%s, client_id=%s)",
             cs.peer_addr.c_str(), cs.client_id.c_str());
    send_to(fd, make_auth_ok_frame(cs.client_id, protocol_version_, echo_token),
            MessageClass::Response);
    return true;
}

void IpcServer::set_resume_token_resolver(ResumeTokenResolver r) {
    resume_resolver_ = std::move(r);
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

    // Phase C.1: feed the raw bytes into the per-connection FrameReader
    // state machine. It owns the unconsumed-byte accumulator internally
    // and yields fully-assembled frames — replacing the A.2-era
    // `read_buf` + `rbuf.find('\n')` scan.
    it->second.reader.feed(buf, static_cast<size_t>(n));

    // Phase A.2 NDJSON line cap, re-expressed against the FrameReader.
    // Apply BEFORE draining frames so a malicious peer that opens a `0x00`
    // NDJSON frame and never sends `\n` (slowloris) cannot grow the
    // reader's internal buffer unboundedly. The cap is meaningful only
    // while the reader is mid-NDJSON-frame — a binary frame is bounded
    // separately by `max_binary_frame_bytes_` inside the FrameReader, and
    // a reader sitting at a frame boundary has nothing buffered to abuse.
    if (it->second.reader.in_ndjson_frame()
        && it->second.reader.buffered() > max_message_bytes_) {
        log_warn("ipc_server: NDJSON line cap exceeded "
                 "(fd=%d, peer=%s, buffered=%zu, cap=%zu); closing client",
                 fd, it->second.peer_addr.c_str(),
                 it->second.reader.buffered(), max_message_bytes_);
        remove_client(fd);
        return;
    }

    // Drain every complete frame the reader can assemble from the bytes
    // received so far. Each iteration produces exactly one frame; partial
    // frames leave the reader in NeedMore and we return until more bytes
    // arrive. A bad discriminator or oversized binary frame is terminal.
    for (;;) {
        Frame frame;
        FrameStatus st = it->second.reader.next(frame);
        if (st == FrameStatus::NeedMore)
            break;
        if (st == FrameStatus::BadDiscriminator) {
            log_warn("ipc_server: unknown frame discriminator "
                     "(fd=%d, peer=%s); closing client",
                     fd, it->second.peer_addr.c_str());
            remove_client(fd);
            return;
        }
        if (st == FrameStatus::FrameTooLarge) {
            log_warn("ipc_server: binary frame exceeds cap "
                     "(fd=%d, peer=%s, cap=%zu); closing client",
                     fd, it->second.peer_addr.c_str(),
                     max_binary_frame_bytes_);
            remove_client(fd);
            return;
        }
        // st == FrameStatus::Ok — `frame` holds one complete frame.

        // Phase C.10a: binary frames (`0x01`/`0x02`/`0x03`) are routed to
        // the registered binary-frame handler if one exists. C.10a wires
        // it to feed `0x03` streaming-audio payloads into the matching
        // streaming session; C.2/C.4 will consume `0x01`/`0x02`. A binary
        // frame is only dispatched once the client is Authed — an
        // unauthenticated TCP client must clear PSK first. When no handler
        // is registered the frame is discarded with a debug trace,
        // preserving the C.1 transport-only behavior. A handler that
        // returns false signals a protocol violation and the connection
        // is torn down.
        if (frame.type != FrameType::Ndjson) {
            if (it->second.auth_state != AuthState::Authed) {
                log_warn("ipc_server: binary frame from non-Authed client "
                         "(fd=%d, peer=%s); closing client",
                         fd, it->second.peer_addr.c_str());
                remove_client(fd);
                return;
            }
            if (binary_frame_handler_) {
                std::string client_id = it->second.client_id;
                bool ok = binary_frame_handler_(client_id, frame.type,
                                                frame.payload);
                // The handler may have triggered a send_to_client() that
                // closed the fd on Response-class overflow; re-look-up.
                it = clients_.find(fd);
                if (it == clients_.end()) return;
                if (!ok) {
                    log_warn("ipc_server: binary-frame handler rejected "
                             "frame type=0x%02x (fd=%d, peer=%s); "
                             "closing client",
                             static_cast<unsigned>(frame.type), fd,
                             it->second.peer_addr.c_str());
                    remove_client(fd);
                    return;
                }
            } else {
                log_debug("ipc: received binary frame type=0x%02x len=%zu "
                          "from fd=%d (no handler — discarded)",
                          static_cast<unsigned>(frame.type),
                          frame.payload.size(), fd);
            }
            continue;
        }

        const std::string& line = frame.payload;
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
            send_to(fd, frame_ndjson(serialize(err)), MessageClass::Response);
            // Response-class overflow may have closed the client; re-look
            // up to keep the loop safe.
            it = clients_.find(fd);
            if (it == clients_.end()) return;
            continue;
        }

        // Phase A.6: server-side stamp the originating `client_id` on the
        // request so handlers can identify the per-client session slot
        // without reaching into the IpcServer's private maps. The field
        // is server-stamped only — `serialize(IpcRequest)` does not write
        // it onto the wire, and clients never set it.
        msg.request.client_id = it->second.client_id;

        log_debug("ipc: handling '%s' from fd=%d", msg.request.method.c_str(), fd);
        auto handler_it = handlers_.find(msg.request.method);
        if (handler_it == handlers_.end()) {
            IpcError err;
            err.id = msg.request.id;
            err.code = static_cast<int>(IpcErrorCode::MethodNotFound);
            err.message = "Method not found: " + msg.request.method;
            send_to(fd, frame_ndjson(serialize(err)), MessageClass::Response);
            it = clients_.find(fd);
            if (it == clients_.end()) return;
            continue;
        }

        IpcResponse resp;
        IpcError err;
        resp.id = msg.request.id;
        err.id = msg.request.id;

        if (handler_it->second(msg.request, resp, err))
            send_to(fd, frame_ndjson(serialize(resp)), MessageClass::Response);
        else
            send_to(fd, frame_ndjson(serialize(err)), MessageClass::Response);

        // Re-look-up the iterator: send_to() with Response-class can close
        // the fd on overflow, invalidating any cached reference.
        it = clients_.find(fd);
        if (it == clients_.end()) return;
    }
}

void IpcServer::remove_client(int fd) {
    // Phase A.4: erase the `client_id → fd` reverse-map entry alongside
    // the primary map. Done BEFORE the close() / erase() so a callback
    // posted concurrently (e.g. a C.7 event for this exact client) sees
    // a consistent "gone" view — `send_to_client()` looks up the reverse
    // map first.
    auto it = clients_.find(fd);
    std::string gone_client_id;
    if (it != clients_.end() && !it->second.client_id.empty()) {
        gone_client_id = it->second.client_id;
        client_id_to_fd_.erase(it->second.client_id);
    }
    log_info("ipc_server: client disconnected (fd=%d)", fd);
    close(fd);
    clients_.erase(fd);

    // Phase C.10a: notify the disconnect handler AFTER the maps are
    // consistent (fd closed, client_id_to_fd_ + clients_ erased). The
    // handler runs inline on the poll thread and must not block — C.10a
    // wires it to abort the disconnected client's streaming session (mark
    // the JobQueue job failed, unlink the temp WAV, release the slot).
    if (!gone_client_id.empty() && client_disconnect_handler_) {
        client_disconnect_handler_(gone_client_id);
    }
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

// Phase A.6 session-state accessors. `clients_` and `client_id_to_fd_` are
// owned by the poll thread; the daemon's rec_worker thread reads via
// `get_session()` off-thread. The implementation here accepts that
// snapshot-on-read semantics — a concurrent `set_session_*` from the poll
// thread mid-copy is bounded by the underlying string-copy operations,
// each individually wait-free, and the worst-case observation is "either
// pre-update or post-update view, never torn". The acceptable failure
// mode if the client disconnects mid-rec_worker is `get_session()`
// returning false; callers must then fall back to daemon.yaml-only.
bool IpcServer::get_session(const std::string& client_id,
                            SessionCredentials& out_creds,
                            SessionPreferences& out_prefs) const {
    auto rit = client_id_to_fd_.find(client_id);
    if (rit == client_id_to_fd_.end()) return false;
    auto it = clients_.find(rit->second);
    if (it == clients_.end()) return false;
    out_creds = it->second.creds;
    out_prefs = it->second.prefs;
    return true;
}

// Wholesale replacement. Handlers implement the partial-update by first
// calling `get_session()` to snapshot the existing slot, overlaying only
// the fields present in the incoming request, then calling these setters
// with the merged result. Booleans and ints have no "unset" sentinel at
// the C++ struct level, so deferring the merge logic to the handler
// keeps it honest: the handler sees the raw JSON params and knows which
// keys were actually sent.
bool IpcServer::set_session_credentials(const std::string& client_id,
                                        const SessionCredentials& creds) {
    auto rit = client_id_to_fd_.find(client_id);
    if (rit == client_id_to_fd_.end()) return false;
    auto it = clients_.find(rit->second);
    if (it == clients_.end()) return false;
    it->second.creds = creds;
    return true;
}

bool IpcServer::set_session_preferences(const std::string& client_id,
                                        const SessionPreferences& prefs) {
    auto rit = client_id_to_fd_.find(client_id);
    if (rit == client_id_to_fd_.end()) return false;
    auto it = clients_.find(rit->second);
    if (it == clients_.end()) return false;
    it->second.prefs = prefs;
    return true;
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
