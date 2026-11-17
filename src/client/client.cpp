#include "src/client/client.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>

#include "src/protocol/protocol.hpp"

namespace {

void ignore_sigpipe_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { std::signal(SIGPIPE, SIG_IGN); });
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

}  // namespace

Client::Client(std::string host, int port)
    : host_(std::move(host)), port_(port) {
    ignore_sigpipe_once();
}

Client::~Client() {
    disconnect();
}

Client::Client(Client&& other) noexcept
    : host_(std::move(other.host_)),
      port_(other.port_),
      fd_(other.fd_),
      read_buffer_(std::move(other.read_buffer_)),
      last_error_(std::move(other.last_error_)) {
    other.fd_ = -1;
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        disconnect();
        host_ = std::move(other.host_);
        port_ = other.port_;
        fd_ = other.fd_;
        read_buffer_ = std::move(other.read_buffer_);
        last_error_ = std::move(other.last_error_);
        other.fd_ = -1;
    }
    return *this;
}

bool Client::connect() {
    disconnect();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port_);
    int rc = ::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        last_error_ = gai_strerror(rc);
        return false;
    }

    int sock = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;

#ifdef SO_NOSIGPIPE
        int on = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

        if (::connect(sock, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        ::close(sock);
        sock = -1;
    }

    ::freeaddrinfo(res);

    if (sock < 0) {
        last_error_ = std::strerror(errno);
        return false;
    }

    fd_ = sock;
    read_buffer_.clear();
    last_error_.clear();
    return true;
}

void Client::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    read_buffer_.clear();
}

bool Client::ensure_connected() {
    if (fd_ >= 0) {
        return true;
    }
    return connect();
}

bool Client::write_all(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            last_error_ = std::strerror(errno);
            disconnect();
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::optional<std::string> Client::read_line() {
    while (true) {
        auto nl = read_buffer_.find('\n');
        if (nl != std::string::npos) {
            std::string line = read_buffer_.substr(0, nl);
            read_buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }

        char buf[4096];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            last_error_ = std::strerror(errno);
            disconnect();
            return std::nullopt;
        }
        if (n == 0) {
            last_error_ = "connection closed by server";
            disconnect();
            return std::nullopt;
        }
        read_buffer_.append(buf, static_cast<std::size_t>(n));
    }
}

std::optional<std::string> Client::send_raw(const std::string& line) {
    if (!ensure_connected()) {
        return std::nullopt;
    }
    std::string wire = line;
    if (wire.empty() || wire.back() != '\n') {
        wire.push_back('\n');
    }
    if (!write_all(wire)) {
        return std::nullopt;
    }
    return read_line();
}

std::optional<std::uint64_t> Client::push(const std::string& queue,
                                          const std::string& body) {
    Command cmd{CommandType::Push, queue, body, 0, 0};
    auto resp = send_raw(serialize_command(cmd));
    if (!resp) return std::nullopt;

    if (resp->rfind("OK ", 0) == 0) {
        std::uint64_t id = 0;
        if (parse_u64(resp->substr(3), id)) {
            return id;
        }
    }
    last_error_ = *resp;
    return std::nullopt;
}

std::optional<Message> Client::pop(const std::string& queue, int timeout_ms) {
    Command cmd{CommandType::Pop, queue, "", 0, timeout_ms};
    auto resp = send_raw(serialize_command(cmd));
    if (!resp) return std::nullopt;

    if (*resp == "EMPTY") {
        last_error_.clear();
        return std::nullopt;
    }
    if (resp->rfind("MSG ", 0) == 0) {
        std::string rest = resp->substr(4);
        auto sp = rest.find(' ');
        if (sp == std::string::npos) {
            last_error_ = "malformed MSG response";
            return std::nullopt;
        }
        std::uint64_t id = 0;
        if (!parse_u64(rest.substr(0, sp), id)) {
            last_error_ = "malformed message id";
            return std::nullopt;
        }
        return Message{id, unescape_text(rest.substr(sp + 1))};
    }

    last_error_ = *resp;
    return std::nullopt;
}

bool Client::ack(const std::string& queue, std::uint64_t id) {
    Command cmd{CommandType::Ack, queue, "", id, 0};
    auto resp = send_raw(serialize_command(cmd));
    if (resp && *resp == "OK") return true;
    if (resp) last_error_ = *resp;
    return false;
}

bool Client::nack(const std::string& queue, std::uint64_t id) {
    Command cmd{CommandType::Nack, queue, "", id, 0};
    auto resp = send_raw(serialize_command(cmd));
    if (resp && *resp == "OK") return true;
    if (resp) last_error_ = *resp;
    return false;
}

std::optional<std::size_t> Client::size(const std::string& queue) {
    Command cmd{CommandType::Size, queue, "", 0, 0};
    auto resp = send_raw(serialize_command(cmd));
    if (!resp) return std::nullopt;

    if (resp->rfind("OK ", 0) == 0) {
        std::uint64_t count = 0;
        if (parse_u64(resp->substr(3), count)) {
            return static_cast<std::size_t>(count);
        }
    }
    last_error_ = *resp;
    return std::nullopt;
}

std::optional<std::vector<std::string>> Client::list() {
    auto resp = send_raw("LIST");
    if (!resp) return std::nullopt;

    if (*resp == "OK") {
        return std::vector<std::string>{};
    }
    if (resp->rfind("OK ", 0) == 0) {
        std::vector<std::string> names;
        std::stringstream ss(resp->substr(3));
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                names.push_back(item);
            }
        }
        return names;
    }
    last_error_ = *resp;
    return std::nullopt;
}

bool Client::delete_queue(const std::string& queue) {
    Command cmd{CommandType::Delete, queue, "", 0, 0};
    auto resp = send_raw(serialize_command(cmd));
    if (resp && *resp == "OK") return true;
    if (resp) last_error_ = *resp;
    return false;
}

bool Client::ping() {
    auto resp = send_raw("PING");
    if (resp && *resp == "PONG") return true;
    if (resp) last_error_ = *resp;
    return false;
}
