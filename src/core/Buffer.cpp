#include "spice/core/Buffer.hpp"
#include "spice/core/Utf8.hpp"

#include <cstddef>
#include <utility>

namespace {

//! Oldest edits are dropped past this, to bound memory.
constexpr std::size_t history_limit { 1000 };

//! Where a block of text ends when inserted at `begin`.
auto block_end(spice::core::Position begin, std::string_view text) -> spice::core::Position {
    size_t const last_newline { text.rfind('\n') };
    if (last_newline == std::string_view::npos) {
        return {
            begin.line,
            begin.column + static_cast<uint32_t>(spice::core::utf8_count(text)),
            0,
        };
    }
    uint32_t lines_added { 0 };
    for (char const byte : text) {
        if (byte == '\n') {
            ++lines_added;
        }
    }
    return {
        begin.line + lines_added,
        static_cast<uint32_t>(spice::core::utf8_count(text.substr(last_newline + 1))),
        0,
    };
}

}

namespace spice::core {

Buffer::Buffer(std::string&& name, BufferCapability capability, std::string_view content)
    : _name { std::move(name) }
    , _capability { capability }
{
    append(content);
    _dirty = false; // initial content is a starting point, not an edit
}

auto Buffer::name() const -> std::string const& {
    return _name;
}

auto Buffer::capability() const -> BufferCapability {
    return _capability;
}

auto Buffer::path() const -> std::string const& {
    return _path;
}

auto Buffer::set_path(std::string path) -> void {
    _path = std::move(path);
}

auto Buffer::dirty() const -> bool {
    return _dirty;
}

auto Buffer::mark_saved() -> void {
    _dirty = false;
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

// ---------------------------------------------------------------
// Raw mutations: no validation, no recording. The public operations
// and the undo machinery are both built on these.
// ---------------------------------------------------------------

auto Buffer::raw_insert(Position position, std::string_view text) -> void {
    std::string& line { _lines[position.line] };
    line.insert(utf8_offset(line, position.column), text);
}

auto Buffer::raw_erase(Position position, std::size_t bytes) -> void {
    std::string& line { _lines[position.line] };
    line.erase(utf8_offset(line, position.column), bytes);
}

auto Buffer::raw_split(Position position) -> void {
    std::string& line { _lines[position.line] };
    size_t const offset { utf8_offset(line, position.column) };
    std::string rest { line.substr(offset) };
    line.resize(offset);
    _lines.insert(_lines.begin() + position.line + 1, std::move(rest));
}

auto Buffer::raw_join(uint32_t line) -> void {
    _lines[line] += _lines[line + 1];
    _lines.erase(_lines.begin() + line + 1);
}

auto Buffer::raw_insert_block(Position position, std::string_view text) -> Position {
    size_t const newline { text.find('\n') };
    if (newline == std::string_view::npos) {
        raw_insert(position, text);
        return {
            position.line,
            position.column + static_cast<uint32_t>(utf8_count(text)),
            0,
        };
    }
    // split the insertion point, put the first fragment on the first line,
    // then stack the remaining fragments as their own lines before the tail
    raw_split(position);
    raw_insert(position, text.substr(0, newline));
    uint32_t line { position.line + 1 };
    std::string_view rest { text.substr(newline + 1) };
    for (size_t cut { rest.find('\n') }; cut != std::string_view::npos;
         cut = rest.find('\n')) {
        _lines.insert(_lines.begin() + line, std::string(rest.substr(0, cut)));
        rest = rest.substr(cut + 1);
        ++line;
    }
    raw_insert({ line, 0, 0 }, rest);
    return { line, static_cast<uint32_t>(utf8_count(rest)), 0 };
}

auto Buffer::raw_erase_block(Position begin, Position end) -> void {
    if (begin.line == end.line) {
        std::string& line { _lines[begin.line] };
        size_t const from { utf8_offset(line, begin.column) };
        line.erase(from, utf8_offset(line, end.column) - from);
        return;
    }
    std::string& first { _lines[begin.line] };
    std::string const& last { _lines[end.line] };
    first.resize(utf8_offset(first, begin.column));
    first += last.substr(utf8_offset(last, end.column));
    _lines.erase(_lines.begin() + begin.line + 1, _lines.begin() + end.line + 1);
}

auto Buffer::valid(Position position) const -> bool {
    return position.line < _lines.size() && position.column <= line_length(position.line);
}

// ---------------------------------------------------------------
// Public operations: validate, mutate, record
// ---------------------------------------------------------------

auto Buffer::insert(Position position, std::string_view text) -> bool {
    if (_capability != BufferCapability::editable
        || position.line >= _lines.size()
        || position.column > line_length(position.line)
        || text.find('\n') != std::string_view::npos) {
        return false;
    }
    raw_insert(position, text);
    record({ Edit::Kind::insert_text, position, std::string(text) });
    return true;
}

auto Buffer::erase(Position position) -> bool {
    if (_capability != BufferCapability::editable || position.line >= _lines.size()) {
        return false;
    }
    std::string& line { _lines[position.line] };

    if (position.column < line_length(position.line)) {
        size_t const offset { utf8_offset(line, position.column) };
        std::string erased { line.substr(offset, utf8_length(line[offset])) };
        raw_erase(position, erased.size());
        record({ Edit::Kind::erase_text, position, std::move(erased) });
        return true;
    }
    if (position.line + 1 < _lines.size()) { // end of line: join the next one
        raw_join(position.line);
        record({ Edit::Kind::join, position, {} });
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
    raw_split(position);
    record({ Edit::Kind::split, position, {} });
    return true;
}

auto Buffer::text_between(Position begin, Position end) const -> std::string {
    if (document_order(end, begin)) {
        std::swap(begin, end);
    }
    if (!valid(begin) || !valid(end) || begin == end) {
        return {};
    }
    if (begin.line == end.line) {
        std::string_view const line { _lines[begin.line] };
        size_t const from { utf8_offset(line, begin.column) };
        return std::string(line.substr(from, utf8_offset(line, end.column) - from));
    }
    std::string out { _lines[begin.line].substr(
        utf8_offset(_lines[begin.line], begin.column)
    ) };
    for (uint32_t line { begin.line + 1 }; line < end.line; ++line) {
        out += '\n';
        out += _lines[line];
    }
    out += '\n';
    out += _lines[end.line].substr(0, utf8_offset(_lines[end.line], end.column));
    return out;
}

auto Buffer::erase_range(Position begin, Position end) -> bool {
    if (document_order(end, begin)) {
        std::swap(begin, end);
    }
    if (_capability != BufferCapability::editable
        || !valid(begin) || !valid(end) || begin == end) {
        return false;
    }
    std::string removed { text_between(begin, end) };
    raw_erase_block(begin, end);
    record({ Edit::Kind::erase_block, begin, std::move(removed) });
    return true;
}

auto Buffer::insert_block(Position position, std::string_view text) -> std::optional<Position> {
    if (_capability != BufferCapability::editable || !valid(position) || text.empty()) {
        return std::nullopt;
    }
    Position const end { raw_insert_block(position, text) };
    record({ Edit::Kind::insert_block, position, std::string(text) });
    return end;
}

auto Buffer::append(std::string_view text) -> void {
    if (!text.empty()) {
        _dirty = true;
    }
    for (char const byte : text) {
        if (byte == '\n') {
            _lines.emplace_back();
        } else {
            _lines.back() += byte;
        }
    }
}

// ---------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------

auto Buffer::record(Edit&& edit) -> void {
    _dirty = true;
    _redo.clear(); // editing forks history: the undone future is gone

    if (!_undo.empty()) { // coalesce runs at one spot into a single edit
        Edit& top { _undo.back() };
        if (edit.kind == Edit::Kind::insert_text && top.kind == Edit::Kind::insert_text
            && edit.position.line == top.position.line
            && edit.position.column == top.position.column + utf8_count(top.text)) {
            top.text += edit.text; // typing forward
            return;
        }
        if (edit.kind == Edit::Kind::erase_text && top.kind == Edit::Kind::erase_text
            && edit.position.line == top.position.line) {
            if (edit.position.column == top.position.column) {
                top.text += edit.text; // delete-forward run
                return;
            }
            if (edit.position.column + 1 == top.position.column) {
                top.text.insert(0, edit.text); // backspace run
                top.position = edit.position;
                return;
            }
        }
    }

    _undo.push_back(std::move(edit));
    if (_undo.size() > history_limit) {
        _undo.erase(_undo.begin());
    }
}

auto Buffer::apply(Edit const& edit) -> Position {
    switch (edit.kind) {
    case Edit::Kind::insert_text:
        raw_insert(edit.position, edit.text);
        return {
            edit.position.line,
            edit.position.column + static_cast<uint32_t>(utf8_count(edit.text)),
            0,
        };
    case Edit::Kind::erase_text:
        raw_erase(edit.position, edit.text.size());
        return edit.position;
    case Edit::Kind::split:
        raw_split(edit.position);
        return { edit.position.line + 1, 0, 0 };
    case Edit::Kind::join:
        raw_join(edit.position.line);
        return edit.position;
    case Edit::Kind::insert_block:
        return raw_insert_block(edit.position, edit.text);
    case Edit::Kind::erase_block:
        raw_erase_block(edit.position, block_end(edit.position, edit.text));
        return edit.position;
    }
    return edit.position;
}

auto Buffer::revert(Edit const& edit) -> Position {
    switch (edit.kind) {
    case Edit::Kind::insert_text:
        raw_erase(edit.position, edit.text.size());
        return edit.position;
    case Edit::Kind::erase_text:
        raw_insert(edit.position, edit.text);
        return {
            edit.position.line,
            edit.position.column + static_cast<uint32_t>(utf8_count(edit.text)),
            0,
        };
    case Edit::Kind::split:
        raw_join(edit.position.line);
        return edit.position;
    case Edit::Kind::join:
        raw_split(edit.position);
        return edit.position;
    case Edit::Kind::insert_block:
        raw_erase_block(edit.position, block_end(edit.position, edit.text));
        return edit.position;
    case Edit::Kind::erase_block:
        return raw_insert_block(edit.position, edit.text);
    }
    return edit.position;
}

auto Buffer::undo() -> std::optional<Position> {
    if (_undo.empty()) {
        return std::nullopt;
    }
    Edit edit { std::move(_undo.back()) };
    _undo.pop_back();
    Position const position { revert(edit) };
    _redo.push_back(std::move(edit));
    _dirty = true;
    return position;
}

auto Buffer::redo() -> std::optional<Position> {
    if (_redo.empty()) {
        return std::nullopt;
    }
    Edit edit { std::move(_redo.back()) };
    _redo.pop_back();
    Position const position { apply(edit) };
    _undo.push_back(std::move(edit));
    _dirty = true;
    return position;
}

}
