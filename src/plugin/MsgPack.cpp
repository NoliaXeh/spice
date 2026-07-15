#include "spice/plugin/MsgPack.hpp"

#include <cstring>

namespace spice::plugin::msgpack {

// -- construction ------------------------------------------------------

Value::Value(bool value) : _value { value } {}
Value::Value(int value) : _value { static_cast<int64_t>(value) } {}
Value::Value(int64_t value) : _value { value } {}
Value::Value(uint64_t value) : _value { static_cast<int64_t>(value) } {}
Value::Value(double value) : _value { value } {}
Value::Value(char const* value) : _value { std::string(value) } {}
Value::Value(std::string value) : _value { std::move(value) } {}
Value::Value(Array value) : _value { std::move(value) } {}

auto Value::object(std::initializer_list<Field> fields) -> Value {
    Value value;
    value._value = Map(fields);
    return value;
}

auto Value::make_map(Map fields) -> Value {
    Value value;
    value._value = std::move(fields);
    return value;
}

auto Value::bytes(std::string value) -> Value {
    Value result;
    result._value = Bytes { std::move(value) };
    return result;
}

// -- queries -----------------------------------------------------------

auto Value::is_nil() const -> bool { return std::holds_alternative<std::monostate>(_value); }
auto Value::is_int() const -> bool { return std::holds_alternative<int64_t>(_value); }
auto Value::is_string() const -> bool { return std::holds_alternative<std::string>(_value); }
auto Value::is_array() const -> bool { return std::holds_alternative<Array>(_value); }
auto Value::is_map() const -> bool { return std::holds_alternative<Map>(_value); }

auto Value::as_bool(bool fallback) const -> bool {
    auto const* value { std::get_if<bool>(&_value) };
    return value != nullptr ? *value : fallback;
}

auto Value::as_int(int64_t fallback) const -> int64_t {
    if (auto const* value { std::get_if<int64_t>(&_value) }) {
        return *value;
    }
    return fallback;
}

auto Value::as_double(double fallback) const -> double {
    if (auto const* value { std::get_if<double>(&_value) }) {
        return *value;
    }
    if (auto const* value { std::get_if<int64_t>(&_value) }) {
        return static_cast<double>(*value);
    }
    return fallback;
}

auto Value::as_string(std::string_view fallback) const -> std::string {
    if (auto const* value { std::get_if<std::string>(&_value) }) {
        return *value;
    }
    if (auto const* value { std::get_if<Bytes>(&_value) }) {
        return value->data;
    }
    return std::string(fallback);
}

auto Value::as_array() const -> Array const& {
    static Array const empty;
    auto const* value { std::get_if<Array>(&_value) };
    return value != nullptr ? *value : empty;
}

auto Value::as_map() const -> Map const& {
    static Map const empty;
    auto const* value { std::get_if<Map>(&_value) };
    return value != nullptr ? *value : empty;
}

auto Value::operator[](std::string_view key) const -> Value const& {
    static Value const nil;
    if (auto const* map { std::get_if<Map>(&_value) }) {
        for (auto const& [name, value] : *map) {
            if (name == key) {
                return value;
            }
        }
    }
    return nil;
}

auto Value::contains(std::string_view key) const -> bool {
    if (auto const* map { std::get_if<Map>(&_value) }) {
        for (auto const& [name, value] : *map) {
            if (name == key) {
                return true;
            }
        }
    }
    return false;
}

// -- encoding ----------------------------------------------------------

namespace {

auto put(std::string& out, uint8_t tag) -> void {
    out.push_back(static_cast<char>(tag));
}

//! Appends `bytes` bytes of `value`, big-endian.
auto put_be(std::string& out, uint64_t value, int bytes) -> void {
    for (int shift { (bytes - 1) * 8 }; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

auto encode_int(std::string& out, int64_t value) -> void {
    if (value >= 0) {
        auto const u { static_cast<uint64_t>(value) };
        if (u <= 0x7f) {
            put(out, static_cast<uint8_t>(u));
        } else if (u <= 0xff) {
            put(out, 0xcc);
            put_be(out, u, 1);
        } else if (u <= 0xffff) {
            put(out, 0xcd);
            put_be(out, u, 2);
        } else if (u <= 0xffffffff) {
            put(out, 0xce);
            put_be(out, u, 4);
        } else {
            put(out, 0xcf);
            put_be(out, u, 8);
        }
    } else if (value >= -32) {
        put(out, static_cast<uint8_t>(value));
    } else if (value >= -128) {
        put(out, 0xd0);
        put_be(out, static_cast<uint64_t>(value), 1);
    } else if (value >= -32768) {
        put(out, 0xd1);
        put_be(out, static_cast<uint64_t>(value), 2);
    } else if (value >= -2147483648LL) {
        put(out, 0xd2);
        put_be(out, static_cast<uint64_t>(value), 4);
    } else {
        put(out, 0xd3);
        put_be(out, static_cast<uint64_t>(value), 8);
    }
}

auto encode_string(std::string& out, std::string const& text) -> void {
    size_t const size { text.size() };
    if (size <= 31) {
        put(out, static_cast<uint8_t>(0xa0 | size));
    } else if (size <= 0xff) {
        put(out, 0xd9);
        put_be(out, size, 1);
    } else if (size <= 0xffff) {
        put(out, 0xda);
        put_be(out, size, 2);
    } else {
        put(out, 0xdb);
        put_be(out, size, 4);
    }
    out += text;
}

}

auto Value::encode_into(std::string& out) const -> void {
    if (std::holds_alternative<std::monostate>(_value)) {
        put(out, 0xc0);
    } else if (auto const* value { std::get_if<bool>(&_value) }) {
        put(out, *value ? 0xc3 : 0xc2);
    } else if (auto const* value { std::get_if<int64_t>(&_value) }) {
        encode_int(out, *value);
    } else if (auto const* value { std::get_if<double>(&_value) }) {
        put(out, 0xcb);
        uint64_t bits {};
        std::memcpy(&bits, value, sizeof bits);
        put_be(out, bits, 8);
    } else if (auto const* value { std::get_if<std::string>(&_value) }) {
        encode_string(out, *value);
    } else if (auto const* value { std::get_if<Bytes>(&_value) }) {
        size_t const size { value->data.size() };
        if (size <= 0xff) {
            put(out, 0xc4);
            put_be(out, size, 1);
        } else if (size <= 0xffff) {
            put(out, 0xc5);
            put_be(out, size, 2);
        } else {
            put(out, 0xc6);
            put_be(out, size, 4);
        }
        out += value->data;
    } else if (auto const* value { std::get_if<Array>(&_value) }) {
        size_t const size { value->size() };
        if (size <= 15) {
            put(out, static_cast<uint8_t>(0x90 | size));
        } else if (size <= 0xffff) {
            put(out, 0xdc);
            put_be(out, size, 2);
        } else {
            put(out, 0xdd);
            put_be(out, size, 4);
        }
        for (auto const& element : *value) {
            element.encode_into(out);
        }
    } else if (auto const* value { std::get_if<Map>(&_value) }) {
        size_t const size { value->size() };
        if (size <= 15) {
            put(out, static_cast<uint8_t>(0x80 | size));
        } else if (size <= 0xffff) {
            put(out, 0xde);
            put_be(out, size, 2);
        } else {
            put(out, 0xdf);
            put_be(out, size, 4);
        }
        for (auto const& [name, element] : *value) {
            encode_string(out, name);
            element.encode_into(out);
        }
    }
}

auto encode(Value const& value) -> std::string {
    std::string out;
    value.encode_into(out);
    return out;
}

// -- decoding ----------------------------------------------------------

namespace {

//! Reads `count` big-endian bytes from `bytes` at `offset`, advancing it.
//! Returns false when there are not enough bytes.
auto read_be(std::string_view bytes, size_t& offset, int count, uint64_t& out) -> bool {
    if (offset + static_cast<size_t>(count) > bytes.size()) {
        return false;
    }
    out = 0;
    for (int i { 0 }; i < count; ++i) {
        out = (out << 8) | static_cast<uint8_t>(bytes[offset++]);
    }
    return true;
}

auto sign_extend(uint64_t value, int bytes) -> int64_t {
    int const bits { bytes * 8 };
    uint64_t const mask { 1ull << (bits - 1) };
    return static_cast<int64_t>((value ^ mask) - mask);
}

}

namespace {

auto decode_array(std::string_view bytes, size_t& offset, size_t size)
    -> std::optional<Value> {
    Value::Array array;
    array.reserve(size);
    for (size_t i { 0 }; i < size; ++i) {
        auto element { decode(bytes, offset) };
        if (!element) {
            return std::nullopt;
        }
        array.push_back(std::move(*element));
    }
    return Value { std::move(array) };
}

auto decode_map(std::string_view bytes, size_t& offset, size_t size)
    -> std::optional<Value> {
    Value::Map map;
    map.reserve(size);
    for (size_t i { 0 }; i < size; ++i) {
        auto key { decode(bytes, offset) };
        auto value { decode(bytes, offset) };
        if (!key || !value) {
            return std::nullopt;
        }
        map.emplace_back(key->as_string(), std::move(*value));
    }
    return Value::make_map(std::move(map));
}

}

auto decode(std::string_view bytes, size_t& offset) -> std::optional<Value> {
    if (offset >= bytes.size()) {
        return std::nullopt;
    }
    auto const tag { static_cast<uint8_t>(bytes[offset++]) };

    auto read_bytes = [&](size_t size) -> std::optional<std::string> {
        if (offset + size > bytes.size()) {
            return std::nullopt;
        }
        std::string out { bytes.substr(offset, size) };
        offset += size;
        return out;
    };
    auto read_length = [&](int count) -> std::optional<size_t> {
        uint64_t value {};
        if (!read_be(bytes, offset, count, value)) {
            return std::nullopt;
        }
        return static_cast<size_t>(value);
    };

    if (tag <= 0x7f) { // positive fixint
        return Value { static_cast<int64_t>(tag) };
    }
    if (tag >= 0xe0) { // negative fixint
        return Value { static_cast<int64_t>(static_cast<int8_t>(tag)) };
    }
    if ((tag & 0xe0) == 0xa0) { // fixstr
        auto text { read_bytes(tag & 0x1f) };
        return text ? std::optional<Value> { Value { std::move(*text) } } : std::nullopt;
    }
    if ((tag & 0xf0) == 0x90) { // fixarray
        return decode_array(bytes, offset, tag & 0x0f);
    }
    if ((tag & 0xf0) == 0x80) { // fixmap
        return decode_map(bytes, offset, tag & 0x0f);
    }

    switch (tag) {
    case 0xc0: return Value {};
    case 0xc2: return Value { false };
    case 0xc3: return Value { true };
    case 0xcc: case 0xcd: case 0xce: case 0xcf: {
        int const count { 1 << (tag - 0xcc) };
        uint64_t value {};
        if (!read_be(bytes, offset, count, value)) return std::nullopt;
        return Value { static_cast<int64_t>(value) };
    }
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        int const count { 1 << (tag - 0xd0) };
        uint64_t value {};
        if (!read_be(bytes, offset, count, value)) return std::nullopt;
        return Value { sign_extend(value, count) };
    }
    case 0xca: {
        uint64_t word {};
        if (!read_be(bytes, offset, 4, word)) return std::nullopt;
        float f {};
        uint32_t const bits { static_cast<uint32_t>(word) };
        std::memcpy(&f, &bits, sizeof f);
        return Value { static_cast<double>(f) };
    }
    case 0xcb: {
        uint64_t bits {};
        if (!read_be(bytes, offset, 8, bits)) return std::nullopt;
        double d {};
        std::memcpy(&d, &bits, sizeof d);
        return Value { d };
    }
    case 0xd9: case 0xda: case 0xdb: {
        auto const size { read_length(1 << (tag - 0xd9)) };
        if (!size) return std::nullopt;
        auto text { read_bytes(*size) };
        return text ? std::optional<Value> { Value { std::move(*text) } } : std::nullopt;
    }
    case 0xc4: case 0xc5: case 0xc6: {
        auto const size { read_length(1 << (tag - 0xc4)) };
        if (!size) return std::nullopt;
        auto data { read_bytes(*size) };
        return data ? std::optional<Value> { Value::bytes(std::move(*data)) } : std::nullopt;
    }
    case 0xdc: case 0xdd: {
        auto const size { read_length(tag == 0xdc ? 2 : 4) };
        if (!size) return std::nullopt;
        return decode_array(bytes, offset, *size);
    }
    case 0xde: case 0xdf: {
        auto const size { read_length(tag == 0xde ? 2 : 4) };
        if (!size) return std::nullopt;
        return decode_map(bytes, offset, *size);
    }
    default:
        return std::nullopt;
    }
}

auto decode(std::string_view bytes) -> std::optional<Value> {
    size_t offset { 0 };
    return decode(bytes, offset);
}

}
