#include "src/partition/consistent_hash.hpp"

#include <cassert>
#include <iostream>
#include <unordered_map>

int main() {
    ConsistentHashRing ring(100);
    assert(!ring.get_node("orders").has_value());
    assert(ring.node_count() == 0);

    ring.add_node("127.0.0.1:9001");
    ring.add_node("127.0.0.1:9002");
    ring.add_node("127.0.0.1:9003");
    assert(ring.node_count() == 3);

    // Deterministic lookup
    auto n1 = ring.get_node("orders");
    auto n2 = ring.get_node("orders");
    assert(n1.has_value() && n1 == n2);

    // Distribution across nodes
    std::unordered_map<std::string, int> counts;
    for (int i = 0; i < 300; ++i) {
        auto node = ring.get_node("queue_" + std::to_string(i));
        assert(node.has_value());
        counts[*node]++;
    }
    assert(counts.size() == 3);
    for (const auto& [node, cnt] : counts) {
        (void)node;
        assert(cnt > 40);
    }

    // Replica placement returns distinct nodes
    auto replicas = ring.get_nodes("orders", 3);
    assert(replicas.size() == 3);
    assert(replicas[0] == *n1);
    assert(replicas[0] != replicas[1] && replicas[1] != replicas[2] &&
           replicas[0] != replicas[2]);

    // Removing a node keeps other keys mostly stable and never returns removed node
    ring.remove_node("127.0.0.1:9002");
    assert(ring.node_count() == 2);
    for (int i = 0; i < 100; ++i) {
        auto node = ring.get_node("queue_" + std::to_string(i));
        assert(node.has_value() && *node != "127.0.0.1:9002");
    }

    std::cout << "All ConsistentHashRing tests passed!\n";
    return 0;
}
