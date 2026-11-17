#include "src/queue_manager/queue_manager.hpp"

#include <cassert>
#include <iostream>

int main() {
    QueueManager mgr;
    assert(mgr.get("orders") == nullptr);
    assert(mgr.list_queues().empty());

    auto orders = mgr.get_or_create("orders");
    auto emails = mgr.get_or_create("emails");
    assert(orders != nullptr && emails != nullptr);
    assert(mgr.get_or_create("orders") == orders);

    orders->push("order-1");
    emails->push("email-1");
    assert(mgr.get("orders")->size() == 1);

    auto names = mgr.list_queues();
    assert(names.size() == 2);
    assert(names[0] == "emails" && names[1] == "orders");

    assert(mgr.delete_queue("emails"));
    assert(!mgr.delete_queue("emails"));
    assert(mgr.get("emails") == nullptr);
    assert(mgr.list_queues().size() == 1);

    std::cout << "All QueueManager tests passed!\n";
    return 0;
}
