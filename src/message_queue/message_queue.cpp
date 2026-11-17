#include "src/message_queue/message_queue.hpp"

#include <algorithm>
#include <utility>

std::uint64_t MessageQueue::push(const std::string& body) {
    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;
        messages_.push_back(Message{id, body});
    }
    cv_.notify_one();
    return id;
}

void MessageQueue::push_with_id(std::uint64_t id, const std::string& body) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(Message{id, body});
        if (id >= next_id_) {
            next_id_ = id + 1;
        }
    }
    cv_.notify_one();
}

std::optional<Message> MessageQueue::pop(
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds visibility_timeout) {
    std::unique_lock<std::mutex> lock(mutex_);

    requeue_expired_locked(std::chrono::steady_clock::now());

    if (messages_.empty()) {
        if (timeout.count() <= 0) {
            return std::nullopt;
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (messages_.empty()) {
            requeue_expired_locked(std::chrono::steady_clock::now());
            if (!messages_.empty()) {
                break;
            }
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                requeue_expired_locked(std::chrono::steady_clock::now());
                break;
            }
        }
        if (messages_.empty()) {
            return std::nullopt;
        }
    }

    Message msg = std::move(messages_.front());
    messages_.pop_front();

    if (visibility_timeout.count() > 0) {
        in_flight_.emplace(
            msg.id,
            InFlight{msg, std::chrono::steady_clock::now() + visibility_timeout});
    }

    return msg;
}

bool MessageQueue::ack(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_.erase(id) > 0;
}

bool MessageQueue::nack(std::uint64_t id) {
    bool requeued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = in_flight_.find(id);
        if (it != in_flight_.end()) {
            messages_.push_front(std::move(it->second.msg));
            in_flight_.erase(it);
            requeued = true;
        }
    }
    if (requeued) {
        cv_.notify_one();
    }
    return requeued;
}

bool MessageQueue::remove_by_id(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (in_flight_.erase(id) > 0) {
        return true;
    }
    for (auto it = messages_.begin(); it != messages_.end(); ++it) {
        if (it->id == id) {
            messages_.erase(it);
            return true;
        }
    }
    return false;
}

std::size_t MessageQueue::requeue_expired_locked(
    std::chrono::steady_clock::time_point now) {
    std::size_t count = 0;
    for (auto it = in_flight_.begin(); it != in_flight_.end();) {
        if (now >= it->second.expires_at) {
            messages_.push_front(std::move(it->second.msg));
            it = in_flight_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    return count;
}

std::size_t MessageQueue::requeue_expired() {
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count = requeue_expired_locked(std::chrono::steady_clock::now());
    }
    if (count > 0) {
        cv_.notify_all();
    }
    return count;
}

std::size_t MessageQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_.size();
}

std::size_t MessageQueue::inflight_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_.size();
}

std::vector<Message> MessageQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Message> out;
    out.reserve(messages_.size() + in_flight_.size());
    for (const auto& [id, inflight] : in_flight_) {
        (void)id;
        out.push_back(inflight.msg);
    }
    for (const auto& msg : messages_) {
        out.push_back(msg);
    }
    std::sort(out.begin(), out.end(),
              [](const Message& a, const Message& b) { return a.id < b.id; });
    return out;
}
