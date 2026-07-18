#include "spice/core/Buffer.hpp"
#include "spice/core/Utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace {

//! Oldest edits are dropped past this, to bound memory.
constexpr std::size_t history_limit { 1000 };

//! Past this many undrained journal entries the journal gives up: the
//! splices are dropped and the ChangeSet is marked incomplete. Bounds
//! memory when nobody drains (a session with no plugins running).
constexpr std::size_t changes_limit { 1024 };

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
    _changes.clear(); // ...and not a change either
    _changes_base_version = _version;
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

auto Buffer::version() const -> uint64_t {
    return _version;
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

auto Buffer::apply_batch(std::vector<BatchSplice> const& splices) -> bool {
    if (_capability != BufferCapability::editable || splices.empty()) {
        return false;
    }
    // one compound edit: each part mutates, then journals (the journal and
    // the marks want post-mutation content, part by part)
    Edit batch { Edit::Kind::batch, splices.front().begin, {}, {} };
    for (auto const& splice : splices) {
        if (!(splice.begin == splice.end)) {
            std::string removed { text_between(splice.begin, splice.end) };
            raw_erase_block(splice.begin, splice.end);
            journal_change(splice.begin, removed, true);
            batch.children.push_back(
                { Edit::Kind::erase_block, splice.begin, std::move(removed), {} }
            );
        }
        if (!splice.text.empty()) {
            raw_insert_block(splice.begin, splice.text);
            journal_change(splice.begin, splice.text, false);
            batch.children.push_back(
                { Edit::Kind::insert_block, splice.begin, splice.text, {} }
            );
        }
    }
    if (batch.children.empty()) {
        return true; // all no-ops: nothing changed, nothing to record
    }

    // recorded directly - record() would journal it a second time, and a
    // batch never coalesces with neighbouring keystrokes
    _dirty = true;
    ++_version;
    _redo.clear();
    _undo.push_back(std::move(batch));
    if (_undo.size() > history_limit) {
        _undo.erase(_undo.begin());
    }
    return true;
}

auto Buffer::append(std::string_view text) -> void {
    if (text.empty()) {
        return;
    }
    _dirty = true;
    ++_version;
    Position const at { // where the buffer ended before the append
        static_cast<uint32_t>(_lines.size()) - 1,
        static_cast<uint32_t>(utf8_count(_lines.back())),
        0,
    };
    for (char const byte : text) {
        if (byte == '\n') {
            _lines.emplace_back();
        } else {
            _lines.back() += byte;
        }
    }
    journal_change(at, text, false);
}

// ---------------------------------------------------------------
// The change journal: every mutation, as a protocol splice
// ---------------------------------------------------------------

auto Buffer::take_changes() -> ChangeSet {
    ChangeSet set {
        _changes_base_version,
        _changes_complete,
        std::exchange(_changes, {}),
    };
    _changes_base_version = _version;
    _changes_complete = true;
    return set;
}

auto Buffer::journal(Edit const& edit, bool inverted) -> void {
    // an undone insert is an erase and vice versa; a split is the insertion
    // of a newline, a join its erasure - so every edit reduces to "this
    // text appeared here" or "this text vanished from here"
    switch (edit.kind) {
    case Edit::Kind::insert_text:
    case Edit::Kind::insert_block:
        journal_change(edit.position, edit.text, inverted);
        break;
    case Edit::Kind::erase_text:
    case Edit::Kind::erase_block:
        journal_change(edit.position, edit.text, !inverted);
        break;
    case Edit::Kind::split:
        journal_change(edit.position, "\n", inverted);
        break;
    case Edit::Kind::join:
        journal_change(edit.position, "\n", !inverted);
        break;
    case Edit::Kind::batch:
        break; // its children were journaled one by one
    }
}

auto Buffer::journal_change(Position position, std::string_view text, bool erased) -> void {
    // this runs after the mutation, and stays exact anyway: the text before
    // the change point is untouched (so its byte offset still holds), and
    // the changed text itself says where the range ends
    uint32_t const line { position.line };
    auto const byte {
        static_cast<uint32_t>(utf8_offset(_lines[line], position.column))
    };
    Change change { line, byte, line, byte, {} };
    if (erased) {
        uint32_t newlines { 0 };
        for (char const c : text) {
            if (c == '\n') {
                ++newlines;
            }
        }
        change.end_line = line + newlines;
        change.end_byte = newlines == 0
            ? byte + static_cast<uint32_t>(text.size())
            : static_cast<uint32_t>(text.size() - (text.rfind('\n') + 1));
    } else {
        change.text = text;
    }
    shift_marks(change); // marks always ride along, journal cap or not

    if (_changes.size() >= changes_limit) {
        _changes.clear();
        _changes_complete = false; // too much history: readers must refetch
        return;
    }
    _changes.push_back(std::move(change));
}

// A change replaces [start, end) with text; marks move accordingly. The
// change's text extent says where inserted text ends.
auto Buffer::shift_marks(Change const& change) -> void {
    uint32_t inserted_lines { 0 };
    size_t last_fragment { change.text.size() };
    if (size_t const cut { change.text.rfind('\n') }; cut != std::string::npos) {
        last_fragment = change.text.size() - cut - 1;
        for (char const c : change.text) {
            inserted_lines += c == '\n' ? 1 : 0;
        }
    }
    uint32_t const insert_end_line { change.start_line + inserted_lines };
    auto const insert_end_byte { static_cast<uint32_t>(inserted_lines == 0
        ? change.start_byte + change.text.size() : last_fragment) };

    auto const before = [](uint32_t line, uint32_t byte, uint32_t l, uint32_t b) {
        return line < l || (line == l && byte < b);
    };
    for (MarkSlot& slot : _marks) {
        MarkInfo& at { slot.at };
        bool const at_start { at.line == change.start_line && at.byte == change.start_byte };
        if (before(at.line, at.byte, change.start_line, change.start_byte)
            || (at_start && slot.gravity == MarkGravity::left)) {
            continue; // wholly before the change (or holding its ground)
        }
        if (before(at.line, at.byte, change.end_line, change.end_byte) && !at_start) {
            // the text around it was removed: park it at the change, invalid
            at = { change.start_line, change.start_byte, false };
            continue;
        }
        // at or past the end (or right-gravity at the start): remap through
        // the replacement
        if (at.line == change.end_line) {
            at.byte = insert_end_byte + (at.byte - change.end_byte);
        }
        at.line = insert_end_line + (at.line - change.end_line);
    }
}

// -- marks --------------------------------------------------------------

auto Buffer::set_mark(uint32_t line, uint32_t byte, MarkGravity gravity) -> uint64_t {
    uint32_t const at_line { std::min(line, line_count() - 1) };
    auto const at_byte {
        static_cast<uint32_t>(std::min<size_t>(byte, _lines[at_line].size()))
    };
    uint64_t const id { _next_mark_id++ };
    _marks.push_back({ id, gravity, { at_line, at_byte, true } });
    return id;
}

auto Buffer::mark(uint64_t id) const -> std::optional<MarkInfo> {
    for (MarkSlot const& slot : _marks) {
        if (slot.id == id) {
            return slot.at;
        }
    }
    return std::nullopt;
}

auto Buffer::delete_mark(uint64_t id) -> bool {
    return std::erase_if(_marks, [&](MarkSlot const& slot) { return slot.id == id; }) > 0;
}

// -- highlights ---------------------------------------------------------

auto Buffer::set_highlights(std::vector<Highlight> highlights) -> void {
    _highlights = std::move(highlights);
}

auto Buffer::highlights() const -> std::vector<Highlight> const& {
    return _highlights;
}

// ---------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------

auto Buffer::record(Edit&& edit) -> void {
    _dirty = true;
    ++_version;
    journal(edit, false); // before coalescing: the journal wants each edit
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
    case Edit::Kind::batch:
        break; // handled by apply_and_journal, child by child
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
    case Edit::Kind::batch:
        break; // handled by revert_and_journal, child by child
    }
    return edit.position;
}

