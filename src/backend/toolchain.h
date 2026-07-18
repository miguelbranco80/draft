// Pinned LLVM command adapter for IR-to-object emission and Mach-O linking.

#pragma once

#include "backend/locked_inputs.h"
#include "backend/foreign_inputs.h"
#include "backend/runtime_assets.h"
#include "base/sha256.h"
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
  // Final Mach-O executables and dylibs carry a debug map, while their linked
  // DWARF lives in a sibling dSYM bundle. Locked builds ignore this path and
  // use the verified dsymutil from the pinned LLVM tree.
  std::string dsymutil_path = "dsymutil";
  // The supported host path is Apple's libtool, whose -D switch removes
  // timestamps and ownership from archives. Locked builds ignore this path
  // and use the verified LLVM ar from the pinned toolchain instead.
  std::string archiver_path = "libtool";
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
  // Runtime assets participate in resolved-program identity but are not passed
  // to Clang. A manifest-bearing build must supply its complete relocated set.
  std::vector<RuntimeAssetInput> runtime_assets;
};

struct NativeBuildResult {
  bool ok = false;
  std::string toolchain_version;
  std::string output_path;
  // Every native build emits a canonical operation-to-source sidecar in its
  // isolated build directory. Coverage and sampling tools can bind their data
  // to this digest without making a derived file part of program identity.
  std::string source_correlation_path;
  Sha256Digest source_correlation_digest;
  // Executables and dynamic libraries have a conventional sibling dSYM. The
  // digest covers the path-stable bundle after removing dsymutil's redundant
  // relocation cache, which embeds the physical binary path. Other artifact
  // kinds leave these fields empty/zero because their DWARF remains in object
  // members or emitted assembly.
  std::string debug_symbols_path;
  Sha256Digest debug_symbols_digest;
  // Canonical physical roots verified for the exact manifest rows. This lets
  // an embedding build system deploy them without the compiler inventing a
  // target-specific output layout. Empty on failure.
  std::vector<VerifiedRuntimeAssetInput> runtime_assets;
};

// Writes one LLVM module per compiled package and emits the requested native
// artifact. Object mode performs a relocatable link over every package object;
// archive and dynamic-library modes retain every package and package-assembly
// object; assembly mode produces a directory with one collision-free source per
// module/input. Arguments are passed directly to exec rather than through a
// shell. Locked mode additionally verifies exact trees and uses only the
// selected linker, SDK, and archiver. Runtime assets are verified as external
// identity inputs and returned to the caller, but are never linker operands.
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
