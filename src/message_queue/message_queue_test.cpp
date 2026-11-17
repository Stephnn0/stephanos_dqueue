#include "src/message_queue/message_queue.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

void test_basic_push_pop() {
    MessageQueue q;
    assert(q.size() == 0);
    assert(!q.pop().has_value());

    auto id1 = q.push("first");
    auto id2 = q.push("second");
    assert(id1 == 1);
    assert(id2 == 2);
    assert(q.size() == 2);

    auto m1 = q.pop();
    assert(m1.has_value() && m1->id == 1 && m1->body == "first");

    auto m2 = q.pop();
    assert(m2.has_value() && m2->id == 2 && m2->body == "second");

    assert(q.size() == 0);
    assert(!q.pop().has_value());
}

void test_pop_timeout() {
    MessageQueue q;
    auto msg = q.pop(std::chrono::milliseconds{20});
    assert(!msg.has_value());
}

void test_ack_and_nack() {
    MessageQueue q;
    auto id = q.push("task-1");
    auto m = q.pop(std::chrono::milliseconds{0}, std::chrono::milliseconds{5000});
    assert(m.has_value() && m->id == id);
    assert(q.size() == 0);
    assert(q.inflight_size() == 1);

    // NACK puts it back into ready queue immediately
    assert(q.nack(id));
    assert(q.size() == 1);
    assert(q.inflight_size() == 0);

    // Pop again and ACK
    auto m2 = q.pop(std::chrono::milliseconds{0}, std::chrono::milliseconds{5000});
    assert(m2.has_value() && m2->id == id);
    assert(q.ack(id));
    assert(!q.ack(id));
    assert(q.size() == 0);
    assert(q.inflight_size() == 0);
}

void test_visibility_timeout_redelivery() {
    MessageQueue q;
    auto id = q.push("lease-job");

    auto m1 = q.pop(std::chrono::milliseconds{0}, std::chrono::milliseconds{30});
    assert(m1.has_value() && m1->id == id);
    assert(q.inflight_size() == 1);

    // Before expiry, nothing available
    assert(!q.pop().has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds{45});
    assert(q.requeue_expired() == 1);
    assert(q.size() == 1);
    assert(q.inflight_size() == 0);

    auto m2 = q.pop(std::chrono::milliseconds{0}, std::chrono::milliseconds{500});
    assert(m2.has_value() && m2->id == id && m2->body == "lease-job");
    assert(q.ack(id));
}

void test_push_with_id_and_snapshot() {
    MessageQueue q;
    q.push_with_id(10, "ten");
    q.push_with_id(5, "five");
    auto next = q.push("eleven");
    assert(next == 11);

    auto snap = q.snapshot();
    assert(snap.size() == 3);
    assert(snap[0].id == 5 && snap[1].id == 10 && snap[2].id == 11);
}

void test_multithreaded_push_pop() {
    MessageQueue q;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 100;
    constexpr int kTotal = kProducers * kPerProducer;

    std::atomic<int> consumed{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < 2; ++i) {
        consumers.emplace_back([&] {
            while (consumed.load() < kTotal) {
                if (q.pop(std::chrono::milliseconds{50}).has_value()) {
                    consumed.fetch_add(1);
                }
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.push("job");
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    assert(consumed.load() == kTotal);
    assert(q.size() == 0);
}

int main() {
    test_basic_push_pop();
    test_pop_timeout();
    test_ack_and_nack();
    test_visibility_timeout_redelivery();
    test_push_with_id_and_snapshot();
    test_multithreaded_push_pop();
    std::cout << "All MessageQueue tests passed!\n";
    return 0;
}
