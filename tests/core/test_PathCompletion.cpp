#include "doctest.h"

#include "spice/core/PathCompletion.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <unistd.h>

using namespace spice::core;
namespace fs = std::filesystem;

namespace {

//! A throwaway directory tree, entered for the test's duration:
//!   alpha.txt, beta.md, .hidden, sub/deep.txt, .git/secret.txt
struct TempTree {
    fs::path root;
    fs::path previous;

    TempTree() {
        previous = fs::current_path();
        root = fs::temp_directory_path() / std::format("spice-pathtest-{}", ::getpid());
        fs::remove_all(root);
        fs::create_directories(root / "sub");
        fs::create_directories(root / ".git");
        for (auto const& file :
             { "alpha.txt", "beta.md", ".hidden", "sub/deep.txt", ".git/secret.txt" }) {
            std::ofstream { root / file } << "x";
        }
        fs::current_path(root);
    }

    ~TempTree() {
        fs::current_path(previous);
        fs::remove_all(root);
    }
};

}

TEST_CASE("core::complete_path() lists the current directory, directories first") {
    TempTree tree;
    auto const entries { complete_path("") };
    REQUIRE_EQ(entries.size(), 3u); // sub/, alpha.txt, beta.md - hidden skipped
    CHECK_EQ(entries[0], PathEntry { "sub", true });
    CHECK_EQ(entries[1], PathEntry { "alpha.txt", false });
    CHECK_EQ(entries[2], PathEntry { "beta.md", false });
}

TEST_CASE("core::complete_path() filters on the typed prefix, case-insensitively") {
    TempTree tree;
    auto const entries { complete_path("AL") };
    REQUIRE_EQ(entries.size(), 1u);
    CHECK_EQ(entries[0], PathEntry { "alpha.txt", false });
    CHECK(complete_path("zz").empty());
}

TEST_CASE("core::complete_path() descends into a directory part") {
    TempTree tree;
    auto const entries { complete_path("sub/") };
    REQUIRE_EQ(entries.size(), 1u);
    CHECK_EQ(entries[0], PathEntry { "sub/deep.txt", false });
    CHECK_EQ(complete_path("sub/d")[0].path, "sub/deep.txt");
}

TEST_CASE("core::complete_path() walks up with .. and down from /") {
    TempTree tree;
    fs::current_path(tree.root / "sub");
    auto const up { complete_path("../al") };
    REQUIRE_EQ(up.size(), 1u);
    CHECK_EQ(up[0], PathEntry { "../alpha.txt", false });

    auto const absolute { complete_path("/") };
    CHECK_FALSE(absolute.empty()); // the filesystem root always has entries
    CHECK(absolute[0].path.starts_with("/"));
}

TEST_CASE("core::complete_path() shows hidden entries once the prefix says so") {
    TempTree tree;
    for (auto const& entry : complete_path("")) {
        CHECK_FALSE(entry.path.starts_with("."));
    }
    auto const hidden { complete_path(".h") };
    REQUIRE_EQ(hidden.size(), 1u);
    CHECK_EQ(hidden[0], PathEntry { ".hidden", false });
}

TEST_CASE("core::fuzzy_find_files() matches subsequences across the tree") {
    TempTree tree;
    auto const all { fuzzy_find_files("", 50) };
    REQUIRE_EQ(all.size(), 3u); // .git skipped, .hidden skipped
    CHECK_EQ(all[0], "beta.md"); // shortest first
    CHECK_EQ(all[1], "alpha.txt");
    CHECK_EQ(all[2], "sub/deep.txt");

    auto const deep { fuzzy_find_files("sdp", 50) }; // s..d..p in sub/deep
    REQUIRE_EQ(deep.size(), 1u);
    CHECK_EQ(deep[0], "sub/deep.txt");

    CHECK(fuzzy_find_files("zzz", 50).empty());
    CHECK_EQ(fuzzy_find_files("", 2).size(), 2u); // limit respected
}
