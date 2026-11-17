#include "message_queue.hpp"

#include <utility>

void MessageQueue::push(const std::string& body) {
    Message msg;
    msg.id = next_id_++;
    msg.body = body;
    messages_.push_back(msg);
}

std::optional<Message> MessageQueue::pop() {
    if (messages_.empty()) {
        return std::nullopt;
    }
    Message msg = std::move(messages_.front());
    messages_.pop_front();
    return msg;
}

std::size_t MessageQueue::size() const {
    return messages_.size();
}