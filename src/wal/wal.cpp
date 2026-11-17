#include "src/wal/wal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include "src/protocol/protocol.hpp"

namespace {

bool write_all(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool parse_u64_token(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

}  // namespace

WriteAheadLog::WriteAheadLog(std::string path) : path_(std::move(path)) {}

WriteAheadLog::~WriteAheadLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool WriteAheadLog::open_append_locked() {
    if (fd_ >= 0) {
        return true;
    }
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    return fd_ >= 0;
}

bool WriteAheadLog::append_line_locked(const std::string& line) {
    if (!open_append_locked()) {
        return false;
    }
    if (!write_all(fd_, line.data(), line.size())) {
        return false;
    }
    ::fsync(fd_);
    return true;
}

std::size_t WriteAheadLog::recover(QueueManager& mgr) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t replayed = 0;

    std::ifstream in(path_);
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string op;
            iss >> op;

            if (op == "PUSH") {
                std::string queue;
                std::string id_str;
                if (!(iss >> queue >> id_str)) continue;
                std::uint64_t id = 0;
                if (!parse_u64_token(id_str, id)) continue;

                std::string rest;
                std::getline(iss, rest);
                if (!rest.empty() && rest[0] == ' ') {
                    rest.erase(0, 1);
                }
                mgr.get_or_create(queue)->push_with_id(id, unescape_text(rest));
                ++replayed;
            } else if (op == "ACK") {
                std::string queue;
                std::string id_str;
                if (!(iss >> queue >> id_str)) continue;
                std::uint64_t id = 0;
                if (!parse_u64_token(id_str, id)) continue;
                if (auto q = mgr.get(queue)) {
                    q->remove_by_id(id);
                }
                ++replayed;
            } else if (op == "DELETE") {
                std::string queue;
                if (!(iss >> queue)) continue;
                mgr.delete_queue(queue);
                ++replayed;
            }
        }
    }

    open_append_locked();
    return replayed;
}

bool WriteAheadLog::log_push(const std::string& queue,
                             std::uint64_t id,
                             const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string line =
        "PUSH " + queue + " " + std::to_string(id) + " " + escape_text(body) + "\n";
    return append_line_locked(line);
}

bool WriteAheadLog::log_ack(const std::string& queue, std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string line = "ACK " + queue + " " + std::to_string(id) + "\n";
    return append_line_locked(line);
}

bool WriteAheadLog::log_delete(const std::string& queue) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string line = "DELETE " + queue + "\n";
    return append_line_locked(line);
}

bool WriteAheadLog::compact(const QueueManager& mgr) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string tmp_path = path_ + ".tmp";
    int tmp_fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
        return false;
    }

    auto all = mgr.snapshot_all();
    bool ok = true;
    for (const auto& [queue_name, msgs] : all) {
        for (const auto& m : msgs) {
            std::string line = "PUSH " + queue_name + " " + std::to_string(m.id) +
                               " " + escape_text(m.body) + "\n";
            if (!write_all(tmp_fd, line.data(), line.size())) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
    }

    if (ok) {
        ::fsync(tmp_fd);
    }
    ::close(tmp_fd);

    if (!ok) {
        std::remove(tmp_path.c_str());
        return false;
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    if (std::rename(tmp_path.c_str(), path_.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        open_append_locked();
        return false;
    }

    return open_append_locked();
}
