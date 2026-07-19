// Exact physical toolchain and system-root selection for AArch64 native builds.
//
// The resolution manifest stores portable content identities, never host
// paths. A resolve invocation hashes explicit physical roots into those rows;
// a later locked build supplies roots again and verifies their complete trees
// before invoking a tool. This module is the sole mapping between those two
// representations for the implemented AArch64 native targets.

#pragma once

#include "elaborator/resolution.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace draft {

// Physical roots supplied by one resolve or locked-build invocation. The
// toolchain root owns every executable and non-platform host dependency; the
// sdk_root name is retained for the public bootstrap API and represents either
// the macOS SDK or the GNU/Linux sysroot selected by the target profile. These
// paths never enter resolved-program identity.
struct LockedNativeInputRoots {
  std::filesystem::path toolchain_root;
  std::filesystem::path sdk_root;
};

// Absolute entry paths derived only after a complete tree and manifest match.
// Values are valid for the duration of the native build because the caller
// re-verifies both roots immediately before every child process. Optional
// instrumentation entries are populated only when the selected distribution
// has the complete qualified capability; ELF currently leaves them empty.
struct VerifiedLockedNativeInputs {
  std::filesystem::path clang;
  std::filesystem::path linker;
  std::filesystem::path archiver;
  std::filesystem::path dsymutil;
  std::filesystem::path toolchain_root;
  std::filesystem::path sdk_root;
  // The address-sanitizer runtime is an optional capability of a locked LLVM
  // distribution. Ordinary builds do not require it, but an instrumented
  // validation build may use only this already verified, content-pinned path.
  std::optional<std::filesystem::path> address_sanitizer_runtime;
  std::optional<std::filesystem::path> llvm_symbolizer;
};

// Produces the complete canonical external-input set for the selected target.
// Both roots must be absolute real directories and their complete trees are
// hashed. macOS requires clang, ld, ld-classic, llvm-ar, and dsymutil; Linux
// requires clang, ld.lld, and llvm-ar plus a glibc/GNU AArch64 sysroot.
[[nodiscard]] bool pin_locked_native_inputs(
    const TargetProfile &target,
    const LockedNativeInputRoots &roots,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics);

// Re-hashes roots and requires exact equality with the complete manifest set.
// Unknown, extra, or missing external rows are errors rather than ignored
// future semantics. Successful output contains only absolute verified paths;
// dsymutil is empty for an ELF target.
[[nodiscard]] bool verify_locked_native_inputs(
    const TargetProfile &target,
    const LockedNativeInputRoots &roots,
    std::span<const ExternalInputPin> manifest_pins,
    VerifiedLockedNativeInputs &verified,
    DiagnosticSink &diagnostics);

} // namespace draft
