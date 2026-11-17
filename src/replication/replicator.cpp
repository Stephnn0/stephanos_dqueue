#include "src/replication/replicator.hpp"

#include "src/protocol/protocol.hpp"

Replicator::Replicator(std::vector<FollowerEndpoint> followers) {
    followers_.reserve(followers.size());
    for (auto& ep : followers) {
        auto client = std::make_unique<Client>(ep.host, ep.port);
        followers_.push_back(FollowerConn{std::move(ep), std::move(client)});
    }
    if (!followers_.empty()) {
        worker_ = std::thread(&Replicator::worker_loop, this);
    }
}

Replicator::~Replicator() {
    stop();
}

void Replicator::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    flush_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Replicator::replicate_push(const std::string& queue,
                                std::uint64_t id,
                                const std::string& body) {
    if (followers_.empty()) return;
    Command cmd{CommandType::ReplPush, queue, body, id, 0};
    std::string wire = serialize_command(cmd);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        pending_.push_back(std::move(wire));
    }
    cv_.notify_one();
}

void Replicator::replicate_ack(const std::string& queue, std::uint64_t id) {
    if (followers_.empty()) return;
    Command cmd{CommandType::ReplAck, queue, "", id, 0};
    std::string wire = serialize_command(cmd);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        pending_.push_back(std::move(wire));
    }
    cv_.notify_one();
}

void Replicator::replicate_delete(const std::string& queue) {
    if (followers_.empty()) return;
    Command cmd{CommandType::Delete, queue, "", 0, 0};
    std::string wire = serialize_command(cmd);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        pending_.push_back(std::move(wire));
    }
    cv_.notify_one();
}

void Replicator::flush() {
    if (followers_.empty()) return;
    std::unique_lock<std::mutex> lock(mutex_);
    flush_cv_.wait(lock, [this] {
        return stopping_ || (pending_.empty() && !in_flight_);
    });
}

std::size_t Replicator::follower_count() const {
    return followers_.size();
}

void Replicator::worker_loop() {
    while (true) {
        std::string cmd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) {
                return;
            }
            cmd = std::move(pending_.front());
            pending_.pop_front();
            in_flight_ = true;
        }

        send_to_all(cmd);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_flight_ = false;
        }
        flush_cv_.notify_all();
    }
}

void Replicator::send_to_all(const std::string& wire_cmd) {
    for (auto& f : followers_) {
        auto resp = f.client->send_raw(wire_cmd);
        if (!resp) {
            // Retry once with a fresh connection in case the follower restarted.
            f.client->disconnect();
            f.client->send_raw(wire_cmd);
        }
    }
}
