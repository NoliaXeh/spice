#include "spice/core/Layout.hpp"

#include <algorithm>
#include <utility>

namespace spice::core {

//! A leaf holds a pane; a split holds two children, an orientation and the
//! ratio of the space its first child takes (resizing adjusts it).
struct Layout::Node {
    uint32_t pane {};             //!< meaningful for leaves
    bool horizontal {};           //!< split orientation: children side by side
    float ratio { 0.5f };         //!< the first child's share of the split
    std::unique_ptr<Node> first;  //!< non-null means this is a split
    std::unique_ptr<Node> second;

    auto is_leaf() const -> bool { return first == nullptr; }
};

namespace {

//! Panes never shrink below this many cells on either axis.
constexpr uint32_t min_pane_cells { 3 };

auto has_leaf(Layout::Node const* node, uint32_t pane) -> bool;

//! The first child's share of `total` cells, clamped so both sides keep at
//! least the minimum (when there is room for it).
auto first_share(uint32_t total, float ratio) -> uint32_t {
    auto first { static_cast<uint32_t>(static_cast<float>(total) * ratio + 0.5f) };
    if (total >= 2 * min_pane_cells) {
        first = std::clamp(first, min_pane_cells, total - min_pane_cells);
    } else {
        first = std::min(first, total);
    }
    return first;
}

//! The two rectangles a split node divides `rect` into, covering it exactly.
auto split_rect(Rectangle rect, bool horizontal, float ratio)
    -> std::pair<Rectangle, Rectangle> {
    if (horizontal) {
        uint32_t const first_width { first_share(rect.width, ratio) };
        return {
            Rectangle { rect.position, first_width, rect.height },
            Rectangle {
                { rect.position.line, rect.position.column + first_width, 0 },
                rect.width - first_width, rect.height,
            },
        };
    }
    uint32_t const first_height { first_share(rect.height, ratio) };
    return {
        Rectangle { rect.position, rect.width, first_height },
        Rectangle {
            { rect.position.line + first_height, rect.position.column, 0 },
            rect.width, rect.height - first_height,
        },
    };
}

}

// out-of-line so Node's definition can stay in this translation unit
Layout::Layout() = default;
Layout::~Layout() = default;
Layout::Layout(Layout&&) = default;
auto Layout::operator=(Layout&&) -> Layout& = default;

namespace {

auto collect_tiles(
    Layout::Node const* node, Rectangle rect,
    std::vector<std::pair<uint32_t, Rectangle>>& out
) -> void {
    if (node == nullptr) {
        return;
    }
    if (node->is_leaf()) {
        out.emplace_back(node->pane, rect);
        return;
    }
    auto const [first, second] { split_rect(rect, node->horizontal, node->ratio) };
    collect_tiles(node->first.get(), first, out);
    collect_tiles(node->second.get(), second, out);
}

//! Adjusts the deepest split of `orientation` above `pane` so the pane's
//! side changes by `delta` cells. Deepest, because that split's divider is
//! the one adjacent to the pane - the divider the user means to move.
auto resize_in_tree(
    std::unique_ptr<Layout::Node> const& node, Rectangle rect,
    uint32_t pane, bool horizontal, int delta
) -> bool {
    if (node == nullptr || node->is_leaf()) {
        return false;
    }
    auto const [first_rect, second_rect] { split_rect(rect, node->horizontal, node->ratio) };
    if (resize_in_tree(node->first, first_rect, pane, horizontal, delta)
        || resize_in_tree(node->second, second_rect, pane, horizontal, delta)) {
        return true;
    }
    if (node->horizontal != horizontal) {
        return false;
    }
    bool const in_first { has_leaf(node->first.get(), pane) };
    if (!in_first && !has_leaf(node->second.get(), pane)) {
        return false;
    }

    uint32_t const total { horizontal ? rect.width : rect.height };
    if (total < 2 * min_pane_cells) {
        return false; // nothing to redistribute
    }
    int const current { static_cast<int>(horizontal ? first_rect.width : first_rect.height) };
    int const wanted { current + (in_first ? delta : -delta) };
    int const clamped { std::clamp(
        wanted, static_cast<int>(min_pane_cells),
        static_cast<int>(total - min_pane_cells)
    ) };
    node->ratio = static_cast<float>(clamped) / static_cast<float>(total);
    return true;
}

//! The leaf node holding `pane`, or nullptr.
auto find_leaf(std::unique_ptr<Layout::Node>& node, uint32_t pane)
    -> std::unique_ptr<Layout::Node>* {
    if (node == nullptr) {
        return nullptr;
    }
    if (node->is_leaf()) {
        return node->pane == pane ? &node : nullptr;
    }
    if (auto* found { find_leaf(node->first, pane) }) {
        return found;
    }
    return find_leaf(node->second, pane);
}

auto has_leaf(Layout::Node const* node, uint32_t pane) -> bool {
    if (node == nullptr) {
        return false;
    }
    if (node->is_leaf()) {
        return node->pane == pane;
    }
    return has_leaf(node->first.get(), pane) || has_leaf(node->second.get(), pane);
}

}

auto Layout::empty() const -> bool {
    return _root == nullptr && _floats.empty();
}

auto Layout::contains(uint32_t pane) const -> bool {
    return is_floating(pane) || has_leaf(_root.get(), pane);
}

auto Layout::is_floating(uint32_t pane) const -> bool {
    return std::ranges::any_of(_floats, [&](Float const& f) { return f.pane == pane; });
}

auto Layout::insert(uint32_t pane, uint32_t at, bool horizontal) -> bool {
    if (contains(pane)) {
        return false;
    }
    auto leaf { std::make_unique<Node>() };
    leaf->pane = pane;

    if (_root == nullptr) {
        _root = std::move(leaf);
        return true;
    }

    auto* target { find_leaf(_root, at) };
    if (target == nullptr) {
        return false;
    }
    auto split { std::make_unique<Node>() };
    split->horizontal = horizontal;
    split->first = std::move(*target);
    split->second = std::move(leaf);
    *target = std::move(split);
    return true;
}

auto Layout::remove(uint32_t pane) -> bool {
    auto const floating {
        std::ranges::find_if(_floats, [&](Float const& f) { return f.pane == pane; })
    };
    if (floating != _floats.end()) {
        _floats.erase(floating);
        return true;
    }

    auto* leaf { find_leaf(_root, pane) };
    if (leaf == nullptr) {
        return false;
    }
    if (leaf == &_root) {
        _root.reset();
        return true;
    }

    // graft the sibling onto the parent split's place. find_leaf gave us the
    // owning pointer, which lives inside the parent node; walk again to find
    // that parent so we can replace it wholesale.
    struct Walker {
        static auto parent_of(std::unique_ptr<Node>& node, Node const* child)
            -> std::unique_ptr<Node>* {
            if (node == nullptr || node->is_leaf()) {
                return nullptr;
            }
            if (node->first.get() == child || node->second.get() == child) {
                return &node;
            }
            if (auto* found { parent_of(node->first, child) }) {
                return found;
            }
            return parent_of(node->second, child);
        }
    };
    auto* parent { Walker::parent_of(_root, leaf->get()) };
    if (parent == nullptr) {
        return false;
    }
    auto sibling {
        (*parent)->first.get() == leaf->get()
            ? std::move((*parent)->second)
            : std::move((*parent)->first)
    };
    *parent = std::move(sibling);
    return true;
}

auto Layout::float_pane(uint32_t pane, Rectangle rect) -> bool {
    if (is_floating(pane)) {
        return false;
    }
    remove(pane); // take it out of the tree if it was tiled
    _floats.push_back(Float { pane, rect });
    return true;
}

auto Layout::dock_pane(uint32_t pane, uint32_t at, bool horizontal) -> bool {
    if (!is_floating(pane)) {
        return false;
    }
    std::erase_if(_floats, [&](Float const& f) { return f.pane == pane; });
    return insert(pane, at, horizontal);
}

auto Layout::move_float(uint32_t pane, Rectangle rect) -> bool {
    for (Float& f : _floats) {
        if (f.pane == pane) {
            f.rect = rect;
            return true;
        }
    }
    return false;
}

auto Layout::raise_float(uint32_t pane) -> bool {
    auto const found {
        std::ranges::find_if(_floats, [&](Float const& f) { return f.pane == pane; })
    };
    if (found == _floats.end()) {
        return false;
    }
    std::rotate(found, found + 1, _floats.end()); // to the back: topmost
    return true;
}

auto Layout::resize_pane(
    uint32_t pane, int width_delta, int height_delta, Rectangle screen
) -> bool {
    for (Float& f : _floats) { // floats resize their own rectangle
        if (f.pane != pane) {
            continue;
        }
        auto const grow = [](uint32_t size, int delta, uint32_t minimum) {
            int const wanted { static_cast<int>(size) + delta };
            return static_cast<uint32_t>(std::max(wanted, static_cast<int>(minimum)));
        };
        Rectangle resized { f.rect };
        resized.width = grow(f.rect.width, width_delta, 2 * min_pane_cells);
        resized.height = grow(f.rect.height, height_delta, min_pane_cells);
        bool const changed { resized != f.rect };
        f.rect = resized;
        return changed;
    }

    // tiles move the divider of the nearest split on each axis
    bool changed { false };
    if (width_delta != 0) {
        changed |= resize_in_tree(_root, screen, pane, true, width_delta);
    }
    if (height_delta != 0) {
        changed |= resize_in_tree(_root, screen, pane, false, height_delta);
    }
    return changed;
}

auto Layout::swap(uint32_t a, uint32_t b) -> bool {
    auto const slot_of = [this](uint32_t pane) -> uint32_t* {
        for (Float& f : _floats) {
            if (f.pane == pane) {
                return &f.pane;
            }
        }
        if (auto* leaf { find_leaf(_root, pane) }) {
            return &(*leaf)->pane;
        }
        return nullptr;
    };

    uint32_t* slot_a { slot_of(a) };
    uint32_t* slot_b { slot_of(b) };
    if (a == b || slot_a == nullptr || slot_b == nullptr) {
        return false;
    }
    std::swap(*slot_a, *slot_b);
    return true;
}

auto Layout::tiles(Rectangle screen) const -> std::vector<std::pair<uint32_t, Rectangle>> {
    std::vector<std::pair<uint32_t, Rectangle>> out;
    collect_tiles(_root.get(), screen, out);
    return out;
}

auto Layout::floats() const -> std::vector<std::pair<uint32_t, Rectangle>> {
    std::vector<std::pair<uint32_t, Rectangle>> out;
    out.reserve(_floats.size());
    for (Float const& f : _floats) {
        out.emplace_back(f.pane, f.rect);
    }
    return out;
}

auto Layout::pane_at(Rectangle screen, Position point) const -> std::optional<uint32_t> {
    for (auto it { _floats.rbegin() }; it != _floats.rend(); ++it) { // topmost first
        if (it->rect.contains(point)) {
            return it->pane;
        }
    }
    for (auto const& [pane, rect] : tiles(screen)) {
        if (rect.contains(point)) {
            return pane;
        }
    }
    return std::nullopt;
}

auto Layout::rect_of(Rectangle screen, uint32_t pane) const -> std::optional<Rectangle> {
    for (Float const& f : _floats) {
        if (f.pane == pane) {
            return f.rect;
        }
    }
    for (auto const& [id, rect] : tiles(screen)) {
        if (id == pane) {
            return rect;
        }
    }
    return std::nullopt;
}

auto Layout::neighbor(Rectangle screen, uint32_t pane, Direction direction) const
    -> std::optional<uint32_t> {
    auto const rect { rect_of(screen, pane) };
    if (!rect) {
        return std::nullopt;
    }

    // probe one cell beyond the pane's edge, level with its center
    // (upper-left biased so an even size doesn't spill into the next tile)
    Position probe {
        rect->position.line + (rect->height > 0 ? (rect->height - 1) / 2 : 0),
        rect->position.column + (rect->width > 0 ? (rect->width - 1) / 2 : 0),
        0,
    };
    switch (direction) {
        case Direction::left:
            if (rect->position.column == 0) return std::nullopt;
            probe.column = rect->position.column - 1;
            break;
        case Direction::right:
            probe.column = rect->position.column + rect->width;
            break;
        case Direction::up:
            if (rect->position.line == 0) return std::nullopt;
            probe.line = rect->position.line - 1;
            break;
        case Direction::down:
            probe.line = rect->position.line + rect->height;
            break;
    }

    for (auto const& [id, tile] : tiles(screen)) {
        if (id != pane && tile.contains(probe)) {
            return id;
        }
    }
    return std::nullopt;
}

}
