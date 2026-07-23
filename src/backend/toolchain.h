// Native artifact adapter for target-profiled objects, archives, and final
// links.
//
// This module receives a completely lowered Draft program, schedules its
// independent semantic-package modules and package-assembly objects, and
// invokes the remaining selected platform tools to materialize the requested
// artifact. Tool paths are operational configuration: they do not enter
// resolution manifests or synthesis identities. The adapter owns the exact
// argument/publication
// contract for each Draft target and keeps language semantics in MIR/LLVM
// lowering rather than inferring them from the host environment.
//
// Build options and results are command-owned values. The implementation owns
// temporary files and subprocess lifetimes only during a synchronous call; no
// LLVM handle, borrowed source view, or worker survives the return. This layer
// depends on lowered compiler products, target facts, backend input adapters,
// diagnostics, and base timing. No semantic layer may depend on it.
// Relevant specification: docs/specification/04-native-interop.md sections
// 11-12 and docs/specification/06-compiler.md, "Native lowering and summaries".

#pragma once

#include "backend/foreign_inputs.h"
#include "backend/llvm_object_emitter.h"
#include "backend/runtime_assets.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// NativeArtifactKind selects the publication contract, not a language type.
// Each value has one fixed output shape described by build_native_artifact;
// enum order is incidental and must not enter manifests or hashes.
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

// Ordinary native builds emit every artifact-layout LLVM unit through the
// library linked into draftc. ExternalClangOracle preserves the former
// subprocess path only so qualification tests can compile the exact same IR
// with an independent driver; it is not a user build mode or a second semantic
// backend.
enum class NativeObjectEmitter {
  InProcessLlvm,
  ExternalClangOracle,
};

// NativeBuildOptions is complete operational configuration for one synchronous
// build. String paths and operational input vectors are owned by the caller's
// value; timings is the sole non-owning pointer and must outlive the call. None
// of these fields is serialized as Draft semantic identity.
struct NativeBuildOptions {
  // Clang remains the platform linker driver and package-assembly tool. The
  // qualification emitter also uses this exact executable when selected. An
  // empty path selects clang from the linked LLVM distribution.
  std::string clang_path;
  // Requested Mach-O debug information is linked into a sibling dSYM bundle.
  // This path is unused when emit_debug_symbols is false.
  // Empty selects dsymutil from the linked LLVM distribution.
  std::string dsymutil_path;
  // The macOS host default is Apple's libtool, whose -D switch removes
  // timestamps and ownership. ELF selects deterministic LLVM ar; COFF selects
  // llvm-lib from the linked LLVM distribution.
  std::string archiver_path = "libtool";
  std::string build_directory;
  std::string output_path;
  NativeArtifactKind artifact_kind = NativeArtifactKind::Executable;
  // O0 is the fast-build default. O2 optimizes each complete semantic-package
  // module independently before native emission; it never changes semantic
  // scheduling, package granularity, source identity, or resolution pins.
  NativeOptimizationLevel optimization = NativeOptimizationLevel::O0;
  // Linked debug companions are deliberately opt-in. The compiled package
  // modules must have been constructed with matching debug information when
  // this is true. False is the ordinary fast-development path.
  bool emit_debug_symbols = false;
  NativeInstrumentationProfile instrumentation =
      NativeInstrumentationProfile::None;
  NativeObjectEmitter object_emitter = NativeObjectEmitter::InProcessLlvm;
  // Zero selects bounded hardware concurrency through WorkGraph. Embedding
  // tests and qualification oracles may request one worker without changing
  // task identity, object order, or any later publication step.
  std::size_t object_worker_count = 0;
  std::vector<ForeignProviderInput> foreign_providers;
  // Runtime assets are deployment roots returned to embedding callers and are
  // not passed to Clang or included in Draft source identity.
  std::vector<RuntimeAssetInput> runtime_assets;
  // Non-owning command diagnostic sink shared with semantic compilation.
  // Native timing includes parent wait time and, where the host exposes it,
  // separately reports user/system CPU consumed by child tools.
  TimingRecorder *timings = nullptr;
};

// NativeBuildResult describes only products published by this invocation. ok
// is false until the requested artifact is complete; callers must not consume
// output paths from a failed result. Operational evidence such as worker count
// and LLVM version remains outside program identity.
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
  // Requested Mach-O executable/dylib debug information publishes a sibling
  // dSYM; requested PE executable/DLL information publishes a deterministic
  // PDB. The fast path and every ELF/non-linked artifact leave this empty.
  // The compiler verifies requested companion structure but does not hash it.
  std::string debug_symbols_path;
  // A Windows DLL also publishes the import library required by an ordinary
  // statically linked C client. Other artifact kinds and targets leave these
  // fields empty. The library is a first-class output rather than an implicit
  // linker side effect, so callers can copy and verify the exact companion.
  std::string import_library_path;
  // Canonical physical roots checked for this invocation. This lets an
  // embedding build system deploy them without the compiler inventing a
  // target-specific output layout. Empty on failure.
  std::vector<VerifiedRuntimeAssetInput> runtime_assets;
};

// Emits each complete semantic-package module in-process and
// materializes the requested native artifact. Mach-O/ELF object mode performs
// a relocatable link over every layout object; COFF object mode requires one
// native input because COFF defines no partial link. Archive and dynamic modes
// retain every LLVM and package-assembly object; assembly mode produces a
// directory with one collision-free source per unit/input. Remaining host-tool
// arguments are passed directly to exec rather than through a shell. Runtime
// assets are validated as deployment inputs and returned to the caller, but
// are never linker operands. The linked LLVM version is reported for
// diagnostics/evidence and is not a semantic pin.
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
