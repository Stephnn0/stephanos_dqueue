#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/client/client.hpp"
#include "src/partition/consistent_hash.hpp"

struct BrokerEndpoint {
    std::string host;
    int port;

    std::string id() const { return host + ":" + std::to_string(port); }
};

class PartitionedClient {
public:
    explicit PartitionedClient(const std::vector<BrokerEndpoint>& brokers,
                               std::size_t virtual_nodes = 100);

    void add_broker(const BrokerEndpoint& broker);
    void remove_broker(const BrokerEndpoint& broker);

    std::optional<std::string> route_queue(const std::string& queue) const;

    std::optional<std::uint64_t> push(const std::string& queue,
                                      const std::string& body);
    std::optional<Message> pop(const std::string& queue, int timeout_ms = 0);
    bool ack(const std::string& queue, std::uint64_t id);
    bool nack(const std::string& queue, std::uint64_t id);
    std::optional<std::size_t> size(const std::string& queue);
    bool delete_queue(const std::string& queue);

private:
    struct BrokerEntry {
        BrokerEndpoint endpoint;
        std::mutex mutex;
        std::unique_ptr<Client> client;
    };

    std::shared_ptr<BrokerEntry> client_for_queue(const std::string& queue);

    ConsistentHashRing ring_;
    mutable std::mutex brokers_mutex_;
    std::unordered_map<std::string, std::shared_ptr<BrokerEntry>> brokers_;
};
