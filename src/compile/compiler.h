// Dependency-ordered orchestration of provider-free Draft compiler phases.

#pragma once

#include "backend/llvm_ir.h"
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
  bool lower_mir = false;
  bool emit_llvm = false;
};

// One row owns every representation of one package. Keeping phase products
// together makes driver commands thin and gives later manifests a single place
// to collect canonical inputs without rerunning semantic analysis.
struct CompiledPackage {
  PackageIdentity identity;
  SemanticAnalysisResult semantics;
  BodyCheckResult bodies;
  AgentMetadataResult metadata;
  EffectSummaryResult effects;
  PackageInterface interface;
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

} // namespace draft
