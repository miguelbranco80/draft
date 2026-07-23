// Dependency-ordered orchestration of provider-free Draft compiler phases.
//
// The public records in this module are the command-lifetime ownership spine of
// the bootstrap compiler. A CompileWorkspaceResult owns one closed package
// graph and all declaration, type, HIR, effect, interop, MIR, and LLVM products
// produced for it. Source bytes remain in the caller-owned SourceManager, whose
// lifetime must enclose the result and every continuation or diagnostic render.
//
// Compilation is deterministic and provider-free. A dynamic semantic product
// graph orders target, source, parsed-file, package-name, declaration type,
// nominal and canonical generic layout, conditional choice, named-constant,
// synthesis, and package-interface facts; dependencies publish before
// consumers. Declaration, generic-owner, and constant workers evaluate one root
// against immutable published prerequisites; declaration and generic workers
// return append-only suffixes plus exact collected-row patches, while constant
// workers return task-owned values or selections. The coordinator publishes
// accepted results in product-ID order. An unchanged source
// graph advances from interface discovery to semantic closure to target
// lowering. A checked source overlay appends a successor source generation and
// new declaration products only for affected packages, then changes the
// selected set of immutable body products without rechecking completed work. The
// continuation API mutates only this command-owned graph and creates no
// persistent cache. Lower layers never call a provider or update pins. Relevant
// specification: sections 10 and 15.

#pragma once

#include "assembly/analyze.h"
#include "backend/llvm_ir.h"
#include "backend/llvm_object_emitter.h"
#include "compile/body_work.h"
#include "compile/configuration.h"
#include "compile/semantic_work_graph.h"
#include "elaborator/obligation.h"
#include "elaborator/resolution.h"
#include "interop/c_abi.h"
#include "interop/native.h"
#include "mir/mir.h"
#include "mir/native_reachability.h"
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
#include <limits>
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
  std::string compiler_content_identity = "draft-bootstrap-cpp-v144";
  // Explicit build-time language choices are kept together and included in
  // resolved-program identity. They are not inferred from host environment or
  // optimization level.
  CompileConfiguration configuration;
  CompileWorkspaceStage stage = CompileWorkspaceStage::Complete;
  bool lower_mir = false;
  bool emit_llvm = false;
  // A native-producing command can request that each package-owned LLVM task
  // immediately produce object or assembly bytes. O2 stays package-wide while
  // native-only O0 may use deterministic internal units. Those bytes remain
  // command-local derived products and let final artifact assembly avoid a
  // second broad LLVM phase after every package has finished lowering.
  bool emit_native_output = false;
  LlvmObjectEmissionOptions native_output;
  // Native debug information is opt-in so an ordinary development build does
  // not construct and lower source-location metadata. This affects derived
  // artifacts only and never enters semantic or resolved-program identity.
  bool emit_debug_information = false;
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
  // Bounds independent semantic work in one frozen ready wave. Zero selects
  // host hardware concurrency with a one-worker fallback; the executor caps it
  // to the wave size. This is scheduling policy only and must not alter semantic
  // identity, diagnostics, HIR, MIR, or emitted bytes. Interface products,
// declaration/type products, procedure bodies, and every later frozen semantic
// wave use the same bound.
  std::size_t semantic_worker_count = 0;
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

// One retained cross-package procedure demand and the exact owner-local body
// product which satisfies it. The portable demand remains the stable lookup
// key across later source selections in the same command. work_index addresses
// CompiledPackage::bodies.work/procedures and never changes while that package
// package interface remains selected. requester is the first completed body
// product which discovered this demand and becomes the new product's explicit
// graph prerequisite when materialization creates a new owner body. A row may
// be unselected; completed products remain immutable and become active again
// without rechecking if an equivalent demand returns.
struct ExternalProcedureBodyProduct {
  ProcedureInstantiationDemand demand;
  std::size_t work_index = 0;
  SemanticProductId requester;
};

