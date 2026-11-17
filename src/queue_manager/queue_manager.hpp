#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/message_queue/message_queue.hpp"

class QueueManager {
public:
    std::shared_ptr<MessageQueue> get_or_create(const std::string& name);
    std::shared_ptr<MessageQueue> get(const std::string& name) const;
    std::vector<std::string> list_queues() const;
    bool delete_queue(const std::string& name);
    std::size_t requeue_all_expired();

    std::vector<std::pair<std::string, std::vector<Message>>> snapshot_all() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<MessageQueue>> queues_;
};
