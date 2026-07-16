#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "spice/core/Spice.hpp"

#include <chrono>
#include <thread>

using namespace spice;

namespace {

auto make_session() -> core::Spice {
    core::Spice session { "Spice" };
    session.set_screen({ { 0, 0, 0 }, 80, 24 });
    return session;
}

}

TEST_CASE("core::Spice::name()") {
    auto sp { core::Spice("name") };
    CHECK_EQ(sp.name(), "name");
}

TEST_CASE("core::Spice opens the Welcome pane on request") {
    auto session { make_session() };
    uint32_t const id { session.open_welcome_pane() };
    CHECK_EQ(session.pane_count(), 1u);
    CHECK_EQ(session.focused_id(), id);
    REQUIRE(session.focused_pane());
    CHECK_EQ(session.focused_pane()->buffer()->name(), "Welcome");
    CHECK(session.focused_pane()->read_only()); // a greeting, not a scratchpad
    CHECK_EQ(session.pane_area(id), core::Rectangle { { 0, 0, 0 }, 80, 24 });
}

TEST_CASE("core::Spice::open_pane() splits the focused tile and focuses the new pane") {
    auto session { make_session() };
    uint32_t const first { session.open_welcome_pane() };
    auto buffer { session.create_buffer("scratch", core::BufferCapability::editable) };
    uint32_t const second { session.open_pane(core::PaneType::edit, buffer) };

    CHECK_EQ(session.pane_count(), 2u);
    CHECK_EQ(session.focused_id(), second);

    auto const first_area { session.pane_area(first) };
    auto const second_area { session.pane_area(second) };
    REQUIRE(first_area.has_value());
    REQUIRE(second_area.has_value());
    CHECK_NE(*first_area, *second_area);
}

TEST_CASE("core::Spice::open_pane() with an explicit orientation") {
    auto session { make_session() }; // 80x24
    uint32_t const first { session.open_welcome_pane() };
    auto buffer { session.create_buffer("s", core::BufferCapability::editable) };

    // stacked: 80x24 would normally split side by side
    uint32_t const below { session.open_pane(core::PaneType::edit, buffer, false) };
    CHECK_EQ(session.pane_area(first), core::Rectangle { { 0, 0, 0 }, 80, 12 });
    CHECK_EQ(session.pane_area(below), core::Rectangle { { 12, 0, 0 }, 80, 12 });

    // side by side: splits the focused (bottom) tile even though it is wide
    uint32_t const right { session.open_pane(core::PaneType::edit, buffer, true) };
    CHECK_EQ(session.pane_area(below), core::Rectangle { { 12, 0, 0 }, 40, 12 });
    CHECK_EQ(session.pane_area(right), core::Rectangle { { 12, 40, 0 }, 40, 12 });
}

TEST_CASE("core::Spice closing a pane keeps its buffer") {
    auto session { make_session() };
    session.open_welcome_pane();
    CHECK_EQ(session.buffers().size(), 1u);

    session.close_focused_pane();
    CHECK_EQ(session.pane_count(), 0u); // program should end now (README)
    CHECK_EQ(session.focused_id(), 0u);
    CHECK_EQ(session.buffers().size(), 1u); // Welcome buffer survives
}

TEST_CASE("core::Spice::pane_ids() lists every open pane") {
    auto session { make_session() };
    uint32_t const first { session.open_welcome_pane() };
    auto buffer { session.create_buffer("s", core::BufferCapability::editable) };
    uint32_t const second { session.open_pane(core::PaneType::edit, buffer) };
    auto const ids { session.pane_ids() };
    REQUIRE_EQ(ids.size(), 2u);
    CHECK_EQ(ids[0], first);
    CHECK_EQ(ids[1], second);
}

TEST_CASE("core::Spice the same buffer can be shown in two panes") {
    auto session { make_session() };
    auto buffer { session.create_buffer("shared", core::BufferCapability::editable, "hi") };
    uint32_t const a { session.open_pane(core::PaneType::edit, buffer) };
    uint32_t const b { session.open_pane(core::PaneType::edit, buffer) };
    CHECK_EQ(session.pane(a)->buffer().get(), session.pane(b)->buffer().get());
}

