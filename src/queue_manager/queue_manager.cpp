#include "src/queue_manager/queue_manager.hpp"

#include <algorithm>

std::shared_ptr<MessageQueue> QueueManager::get_or_create(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& queue = queues_[name];
    if (!queue) {
        queue = std::make_shared<MessageQueue>();
    }
    return queue;
}

std::shared_ptr<MessageQueue> QueueManager::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(name);
    if (it == queues_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::string> QueueManager::list_queues() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(queues_.size());
    for (const auto& [name, q] : queues_) {
        (void)q;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool QueueManager::delete_queue(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return queues_.erase(name) > 0;
}

std::size_t QueueManager::requeue_all_expired() {
    std::vector<std::shared_ptr<MessageQueue>> snapshot_queues;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_queues.reserve(queues_.size());
        for (const auto& [name, q] : queues_) {
            (void)name;
            snapshot_queues.push_back(q);
        }
    }
    std::size_t total = 0;
    for (const auto& q : snapshot_queues) {
        total += q->requeue_expired();
    }
    return total;
}

std::vector<std::pair<std::string, std::vector<Message>>>
QueueManager::snapshot_all() const {
    std::vector<std::pair<std::string, std::shared_ptr<MessageQueue>>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.reserve(queues_.size());
        for (const auto& [name, q] : queues_) {
            entries.emplace_back(name, q);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<std::string, std::vector<Message>>> out;
    out.reserve(entries.size());
    for (const auto& [name, q] : entries) {
        out.emplace_back(name, q->snapshot());
    }
    return out;
}
