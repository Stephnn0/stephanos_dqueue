#include "src/protocol/protocol.hpp"

#include <charconv>
#include <sstream>

namespace {

std::string trim_cr(const std::string& s) {
    if (!s.empty() && s.back() == '\r') {
        return s.substr(0, s.size() - 1);
    }
    return s;
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

// Reads the next whitespace-delimited token starting from pos, advancing pos past it.
std::string next_token(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
        ++pos;
    }
    std::size_t start = pos;
    while (pos < s.size() && s[pos] != ' ' && s[pos] != '\t') {
        ++pos;
    }
    return s.substr(start, pos - start);
}

std::string rest_trimmed_left(const std::string& s, std::size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
        ++pos;
    }
    return s.substr(pos);
}

}  // namespace

std::string escape_text(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out;
}

std::string unescape_text(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[i + 1];
            if (next == '\\') {
                out += '\\';
                ++i;
                continue;
            } else if (next == 'n') {
                out += '\n';
                ++i;
                continue;
            } else if (next == 'r') {
                out += '\r';
                ++i;
                continue;
            }
        }
        out += input[i];
    }
    return out;
}

ParseResult parse_command(const std::string& raw_line) {
    std::string line = trim_cr(raw_line);
    std::size_t pos = 0;
    std::string op = next_token(line, pos);
    if (op.empty()) {
        return {std::nullopt, "empty command"};
    }

    if (op == "PING") {
        if (!next_token(line, pos).empty()) {
            return {std::nullopt, "PING takes no arguments"};
        }
        return {Command{CommandType::Ping, "", "", 0, 0}, ""};
    }

    if (op == "LIST") {
        if (!next_token(line, pos).empty()) {
            return {std::nullopt, "LIST takes no arguments"};
        }
        return {Command{CommandType::List, "", "", 0, 0}, ""};
    }

    if (op == "PUSH") {
        std::string queue = next_token(line, pos);
        if (queue.empty()) {
            return {std::nullopt, "usage: PUSH <queue> <body>"};
        }
        std::string body_raw = rest_trimmed_left(line, pos);
        if (body_raw.empty()) {
            return {std::nullopt, "usage: PUSH <queue> <body>"};
        }
        return {Command{CommandType::Push, queue, unescape_text(body_raw), 0, 0}, ""};
    }

    if (op == "POP") {
        std::string queue = next_token(line, pos);
        if (queue.empty()) {
            return {std::nullopt, "usage: POP <queue> [timeout_ms]"};
        }
        int timeout_ms = 0;
        std::string timeout_tok = next_token(line, pos);
        if (!timeout_tok.empty()) {
            if (!parse_int(timeout_tok, timeout_ms) || timeout_ms < 0) {
                return {std::nullopt, "invalid timeout_ms"};
            }
            if (!next_token(line, pos).empty()) {
                return {std::nullopt, "usage: POP <queue> [timeout_ms]"};
            }
        }
        return {Command{CommandType::Pop, queue, "", 0, timeout_ms}, ""};
    }

    if (op == "ACK" || op == "NACK" || op == "REPL_ACK") {
        std::string queue = next_token(line, pos);
        std::string id_tok = next_token(line, pos);
        std::uint64_t id = 0;
        if (queue.empty() || id_tok.empty() || !parse_u64(id_tok, id) ||
            !next_token(line, pos).empty()) {
            return {std::nullopt, "usage: " + op + " <queue> <id>"};
        }
        CommandType t = CommandType::Ack;
        if (op == "NACK") t = CommandType::Nack;
        if (op == "REPL_ACK") t = CommandType::ReplAck;
        return {Command{t, queue, "", id, 0}, ""};
    }

    if (op == "SIZE" || op == "DELETE") {
        std::string queue = next_token(line, pos);
        if (queue.empty() || !next_token(line, pos).empty()) {
            return {std::nullopt, "usage: " + op + " <queue>"};
        }
        CommandType t = (op == "SIZE") ? CommandType::Size : CommandType::Delete;
        return {Command{t, queue, "", 0, 0}, ""};
    }

    if (op == "REPL_PUSH") {
        std::string queue = next_token(line, pos);
        std::string id_tok = next_token(line, pos);
        std::uint64_t id = 0;
        if (queue.empty() || id_tok.empty() || !parse_u64(id_tok, id)) {
            return {std::nullopt, "usage: REPL_PUSH <queue> <id> <body>"};
        }
        std::string body_raw = rest_trimmed_left(line, pos);
        if (body_raw.empty()) {
            return {std::nullopt, "usage: REPL_PUSH <queue> <id> <body>"};
        }
        return {Command{CommandType::ReplPush, queue, unescape_text(body_raw), id, 0}, ""};
    }

    return {std::nullopt, "unknown command: " + op};
}

std::string serialize_command(const Command& cmd) {
    std::ostringstream oss;
    switch (cmd.type) {
        case CommandType::Push:
            oss << "PUSH " << cmd.queue << " " << escape_text(cmd.body);
            break;
        case CommandType::Pop:
            oss << "POP " << cmd.queue;
            if (cmd.timeout_ms > 0) {
                oss << " " << cmd.timeout_ms;
            }
            break;
        case CommandType::Ack:
            oss << "ACK " << cmd.queue << " " << cmd.id;
            break;
        case CommandType::Nack:
            oss << "NACK " << cmd.queue << " " << cmd.id;
            break;
        case CommandType::Size:
            oss << "SIZE " << cmd.queue;
            break;
        case CommandType::List:
            oss << "LIST";
            break;
        case CommandType::Delete:
            oss << "DELETE " << cmd.queue;
            break;
        case CommandType::Ping:
            oss << "PING";
            break;
        case CommandType::ReplPush:
            oss << "REPL_PUSH " << cmd.queue << " " << cmd.id << " "
                << escape_text(cmd.body);
            break;
        case CommandType::ReplAck:
            oss << "REPL_ACK " << cmd.queue << " " << cmd.id;
            break;
    }
    return oss.str();
}

std::string format_ok(const std::string& value) {
    if (value.empty()) {
        return "OK\n";
    }
    return "OK " + value + "\n";
}

std::string format_msg(std::uint64_t id, const std::string& body) {
    return "MSG " + std::to_string(id) + " " + escape_text(body) + "\n";
}

std::string format_empty() {
    return "EMPTY\n";
}

std::string format_pong() {
    return "PONG\n";
}

std::string format_err(const std::string& reason) {
    return "ERR " + escape_text(reason) + "\n";
}
