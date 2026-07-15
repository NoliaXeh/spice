#include "doctest.h"

#include "spice/plugin/Message.hpp"

using namespace spice::plugin;
using msgpack::Value;

TEST_CASE("plugin frame round-trips through the length prefix") {
    Message const request {
        Kind::request, 7, true, "buffer.splice",
        Value::object({ { "buffer", Value { 3 } }, { "version", Value { 9 } } }),
    };
    std::string buffer { encode_frame(request) };

    // 4-byte big-endian length prefix in front
    size_t const length {
        static_cast<size_t>(static_cast<uint8_t>(buffer[0])) << 24
        | static_cast<size_t>(static_cast<uint8_t>(buffer[1])) << 16
        | static_cast<size_t>(static_cast<uint8_t>(buffer[2])) << 8
        | static_cast<size_t>(static_cast<uint8_t>(buffer[3]))
    };
    CHECK_EQ(length, buffer.size() - 4);

    auto const message { take_frame(buffer) };
    REQUIRE(message.has_value());
    CHECK_EQ(message->kind, Kind::request);
    CHECK_EQ(message->id, 7u);
    CHECK(message->has_id);
    CHECK_EQ(message->method, "buffer.splice");
    CHECK_EQ(message->params["version"].as_int(), 9);
    CHECK(buffer.empty()); // consumed
}

TEST_CASE("plugin events carry no id") {
    Message const event { Message::event("spice.input.key", Value::object({})) };
    std::string buffer { encode_frame(event) };
    auto const message { take_frame(buffer) };
    REQUIRE(message.has_value());
    CHECK_EQ(message->kind, Kind::event);
    CHECK_FALSE(message->has_id);
}

TEST_CASE("plugin take_frame waits for a whole frame") {
    Message const notify { Kind::notify, 0, false, "log", Value::object({}) };
    std::string const whole { encode_frame(notify) };

    std::string partial { whole.substr(0, whole.size() - 3) };
    CHECK_FALSE(take_frame(partial).has_value()); // incomplete: nothing yet
    CHECK_EQ(partial.size(), whole.size() - 3);    // and left untouched

    partial += whole.substr(whole.size() - 3);     // the rest arrives
    CHECK(take_frame(partial).has_value());
}

TEST_CASE("plugin take_frame handles two frames back to back") {
    std::string buffer;
    buffer += encode_frame(Message::event("a", Value::object({})));
    buffer += encode_frame(Message::event("b", Value::object({})));

    auto const first { take_frame(buffer) };
    auto const second { take_frame(buffer) };
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK_EQ(first->method, "a");
    CHECK_EQ(second->method, "b");
    CHECK(buffer.empty());
    CHECK_FALSE(take_frame(buffer).has_value());
}

TEST_CASE("plugin take_frame drops a malformed envelope but consumes it") {
    std::string const junk_payload { msgpack::encode(Value { "not an array" }) };
    std::string buffer;
    auto const length { static_cast<uint32_t>(junk_payload.size()) };
    buffer.push_back(static_cast<char>((length >> 24) & 0xff));
    buffer.push_back(static_cast<char>((length >> 16) & 0xff));
    buffer.push_back(static_cast<char>((length >> 8) & 0xff));
    buffer.push_back(static_cast<char>(length & 0xff));
    buffer += junk_payload;
    buffer += encode_frame(Message::event("after", Value::object({})));

    CHECK_FALSE(take_frame(buffer).has_value()); // the junk frame
    auto const good { take_frame(buffer) };       // recovers on the next one
    REQUIRE(good.has_value());
    CHECK_EQ(good->method, "after");
}
