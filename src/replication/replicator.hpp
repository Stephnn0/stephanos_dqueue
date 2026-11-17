#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/client/client.hpp"

struct FollowerEndpoint {
    std::string host;
    int port;
};

class Replicator {
public:
    explicit Replicator(std::vector<FollowerEndpoint> followers);
    ~Replicator();

    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    void replicate_push(const std::string& queue,
                        std::uint64_t id,
                        const std::string& body);
    void replicate_ack(const std::string& queue, std::uint64_t id);
    void replicate_delete(const std::string& queue);

    // Blocks until all currently queued replication commands have been sent.
    void flush();
    void stop();

    std::size_t follower_count() const;

private:
    void worker_loop();
    void send_to_all(const std::string& wire_cmd);

    struct FollowerConn {
        FollowerEndpoint endpoint;
        std::unique_ptr<Client> client;
    };

    std::vector<FollowerConn> followers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable flush_cv_;
    std::deque<std::string> pending_;
    bool in_flight_ = false;
    bool stopping_ = false;
    std::thread worker_;
};
