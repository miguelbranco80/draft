// Exact physical toolchain and SDK selection for AArch64 macOS builds.
//
// The resolution manifest stores portable content identities, never host
// paths. A resolve invocation hashes explicit physical roots into those rows;
// a later locked build supplies roots again and verifies their complete trees
// before invoking a tool. This module is the sole mapping between those two
// representations for the first native target.

#pragma once

#include "elaborator/resolution.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <vector>

namespace draft {

struct LockedNativeInputRoots {
  std::filesystem::path toolchain_root;
  std::filesystem::path sdk_root;
};

struct VerifiedLockedNativeInputs {
  std::filesystem::path clang;
  std::filesystem::path linker;
  std::filesystem::path archiver;
  std::filesystem::path dsymutil;
  std::filesystem::path toolchain_root;
  std::filesystem::path sdk_root;
};

// Produces the complete canonical external-input set understood by the first
// native adapter. Both roots must be absolute real directories; symlinked
// parent components are canonicalized before hashing. The LLVM tree must
// contain executable bin/clang, bin/ld, bin/ld-classic, bin/llvm-ar, and
// bin/dsymutil entries. Apple ld delegates relocatable links to the colocated
// classic implementation. The last tool turns Mach-O debug maps into the
// conventional dSYM companion needed by debuggers and disassemblers.
[[nodiscard]] bool pin_locked_native_inputs(
    const LockedNativeInputRoots &roots,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics);

// Re-hashes roots and requires exact equality with the complete manifest set.
// Unknown, extra, or missing external rows are errors rather than ignored
// future semantics. Successful output contains only absolute verified paths.
[[nodiscard]] bool verify_locked_native_inputs(
    const LockedNativeInputRoots &roots,
    std::span<const ExternalInputPin> manifest_pins,
    VerifiedLockedNativeInputs &verified,
    DiagnosticSink &diagnostics);

} // namespace draft