auto Buffer::apply_and_journal(Edit const& edit) -> Position {
    if (edit.kind == Edit::Kind::batch) {
        Position last { edit.position };
        for (Edit const& child : edit.children) {
            last = apply_and_journal(child);
        }
        return last;
    }
    Position const position { apply(edit) };
    journal(edit, false);
    return position;
}

auto Buffer::revert_and_journal(Edit const& edit) -> Position {
    if (edit.kind == Edit::Kind::batch) {
        Position last { edit.position };
        for (auto child { edit.children.rbegin() }; child != edit.children.rend(); ++child) {
            last = revert_and_journal(*child);
        }
        return last;
    }
    Position const position { revert(edit) };
    journal(edit, true);
    return position;
}

auto Buffer::undo() -> std::optional<Position> {
    if (_undo.empty()) {
        return std::nullopt;
    }
    Edit edit { std::move(_undo.back()) };
    _undo.pop_back();
    Position const position { revert_and_journal(edit) }; // inverse splices
    _redo.push_back(std::move(edit));
    _dirty = true;
    ++_version;
    return position;
}

auto Buffer::redo() -> std::optional<Position> {
    if (_redo.empty()) {
        return std::nullopt;
    }
    Edit edit { std::move(_redo.back()) };
    _redo.pop_back();
    Position const position { apply_and_journal(edit) };
    _undo.push_back(std::move(edit));
    _dirty = true;
    ++_version;
    return position;
}

}
