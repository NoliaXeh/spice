#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "spice/plugin/MsgPack.hpp"

using namespace spice::plugin::msgpack;

namespace {

//! Encodes then decodes a value, for testing the codec round-trips.
auto roundtrip(Value const& value) -> Value {
    return decode(encode(value)).value();
}

}

TEST_CASE("msgpack round-trips scalars") {
    CHECK(roundtrip(Value {}).is_nil());
    CHECK_EQ(roundtrip(Value { true }).as_bool(), true);
    CHECK_EQ(roundtrip(Value { false }).as_bool(), false);
    CHECK_EQ(roundtrip(Value { 0 }).as_int(), 0);
    CHECK_EQ(roundtrip(Value { 127 }).as_int(), 127);
    CHECK_EQ(roundtrip(Value { -1 }).as_int(), -1);
    CHECK_EQ(roundtrip(Value { -32 }).as_int(), -32);
    CHECK_EQ(roundtrip(Value { 300 }).as_int(), 300);
    CHECK_EQ(roundtrip(Value { -300 }).as_int(), -300);
    CHECK_EQ(roundtrip(Value { 70000 }).as_int(), 70000);
    CHECK_EQ(roundtrip(Value { int64_t { 5'000'000'000 } }).as_int(), 5'000'000'000);
    CHECK_EQ(roundtrip(Value { uint64_t { 12345678901234 } }).as_int(), 12345678901234);
    CHECK_EQ(roundtrip(Value { 3.5 }).as_double(), 3.5);
}

TEST_CASE("msgpack round-trips strings of every length class") {
    CHECK_EQ(roundtrip(Value { "" }).as_string(), "");
    CHECK_EQ(roundtrip(Value { "hi" }).as_string(), "hi");
    CHECK_EQ(roundtrip(Value { std::string(200, 'x') }).as_string(), std::string(200, 'x'));
    CHECK_EQ(roundtrip(Value { std::string(70000, 'y') }).as_string().size(), 70000u);
    CHECK_EQ(roundtrip(Value { "utf-8 \xc3\xa9!" }).as_string(), "utf-8 \xc3\xa9!");
}

TEST_CASE("msgpack round-trips bytes as bin, distinct from str") {
    std::string const data { "a\x00\xff b", 5 };
    Value const value { Value::bytes(data) };
    CHECK_EQ(roundtrip(value).as_string(), data);
    CHECK_EQ(encode(value)[0] & 0xff, 0xc4); // bin8, not str
}

TEST_CASE("msgpack round-trips arrays and maps") {
    Value const array { Value::Array { Value { 1 }, Value { "two" }, Value {} } };
    Value const back { roundtrip(array) };
    REQUIRE(back.is_array());
    REQUIRE_EQ(back.as_array().size(), 3u);
    CHECK_EQ(back.as_array()[0].as_int(), 1);
    CHECK_EQ(back.as_array()[1].as_string(), "two");
    CHECK(back.as_array()[2].is_nil());

    Value const map { Value::object({
        { "kind", Value { 2 } },
        { "method", Value { "buffer.splice" } },
        { "nested", Value::object({ { "ok", Value { true } } }) },
    }) };
    Value const decoded { roundtrip(map) };
    REQUIRE(decoded.is_map());
    CHECK_EQ(decoded["kind"].as_int(), 2);
    CHECK_EQ(decoded["method"].as_string(), "buffer.splice");
    CHECK_EQ(decoded["nested"]["ok"].as_bool(), true);
    CHECK(decoded["absent"].is_nil());
    CHECK_FALSE(decoded.contains("absent"));
}

TEST_CASE("msgpack round-trips a nested envelope") {
    Value const envelope { Value::Array {
        Value { 2 },
        Value { uint64_t { 42 } },
        Value { "buffer.get_lines" },
        Value::object({
            { "buffer", Value { 7 } },
            { "start", Value { 0 } },
            { "end", Value { 100 } },
        }),
    } };
    Value const back { roundtrip(envelope) };
    REQUIRE_EQ(back.as_array().size(), 4u);
    CHECK_EQ(back.as_array()[0].as_int(), 2);
    CHECK_EQ(back.as_array()[1].as_int(), 42);
    CHECK_EQ(back.as_array()[3]["buffer"].as_int(), 7);
}

TEST_CASE("msgpack decode advances the offset and rejects garbage") {
    std::string const stream { encode(Value { 1 }) + encode(Value { "next" }) };
    size_t offset { 0 };
    CHECK_EQ(decode(stream, offset)->as_int(), 1);
    CHECK_EQ(decode(stream, offset)->as_string(), "next");
    CHECK_EQ(offset, stream.size());
    CHECK_FALSE(decode(stream, offset).has_value()); // nothing left

    CHECK_FALSE(decode("").has_value());
    CHECK_FALSE(decode("\xdb\xff\xff\xff\xff").has_value()); // str length past the end
}
