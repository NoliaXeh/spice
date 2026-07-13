#include "spice/core/FileIo.hpp"

#include <fstream>
#include <sstream>

namespace spice::core {

auto read_file(std::string const& path) -> std::optional<std::string> {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream content;
    content << file.rdbuf();
    if (file.bad()) {
        return std::nullopt;
    }
    std::string text { std::move(content).str() };
    if (text.ends_with('\n')) { // the trailing newline is ours to manage
        text.pop_back();
    }
    return text;
}

auto write_file(std::string const& path, Buffer const& buffer) -> bool {
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        return false;
    }
    for (uint32_t index { 0 }; index < buffer.line_count(); ++index) {
        file << buffer.line(index) << '\n';
    }
    file.flush();
    return file.good();
}

}
