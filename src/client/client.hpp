#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/message_queue/message_queue.hpp"

class Client {
public:
    Client(std::string host, int port);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;

    bool connect();
    void disconnect();
    bool is_connected() const { return fd_ >= 0; }

    std::optional<std::uint64_t> push(const std::string& queue,
                                      const std::string& body);
    std::optional<Message> pop(const std::string& queue, int timeout_ms = 0);
    bool ack(const std::string& queue, std::uint64_t id);
    bool nack(const std::string& queue, std::uint64_t id);
    std::optional<std::size_t> size(const std::string& queue);
    std::optional<std::vector<std::string>> list();
    bool delete_queue(const std::string& queue);
    bool ping();

    std::optional<std::string> send_raw(const std::string& line);
    const std::string& last_error() const { return last_error_; }

    const std::string& host() const { return host_; }
    int port() const { return port_; }

private:
    bool ensure_connected();
    std::optional<std::string> read_line();
    bool write_all(const std::string& data);

    std::string host_;
    int port_;
    int fd_ = -1;
    std::string read_buffer_;
    std::string last_error_;
};
