// Pinned LLVM command adapter for IR-to-object emission and Mach-O linking.

#pragma once

#include "backend/locked_inputs.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <string>

namespace draft {

struct NativeBuildOptions {
  std::string clang_path = "clang";
  std::string build_directory;
  std::string output_path;
  // Development-only escape hatch. Release/locked builds must leave this false
  // so an ambient Apple Clang or another LLVM revision cannot alter artifacts.
  bool allow_unpinned_toolchain = false;
  // Locked mode ignores clang_path, requires a verified resolution-manifest
  // snapshot on the compiled result, and invokes only these explicit roots.
  // It also replaces the child environment and disables Clang configuration
  // discovery so host search paths cannot affect the artifact.
  bool locked = false;
  LockedNativeInputRoots locked_inputs;
};

struct NativeBuildResult {
  bool ok = false;
  std::string toolchain_version;
  std::string output_path;
};

// Writes one LLVM module per compiled package, emits one object per module, and
// links them into an AArch64 macOS executable. Arguments are passed directly to
// exec rather than through a shell. The normal path accepts only LLVM/Clang
// 22.1.x, matching the target contract in IMPLEMENTATION_PLAN.md. Locked mode
// additionally verifies exact trees and supplies an explicit SDK and linker.
[[nodiscard]] NativeBuildResult build_native_executable(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