TEST_CASE("core::Spice::move_focus() crosses the split") {
    auto session { make_session() };
    uint32_t const first { session.open_welcome_pane() };
    auto buffer { session.create_buffer("s", core::BufferCapability::editable) };
    uint32_t const second { session.open_pane(core::PaneType::edit, buffer) };
    // 80x24 splits horizontally: first left, second right, second focused
    session.move_focus(core::Direction::left);
    CHECK_EQ(session.focused_id(), first);
    session.move_focus(core::Direction::right);
    CHECK_EQ(session.focused_id(), second);
    session.move_focus(core::Direction::right); // nothing there: focus stays
    CHECK_EQ(session.focused_id(), second);
}

TEST_CASE("core::Spice float and dock round-trip") {
    auto session { make_session() };
    uint32_t const first { session.open_welcome_pane() };
    auto buffer { session.create_buffer("s", core::BufferCapability::editable) };
    uint32_t const second { session.open_pane(core::PaneType::edit, buffer) };

    session.float_focused();
    auto const area { session.pane_area(second) };
    REQUIRE(area.has_value());
    CHECK_EQ(*area, core::Rectangle { { 6, 20, 0 }, 40, 12 }); // centered half-screen
    CHECK_EQ(session.pane_area(first), core::Rectangle { { 0, 0, 0 }, 80, 24 });

    session.dock_focused();
    CHECK_NE(*session.pane_area(second), core::Rectangle { { 6, 20, 0 }, 40, 12 });
    CHECK_EQ(session.pane_count(), 2u);
}

TEST_CASE("core::Spice::move_float() moves a floating pane") {
    auto session { make_session() };
    session.open_welcome_pane();
    auto buffer { session.create_buffer("f", core::BufferCapability::editable) };
    uint32_t const floating {
        session.open_float(core::PaneType::grid, buffer, { { 5, 5, 0 }, 20, 10 })
    };
    CHECK(session.move_float(floating, { { 1, 1, 0 }, 30, 8 }));
    CHECK_EQ(session.pane_area(floating), core::Rectangle { { 1, 1, 0 }, 30, 8 });
    CHECK_FALSE(session.move_float(9999, { { 0, 0, 0 }, 5, 5 }));
}

TEST_CASE("core::Spice::swap_panes() exchanges places and keeps focus by id") {
    auto session { make_session() };
    uint32_t const first { session.open_welcome_pane() };
    auto buffer { session.create_buffer("s", core::BufferCapability::editable) };
    uint32_t const second { session.open_pane(core::PaneType::edit, buffer) };
    auto const area_first { *session.pane_area(first) };
    auto const area_second { *session.pane_area(second) };

    CHECK(session.swap_panes(first, second));
    CHECK_EQ(session.pane_area(first), area_second);
    CHECK_EQ(session.pane_area(second), area_first);
    CHECK_EQ(session.focused_id(), second); // focus follows the pane, not the spot
    CHECK_FALSE(session.swap_panes(first, 9999));
}

TEST_CASE("core::Spice::resize_focused() reshapes the focused tile") {
    auto session { make_session() }; // 80x24
    uint32_t const first { session.open_welcome_pane() };
    auto buffer { session.create_buffer("s", core::BufferCapability::editable) };
    uint32_t const second { session.open_pane(core::PaneType::edit, buffer) };

    CHECK(session.resize_focused(6, 0)); // grow the focused (right) pane
    CHECK_EQ(session.pane_area(second)->width, 46u);
    CHECK_EQ(session.pane_area(first)->width, 34u);

    session.close_focused_pane();
    CHECK_FALSE(session.resize_focused(6, 0)); // a lone tile has no divider
}

