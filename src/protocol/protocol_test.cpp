#include "src/protocol/protocol.hpp"

#include <cassert>
#include <iostream>

int main() {
    // Round-trip PUSH with JSON + newline escaping
    {
        Command cmd{CommandType::Push, "orders", "{\"id\":1}\nnext", 0, 0};
        std::string wire = serialize_command(cmd);
        auto res = parse_command(wire);
        assert(res.command.has_value());
        assert(res.command->type == CommandType::Push);
        assert(res.command->queue == "orders");
        assert(res.command->body == "{\"id\":1}\nnext");
    }

    // POP with and without timeout
    {
        auto r1 = parse_command("POP orders\r");
        assert(r1.command.has_value() && r1.command->timeout_ms == 0);

        auto r2 = parse_command("POP orders 2500");
        assert(r2.command.has_value() && r2.command->timeout_ms == 2500);

        auto r3 = parse_command("POP orders -5");
        assert(!r3.command.has_value());
    }

    // ACK / NACK / REPL_PUSH / REPL_ACK / LIST / DELETE / PING
    {
        auto ack = parse_command("ACK orders 42");
        assert(ack.command.has_value() && ack.command->type == CommandType::Ack &&
               ack.command->id == 42);

        auto rpush = parse_command("REPL_PUSH jobs 99 hello world");
        assert(rpush.command.has_value() &&
               rpush.command->type == CommandType::ReplPush &&
               rpush.command->id == 99 && rpush.command->body == "hello world");

        auto bad = parse_command("UNKNOWN foo");
        assert(!bad.command.has_value());
    }

    assert(format_ok() == "OK\n");
    assert(format_ok("7") == "OK 7\n");
    assert(format_msg(3, "a\nb") == "MSG 3 a\\nb\n");
    assert(format_empty() == "EMPTY\n");
    assert(format_pong() == "PONG\n");

    std::cout << "All Protocol tests passed!\n";
    return 0;
}
