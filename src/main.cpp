#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/server/server.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested.store(true);
}

std::vector<FollowerEndpoint> parse_followers(const std::string& arg) {
    std::vector<FollowerEndpoint> out;
    std::stringstream ss(arg);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        auto colon = item.rfind(':');
        if (colon == std::string::npos) continue;
        std::string host = item.substr(0, colon);
        int port = std::atoi(item.substr(colon + 1).c_str());
        if (!host.empty() && port > 0) {
            out.push_back(FollowerEndpoint{host, port});
        }
    }
    return out;
}

void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "  --port <N>              Listen port (default: 8080)\n"
        << "  --threads <N>           Worker threads (default: 4)\n"
        << "  --visibility-ms <N>     Ack visibility timeout in ms (default: 5000)\n"
        << "  --wal <path>            Write-ahead log path (optional)\n"
        << "  --followers <h:p,...>   Comma-separated follower brokers (optional)\n";
}

}  // namespace

int main(int argc, char** argv) {
    ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = std::atoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            config.worker_threads = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--visibility-ms" && i + 1 < argc) {
            config.visibility_timeout =
                std::chrono::milliseconds{std::atoi(argv[++i])};
        } else if (arg == "--wal" && i + 1 < argc) {
            config.wal_path = argv[++i];
        } else if (arg == "--followers" && i + 1 < argc) {
            config.followers = parse_followers(argv[++i]);
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    Server server(config);
    if (!server.start()) {
        std::cerr << "Failed to start dqueue broker: " << server.last_error()
                  << "\n";
        return 1;
    }

    std::cout << "dqueue broker listening on port " << server.port()
              << " (threads=" << config.worker_threads
              << ", visibility_ms=" << config.visibility_timeout.count()
              << (config.wal_path.empty() ? "" : ", wal=" + config.wal_path)
              << ")\n";

    while (!g_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    std::cout << "\nShutting down dqueue broker...\n";
    server.stop();
    return 0;
}