TEST_CASE("core::Spice::is_floating()") {
    auto session { make_session() };
    uint32_t const tiled { session.open_welcome_pane() };
    auto buffer { session.create_buffer("f", core::BufferCapability::editable) };
    uint32_t const floating {
        session.open_float(core::PaneType::grid, buffer, { { 2, 2, 0 }, 10, 5 })
    };
    CHECK(session.is_floating(floating));
    CHECK_FALSE(session.is_floating(tiled));
}

TEST_CASE("core::Spice focusing a float raises it above the other floats") {
    auto session { make_session() };
    session.open_welcome_pane();
    auto buffer { session.create_buffer("f", core::BufferCapability::editable) };
    uint32_t const below {
        session.open_float(core::PaneType::grid, buffer, { { 3, 3, 0 }, 10, 5 })
    };
    uint32_t const above {
        session.open_float(core::PaneType::grid, buffer, { { 3, 3, 0 }, 10, 5 })
    };
    CHECK_EQ(session.pane_at({ 4, 4, 0 }), above); // newest float on top

    session.focus(below);
    CHECK_EQ(session.pane_at({ 4, 4, 0 }), below); // raised by focus
}

TEST_CASE("core::Spice::pane_at() prefers floats over tiles") {
    auto session { make_session() };
    uint32_t const tiled { session.open_welcome_pane() };
    auto buffer { session.create_buffer("f", core::BufferCapability::editable) };
    uint32_t const floating {
        session.open_float(core::PaneType::grid, buffer, { { 5, 5, 0 }, 20, 10 })
    };
    CHECK_EQ(session.pane_at({ 6, 6, 0 }), floating);
    CHECK_EQ(session.pane_at({ 0, 0, 0 }), tiled);
}

TEST_CASE("core::Spice runs a pty pane end to end") {
    auto session { make_session() };
    session.open_welcome_pane();

    uint32_t const id { session.open_pty_pane({ "/bin/sh", "-c", "printf 'pty-works\\n'" }) };
    REQUIRE_NE(id, 0u);
    REQUIRE(session.pane(id));
    CHECK_EQ(session.pane(id)->type(), core::PaneType::pty);
    CHECK_EQ(session.pane(id)->buffer()->capability(), core::BufferCapability::append_only);

    auto const buffer { session.pane(id)->buffer() };
    auto const deadline { std::chrono::steady_clock::now() + std::chrono::seconds(2) };
    bool seen { false };
    while (!seen && std::chrono::steady_clock::now() < deadline) {
        session.pump_ptys();
        for (uint32_t line { 0 }; line < buffer->line_count(); ++line) {
            if (buffer->line(line).find("pty-works") != std::string_view::npos) {
                seen = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(seen);

    // once the child exits, the scrollback gets the closing line and
    // writes are refused; the buffer itself survives
    bool exited { false };
    for (uint32_t line { 0 }; line < buffer->line_count(); ++line) {
        if (buffer->line(line) == "[exited]") {
            exited = true;
        }
    }
    CHECK(exited);
    CHECK_FALSE(session.write_to_pty(id, "x"));
}

TEST_CASE("core::Spice::open_pty_pane() cleans up when the spawn fails") {
    auto session { make_session() };
    session.open_welcome_pane();
    CHECK_EQ(session.open_pty_pane({}), 0u);
    CHECK_EQ(session.pane_count(), 1u); // no orphan pane left behind
}

TEST_CASE("core::Spice::draw() renders every pane into the grid") {
    auto session { make_session() };
    session.set_screen({ { 0, 0, 0 }, 20, 10 });
    session.open_welcome_pane();

    core::Grid grid { 20, 10 };
    core::Theme const theme;
    session.draw(grid, theme);

    CHECK_EQ(grid.char_at({ 0, 3, 0 }), "W"); // title bar " • Welcome"
    CHECK_EQ(grid.char_at({ 9, 0, 0 }), "╰"); // rounded bottom
    CHECK_EQ(grid.char_at({ 9, 19, 0 }), "╯");
}
