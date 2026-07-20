// Dependency-ordered orchestration of provider-free Draft compiler phases.
//
// The public records in this module are the command-lifetime ownership spine of
// the bootstrap compiler. A CompileWorkspaceResult owns one closed package
// graph and all declaration, type, HIR, effect, interop, MIR, and LLVM products
// produced for it. Source bytes remain in the caller-owned SourceManager, whose
// lifetime must enclose the result and every continuation or diagnostic render.
//
// Compilation is deterministic and provider-free. Dependencies publish before
// consumers where semantic interfaces require it; state advances monotonically
// from interface discovery to semantic closure to target lowering. The
// continuation API mutates only an existing successful graph and creates no
// persistent cache. Resolution may overlay checked generated Draft at the
// source boundary, but lower layers never call a provider or update pins.
// Relevant specification: sections 10 and 15.

#pragma once

#include "assembly/aarch64.h"
#include "backend/llvm_ir.h"
#include "compile/configuration.h"
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

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace draft {

class TimingRecorder;

// Interface synthesis must be discovered before bodies that may name generated
// declarations or aggregate members. Complete performs every provider-free
// phase. DiscoverInterfaceSynthesis stops after dependency interfaces, type
// skeletons, constants, and early agent metadata are available. It checks only
// the procedure bodies actually blocked by compile-time synthesis, so an
// unrelated runtime-body error cannot preempt an interface obligation that
// would repair the program.
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
  std::string compiler_content_identity = "draft-bootstrap-cpp-v139";
  // Explicit build-time language choices are kept together and included in
  // resolved-program identity. They are not inferred from host environment or
  // optimization level.
  CompileConfiguration configuration;
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
  // Optional command-owned diagnostic recorder. Timing never participates in
  // semantic configuration, resolved-program identity, or emitted artifacts.
  // The caller must keep it alive through this synchronous compilation.
  TimingRecorder *timings = nullptr;
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
  // True only after every retained test/benchmark context row was resolved
  // against a checked validation graph. Body-category source replacement
  // cannot change declarations, so this command-local fact survives that
  // transition and prevents a duplicate validation compilation.
  bool validation_context_is_typed = false;
  EffectSummaryResult effects;
  PackageInterface interface;
  NativeInteropResult native_interop;
  AssemblyProgram assembly;
  MirLoweringResult mir;
  LlvmIrResult llvm;
};

// One result advances monotonically through these command-local states. Empty
// is a failed or not-yet-started result. InterfaceDiscovery may intentionally
// omit packages blocked by declaration/member synthesis. SemanticClosure owns
// complete checked declarations, bodies, effects, denials, and native-interop
// facts but no MIR. ValidationDiscovery additionally owns the canonical test or
// benchmark entry set but still has no target IR. TargetLowering owns every
// requested assembly program, MIR package, and LLVM module. The state is never
// serialized or cached; it exists so later command stages can continue the
// exact checked graph without guessing from empty output vectors.
enum class CompileWorkspaceProgress {
  Empty,
  InterfaceDiscovery,
  SemanticClosure,
  ValidationDiscovery,
  TargetLowering,
};

// Command-lifetime derived index over one immutable WorkspaceGraph topology.
// Every outer adjacency vector is indexed by PackageId::value. Import-edge rows
// retain indices into WorkspaceGraph::imports so duplicate syntax occurrences
// remain visible to semantic import binding; reverse consumer rows are sorted
// and unique because source invalidation visits each importing package once.
// package_indices_by_identity is a sorted indirection for logarithmic semantic
// identity lookup without changing discovery-order PackageIds. The consumer-
// first topological order uses PackageId as its ready-set tie breaker and is
// reversed for dependency-first interface/effect publication.
//
// The index is built once with its graph and remains valid because checked
// complete-file source replacement is forbidden to change package/import
// topology. It owns only vectors of integers, contains no source or semantic
// objects, is copied with speculative resolver graphs, and is never serialized
// or reused across commands. valid is false only when construction saw a
// malformed edge; a cycle instead leaves consumer_first_order incomplete.
struct WorkspaceDependencyIndex {
  bool valid = true;
  std::vector<std::vector<std::size_t>> import_edges_by_consumer;
  std::vector<std::vector<std::size_t>> consumers_by_dependency;
  std::vector<std::size_t> package_indices_by_identity;
  std::vector<std::size_t> consumer_first_order;
};