// One compiler-owned native input in the exact order eventually presented to
// object emission and the platform linker. index addresses
// CompiledPackage::native_outputs for PackageLlvmUnit, is zero for the hosted
// runtime, and addresses CompiledPackage::assembly_sources for PackageAssembly.
// A root runtime row names the target product as producer; its immutable bytes
// live in the compiler distribution rather than this result. producer is
// otherwise the exact semantic product which made the input ready. Rows contain
// no physical paths and may be serialized or compared for command-local
// determinism.
enum class PackageArtifactInputKind {
  PackageLlvmUnit,
  HostedRuntime,
  PackageAssembly,
};

struct PackageArtifactInput {
  PackageArtifactInputKind kind = PackageArtifactInputKind::PackageLlvmUnit;
  std::size_t index = 0;
  SemanticProductId producer;
};

struct PackageArtifactLayout {
  bool ok = false;
  std::vector<PackageArtifactInput> inputs;
};

// PackageNativeOutput is the copyable terminal product of one package LLVM
// unit task. No LLVM context or module escapes the worker: bytes own exactly one
// object or assembly unit, while configuration records the choices used to
// create them so a later artifact request cannot silently consume a mismatched
// product. One package owns one unit for O2, assembly, and retained LLVM text;
// a native-only O0 object build may own several deterministic units. Phase
// timings are observations only and never enter identity.
struct PackageNativeOutput {
  bool ok = false;
  LlvmNativeOutputKind output_kind = LlvmNativeOutputKind::Object;
  NativeOptimizationLevel optimization = NativeOptimizationLevel::O0;
  LlvmNativeInstrumentation instrumentation =
      LlvmNativeInstrumentation::None;
  std::string bytes;
  LlvmObjectEmissionPhaseTimings phase_timings;
};

// One row owns every representation of one package. Keeping phase products
// together makes driver commands thin and gives later manifests a single place
// to collect canonical inputs without rerunning semantic analysis.
struct CompiledPackage {
  PackageIdentity identity;
  std::vector<CompiledAssemblySource> assembly_sources;
  // PackageNameSet publishes this terminal declaration/type payload before
  // named constants are ready. ConstantValue tasks fill published_constants;
  // PackageInterface consumes the record into declarations and clears it.
  PackageDeclarationDiscovery declaration_discovery;
  // declarations is the immutable baseline published by package_interface.
  // Body checking receives it by const reference and owns all later semantic
  // rows in bodies; no continuation may append into this value.
  SemanticAnalysisResult declarations;
  // Live command-local body publication state. Completed roots, their exact
  // product-local HIR, and the work-to-result order remain available when a
  // later consumer discovers another concrete dependency instance.
  PackageBodyWorkState bodies;
  // Every external procedure specialization completed for this declaration
  // generation. Selection is separate: selected_procedure_work contains the
  // canonical source-order closure of authored roots, current external roots,
  // and their discovered descendants. selected_external_procedure_work names
  // only roots requested by current consumers; a locally live specialization
  // need not be exported. Removing a consumer demand changes only these
  // selections and never reconstructs or mutates a completed body.
  std::vector<ExternalProcedureBodyProduct> external_procedure_products;
  std::vector<std::size_t> selected_procedure_work;
  std::vector<std::size_t> selected_external_procedure_work;
  // Complete target ABI facet for the source-semantic TypeId prefix. Rows are
  // published by TypeAbiClassification products after each body fixed point
  // and extend only when later command-local body work appends canonical types.
  // MIR reads this prefix immutably; compiler-only addresses carry their
  // pointee as MIR metadata instead of appending synthetic pointer TypeIds.
  CAbiTable c_abi;
  AgentMetadataResult metadata;
  AgentObligationResult obligations;
  std::vector<AgentValidationContext> validation_context;
  // True only after every retained test/benchmark context row was resolved
  // against a checked validation graph. Body-category source replacement
  // cannot change declarations, so this command-local fact survives that
  // transition and prevents a duplicate validation compilation.
  bool validation_context_is_typed = false;
  // Final consumer-local dependency contracts rebuilt from dependency
  // interfaces after their closure. This is separate from bodies.package:
  // procedure products keep addressing their original semantic generation and
  // no post-body pass clears or repopulates its imported contract tables.
  ImportedProcedureContracts imported_contracts;
  // Immutable body-local facts published before transitive call/value-flow
  // closure. This payload is kept separate so DirectEffectSummary products can
  // be inspected without confusing them with ClosedEffectScc results.
  DirectEffectSummaryResult direct_effects;
  EffectSummaryResult effects;
  PackageInterface interface;
  NativeInteropResult native_interop;
  // Native reference rows are produced for every checked concrete runtime
  // procedure before artifact roots are traversed. Global rows cover every
  // defined package global. The live vectors are the artifact projection:
  // bodies remain fully checked in bodies/procedure products even when absent
  // here, while MIR and LLVM consume only these exact rows.
  std::vector<NativeGlobalReferenceSummary> native_global_references;
  std::vector<std::size_t> native_live_body_work_indices;
  std::vector<SymbolId> native_live_globals;
  AssemblyProgram assembly;
  // emit-llvm and every package-wide emission mode retain one complete module
  // here for inspection. Native-only O0 object emission may instead construct
  // several private LLVM units and leave this result empty; procedure MIR
  // remains independently owned by semantic-product side-table rows in both
  // modes.
  LlvmIrResult llvm_module;
  // Canonical LLVM-unit order within this package. Each row is independently
  // emitted and later appears at the same index in artifact_layout. O2 always
  // has exactly one row so optimization sees the complete semantic package.
  std::vector<PackageNativeOutput> native_outputs;
  PackageArtifactLayout artifact_layout;
};

