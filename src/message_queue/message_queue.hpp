#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct Message {
    std::uint64_t id = 0;
    std::string body;
};

class MessageQueue {
public:
    std::uint64_t push(const std::string& body);
    void push_with_id(std::uint64_t id, const std::string& body);

    // If visibility_timeout > 0ms, the message stays in-flight until ack(),
    // nack(), or visibility_timeout expires (at-least-once delivery).
    std::optional<Message> pop(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
        std::chrono::milliseconds visibility_timeout = std::chrono::milliseconds{0});

    bool ack(std::uint64_t id);
    bool nack(std::uint64_t id);
    bool remove_by_id(std::uint64_t id);
    std::size_t requeue_expired();

    std::size_t size() const;
    std::size_t inflight_size() const;
    std::vector<Message> snapshot() const;

private:
    struct InFlight {
        Message msg;
        std::chrono::steady_clock::time_point expires_at;
    };

    std::size_t requeue_expired_locked(std::chrono::steady_clock::time_point now);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Message> messages_;
    std::unordered_map<std::uint64_t, InFlight> in_flight_;
    std::uint64_t next_id_ = 1;
};
