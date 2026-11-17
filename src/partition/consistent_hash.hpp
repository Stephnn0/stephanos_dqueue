#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class ConsistentHashRing {
public:
    explicit ConsistentHashRing(std::size_t virtual_nodes = 100);

    void add_node(const std::string& node_id);
    void remove_node(const std::string& node_id);

    std::optional<std::string> get_node(const std::string& key) const;
    std::vector<std::string> get_nodes(const std::string& key,
                                       std::size_t count) const;

    std::size_t node_count() const;

    static std::uint64_t hash64(const std::string& data);

private:
    std::size_t virtual_nodes_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::string> ring_;
    std::unordered_set<std::string> nodes_;
};