// CompileWorkspaceProgress is the aggregate command boundary. It advances
// monotonically while source bytes are unchanged. A checked source transition
// deliberately returns it to InterfaceDiscovery; exact per-package product IDs
// and payloads say which unaffected rows still own valid bodies or closure.
// Empty is a failed or not-yet-started result.
// InterfaceDiscovery may intentionally omit packages blocked by
// declaration/member synthesis. SemanticClosure owns complete checked
// declarations, bodies, effects, denials, and native-interop facts but no MIR.
// ValidationDiscovery additionally owns the canonical test or benchmark entry
// set but still has no target IR. TargetLowering owns every requested assembly
// program, procedure-owned MIR, package LLVM unit, and artifact layout. The
// state is never serialized or cached; it exists so later command stages can
// continue the exact checked graph without guessing from empty output vectors.
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

// GenericTypeDemandId is the stable command-local index into
// WorkspaceSemanticProducts::generic_type_demands. The invalid sentinel marks
// ordinary semantic products in the parallel product-to-demand table. It is
// never serialized and has no meaning in another compiler invocation.
struct GenericTypeDemandId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const {
    return value != std::numeric_limits<std::uint32_t>::max();
  }
  bool operator==(const GenericTypeDemandId &) const = default;
};

// GenericTypeDemand is one canonical concrete application owned by the package
// which defines its public parametric type. source and every TypeId inside
// arguments use that owner's append-only SemanticPackage tables. Equality of
// owner, source, and arguments is the complete command-local key; no digest or
// requester-local ID participates. product is a TypeNaturalLayout row because
// owner evaluation exists precisely to publish the concrete runtime shape.
//
// result is absent while the product is waiting and becomes one immutable,
// package-independent type graph at publication. Consumers import that graph
// into their private declaration attempts after the product completes. This
// avoids mutating a PackageInterface which was already published complete.
struct GenericTypeDemand {
  PackageId owner;
  SymbolId source;
  std::vector<ParametricArgument> arguments;
  SemanticProductId product;
  std::optional<InterfaceTypeGraph> result;
};

