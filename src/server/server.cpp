#include "src/server/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <utility>

#include "src/protocol/protocol.hpp"

namespace {

void ignore_sigpipe_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { std::signal(SIGPIPE, SIG_IGN); });
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

Server::Server(ServerConfig config) : config_(std::move(config)) {
    ignore_sigpipe_once();
}

Server::~Server() {
    stop();
}

bool Server::start() {
    if (running_.load()) {
        return true;
    }

    if (!config_.wal_path.empty()) {
        wal_ = std::make_unique<WriteAheadLog>(config_.wal_path);
        wal_->recover(queues_);
    }

    if (!config_.followers.empty()) {
        replicator_ = std::make_unique<Replicator>(config_.followers);
    }

    if (::pipe(wakeup_fds_) != 0) {
        last_error_ = std::strerror(errno);
        return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        last_error_ = std::strerror(errno);
        ::close(wakeup_fds_[0]);
        ::close(wakeup_fds_[1]);
        wakeup_fds_[0] = wakeup_fds_[1] = -1;
        return false;
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        last_error_ = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::close(wakeup_fds_[0]);
        ::close(wakeup_fds_[1]);
        wakeup_fds_[0] = wakeup_fds_[1] = -1;
        return false;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        bound_port_.store(ntohs(addr.sin_port));
    } else {
        bound_port_.store(config_.port);
    }

    if (::listen(listen_fd_, 128) != 0) {
        last_error_ = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::close(wakeup_fds_[0]);
        ::close(wakeup_fds_[1]);
        wakeup_fds_[0] = wakeup_fds_[1] = -1;
        return false;
    }

    pool_ = std::make_unique<ThreadPool>(config_.worker_threads);
    running_.store(true);

    reaper_thread_ = std::thread(&Server::reaper_loop, this);
    accept_thread_ = std::thread(&Server::accept_loop, this);
    return true;
}

void Server::run() {
    if (!running_.load()) {
        if (!start()) {
            return;
        }
    }
    std::unique_lock<std::mutex> lock(stop_mutex_);
    stop_cv_.wait(lock, [this] { return !running_.load(); });
}

void Server::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    stop_cv_.notify_all();

    if (wakeup_fds_[1] >= 0) {
        char byte = 1;
        ssize_t ignored = ::write(wakeup_fds_[1], &byte, 1);
        (void)ignored;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (reaper_thread_.joinable()) {
        reaper_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int fd : client_fds_) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    if (pool_) {
        pool_->shutdown();
        pool_.reset();
    }

    if (replicator_) {
        replicator_->stop();
        replicator_.reset();
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    for (int& fd : wakeup_fds_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

void Server::accept_loop() {
    pollfd fds[2];
    fds[0].fd = listen_fd_;
    fds[0].events = POLLIN;
    fds[1].fd = wakeup_fds_[0];
    fds[1].events = POLLIN;

    while (running_.load()) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        int rc = ::poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            break;
        }
        if (fds[0].revents & POLLIN) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(
                listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
#ifdef SO_NOSIGPIPE
            int on = 1;
            ::setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                client_fds_.insert(client_fd);
            }
            if (!pool_->enqueue([this, client_fd] { handle_client(client_fd); })) {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                client_fds_.erase(client_fd);
                ::close(client_fd);
            }
        }
    }
}

void Server::reaper_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(stop_mutex_);
        stop_cv_.wait_for(lock, std::chrono::milliseconds{50},
                          [this] { return !running_.load(); });
        if (!running_.load()) {
            break;
        }
        lock.unlock();
        queues_.requeue_all_expired();
    }
}

void Server::handle_client(int client_fd) {
    std::string buffer;
    char chunk[4096];

    while (running_.load()) {
        ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            break;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));

        std::size_t nl = std::string::npos;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            std::string resp = process_line(line);
            if (!send_all(client_fd, resp)) {
                goto cleanup;
            }
        }
    }

cleanup:
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_fds_.erase(client_fd);
    }
    ::close(client_fd);
}

std::string Server::process_line(const std::string& line) {
    auto parsed = parse_command(line);
    if (!parsed.command) {
        return format_err(parsed.error);
    }

    const Command& cmd = *parsed.command;
    switch (cmd.type) {
        case CommandType::Ping:
            return format_pong();

        case CommandType::List: {
            auto names = queues_.list_queues();
            std::ostringstream oss;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i > 0) oss << ',';
                oss << names[i];
            }
            return format_ok(oss.str());
        }

        case CommandType::Push: {
            auto q = queues_.get_or_create(cmd.queue);
            std::uint64_t id = q->push(cmd.body);
            if (wal_) {
                wal_->log_push(cmd.queue, id, cmd.body);
            }
            if (replicator_) {
                replicator_->replicate_push(cmd.queue, id, cmd.body);
            }
            return format_ok(std::to_string(id));
        }

        case CommandType::Pop: {
            auto q = queues_.get_or_create(cmd.queue);
            auto msg = q->pop(std::chrono::milliseconds{cmd.timeout_ms},
                              config_.visibility_timeout);
            if (!msg) {
                return format_empty();
            }
            return format_msg(msg->id, msg->body);
        }

        case CommandType::Ack: {
            auto q = queues_.get(cmd.queue);
            if (!q || !q->ack(cmd.id)) {
                return format_err("message not in-flight");
            }
            if (wal_) {
                wal_->log_ack(cmd.queue, cmd.id);
            }
            if (replicator_) {
                replicator_->replicate_ack(cmd.queue, cmd.id);
            }
            return format_ok();
        }

        case CommandType::Nack: {
            auto q = queues_.get(cmd.queue);
            if (!q || !q->nack(cmd.id)) {
                return format_err("message not in-flight");
            }
            return format_ok();
        }

        case CommandType::Size: {
            auto q = queues_.get(cmd.queue);
            std::size_t sz = q ? q->size() : 0;
            return format_ok(std::to_string(sz));
        }

        case CommandType::Delete: {
            if (!queues_.delete_queue(cmd.queue)) {
                return format_err("queue not found");
            }
            if (wal_) {
                wal_->log_delete(cmd.queue);
            }
            if (replicator_) {
                replicator_->replicate_delete(cmd.queue);
            }
            return format_ok();
        }

        case CommandType::ReplPush: {
            auto q = queues_.get_or_create(cmd.queue);
            q->push_with_id(cmd.id, cmd.body);
            if (wal_) {
                wal_->log_push(cmd.queue, cmd.id, cmd.body);
            }
            return format_ok();
        }

        case CommandType::ReplAck: {
            auto q = queues_.get(cmd.queue);
            if (q) {
                q->remove_by_id(cmd.id);
            }
            if (wal_) {
                wal_->log_ack(cmd.queue, cmd.id);
            }
            return format_ok();
        }
    }

    return format_err("unhandled command");
}
