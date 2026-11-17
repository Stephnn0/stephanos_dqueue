#include "src/partition/consistent_hash.hpp"

#include <algorithm>

ConsistentHashRing::ConsistentHashRing(std::size_t virtual_nodes)
    : virtual_nodes_(std::max<std::size_t>(1, virtual_nodes)) {}

std::uint64_t ConsistentHashRing::hash64(const std::string& data) {
    // FNV-1a 64-bit hash
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    // Finalizer mix (SplitMix64) for better avalanche across vnodes
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

void ConsistentHashRing::add_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!nodes_.insert(node_id).second) {
        return;
    }
    for (std::size_t i = 0; i < virtual_nodes_; ++i) {
        std::uint64_t h = hash64(node_id + "#" + std::to_string(i));
        ring_[h] = node_id;
    }
}

void ConsistentHashRing::remove_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.erase(node_id) == 0) {
        return;
    }
    for (auto it = ring_.begin(); it != ring_.end();) {
        if (it->second == node_id) {
            it = ring_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<std::string> ConsistentHashRing::get_node(
    const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty()) {
        return std::nullopt;
    }
    std::uint64_t h = hash64(key);
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) {
        it = ring_.begin();
    }
    return it->second;
}

std::vector<std::string> ConsistentHashRing::get_nodes(
    const std::string& key, std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    if (ring_.empty() || count == 0) {
        return result;
    }
    std::size_t target = std::min(count, nodes_.size());
    result.reserve(target);

    std::unordered_set<std::string> seen;
    std::uint64_t h = hash64(key);
    auto it = ring_.lower_bound(h);

    for (std::size_t visited = 0;
         visited < ring_.size() && result.size() < target; ++visited) {
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        if (seen.insert(it->second).second) {
            result.push_back(it->second);
        }
        ++it;
    }
    return result;
}

std::size_t ConsistentHashRing::node_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.size();
}
