// Dependency-ordered orchestration of provider-free Draft compiler phases.

#pragma once

#include "assembly/aarch64.h"
#include "backend/llvm_ir.h"
#include "elaborator/obligation.h"
#include "interop/native.h"
#include "mir/lower.h"
#include "sema/agent_metadata.h"
#include "sema/body_checker.h"
#include "sema/effect.h"
#include "sema/interface.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/workspace.h"

#include <optional>
#include <string>
#include <vector>

namespace draft {

struct CompileWorkspaceOptions {
  TargetProfile target;
  WorkspaceLoadOptions workspace;
  AttachmentPolicy attachments;
  // Versioned identity of the compiler semantics and manifest algorithm. It is
  // a resolved-program input and must change when an implementation change can
  // alter accepted meaning or emitted behavior for the same other inputs.
  std::string compiler_content_identity = "draft-bootstrap-cpp-v1";
  bool lower_mir = false;
  bool emit_llvm = false;
};

// Exact package assembly bytes are copied out of SourceManager when a package
// is compiled.  Native building is a later adapter call and deliberately does
// not reread the physical source path: the file could have changed between
// semantic checking and object emission, and generated assembly may have no
// physical path at all.  relative_name retains the selected target-qualified
// extension and is suitable for diagnostics and manifests.
struct CompiledAssemblySource {
  std::string relative_name;
  std::string contents;
};

// One row owns every representation of one package. Keeping phase products
// together makes driver commands thin and gives later manifests a single place
// to collect canonical inputs without rerunning semantic analysis.
struct CompiledPackage {
  PackageIdentity identity;
  std::vector<CompiledAssemblySource> assembly_sources;
  SemanticAnalysisResult semantics;
  BodyCheckResult bodies;
  AgentMetadataResult metadata;
  AgentObligationResult obligations;
  EffectSummaryResult effects;
  PackageInterface interface;
  NativeInteropResult native_interop;
  AssemblyProgram assembly;
  MirLoweringResult mir;
  LlvmIrResult llvm;
};

struct CompileWorkspaceResult {
  bool ok = false;
  WorkspaceGraph graph;
  // Indices exactly match graph.packages. A missing row means that package
  // failed before it could publish a usable semantic interface.
  std::vector<std::optional<CompiledPackage>> packages;
};

// Loads a closed workspace graph and checks dependencies before their consumers.
// emit_llvm implies MIR lowering. No provider, network, assembler, or linker is
// invoked by this function.
[[nodiscard]] CompileWorkspaceResult compile_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics);

// Compiles the surface graph first to obtain stable typed obligations, consumes
// .draft/resolution.json when synthesis sites exist (or when a manifest is
// already present), constructs resolved source overrides, and compiles those
// complete files through the same pipeline. Missing, stale, corrupt, extra, or
// target-mismatched pins are errors. Generated source may not introduce a new
// synthesis or judgment site. A handwritten graph with no manifest or
// synthesis sites behaves exactly like compile_workspace.
//
// `draft resolve` deliberately calls compile_workspace instead so it can inspect
// missing/stale surface obligations. Ordinary check/build/emit operations call
// this function and never contact a provider or modify the pin store.
[[nodiscard]] CompileWorkspaceResult compile_workspace_with_resolution(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics);

} // namespace draft
