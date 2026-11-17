#include "src/thread_pool/thread_pool.hpp"

#include <atomic>
#include <cassert>
#include <iostream>

int main() {
    std::atomic<int> sum{0};
    {
        ThreadPool pool(4);
        for (int i = 1; i <= 100; ++i) {
            assert(pool.enqueue([&sum, i] { sum.fetch_add(i); }));
        }
    }
    assert(sum.load() == 5050);

    std::cout << "All ThreadPool tests passed!\n";
    return 0;
}
