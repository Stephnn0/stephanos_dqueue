#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class CommandType {
    Push,
    Pop,
    Ack,
    Nack,
    Size,
    List,
    Delete,
    Ping,
    ReplPush,
    ReplAck,
};

struct Command {
    CommandType type;
    std::string queue;
    std::string body;
    std::uint64_t id = 0;
    int timeout_ms = 0;
};

struct ParseResult {
    std::optional<Command> command;
    std::string error;
};

std::string escape_text(const std::string& input);
std::string unescape_text(const std::string& input);

ParseResult parse_command(const std::string& line);
std::string serialize_command(const Command& cmd);

std::string format_ok(const std::string& value = "");
std::string format_msg(std::uint64_t id, const std::string& body);
std::string format_empty();
std::string format_pong();
std::string format_err(const std::string& reason);
