#ifndef SPICE_CORE_PATHCOMPLETION_H
#define SPICE_CORE_PATHCOMPLETION_H

#include <cstddef>
#include <string>
#include <vector>

namespace spice::core {

//! One completion candidate: the path as it should replace the query
//! (directory part included), and whether it is a directory.
struct PathEntry {
    std::string path;
    bool directory;

    auto operator==(PathEntry const&) const -> bool = default;
};

//! Path auto-completion for a partially typed `query`, relative to the
//! current directory unless the query is absolute. The query is split at
//! its last '/': the left side names the directory to list, the right side
//! is a (case-insensitive) prefix filter on its entries. ".." and "/" work
//! the way paths do. Hidden entries only show once the prefix starts with
//! a dot. Directories come first, each side sorted alphabetically.
auto complete_path(std::string const& query) -> std::vector<PathEntry>;

//! Recursive fuzzy file finder under the current directory: files whose
//! relative path contains the characters of `needle` in order
//! (case-insensitive), shortest paths first, at most `limit` results.
//! Hidden directories (.git and friends) are skipped, and scanning is
//! capped so a huge tree cannot hang the UI.
auto fuzzy_find_files(std::string const& needle, std::size_t limit) -> std::vector<std::string>;

}

#endif // SPICE_CORE_PATHCOMPLETION_H
