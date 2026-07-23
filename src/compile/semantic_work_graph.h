// Dynamic command-local scheduling for semantic compiler products.
//
// Unlike base/WorkGraph, which executes a closed acyclic batch, this module
// owns the lifecycle of semantic products whose dependencies may be discovered
// while another product is checked. The compiler freezes one canonically
// ordered ready wave, lets each task write an isolated outcome, then publishes
// those outcomes in product-ID order. Publication may complete a product,
// diagnose it, suspend it on generated source, or attach newly discovered
// dependency IDs before the next wave.
//
// SemanticProductGraph owns only scheduling rows. Product payloads—parsed
// syntax, type facets, checked HIR, effects, MIR, and native fragments—live in
// phase-specific side tables indexed by SemanticProductId. No source pointer or
// payload lifetime is hidden here. IDs are stable only for one selected source
// graph in one compiler command and are never serialized or hashed.
//
// The graph is deliberately direct: flat vectors, explicit states, and frozen
// wave operations. It does not provide futures, callbacks, recursive demand
// evaluation, persistent caching, or concurrent mutation. Legal semantic
// cycles must be collapsed into an explicit SCC product by their owning phase;
// an uncollapsed dependency cycle is reported as a stalled graph.
//
// Relevant contracts: docs/specification/06-compiler.md,
// "Dependency-ordered elaboration", and
// docs/implementation/semantic-work-graph.md.

#pragma once

#include "source/diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// SemanticProductId is the stable zero-based index into one
// SemanticProductGraph. The invalid sentinel is used only for uninitialized
// caller fields; every published graph row has a value below products.size().
struct SemanticProductId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const {
    return value != std::numeric_limits<std::uint32_t>::max();
  }
  bool operator==(const SemanticProductId &) const = default;
};

// SemanticProductKind names the architectural fact produced by one task. The
// enum intentionally mirrors the target-state document so dumps and tests can
// identify missing phase boundaries without interpreting an opaque callback.
// A kind does not determine its payload representation; the owning compiler
// phase keeps that data in a typed side table.
enum class SemanticProductKind {
  TargetProfile,
  SourceGeneration,
  ParsedFile,
  PackageImports,
  PackageNameSet,
  PackageInterface,
  OpaqueSynthesisSet,
  ConstantValue,
  TypeIdentity,
  TypeMembers,
  TypeMemberTypes,
  TypeNaturalLayout,
  TypeAbiClassification,
  ProcedureTemplateBody,
  ProcedureInstanceBody,
  DirectEffectSummary,
  ClosedEffectScc,
  DenialResult,
  NativeReferenceSummary,
  ArtifactReachability,
  MirProcedure,
  PackageAssembly,
  PackageLlvmUnit,
  ArtifactLayout,
};

// SemanticProductState is the complete visible lifecycle of one immutable
// product. Waiting products have explicit prerequisites which are not complete.
// Running products belong to the currently frozen wave. WaitingForSynthesis is
// a semantic suspension, not an error: accepted ordinary source creates a
// successor source generation. Superseded identifies a product from an earlier
// source generation which is no longer selected; its immutable payload may
// remain inspectable but it cannot schedule consumers. DependencyFailed is
// terminal and identifies work which was correctly not invoked after a
// prerequisite failed.
enum class SemanticProductState {
  Waiting,
  Running,
  Complete,
  Error,
  DependencyFailed,
  WaitingForSynthesis,
  Superseded,
};

// One scheduling row owns a canonical dependency set and terminal failure
// reason. dependencies are sorted by ID and contain no duplicates or self-edge.
// failure is non-empty only for Error or DependencyFailed. The vector index is
// this row's SemanticProductId; kind and ID never change during the command.
struct SemanticProduct {
  SemanticProductKind kind = SemanticProductKind::ParsedFile;
  SemanticProductState state = SemanticProductState::Waiting;
  std::vector<SemanticProductId> dependencies;
  std::string failure;
};

// The graph is append-only. New products may be appended after workers finish
// a frozen wave and before their outcomes are published. Existing rows change
// only through the state transitions performed by the operations below.
struct SemanticProductGraph {
  std::vector<SemanticProduct> products;
};

