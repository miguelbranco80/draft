// Relocatable dynamic-dependency validation for locked macOS toolchains.
//
// Hashing a toolchain directory is insufficient if one of its Mach-O tools can
// load a Homebrew or other host-local dylib outside that directory. This small
// parser follows the exact LC_LOAD_* closure without invoking `otool`, so the
// check itself introduces no additional executable or command-search input.

#pragma once

#include "source/diagnostic.h"

#include <filesystem>
#include <span>

namespace draft {

// Requires every entry to be a thin AArch64 Mach-O executable. Non-system
// dependencies must use a relocatable @rpath/@loader_path/@executable_path
// spelling, resolve to a regular file inside root, and recursively satisfy the
// same rule. Absolute Apple system-library, framework, and dynamic-loader paths
// are the only allowed dependencies outside root. Dylib IDs and runpaths
// inside the closure must also be relocatable, and embedded DYLD_* environment
// commands are forbidden.
[[nodiscard]] bool validate_macho_dependency_closure(
    const std::filesystem::path &root,
    std::span<const std::filesystem::path> entries,
    DiagnosticSink &diagnostics);

// Applies the same closed dependency policy to one or more dylibs that are
// distributed as compiler runtime inputs rather than executable tools. Each
// entry must be a thin AArch64 MH_DYLIB with a relocatable ID. Keeping this
// separate from the executable entry point prevents a runtime from passing
// validation merely because its dependency list happens to be acceptable.
[[nodiscard]] bool validate_macho_dylib_dependency_closure(
    const std::filesystem::path &root,
    std::span<const std::filesystem::path> entries,
    DiagnosticSink &diagnostics);

} // namespace draft
