#ifndef SPICE_CORE_FILEIO_H
#define SPICE_CORE_FILEIO_H

#include "spice/core/Buffer.hpp"
#include <optional>
#include <string>

namespace spice::core {

//! The whole file as a string, or nothing if it cannot be read.
//! (File IO belongs to the Files plugin eventually - see README - these
//! free functions are the built-in stopgap and its future backend.)
auto read_file(std::string const& path) -> std::optional<std::string>;

//! Writes the buffer's lines joined with newlines (plus a trailing one,
//! POSIX style). False when the file cannot be written.
auto write_file(std::string const& path, Buffer const& buffer) -> bool;

}

#endif // SPICE_CORE_FILEIO_H
