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
        std::vector<struct pollfd> fds;
        fds.push_back({wakeup_read_, POLLIN, 0});
        fds.push_back({listen_fd_, POLLIN, 0});
        for (auto& [fd, _] : clients_)
            fds.push_back({fd, POLLIN, 0});

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

        // Check client sockets
        for (size_t i = 2; i < fds.size(); ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
                handle_client_data(fds[i].fd);
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
    // Copy client fds in case send_to removes one
    std::vector<int> fds;
    for (auto& [fd, _] : clients_) fds.push_back(fd);
    for (int fd : fds)
        send_to(fd, wire);
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
    }

    clients_[fd] = {};
    log_info("ipc_server: client connected (fd=%d, total=%zu)", fd, clients_.size());
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

    // Process complete lines (NDJSON)
    std::string& rbuf = it->second.read_buf;
    size_t pos;
    while ((pos = rbuf.find('\n')) != std::string::npos) {
        std::string line = rbuf.substr(0, pos);
        rbuf.erase(0, pos + 1);

        if (line.empty()) continue;

        IpcMessage msg;
        if (!parse_ipc_message(line, msg) || msg.type != IpcMessageType::Request) {
            IpcError err;
            err.id = 0;
            err.code = static_cast<int>(IpcErrorCode::InvalidRequest);
            err.message = "Invalid request";
            send_to(fd, serialize(err) + "\n");
            continue;
        }

        log_debug("ipc: handling '%s' from fd=%d", msg.request.method.c_str(), fd);
        auto handler_it = handlers_.find(msg.request.method);
        if (handler_it == handlers_.end()) {
            IpcError err;
            err.id = msg.request.id;
            err.code = static_cast<int>(IpcErrorCode::MethodNotFound);
            err.message = "Method not found: " + msg.request.method;
            send_to(fd, serialize(err) + "\n");
            continue;
        }

        IpcResponse resp;
        IpcError err;
        resp.id = msg.request.id;
        err.id = msg.request.id;

        if (handler_it->second(msg.request, resp, err))
            send_to(fd, serialize(resp) + "\n");
        else
            send_to(fd, serialize(err) + "\n");
    }
}

void IpcServer::remove_client(int fd) {
    log_info("ipc_server: client disconnected (fd=%d)", fd);
    close(fd);
    clients_.erase(fd);
}

void IpcServer::send_to(int fd, const std::string& msg) {
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(msg.size())) {
        ssize_t n = write(fd, msg.data() + total, msg.size() - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            remove_client(fd);
            return;
        }
        total += n;
    }
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
