#ifndef SPICE_PLUGIN_MSGPACK_H
#define SPICE_PLUGIN_MSGPACK_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace spice::plugin::msgpack {

//! A msgpack value - the wire vocabulary of the plugin protocol. Integers
//! are kept as int64_t (the protocol's u64 ids fit for any realistic
//! session); str and bin both map to strings, distinguished only on the
//! wire. Maps are string-keyed here, which is all the protocol uses.
class Value {
public:
    using Array = std::vector<Value>;
    using Field = std::pair<std::string, Value>;
    using Map = std::vector<Field>;

    Value() = default; //!< nil
    Value(bool value);
    Value(int value);
    Value(int64_t value);
    Value(uint64_t value);
    Value(double value);
    Value(char const* value);
    Value(std::string value);
    Value(Array value);

    //! An object literal: `Value::object({{"key", 1}, {"name", "x"}})`.
    static auto object(std::initializer_list<Field> fields) -> Value;
    //! A map from its fields (the decoder's constructor).
    static auto make_map(Map fields) -> Value;
    //! A string carrying opaque bytes (encoded as msgpack bin, not str).
    static auto bytes(std::string value) -> Value;

    auto is_nil() const -> bool;
    auto is_int() const -> bool;
    auto is_string() const -> bool;
    auto is_array() const -> bool;
    auto is_map() const -> bool;

    auto as_bool(bool fallback = false) const -> bool;
    auto as_int(int64_t fallback = 0) const -> int64_t;
    auto as_double(double fallback = 0.0) const -> double;
    auto as_string(std::string_view fallback = {}) const -> std::string;
    auto as_array() const -> Array const&; //!< empty when not an array
    auto as_map() const -> Map const&;      //!< empty when not a map

    //! Map lookup by key: the value, or nil when absent / not a map.
    auto operator[](std::string_view key) const -> Value const&;
    auto contains(std::string_view key) const -> bool;

    auto operator==(Value const&) const -> bool = default;

private:
    struct Bytes { std::string data; auto operator==(Bytes const&) const -> bool = default; };
    std::variant<std::monostate, bool, int64_t, double, std::string, Bytes, Array, Map>
        _value;

    friend auto encode(Value const& value) -> std::string;
    auto encode_into(std::string& out) const -> void;
};

//! Serializes a value to a msgpack byte string.
auto encode(Value const& value) -> std::string;

//! Decodes one value starting at `offset`, advancing it past the value.
//! Nothing on a truncated or malformed input.
auto decode(std::string_view bytes, size_t& offset) -> std::optional<Value>;

//! Decodes a value that is expected to span the whole input.
auto decode(std::string_view bytes) -> std::optional<Value>;

}

#endif // SPICE_PLUGIN_MSGPACK_H
