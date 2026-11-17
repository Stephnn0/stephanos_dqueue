#include "src/client/partitioned_client.hpp"

PartitionedClient::PartitionedClient(const std::vector<BrokerEndpoint>& brokers,
                                     std::size_t virtual_nodes)
    : ring_(virtual_nodes) {
    for (const auto& b : brokers) {
        add_broker(b);
    }
}

void PartitionedClient::add_broker(const BrokerEndpoint& broker) {
    std::string key = broker.id();
    {
        std::lock_guard<std::mutex> lock(brokers_mutex_);
        if (brokers_.find(key) != brokers_.end()) {
            return;
        }
        auto entry = std::make_shared<BrokerEntry>();
        entry->endpoint = broker;
        entry->client = std::make_unique<Client>(broker.host, broker.port);
        brokers_.emplace(key, std::move(entry));
    }
    ring_.add_node(key);
}

void PartitionedClient::remove_broker(const BrokerEndpoint& broker) {
    std::string key = broker.id();
    ring_.remove_node(key);
    std::lock_guard<std::mutex> lock(brokers_mutex_);
    brokers_.erase(key);
}

std::optional<std::string> PartitionedClient::route_queue(
    const std::string& queue) const {
    return ring_.get_node(queue);
}

std::shared_ptr<PartitionedClient::BrokerEntry>
PartitionedClient::client_for_queue(const std::string& queue) {
    auto node = ring_.get_node(queue);
    if (!node) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(brokers_mutex_);
    auto it = brokers_.find(*node);
    if (it == brokers_.end()) {
        return nullptr;
    }
    return it->second;
}

std::optional<std::uint64_t> PartitionedClient::push(const std::string& queue,
                                                     const std::string& body) {
    auto entry = client_for_queue(queue);
    if (!entry) return std::nullopt;
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->client->push(queue, body);
}

std::optional<Message> PartitionedClient::pop(const std::string& queue,
                                              int timeout_ms) {
    auto entry = client_for_queue(queue);
    if (!entry) return std::nullopt;
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->client->pop(queue, timeout_ms);
}

bool PartitionedClient::ack(const std::string& queue, std::uint64_t id) {
    auto entry = client_for_queue(queue);
    if (!entry) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->client->ack(queue, id);
}

bool PartitionedClient::nack(const std::string& queue, std::uint64_t id) {
    auto entry = client_for_queue(queue);
    if (!entry) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->client->nack(queue, id);
}

std::optional<std::size_t> PartitionedClient::size(const std::string& queue) {
    auto entry = client_for_queue(queue);
    if (!entry) return std::nullopt;
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->client->size(queue);
}

bool PartitionedClient::delete_queue(const std::string& queue) {
    auto entry = client_for_queue(queue);
    if (!entry) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->client->delete_queue(queue);
}
