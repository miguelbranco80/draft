// Dependency-ordered orchestration of provider-free Draft compiler phases.

#pragma once

#include "assembly/aarch64.h"
#include "backend/llvm_ir.h"
#include "elaborator/obligation.h"
#include "elaborator/resolution.h"
#include "interop/native.h"
#include "mir/lower.h"
#include "sema/agent_metadata.h"
#include "sema/body_checker.h"
#include "sema/effect.h"
#include "sema/foreign_summary.h"
#include "sema/interface.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "validation/discovery.h"
#include "workspace/workspace.h"

#include <optional>
#include <string>
#include <vector>

namespace draft {

// Interface synthesis must be discovered before bodies that may name generated
// declarations or aggregate members. Complete performs every provider-free
// phase. DiscoverInterfaceSynthesis stops after dependency interfaces, type
// skeletons, constants, and early agent metadata are available; no body error
// can therefore preempt a declaration/member obligation that would repair it.
enum class CompileWorkspaceStage {
  Complete,
  DiscoverInterfaceSynthesis,
};

struct CompileWorkspaceOptions {
  TargetProfile target;
  WorkspaceLoadOptions workspace;
  AttachmentPolicy attachments;
  // Versioned identity of the compiler semantics and manifest algorithm. It is
  // a resolved-program input and must change when an implementation change can
  // alter accepted meaning or emitted behavior for the same other inputs.
  std::string compiler_content_identity = "draft-bootstrap-cpp-v78";
  CompileWorkspaceStage stage = CompileWorkspaceStage::Complete;
  bool lower_mir = false;
  bool emit_llvm = false;
  // Library and object artifacts lower a complete root without synthesizing a
  // hosted `main`. The root still owns runtime support in either mode.
  bool emit_program_entry = true;
  // Test and benchmark compilations select their otherwise excluded source
  // files and replace the hosted source main with a checked compiler harness.
  ValidationKind validation_kind = ValidationKind::None;
  // Already content-verified artifact-bound summaries. Paths are consumed by
  // the driver/backend verifier and never enter semantic package state.
  std::vector<ForeignProviderAudit> foreign_provider_audits;
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
  std::vector<AgentValidationContext> validation_context;
  EffectSummaryResult effects;
  PackageInterface interface;
  NativeInteropResult native_interop;
  AssemblyProgram assembly;
  MirLoweringResult mir;
  LlvmIrResult llvm;
};

struct CompileWorkspaceResult {
  bool ok = false;
  std::string compiler_content_identity;
  WorkspaceGraph graph;
  // Indices exactly match graph.packages. In Complete, a missing row means the
  // package failed before publishing an interface. Interface discovery may
  // also leave a consumer empty while a dependency has pending generated
  // declarations; ok remains true when every produced ready row is valid.
  std::vector<std::optional<CompiledPackage>> packages;
  // Present only when compile_workspace_with_resolution consumed and verified
  // a manifest. Keeping the exact snapshot beside the compiled graph prevents
  // native building from rereading a manifest that may have changed after
  // semantic checking. Direct compiler phases leave this empty.
  std::optional<ResolutionManifest> resolution_manifest;
  // Present for every successful resolved-program orchestration, including a
  // handwritten graph with no on-disk resolution manifest. Validation graphs
  // include their selected test or benchmark definitions in this identity.
  std::optional<Sha256Digest> resolved_program_digest;
  // Retained so the native adapter can prove that manifest ProviderSummary
  // rows were consumed by this exact semantic compilation.
  std::vector<ForeignProviderAudit> foreign_provider_audits;
  // Canonically ordered, target-checked procedures selected for a validation
  // command. These facts are also the semantic input to evidence generation.
  std::vector<ValidationEntry> validation_entries;
};

// Loads a closed workspace graph and checks dependencies before their consumers.
// emit_llvm implies MIR lowering. No provider, network, assembler, or linker is
// invoked by this function.
[[nodiscard]] CompileWorkspaceResult compile_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics);

// Enforces the agent boundary between a successfully checked surface graph and
// a successfully checked resolved graph. All surface synthesis must disappear;
// no expansion may contain another synthesis or introduce/displace a judgment.
// Input digests of retained judgments are not compared because generated
// declarations may legitimately change their visible semantic context.
[[nodiscard]] bool validate_resolved_agent_boundaries(
    const CompileWorkspaceResult &surface,
    const CompileWorkspaceResult &resolved,
    DiagnosticSink &diagnostics);

// Compiles the surface graph first to obtain stable typed obligations, consumes
// .draft/resolution.json when synthesis sites exist (or when a manifest is
// already present), constructs resolved source overrides, and compiles those
// complete files through the same pipeline. Missing, stale, corrupt, extra, or
// target-mismatched pins are errors. Generated source may not introduce a new
// synthesis or judgment site. A handwritten graph with no manifest or
// synthesis sites behaves exactly like compile_workspace.
//
// `draft resolve` uses the same two compiler stages through resolver.cpp so it
// can inspect missing/stale obligations before committing. Ordinary
// check/build/emit operations call this function and never contact a provider
// or modify the pin store.
[[nodiscard]] CompileWorkspaceResult compile_workspace_with_resolution(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics);

} // namespace draft
