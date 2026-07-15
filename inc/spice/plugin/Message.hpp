#ifndef SPICE_PLUGIN_MESSAGE_H
#define SPICE_PLUGIN_MESSAGE_H

#include "spice/plugin/MsgPack.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace spice::plugin {

//! The kind of a protocol message (PROTOCOL.md envelope table).
enum class Kind : uint8_t {
    event = 0,    //!< core -> plugin, never answered
    notify = 1,   //!< plugin -> core, never answered
    request = 2,  //!< plugin -> core, answered
    response = 3, //!< core -> plugin, answers a request
    error = 4,    //!< core -> plugin, an unsolicited protocol error
};

//! One protocol message: the `[kind, id, method, params]` envelope. `id`
//! is meaningful only for request/response; `method` is the topic (event)
//! or the method name (notify/request); `params` is always a map.
struct Message {
    Kind kind {};
    uint64_t id { 0 };
    bool has_id { false };
    std::string method;
    msgpack::Value params { msgpack::Value::object({}) };

    static auto event(std::string topic, msgpack::Value params) -> Message;
    static auto response(uint64_t id, std::string method, msgpack::Value params) -> Message;
    static auto error(std::string method, msgpack::Value params) -> Message;
};

//! Encodes a message as one length-prefixed msgpack frame (4-byte
//! big-endian length + payload), ready to write to a plugin's stdin.
auto encode_frame(Message const& message) -> std::string;

//! Pulls the first complete frame from the front of `buffer`, decoding it
//! and erasing its bytes. Nothing when a whole frame is not there yet; a
//! frame that decodes to a malformed envelope is dropped and skipped.
auto take_frame(std::string& buffer) -> std::optional<Message>;

}

#endif // SPICE_PLUGIN_MESSAGE_H