// PackageSemanticProducts names the current source generation's eager file and
// import facts, declaration products, and two interface-stage barriers for one
// package.
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
  // Immutable prerequisites shared by every declaration-stage product in this
  // source generation: target facts, this package's parsed/import rows, and the
  // imported package interfaces selected when the generation was created.
  // PackageNameSet begins with this exact set but later gains dynamic edges to
  // declarations, constants, layouts, and conditions. Retaining the original
  // inputs separately prevents a newly discovered product from inheriting those
  // downstream edges and manufacturing a dependency cycle.
  std::vector<SemanticProductId> declaration_inputs;
  SemanticProductId name_set;
  // One TypeMembers row per authored nominal aggregate. The row owns the
  // selected type scope, stable member SymbolIds, and source-order
  // AggregateMember packet. Its dependent declaration_types row owns the
  // declared member types.
  std::vector<SemanticProductId> type_members;
  // One declaration-type row per authored package symbol whose signature or
  // type must be completed. Nominal identities are eager; a nominal row
  // produces MemberTypes after its TypeMembers prerequisite. Other rows produce
  // TypeIdentity directly.
  std::vector<SemanticProductId> declaration_types;
  // One TypeNaturalLayout row per non-parametric authored nominal aggregate.
  // Symbolic templates have no single layout; their concrete applications are
  // owned by the generic-demand path. These products consume the nominal's
  // declaration_types row and may add edges to other layout rows.
  std::vector<SemanticProductId> natural_layouts;
  // One TypeAbiClassification row for every TypeId in the package TypeStore.
  // Declaration-baseline rows depend on package_interface; body-appended rows
  // depend on body_type_producer. The vector is TypeId-indexed and append-only
  // while the current package-interface product remains selected.
  std::vector<SemanticProductId> abi_classifications;
  // Canonical owner-evaluated concrete applications discovered anywhere in the
  // selected workspace. The vector belongs to the owner package and contains
  // each command-local key once; requester products depend on these rows.
  std::vector<SemanticProductId> generic_type_demands;
  // One compile-time branch-choice row per discovered package/member `when`.
  // New selected syntax may append more rows before name_set closes.
  std::vector<SemanticProductId> conditions;
  // One real ConstantValue product per final local Constant symbol, in stable
  // SymbolId order. Dependencies discovered by evaluation are graph edges;
  // this vector is only the typed package index used during publication.
  std::vector<SemanticProductId> constants;
  // Present only when this generation's name-set task discovers one opaque
  // declaration/member synthesis set. name_set blocks on this waiting row and
  // package_interface therefore cannot publish to consumers.
  SemanticProductId opaque_synthesis_set;
  SemanticProductId package_interface;
  // Every authored, nested, and concrete procedure body product materialized
  // for this source generation. Rows are append-only in deterministic discovery
  // order. selected_procedure_bodies is the current program projection in the
  // same order; removing an external demand changes that projection but leaves
  // the completed row here inspectable and reusable. Earlier-generation rows
  // become Superseded only when this package interface is replaced.
  std::vector<SemanticProductId> procedure_bodies;
  std::vector<SemanticProductId> selected_procedure_bodies;
  // One body-local DirectEffectSummary product per selected procedure, in the
  // same projection order. ClosedEffectScc rows align exactly with
  // CompiledPackage::effects.components. DenialResult rows again align with the
  // selected HIR-bearing procedure projection. effect_body_work_indices is
  // parallel to the direct and denial vectors and maps each row back to the
  // retained PackageBodyWorkState tables. A selected work row which owns no HIR
  // procedure has no effect/denial product. Replacing the projection supersedes
  // and clears all three product slices without touching reusable body products.
  std::vector<std::size_t> effect_body_work_indices;
  std::vector<SemanticProductId> direct_effect_summaries;
  std::vector<SemanticProductId> closed_effect_sccs;
  std::vector<SemanticProductId> denial_results;
  // Semantic closure publishes package_assembly over every checked body beside
  // direct effect discovery, and each concrete runtime body owns one
  // NativeReferenceSummary product. ArtifactReachability later selects
  // native_live_body_work_indices without changing the complete checked
  // body/effect sets above. Only those live rows map one-to-one to MirProcedure
  // products. Symbolic, compile-time-only, and artifact-dead bodies
  // intentionally have no runtime MIR product.
  SemanticProductId package_assembly;
  std::vector<std::size_t> checked_runtime_body_work_indices;
  std::vector<SemanticProductId> native_reference_summaries;
  std::vector<std::size_t> native_live_body_work_indices;
  std::vector<SymbolId> native_live_globals;
  std::vector<SemanticProductId> mir_procedures;
  // Each package_llvm_units row depends on only the live procedure MIR assigned
  // to that deterministic unit plus package declarations, target ABI facts,
  // and artifact reachability. O2 and retained-IR modes publish exactly one
  // row; native-only O0 object emission may publish several. artifact_layout is
  // the publication barrier over every unit and package assembly input.
  std::vector<SemanticProductId> package_llvm_units;
  SemanticProductId artifact_layout;
  // Parallel to the package TypeStore after at least one body wave.
  // Declaration-baseline types contain an invalid product because their
  // package interface is the producer barrier. A body-appended TypeId names the
  // exact procedure product whose deterministic publication first installed
  // it. Equal types reused by a later body keep the original producer.
  std::vector<SemanticProductId> body_type_producer;
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
  // One workspace-owned product closes all concrete direct native-reference
  // rows from command-selected artifact roots. It has no package owner because
  // cross-package procedure/global edges are its semantic purpose.
  SemanticProductId artifact_reachability;
  std::vector<PackageSemanticProducts> packages;
  std::vector<PackageId> package_by_product;
  // Parallel to SemanticProductGraph. Non-constant products contain an invalid
  // SymbolId; ConstantValue rows name their package-local root declaration.
  std::vector<SymbolId> constant_by_product;
  // Parallel to SemanticProductGraph. Declaration type rows name their stable
  // package symbol; other products contain an invalid SymbolId.
  std::vector<SymbolId> declaration_by_product;
  // Parallel to SemanticProductGraph. ProcedureTemplateBody,
  // ProcedureInstanceBody, DirectEffectSummary, DenialResult, and MirProcedure
  // rows name their exact package-local procedure symbol; every other product
  // contains an invalid SymbolId.
  std::vector<SymbolId> procedure_by_product;
  // Parallel to SemanticProductGraph. Type facet rows name their canonical
  // command-local TypeId; other products contain an invalid TypeId.
  std::vector<TypeId> type_by_product;
  // Parallel to SemanticProductGraph. Conditional value rows name exact parsed
  // syntax; other products contain an invalid SyntaxReference.
  std::vector<SyntaxReference> condition_by_product;
  // Parallel to SemanticProductGraph. Canonical owner-evaluated type products
  // name their GenericTypeDemand row; ordinary natural-layout products contain
  // an invalid ID.
  std::vector<GenericTypeDemandId> generic_type_demand_by_product;
  // Parallel to SemanticProductGraph. Completed NativeReferenceSummary rows
  // own one compact direct-reference payload; other rows are empty.
  std::vector<std::optional<NativeProcedureReferenceSummary>>
      native_reference_by_product;
  // Parallel to SemanticProductGraph. A completed MirProcedure row owns one
  // immutable procedure payload here; every other product has no value. This
  // is the authoritative MIR storage--packages retain only ordered product IDs
  // and never reconstruct a package-wide MirProgram.
  std::vector<std::optional<MirProcedure>> mir_procedure_by_product;
  // Append-only canonical demand table. Rows from superseded source generations
  // remain inspectable, while each PackageSemanticProducts vector names only
  // the currently selected rows.
  std::vector<GenericTypeDemand> generic_type_demands;
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
  // Complete artifact-rooted liveness result. Input rows remain in package and
  // semantic-product tables; these indices are retained for diagnostics,
  // timings, and deterministic qualification of checked-versus-emitted work.
  NativeReachabilityResult native_reachability;
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
