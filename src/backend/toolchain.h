// Pinned LLVM command adapter for IR-to-object emission and Mach-O linking.

#pragma once

#include "backend/locked_inputs.h"
#include "backend/foreign_inputs.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <string>
#include <string_view>
#include <vector>

namespace draft {

enum class NativeArtifactKind {
  Executable,
  Object,
  StaticLibrary,
  DynamicLibrary,
  Assembly,
};

[[nodiscard]] std::string_view native_artifact_kind_name(NativeArtifactKind kind);

struct NativeBuildOptions {
  std::string clang_path = "clang";
  std::string archiver_path = "ar";
  std::string build_directory;
  std::string output_path;
  NativeArtifactKind artifact_kind = NativeArtifactKind::Executable;
  // Development-only escape hatch. Release/locked builds must leave this false
  // so an ambient Apple Clang or another LLVM revision cannot alter artifacts.
  bool allow_unpinned_toolchain = false;
  // Locked mode ignores clang_path, requires a verified resolution-manifest
  // snapshot on the compiled result, and invokes only these explicit roots.
  // It also replaces the child environment and disables Clang configuration
  // discovery so host search paths cannot affect the artifact.
  bool locked = false;
  LockedNativeInputRoots locked_inputs;
  std::vector<ForeignProviderInput> foreign_providers;
};

struct NativeBuildResult {
  bool ok = false;
  std::string toolchain_version;
  std::string output_path;
};

// Writes one LLVM module per compiled package and emits the requested native
// artifact. Object mode performs a relocatable link over every package object;
// archive and dynamic-library modes retain every package and package-assembly
// object; assembly mode produces a directory with one collision-free source per
// module/input. Arguments are passed directly to exec rather than through a
// shell. Locked mode additionally verifies exact trees and uses only the
// selected linker, SDK, and archiver.
[[nodiscard]] NativeBuildResult build_native_artifact(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    DiagnosticSink &diagnostics);

// Compatibility spelling for callers that specifically request the default
// executable kind.
[[nodiscard]] NativeBuildResult build_native_executable(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
