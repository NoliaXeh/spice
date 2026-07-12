#include "spice/core/Buffer.hpp"
#include "spice/core/Utf8.hpp"

#include <cstddef>
#include <utility>

namespace spice::core {

Buffer::Buffer(std::string&& name, BufferCapability capability, std::string_view content)
    : _name { std::move(name) }
    , _capability { capability }
{
    append(content);
}

auto Buffer::name() const -> std::string const& {
    return _name;
}

auto Buffer::capability() const -> BufferCapability {
    return _capability;
}

auto Buffer::line_count() const -> uint32_t {
    return static_cast<uint32_t>(_lines.size());
}

auto Buffer::line(uint32_t index) const -> std::string_view {
    if (index >= _lines.size()) {
        return {};
    }
    return _lines[index];
}

auto Buffer::line_length(uint32_t index) const -> uint32_t {
    if (index >= _lines.size()) {
        return 0;
    }
    return static_cast<uint32_t>(utf8_count(_lines[index]));
}

auto Buffer::insert(Position position, std::string_view text) -> bool {
    if (_capability != BufferCapability::editable
        || position.line >= _lines.size()
        || position.column > line_length(position.line)
        || text.find('\n') != std::string_view::npos) {
        return false;
    }
    std::string& line { _lines[position.line] };
    line.insert(utf8_offset(line, position.column), text);
    return true;
}

auto Buffer::erase(Position position) -> bool {
    if (_capability != BufferCapability::editable || position.line >= _lines.size()) {
        return false;
    }
    std::string& line { _lines[position.line] };

    if (position.column < line_length(position.line)) {
        size_t const offset { utf8_offset(line, position.column) };
        line.erase(offset, utf8_length(line[offset]));
        return true;
    }
    if (position.line + 1 < _lines.size()) { // end of line: join the next one
        line += _lines[position.line + 1];
        _lines.erase(_lines.begin() + position.line + 1);
        return true;
    }
    return false; // very end of the buffer: nothing to erase
}

auto Buffer::split_line(Position position) -> bool {
    if (_capability != BufferCapability::editable
        || position.line >= _lines.size()
        || position.column > line_length(position.line)) {
        return false;
    }
    std::string& line { _lines[position.line] };
    size_t const offset { utf8_offset(line, position.column) };
    std::string rest { line.substr(offset) };
    line.resize(offset);
    _lines.insert(_lines.begin() + position.line + 1, std::move(rest));
    return true;
}

auto Buffer::append(std::string_view text) -> void {
    for (char const byte : text) {
        if (byte == '\n') {
            _lines.emplace_back();
        } else {
            _lines.back() += byte;
        }
    }
}

}
