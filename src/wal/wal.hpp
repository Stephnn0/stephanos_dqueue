#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "src/queue_manager/queue_manager.hpp"

class WriteAheadLog {
public:
    explicit WriteAheadLog(std::string path);
    ~WriteAheadLog();

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // Replays existing WAL records into `mgr` and opens the log for appending.
    std::size_t recover(QueueManager& mgr);

    bool log_push(const std::string& queue, std::uint64_t id, const std::string& body);
    bool log_ack(const std::string& queue, std::uint64_t id);
    bool log_delete(const std::string& queue);

    // Rewrites the WAL from a snapshot of live (un-acked) messages.
    bool compact(const QueueManager& mgr);

    const std::string& path() const { return path_; }

private:
    bool open_append_locked();
    bool append_line_locked(const std::string& line);

    std::string path_;
    std::mutex mutex_;
    int fd_ = -1;
};