// A frozen wave status distinguishes ordinary work from every no-work terminal
// condition. Ready owns at least one Running task. Complete means every product
// completed. WaitingForSynthesis means unfinished acyclic work is suspended on
// one or more synthesis products. Failed means at least one semantic product
// failed and its consumers were marked DependencyFailed. Stalled diagnoses an
// uncollapsed dependency cycle or a violated scheduler invariant.
enum class SemanticReadyWaveStatus {
  Ready,
  Complete,
  WaitingForSynthesis,
  Failed,
  Stalled,
};

struct SemanticReadyWave {
  SemanticReadyWaveStatus status = SemanticReadyWaveStatus::Stalled;
  std::vector<SemanticProductId> products;
  std::string failure;
};

// Workers return one of these four outcomes in their task-owned result slot.
// Blocked carries newly discovered product dependencies; it is valid only with
// a non-empty set. diagnostics owns the task's source-located messages and is
// merged by the coordinator in stable wave order, never completion order.
enum class SemanticProductOutcomeKind {
  Complete,
  Error,
  Blocked,
  WaitingForSynthesis,
};

struct SemanticProductOutcome {
  SemanticProductOutcomeKind kind = SemanticProductOutcomeKind::Complete;
  std::vector<SemanticProductId> dependencies;
  DiagnosticSink diagnostics;
  std::string failure;
};

// Appends one product in the caller's canonical semantic discovery order.
// dependencies may arrive in any order; this boundary sorts and deduplicates
// them so equivalent discoveries have one representation. On invalid IDs,
// self-dependency, or ID-domain exhaustion, no row is appended and reason
// explains the deterministic construction failure.
[[nodiscard]] SemanticProductId
append_semantic_product(SemanticProductGraph &graph, SemanticProductKind kind,
                        std::span<const SemanticProductId> dependencies,
                        std::string &reason);

// Appends an already complete eager input or product. Every dependency must be
// Complete. This is the explicit boundary used after target selection,
// workspace loading, and parsing have already produced immutable command
// inputs before semantic scheduling begins; semantic tasks must instead use a
// normal Waiting row and publish an outcome.
[[nodiscard]] SemanticProductId append_completed_semantic_product(
    SemanticProductGraph &graph, SemanticProductKind kind,
    std::span<const SemanticProductId> dependencies, std::string &reason);

// Marks products from an earlier selected source generation as Superseded.
// The whole set is validated before mutation. Running products cannot be
// superseded because their worker-owned outputs have not joined; input IDs and
// duplicates are accepted and canonicalized by the caller's semantic set.
[[nodiscard]] bool
supersede_semantic_products(SemanticProductGraph &graph,
                            std::span<const SemanticProductId> products,
                            std::string &reason);

// Freezes every currently ready product in ascending SemanticProductId order.
// Before selection, terminal dependency failures propagate through consumers.
// The operation refuses to freeze a second wave while any earlier product is
// Running. A Ready result changes exactly its returned rows to Running; every
// other status leaves no Running row behind.
[[nodiscard]] SemanticReadyWave
freeze_semantic_ready_wave(SemanticProductGraph &graph);

// Publishes one joined wave. outcomes[index] belongs to
// wave.products[index], irrespective of worker completion order. The operation
// validates the complete batch before changing graph state or appending task
// diagnostics. Blocked outcomes attach canonical new prerequisites and return
// their product to Waiting. Error outcomes require either an explicit failure
// or at least one error diagnostic. On contract failure the graph remains in
// its pre-publication Running state so the caller cannot mistake partial
// publication for semantic progress.
[[nodiscard]] bool
publish_semantic_ready_wave(SemanticProductGraph &graph,
                            const SemanticReadyWave &wave,
                            std::span<SemanticProductOutcome> outcomes,
                            DiagnosticSink &diagnostics, std::string &reason);

[[nodiscard]] std::string_view
semantic_product_kind_name(SemanticProductKind kind);

[[nodiscard]] std::string_view
semantic_product_state_name(SemanticProductState state);

} // namespace draft
