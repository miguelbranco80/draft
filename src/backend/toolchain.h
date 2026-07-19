// Host LLVM command adapter for AArch64 IR, objects, archives, and final links.
//
// This module receives a checked Draft program and invokes the selected host
// tools to materialize it. Tool paths are operational configuration: they do
// not enter resolution manifests or synthesis identities. The adapter owns the
// exact argument contract for each Draft target and keeps language semantics in
// MIR/LLVM lowering rather than inferring them from the host environment.

#pragma once

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

// Native instrumentation is deliberately typed and narrow. The backend never
// accepts caller-supplied Clang flags: each profile is a compiler-owned bundle
// of passes, code-generation options, link inputs, and deployment behavior.
enum class NativeInstrumentationProfile {
  None,
  AddressSanitizer,
};

struct NativeBuildOptions {
  std::string clang_path = "clang";
  // Final Mach-O executables and dylibs carry a debug map, while their linked
  // DWARF lives in a sibling dSYM bundle. ELF retains DWARF in the primary
  // artifact.
  std::string dsymutil_path = "dsymutil";
  // The macOS host default is Apple's libtool, whose -D switch removes
  // timestamps and ownership. ELF selects deterministic LLVM ar instead.
  std::string archiver_path = "libtool";
  std::string build_directory;
  std::string output_path;
  NativeArtifactKind artifact_kind = NativeArtifactKind::Executable;
  NativeInstrumentationProfile instrumentation =
      NativeInstrumentationProfile::None;
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
  // Mach-O executables and dynamic libraries have a conventional sibling dSYM.
  // The digest covers the path-stable bundle after removing dsymutil's
  // path-bearing relocation cache. ELF and other artifact kinds leave these
  // fields empty/zero because their DWARF remains in the primary artifact,
  // object members, or emitted assembly.
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
// shell. Runtime assets are verified as external program inputs and returned to
// the caller, but are never linker operands. The host toolchain version is
// reported for diagnostics and validation evidence; it is not a semantic pin.
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
