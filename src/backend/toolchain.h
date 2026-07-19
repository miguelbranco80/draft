// Native artifact adapter for AArch64 objects, archives, and final links.
//
// This module receives a completely lowered Draft program, schedules its
// independent package objects through embedded LLVM, and invokes the remaining
// selected platform tools to materialize the requested artifact. Tool paths are
// operational configuration: they do not enter resolution manifests or
// synthesis identities. The adapter owns the exact argument/publication
// contract for each Draft target and keeps language semantics in MIR/LLVM
// lowering rather than inferring them from the host environment.

#pragma once

#include "backend/foreign_inputs.h"
#include "backend/runtime_assets.h"
#include "base/sha256.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <cstddef>
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

// Ordinary native builds emit package modules through the LLVM library linked
// into draftc. ExternalClangOracle preserves the former subprocess path only so
// qualification tests can compile the exact same IR with an independent driver;
// it is not a user build mode or a second semantic backend.
enum class NativeObjectEmitter {
  InProcessLlvm,
  ExternalClangOracle,
};

struct NativeBuildOptions {
  // Clang remains the platform linker driver and package-assembly tool. The
  // qualification emitter also uses this exact executable when selected. An
  // empty path selects clang from the linked LLVM distribution.
  std::string clang_path;
  // Final Mach-O executables and dylibs carry a debug map, while their linked
  // DWARF lives in a sibling dSYM bundle. ELF retains DWARF in the primary
  // artifact.
  // Empty selects dsymutil from the linked LLVM distribution.
  std::string dsymutil_path;
  // The macOS host default is Apple's libtool, whose -D switch removes
  // timestamps and ownership. ELF selects deterministic LLVM ar instead.
  std::string archiver_path = "libtool";
  std::string build_directory;
  std::string output_path;
  NativeArtifactKind artifact_kind = NativeArtifactKind::Executable;
  NativeInstrumentationProfile instrumentation =
      NativeInstrumentationProfile::None;
  NativeObjectEmitter object_emitter = NativeObjectEmitter::InProcessLlvm;
  // Zero selects bounded hardware concurrency through WorkGraph. Embedding
  // tests and qualification oracles may request one worker without changing
  // task identity, object order, or any later publication step.
  std::size_t object_worker_count = 0;
  std::vector<ForeignProviderInput> foreign_providers;
  // Runtime assets participate in resolved-program identity but are not passed
  // to Clang. A manifest-bearing build must supply its complete relocated set.
  std::vector<RuntimeAssetInput> runtime_assets;
  // Non-owning command diagnostic sink shared with semantic compilation.
  // Native timing includes parent wait time and, where the host exposes it,
  // separately reports user/system CPU consumed by child tools.
  TimingRecorder *timings = nullptr;
};

struct NativeBuildResult {
  bool ok = false;
  // Number of workers selected for native object tasks. This operational
  // evidence is zero when the build fails before scheduling and never enters
  // program or artifact identity.
  std::size_t object_workers_used = 0;
  // Exact LLVM distribution linked into draftc. This is constant build
  // evidence and no runtime `clang --version` process is launched to obtain it.
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

// Emits each package module in-process and materializes the requested native
// artifact. Object mode performs a relocatable link over every package object;
// archive and dynamic-library modes retain every package and package-assembly
// object; assembly mode produces a directory with one collision-free source per
// module/input. Remaining host-tool arguments are passed directly to exec rather
// than through a shell. Runtime assets are verified as external program inputs
// and returned to the caller, but are never linker operands. The linked LLVM
// version is reported for diagnostics/evidence and is not a semantic pin.
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
