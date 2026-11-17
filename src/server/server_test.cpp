#include "src/server/server.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include "src/client/client.hpp"
#include "src/client/partitioned_client.hpp"

void test_basic_tcp_and_blocking_pop() {
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worker_threads = 4;
    cfg.visibility_timeout = std::chrono::milliseconds{5000};

    Server server(cfg);
    assert(server.start());
    int port = server.port();
    assert(port > 0);

    Client client("127.0.0.1", port);
    assert(client.ping());

    auto id = client.push("orders", "{\"id\":1}");
    assert(id.has_value() && *id == 1);
    assert(client.size("orders") == 1);

    auto queues = client.list();
    assert(queues.has_value() && queues->size() == 1 && (*queues)[0] == "orders");

    auto msg = client.pop("orders");
    assert(msg.has_value() && msg->id == 1 && msg->body == "{\"id\":1}");
    assert(client.ack("orders", msg->id));

    // Blocking pop woken up by another client
    std::thread producer([port] {
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        Client c("127.0.0.1", port);
        assert(c.push("orders", "delayed-job").has_value());
    });

    auto blocked = client.pop("orders", 1000);
    assert(blocked.has_value() && blocked->body == "delayed-job");
    assert(client.ack("orders", blocked->id));
    producer.join();

    assert(client.delete_queue("orders"));
    server.stop();
}

void test_visibility_timeout_redelivery() {
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worker_threads = 2;
    cfg.visibility_timeout = std::chrono::milliseconds{80};

    Server server(cfg);
    assert(server.start());

    Client c("127.0.0.1", server.port());
    auto id = c.push("jobs", "important");
    assert(id.has_value());

    // Consumer 1 pops and "crashes" without acking
    auto m1 = c.pop("jobs");
    assert(m1.has_value() && m1->id == *id);

    // Wait past visibility timeout + reaper tick
    std::this_thread::sleep_for(std::chrono::milliseconds{180});

    // Consumer 2 receives redelivered message
    auto m2 = c.pop("jobs", 500);
    assert(m2.has_value() && m2->id == *id && m2->body == "important");
    assert(c.ack("jobs", m2->id));
    assert(!c.pop("jobs").has_value());

    server.stop();
}

void test_wal_crash_recovery() {
    const char* tmp_env = std::getenv("TEST_TMPDIR");
    if (!tmp_env) tmp_env = std::getenv("TMPDIR");
    if (!tmp_env) tmp_env = "/tmp";
    std::string wal_path = std::string(tmp_env) + "/dqueue_server_wal_test.log";
    std::remove(wal_path.c_str());

    {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.wal_path = wal_path;
        Server server(cfg);
        assert(server.start());

        Client c("127.0.0.1", server.port());
        auto id1 = c.push("persist", "msg-1");
        auto id2 = c.push("persist", "msg-2");
        assert(id1 == 1 && id2 == 2);

        auto m1 = c.pop("persist");
        assert(m1.has_value() && m1->id == 1);
        assert(c.ack("persist", 1));
        server.stop();
    }

    // Restart broker from same WAL
    {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.wal_path = wal_path;
        Server server(cfg);
        assert(server.start());

        Client c("127.0.0.1", server.port());
        assert(c.size("persist") == 1);
        auto m2 = c.pop("persist");
        assert(m2.has_value() && m2->id == 2 && m2->body == "msg-2");
        assert(c.ack("persist", 2));

        // New message ID continues from 3
        auto id3 = c.push("persist", "msg-3");
        assert(id3 == 3);
        server.stop();
    }

    std::remove(wal_path.c_str());
}

void test_leader_follower_replication() {
    ServerConfig follower_cfg;
    follower_cfg.port = 0;
    follower_cfg.worker_threads = 2;
    Server follower(follower_cfg);
    assert(follower.start());

    ServerConfig leader_cfg;
    leader_cfg.port = 0;
    leader_cfg.worker_threads = 2;
    leader_cfg.followers = {{"127.0.0.1", follower.port()}};
    Server leader(leader_cfg);
    assert(leader.start());

    Client leader_client("127.0.0.1", leader.port());
    auto id1 = leader_client.push("events", "event-1");
    auto id2 = leader_client.push("events", "event-2");
    assert(id1 == 1 && id2 == 2);

    auto m1 = leader_client.pop("events");
    assert(m1.has_value() && m1->id == 1);
    assert(leader_client.ack("events", 1));

    // Wait briefly for async replication to catch up on follower
    Client follower_client("127.0.0.1", follower.port());
    for (int i = 0; i < 50; ++i) {
        auto sz = follower_client.size("events");
        if (sz.has_value() && *sz == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    auto fsz = follower_client.size("events");
    assert(fsz.has_value() && *fsz == 1);
    auto fm = follower_client.pop("events");
    assert(fm.has_value() && fm->id == 2 && fm->body == "event-2");

    leader.stop();
    follower.stop();
}

void test_partitioned_client_across_brokers() {
    Server b1(ServerConfig{0, 2, std::chrono::milliseconds{5000}, "", {}});
    Server b2(ServerConfig{0, 2, std::chrono::milliseconds{5000}, "", {}});
    Server b3(ServerConfig{0, 2, std::chrono::milliseconds{5000}, "", {}});
    assert(b1.start() && b2.start() && b3.start());

    PartitionedClient pc({
        {"127.0.0.1", b1.port()},
        {"127.0.0.1", b2.port()},
        {"127.0.0.1", b3.port()},
    });

    for (int i = 0; i < 15; ++i) {
        std::string q = "q_" + std::to_string(i);
        auto id = pc.push(q, "payload-" + std::to_string(i));
        assert(id.has_value() && *id == 1);
    }

    for (int i = 0; i < 15; ++i) {
        std::string q = "q_" + std::to_string(i);
        auto m = pc.pop(q);
        assert(m.has_value() && m->body == "payload-" + std::to_string(i));
        assert(pc.ack(q, m->id));
    }

    b1.stop();
    b2.stop();
    b3.stop();
}

int main() {
    test_basic_tcp_and_blocking_pop();
    test_visibility_timeout_redelivery();
    test_wal_crash_recovery();
    test_leader_follower_replication();
    test_partitioned_client_across_brokers();
    std::cout << "All Server & Integration tests passed!\n";
    return 0;
}
