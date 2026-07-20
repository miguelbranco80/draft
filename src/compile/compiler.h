// Dependency-ordered orchestration of provider-free Draft compiler phases.
//
// The public records in this module are the command-lifetime ownership spine of
// the bootstrap compiler. A CompileWorkspaceResult owns one closed package
// graph and all declaration, type, HIR, effect, interop, MIR, and LLVM products
// produced for it. Source bytes remain in the caller-owned SourceManager, whose
// lifetime must enclose the result and every continuation or diagnostic render.
//
// Compilation is deterministic and provider-free. A dynamic semantic product
// graph orders target, source, parsed-file, package-name, synthesis, and package
// interface facts; dependencies publish before consumers. An unchanged source
// graph advances from interface discovery to semantic closure to target
// lowering. A checked source overlay appends a successor source generation and
// new declaration products only for affected packages, then reuses or extends
// unaffected body generations by exact work key. The continuation API mutates
// only this command-owned graph and creates no persistent cache. Lower layers
// never call a provider or update pins. Relevant specification: sections 10
// and 15.

#pragma once

#include "assembly/aarch64.h"
#include "backend/llvm_ir.h"
#include "compile/body_work.h"
#include "compile/configuration.h"
#include "compile/semantic_work_graph.h"
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
#include <cstdint>
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
  std::string compiler_content_identity = "draft-bootstrap-cpp-v140";
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

// PackageSemanticProgress is the authoritative per-package phase state. A
// source transition may leave one dependency at ClosureReady while rebuilding
// a changed consumer from InterfaceReady, so one workspace-wide progress enum
// cannot describe the rows accurately. InterfaceReady owns only declarations
// and the preliminary public interface. BodiesReady additionally owns one
// checked body result for the current declaration generation and exact external
// generic demand set. ClosureReady additionally owns effects, denials, agent
// obligations, native facts, and the completed public interface.
enum class PackageSemanticProgress {
  InterfaceReady,
  BodiesReady,
  ClosureReady,
};

// PackageBodyWorkKey names the complete inputs that make one retained body
// result authoritative. declaration_generation identifies the immutable local
// symbol/type baseline. procedure_demands is the canonical exact set of
// cross-package generic specializations required by checked consumers. A zero
// generation is the invalid sentinel; locally discovered specializations are a
// deterministic consequence of these inputs and do not need a second key.
struct PackageBodyWorkKey {
  std::uint64_t declaration_generation = 0;
  std::vector<ProcedureInstantiationDemand> procedure_demands;
};

// One row owns every representation of one package. Keeping phase products
// together makes driver commands thin and gives later manifests a single place
// to collect canonical inputs without rerunning semantic analysis.
struct CompiledPackage {
  PackageIdentity identity;
  std::vector<CompiledAssemblySource> assembly_sources;
  // declarations is the immutable baseline for declaration_generation. Body
  // checking receives it by const reference and owns all later semantic rows in
  // bodies; no continuation may append into this value.
  SemanticAnalysisResult declarations;
  // Incremented whenever this package's declaration baseline is rebuilt from a
  // new parsed source generation or changed dependency interface. Zero denotes
  // an uninitialized row and is never a successful package generation.
  std::uint64_t declaration_generation = 0;
  // Installed only with a successful BodyCheckResult. Its generation and
  // demands are compared together before any retained HIR is reused.
  PackageBodyWorkKey body_work_key;
  PackageSemanticProgress semantic_progress =
      PackageSemanticProgress::InterfaceReady;
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

// CompileWorkspaceProgress is the aggregate command boundary. It advances
// monotonically while source bytes are unchanged. A checked source transition
// deliberately returns it to InterfaceDiscovery; PackageSemanticProgress and
// PackageBodyWorkKey then say exactly which unaffected rows still own valid
// bodies or closure. Empty is a failed or not-yet-started result.
// InterfaceDiscovery may intentionally omit packages blocked by
// declaration/member synthesis. SemanticClosure owns complete checked
// declarations, bodies, effects, denials, and native-interop facts but no MIR.
// ValidationDiscovery additionally owns the canonical test or benchmark entry
// set but still has no target IR. TargetLowering owns every requested assembly
// program, MIR package, and LLVM module. The state is never serialized or
// cached; it exists so later command stages can continue the exact checked
// graph without guessing from empty output vectors.
enum class CompileWorkspaceProgress {
  Empty,
  InterfaceDiscovery,
  SemanticClosure,
  ValidationDiscovery,
  TargetLowering,
};

// Source replacements name their semantic invalidation boundary explicitly.
// Interface replacements may change declarations or compile-time interface
// decisions and therefore rebuild the changed package plus transitive
// consumers. Body replacements are grammar-confined statement/expression/
// assembly expansions: only their containing package needs new declaration IDs
// for the reparsed file, while effect/obligation closure is invalidated through
// consumers. Both forms still parse complete files and preserve topology.
enum class WorkspaceSemanticChange {
  Interface,
  Body,
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

// PackageSemanticProducts names the current source generation's eager file and
// import facts plus the two interface-stage semantic barriers for one package.
// parsed_files follow LoadedPackage::files after assembly-only rows are
// omitted. imports depends on those exact parsed-file products. name_set
// depends on the selected target, this package's eager inputs, and every
// imported package's current interface. package_interface then publishes or
// suspends the interface assembled while completing name_set.
//
// A checked source transition appends successor rows and replaces these IDs;
// earlier rows remain immutable and become Superseded. No ID is serialized or
// compared across commands.
struct PackageSemanticProducts {
  std::vector<SemanticProductId> parsed_files;
  SemanticProductId imports;
  SemanticProductId name_set;
  // Present only when this generation's name-set task discovers one opaque
  // declaration/member synthesis set. name_set blocks on this waiting row and
  // package_interface therefore cannot publish to consumers.
  SemanticProductId opaque_synthesis_set;
  SemanticProductId package_interface;
};

// WorkspaceSemanticProducts is the typed index from the general product graph
// back to workspace packages. target is fixed for the command.
// source_generation advances after each accepted in-memory source transition.
// package_by_product has exactly one row per SemanticProductGraph row; input
// products contain an invalid PackageId, while package-owned semantic tasks
// name their package. This direct parallel table avoids an O(products *
// packages) owner search in the coordinator and leaves future procedure/type
// payloads free to add their own typed side tables.
struct WorkspaceSemanticProducts {
  SemanticProductId target;
  SemanticProductId source_generation;
  std::vector<PackageSemanticProducts> packages;
  std::vector<PackageId> package_by_product;
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
  // Dynamic semantic scheduling state for the selected source generations.
  // Payloads remain in packages and later typed side tables; this graph owns
  // only stable command-local product IDs, dependencies, and lifecycle state.
  SemanticProductGraph semantic_graph;
  WorkspaceSemanticProducts semantic_products;
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
    WorkspaceSemanticChange change,
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
