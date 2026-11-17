#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

struct Message {
    std::uint64_t id = 0;
    std::string body;
};

class MessageQueue {
public:
    void push(const std::string& body);
    std::optional<Message> pop();
    std::size_t size() const;

private:
    std::deque<Message> messages_;
    std::uint64_t next_id_ = 1;
};