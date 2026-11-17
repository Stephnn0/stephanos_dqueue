#include "src/wal/wal.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>

int main() {
    const char* tmp_env = std::getenv("TEST_TMPDIR");
    if (!tmp_env) tmp_env = std::getenv("TMPDIR");
    if (!tmp_env) tmp_env = "/tmp";
    std::string wal_path = std::string(tmp_env) + "/dqueue_wal_test.log";
    std::remove(wal_path.c_str());

    // 1. Write operations to WAL
    {
        QueueManager mgr;
        WriteAheadLog wal(wal_path);
        assert(wal.recover(mgr) == 0);

        auto orders = mgr.get_or_create("orders");
        auto id1 = orders->push("first\nline");
        assert(wal.log_push("orders", id1, "first\nline"));

        auto id2 = orders->push("second");
        assert(wal.log_push("orders", id2, "second"));

        auto emails = mgr.get_or_create("emails");
        auto eid = emails->push("hello");
        assert(wal.log_push("emails", eid, "hello"));

        // Ack message 1 in orders, delete emails
        assert(wal.log_ack("orders", id1));
        assert(wal.log_delete("emails"));
    }

    // 2. Recover in a fresh QueueManager ("restart after crash")
    {
        QueueManager mgr;
        WriteAheadLog wal(wal_path);
        std::size_t replayed = wal.recover(mgr);
        assert(replayed == 5);

        assert(mgr.get("emails") == nullptr);
        auto orders = mgr.get("orders");
        assert(orders != nullptr);
        assert(orders->size() == 1);

        auto m = orders->pop();
        assert(m.has_value() && m->id == 2 && m->body == "second");

        // Next push must get ID >= 3 (no ID collision)
        auto id3 = orders->push("third");
        assert(id3 == 3);
        assert(wal.log_push("orders", id3, "third"));

        // Compact WAL and verify clean reload
        assert(wal.compact(mgr));
    }

    {
        QueueManager mgr;
        WriteAheadLog wal(wal_path);
        assert(wal.recover(mgr) == 1);
        auto orders = mgr.get("orders");
        assert(orders != nullptr && orders->size() == 1);
        auto m = orders->pop();
        assert(m.has_value() && m->id == 3 && m->body == "third");
    }

    std::remove(wal_path.c_str());
    std::cout << "All WAL tests passed!\n";
    return 0;
}
