#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "src/client/client.hpp"
#include "src/client/partitioned_client.hpp"

namespace {

std::vector<BrokerEndpoint> parse_brokers(const std::string& s) {
    std::vector<BrokerEndpoint> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        auto colon = item.rfind(':');
        if (colon == std::string::npos) continue;
        std::string host = item.substr(0, colon);
        int port = std::atoi(item.substr(colon + 1).c_str());
        if (!host.empty() && port > 0) {
            out.push_back(BrokerEndpoint{host, port});
        }
    }
    return out;
}

void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options] <command> [args...]\n\n"
        << "Options:\n"
        << "  --host <host>           Broker host (default: 127.0.0.1)\n"
        << "  --port <port>           Broker port (default: 8080)\n"
        << "  --brokers <h:p,...>     Cluster endpoints (consistent hashing)\n\n"
        << "Commands:\n"
        << "  ping\n"
        << "  list\n"
        << "  push <queue> <body>\n"
        << "  pop <queue> [timeout_ms]\n"
        << "  ack <queue> <id>\n"
        << "  nack <queue> <id>\n"
        << "  size <queue>\n"
        << "  delete <queue>\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string brokers_arg;

    int idx = 1;
    while (idx < argc) {
        std::string arg = argv[idx];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--host" && idx + 1 < argc) {
            host = argv[++idx];
            ++idx;
        } else if (arg == "--port" && idx + 1 < argc) {
            port = std::atoi(argv[++idx]);
            ++idx;
        } else if (arg == "--brokers" && idx + 1 < argc) {
            brokers_arg = argv[++idx];
            ++idx;
        } else {
            break;
        }
    }

    if (idx >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[idx++];

    if (!brokers_arg.empty()) {
        auto endpoints = parse_brokers(brokers_arg);
        if (endpoints.empty()) {
            std::cerr << "Invalid --brokers list\n";
            return 1;
        }
        PartitionedClient pc(endpoints);
        if (cmd == "push" && idx + 1 < argc) {
            std::string q = argv[idx++];
            std::string body = argv[idx++];
            auto id = pc.push(q, body);
            if (!id) {
                std::cerr << "ERR push failed\n";
                return 1;
            }
            std::cout << "OK " << *id << "\n";
            return 0;
        } else if (cmd == "pop" && idx < argc) {
            std::string q = argv[idx++];
            int timeout_ms = (idx < argc) ? std::atoi(argv[idx++]) : 0;
            auto m = pc.pop(q, timeout_ms);
            if (!m) {
                std::cout << "EMPTY\n";
                return 0;
            }
            std::cout << "MSG " << m->id << " " << m->body << "\n";
            return 0;
        } else if (cmd == "ack" && idx + 1 < argc) {
            std::string q = argv[idx++];
            std::uint64_t id = std::strtoull(argv[idx++], nullptr, 10);
            std::cout << (pc.ack(q, id) ? "OK\n" : "ERR ack failed\n");
            return 0;
        } else if (cmd == "nack" && idx + 1 < argc) {
            std::string q = argv[idx++];
            std::uint64_t id = std::strtoull(argv[idx++], nullptr, 10);
            std::cout << (pc.nack(q, id) ? "OK\n" : "ERR nack failed\n");
            return 0;
        } else if (cmd == "size" && idx < argc) {
            std::string q = argv[idx++];
            auto sz = pc.size(q);
            if (!sz) {
                std::cerr << "ERR size failed\n";
                return 1;
            }
            std::cout << *sz << "\n";
            return 0;
        } else if (cmd == "delete" && idx < argc) {
            std::string q = argv[idx++];
            std::cout << (pc.delete_queue(q) ? "OK\n" : "ERR delete failed\n");
            return 0;
        }
        std::cerr << "Unsupported partitioned command: " << cmd << "\n";
        return 1;
    }

    Client client(host, port);
    if (!client.connect()) {
        std::cerr << "Connection failed: " << client.last_error() << "\n";
        return 1;
    }

    if (cmd == "ping") {
        if (!client.ping()) {
            std::cerr << "ERR " << client.last_error() << "\n";
            return 1;
        }
        std::cout << "PONG\n";
        return 0;
    } else if (cmd == "list") {
        auto names = client.list();
        if (!names) {
            std::cerr << "ERR " << client.last_error() << "\n";
            return 1;
        }
        for (const auto& n : *names) {
            std::cout << n << "\n";
        }
        return 0;
    } else if (cmd == "push" && idx + 1 < argc) {
        std::string q = argv[idx++];
        std::string body = argv[idx++];
        auto id = client.push(q, body);
        if (!id) {
            std::cerr << "ERR " << client.last_error() << "\n";
            return 1;
        }
        std::cout << "OK " << *id << "\n";
        return 0;
    } else if (cmd == "pop" && idx < argc) {
        std::string q = argv[idx++];
        int timeout_ms = (idx < argc) ? std::atoi(argv[idx++]) : 0;
        auto m = client.pop(q, timeout_ms);
        if (!m) {
            if (!client.last_error().empty()) {
                std::cerr << "ERR " << client.last_error() << "\n";
                return 1;
            }
            std::cout << "EMPTY\n";
            return 0;
        }
        std::cout << "MSG " << m->id << " " << m->body << "\n";
        return 0;
    } else if (cmd == "ack" && idx + 1 < argc) {
        std::string q = argv[idx++];
        std::uint64_t id = std::strtoull(argv[idx++], nullptr, 10);
        if (!client.ack(q, id)) {
            std::cerr << "ERR " << client.last_error() << "\n";
            return 1;
        }
        std::cout << "OK\n";
        return 0;
    } else if (cmd == "nack" && idx + 1 < argc) {
        std::string q = argv[idx++];
        std::uint64_t id = std::strtoull(argv[idx++], nullptr, 10);
        if (!client.nack(q, id)) {
            std::cerr << "ERR " << client.last_error() << "\n";
            return 1;
        }
        std::cout << "OK\n";
        return 0;
    } else if (cmd == "size" && idx < argc) {
        std::string q = argv[idx++];
        auto sz = client.size(q);
        if (!sz) {
            std::cerr << "ERR " << client.last_error() << "\n";
            return 1;
        }
        std::cout << *sz << "\n";
        return 0;
    } else if (cmd == "delete" && idx < argc) {
        std::string q = argv[idx++];
        if (!client.delete_queue(q)) {
            std::cerr << "ERR " << client.last_error() << "\n";
            return 1;
        }
        std::cout << "OK\n";
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
