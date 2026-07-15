#include "spice/plugin/Message.hpp"

namespace spice::plugin {

using msgpack::Value;

auto Message::event(std::string topic, Value params) -> Message {
    return { Kind::event, 0, false, std::move(topic), std::move(params) };
}

auto Message::response(uint64_t id, std::string method, Value params) -> Message {
    return { Kind::response, id, true, std::move(method), std::move(params) };
}

auto Message::error(std::string method, Value params) -> Message {
    return { Kind::error, 0, false, std::move(method), std::move(params) };
}

auto encode_frame(Message const& message) -> std::string {
    Value const envelope { Value::Array {
        Value { static_cast<int64_t>(message.kind) },
        message.has_id ? Value { message.id } : Value {},
        Value { message.method },
        message.params,
    } };
    std::string const payload { msgpack::encode(envelope) };

    std::string frame;
    frame.reserve(4 + payload.size());
    auto const length { static_cast<uint32_t>(payload.size()) };
    frame.push_back(static_cast<char>((length >> 24) & 0xff));
    frame.push_back(static_cast<char>((length >> 16) & 0xff));
    frame.push_back(static_cast<char>((length >> 8) & 0xff));
    frame.push_back(static_cast<char>(length & 0xff));
    frame += payload;
    return frame;
}

auto frame_length(std::string_view buffer) -> size_t {
    return static_cast<size_t>(static_cast<uint8_t>(buffer[0])) << 24U
        | static_cast<size_t>(static_cast<uint8_t>(buffer[1])) << 16U
        | static_cast<size_t>(static_cast<uint8_t>(buffer[2])) << 8U
        | static_cast<size_t>(static_cast<uint8_t>(buffer[3]));
}

auto take_frame(std::string& buffer) -> std::optional<Message> {
    if (buffer.size() < frame_header_bytes) {
        return std::nullopt;
    }
    auto const length { frame_length(buffer) };
    if (buffer.size() < frame_header_bytes + length) {
        return std::nullopt; // the whole frame has not arrived yet
    }

    std::string_view const payload { buffer.data() + frame_header_bytes, length };
    auto const value { msgpack::decode(payload) };
    buffer.erase(0, frame_header_bytes + length); // consume the frame either way

    if (!value || !value->is_array() || value->as_array().size() != 4) {
        return std::nullopt; // malformed envelope: dropped
    }
    auto const& parts { value->as_array() };
    int64_t const kind { parts[0].as_int(-1) };
    if (kind < 0 || kind > 4) {
        return std::nullopt;
    }

    Message message;
    message.kind = static_cast<Kind>(kind);
    if (parts[1].is_int()) {
        message.id = static_cast<uint64_t>(parts[1].as_int());
        message.has_id = true;
    }
    message.method = parts[2].as_string();
    message.params = parts[3].is_map() ? parts[3] : Value::object({});
    return message;
}

}
