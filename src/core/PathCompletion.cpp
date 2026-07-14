#include "spice/core/PathCompletion.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

namespace {

namespace fs = std::filesystem;

//! How many directory entries a fuzzy scan may visit before giving up:
//! completeness is not worth freezing the UI on a huge tree.
constexpr std::size_t scan_limit { 10000 };

auto lowered(std::string_view text) -> std::string {
    std::string out { text };
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

auto starts_with_insensitive(std::string_view text, std::string const& prefix_lower) -> bool {
    return lowered(text.substr(0, prefix_lower.size())) == prefix_lower;
}

//! Every character of `needle_lower` appears in `haystack`, in order.
auto fuzzy_match(std::string const& needle_lower, std::string_view haystack) -> bool {
    std::string const hay { lowered(haystack) };
    size_t at { 0 };
    for (char const wanted : needle_lower) {
        at = hay.find(wanted, at);
        if (at == std::string::npos) {
            return false;
        }
        ++at;
    }
    return true;
}

auto hidden(fs::path const& path) -> bool {
    std::string const name { path.filename().string() };
    return !name.empty() && name.front() == '.';
}

}

namespace spice::core {

auto complete_path(std::string const& query) -> std::vector<PathEntry> {
    // split at the last '/': directory to list + prefix to filter on
    size_t const slash { query.rfind('/') };
    std::string const dir_part { slash == std::string::npos ? "" : query.substr(0, slash + 1) };
    std::string const prefix { slash == std::string::npos ? query : query.substr(slash + 1) };
    std::string const prefix_lower { lowered(prefix) };
    bool const want_hidden { !prefix.empty() && prefix.front() == '.' };

    fs::path const listing { dir_part.empty() ? "." : dir_part };
    std::error_code error;
    std::vector<PathEntry> entries;

    for (auto const& entry : fs::directory_iterator { listing, error }) {
        std::string const name { entry.path().filename().string() };
        if (!starts_with_insensitive(name, prefix_lower)) {
            continue;
        }
        if (!want_hidden && !name.empty() && name.front() == '.') {
            continue;
        }
        entries.push_back(PathEntry {
            dir_part + name,
            entry.is_directory(error),
        });
    }

    std::ranges::sort(entries, [](PathEntry const& a, PathEntry const& b) {
        if (a.directory != b.directory) {
            return a.directory; // directories first
        }
        return lowered(a.path) < lowered(b.path);
    });
    return entries;
}

auto fuzzy_find_files(std::string const& needle, std::size_t limit) -> std::vector<std::string> {
    std::string const needle_lower { lowered(needle) };
    std::vector<std::string> found;
    std::size_t scanned { 0 };

    std::error_code error;
    fs::recursive_directory_iterator it {
        ".", fs::directory_options::skip_permission_denied, error
    };
    for (; !error && it != fs::recursive_directory_iterator {}; it.increment(error)) {
        if (++scanned > scan_limit) {
            break;
        }
        if (it->is_directory(error)) {
            if (hidden(it->path())) {
                it.disable_recursion_pending(); // don't descend into .git etc.
            }
            continue;
        }
        if (hidden(it->path()) || !it->is_regular_file(error)) {
            continue;
        }
        std::string path { it->path().lexically_relative(".").string() };
        if (fuzzy_match(needle_lower, path)) {
            found.push_back(std::move(path));
        }
    }

    std::ranges::sort(found, [](std::string const& a, std::string const& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    if (found.size() > limit) {
        found.resize(limit);
    }
    return found;
}

}
