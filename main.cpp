#include <iostream>

#include "message_queue.hpp"

int main() {
    MessageQueue queue;
    queue.push("hello");
    queue.push("world");

    std::cout << "queue size: " << queue.size() << "\n";

    while (auto msg = queue.pop()) {
        std::cout << "popped id=" << msg->id << " body=" << msg->body << "\n";
    }
    return 0;
}