#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "src/queue_manager/queue_manager.hpp"
#include "src/replication/replicator.hpp"
#include "src/thread_pool/thread_pool.hpp"
#include "src/wal/wal.hpp"

struct ServerConfig {
    int port = 8080;
    std::size_t worker_threads = 4;
    std::chrono::milliseconds visibility_timeout{5000};
    std::string wal_path;
    std::vector<FollowerEndpoint> followers;
};

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts background accept/reaper threads. Non-blocking.
    bool start();

    // Starts (if not already started) and blocks until stop() is called.
    void run();

    // Stops accepting, closes active client sockets, and joins all threads.
    void stop();

    int port() const { return bound_port_.load(); }
    QueueManager& queues() { return queues_; }
    const std::string& last_error() const { return last_error_; }

private:
    void accept_loop();
    void reaper_loop();
    void handle_client(int client_fd);
    std::string process_line(const std::string& line);

    ServerConfig config_;
    QueueManager queues_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::unique_ptr<Replicator> replicator_;
    std::unique_ptr<ThreadPool> pool_;

    int listen_fd_ = -1;
    int wakeup_fds_[2] = {-1, -1};
    std::atomic<int> bound_port_{0};
    std::atomic<bool> running_{false};
    std::string last_error_;

    std::thread accept_thread_;
    std::thread reaper_thread_;

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    std::mutex clients_mutex_;
    std::unordered_set<int> client_fds_;
};