struct CompileWorkspaceResult {
  bool ok = false;
  CompileWorkspaceProgress progress = CompileWorkspaceProgress::Empty;
  std::string compiler_content_identity;
  // The semantic target used for declarations, layouts, assembly validation,
  // and lowering. A continuation must supply this exact identity.
  std::string target_identity;
  CompileConfiguration configuration;
  // Validation source selection changes the checked package graph. A lowering
  // continuation must retain the same selection and may not reinterpret an
  // ordinary graph as a test or benchmark harness.
  ValidationKind validation_kind = ValidationKind::None;
  WorkspaceGraph graph;
  // Derived once from graph and retained through every continuation. Keeping
  // this index beside its owning topology avoids rebuilding or rescanning
  // package edges merely because the graph advances to another compiler phase.
  WorkspaceDependencyIndex dependencies;
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

// Minimal authored agent-site state retained while complete body source is
// replaced in the same semantic graph. Site identities are sorted and unique;
// no syntax, semantic package, or backend product is copied merely to validate
// the generated-source boundary after reanalysis.
struct ResolvedAgentBoundary {
  std::vector<std::string> judgment_sites;
};

// Loads a closed workspace graph and checks dependencies before their consumers.
// emit_llvm implies MIR lowering. No provider, network, assembler, or linker is
// invoked by this function.
[[nodiscard]] CompileWorkspaceResult compile_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics);

// Installs complete checked Draft files into one successful command-local
// graph, then rebuilds declaration/interface rows only for changed packages
// and their transitive import consumers. The workspace transition preserves
// package and import topology; unaffected dependencies retain their parsed and
// semantic state. Any previously completed body or lowering products become
// non-authoritative and progress returns to InterfaceDiscovery. The operation
// performs no filesystem load and creates no persistent compiler cache.
[[nodiscard]] bool apply_compiled_workspace_source_overrides(
    SourceManager &sources,
    const std::vector<WorkspaceSourceOverride> &overrides,
    CompileWorkspaceOptions options,
    CompileWorkspaceResult &compiled,
    DiagnosticSink &diagnostics);

// Advances a successful interface-discovery result through body checking,
// validation-context enrichment, effects, denials, and completed package
// interfaces without reloading or reanalyzing declarations. Every package must
// be ready: callers resolve declaration/member synthesis before crossing this
// boundary. root_package_directory is used only when synthesis context requests
// separately selected test or benchmark source.
[[nodiscard]] bool continue_compiled_workspace_semantics(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    CompileWorkspaceResult &compiled,
    DiagnosticSink &diagnostics);

// Continues one successful Complete result from SemanticClosure through
// validation discovery and requested target lowering, or from an already
// discovered validation graph through its later MIR/LLVM request. The function
// neither reloads source nor rebuilds declarations or bodies: every FileId,
// type, symbol, HIR node, and dependency edge remains owned by `compiled`.
// Calling it on an interface-stage, failed, already-lowered, differently
// targeted, or differently configured result is a diagnosed compiler-API error.
[[nodiscard]] bool continue_compiled_workspace(
    SourceManager &sources,
    const CompileWorkspaceOptions &options,
    CompileWorkspaceResult &compiled,
    DiagnosticSink &diagnostics);

// Captures and later enforces the authored agent boundary while the same graph
// is mutated to resolved source. All synthesis must disappear; no expansion
// may contain synthesis or introduce/displace a judgment. Judgment input
// digests are deliberately not compared because generated declarations may
// change their visible semantic context.
[[nodiscard]] ResolvedAgentBoundary capture_resolved_agent_boundary(
    const CompileWorkspaceResult &surface);

[[nodiscard]] bool validate_resolved_agent_boundaries(
    const ResolvedAgentBoundary &surface,
    const CompileWorkspaceResult &resolved,
    DiagnosticSink &diagnostics);

// Compiles the surface graph first to obtain stable typed obligations, consumes
// the selected root/target manifest when synthesis sites exist (or when one is
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
