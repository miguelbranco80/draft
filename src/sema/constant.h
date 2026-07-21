// Compile-time scalar values, target facts, constant declaration evaluation,
// and discovery of ready declaration-level `when` selections.
//
// This module is the first executable part of Draft's staged semantic dependency
// graph. It evaluates source expressions only when every referenced input is
// available, records successfully computed constants by stable SymbolId, and
// appends deterministic branch decisions without mutating syntax. A
// single-product entry point instead returns exact blockers and leaves branch
// publication to the semantic graph coordinator.
//
// The current value representation covers booleans, arbitrary-precision
// integers, exact decimal rationals, strings, typed categorical target values,
// and the built-in target object. The evaluator also interprets scalar procedure
// bodies,
// including bounded recursion, loops, switches, parametric values, and type/layout
// queries. Aggregate values, procedure identities, type values, and exact target
// rounding after each typed floating operation extend this same table before the
// semantic core is considered complete; unsupported values remain Pending and
// are never silently folded with host behavior.
//
// Relevant specification: docs/specification/01-core-language.md "Constants and compile-time
// evaluation" and "when"; docs/specification/02-types-memory-runtime.md "Target profile".

#pragma once

#include "sema/analyzer.h"
#include "sema/constant_value.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {

// A target profile names SIMD shapes explicitly.  The language does not infer
// legality from an LLVM type or from a coincidentally matching byte size: doing
// so would make the accepted language depend on backend implementation details.
// element is the canonical Draft scalar spelling (for example, "u32").
struct TargetSimdShape {
  std::string element;
  std::uint64_t lanes = 0;

  bool operator==(const TargetSimdShape &) const = default;
};

// TargetFacts is the semantic, LLVM-independent view of a selected profile.
// Categorical fields contain their source enum alternative name without a dot.
// known_features and features are in canonical bytewise order; the former is
// the architecture vocabulary and the latter its enabled subset. simd_shapes
// is sorted by element spelling and then lane count. This lets target checking
// reject a vector before an implementation-specific LLVM type is constructed.
struct TargetFacts {
  std::string identity;
  std::string arch;
  std::string os;
  std::string abi;
  std::string byte_order;
  std::string object_format;
  std::string file_tag;
  std::uint64_t pointer_bits = 0;
  std::uint64_t page_size = 0;
  std::vector<std::string> known_features;
  std::vector<std::string> features;
  std::vector<TargetSimdShape> simd_shapes;
};

struct ConstantBinding {
  ConstantBinding() = default;
  ConstantBinding(
      SymbolId symbol_value,
      ConstantValue constant_value,
      TypeId type_value = {})
      : symbol(symbol_value), value(std::move(constant_value)),
        type(type_value) {}

  SymbolId symbol;
  ConstantValue value;
  // The checked static type travels with the immutable value product. Relying
  // only on Symbol::type would let a stale private declaration snapshot erase
  // this fact before body checking consumes the constant.
  TypeId type;
};

// ConstantTable contains successfully evaluated values and their checked static
// types in stable SymbolId order. A canonical table owns every row in bindings.
// A procedure task instead uses an append-only overlay: it reads the immutable
// package prefix through base_ and owns only lexical `::` constants in bindings.
// The coordinator publishes that exact suffix after accepting the task result.
// A missing entry means not requested, not yet ready, or invalid; diagnostics
// and CompileTimeRoundResult distinguish those cases.
struct ConstantTable {
  ConstantTable() = default;

  std::vector<ConstantBinding> bindings;

  [[nodiscard]] const ConstantValue *find(SymbolId symbol) const;
  [[nodiscard]] const ConstantBinding *find_binding(SymbolId symbol) const;
  [[nodiscard]] std::size_t size() const;

  // Creates an empty append-only overlay over this canonical table. base must
  // outlive the overlay and may not itself be an overlay.
  [[nodiscard]] ConstantTable fork_append_only() const;
  [[nodiscard]] std::vector<ConstantBinding> appended_since(
      std::size_t base_size) const;
  void append_exact(
      std::size_t base_size, std::vector<ConstantBinding> appended);

  // Copies this complete visible sequence into destination while preserving
  // any binding already supplied there. Deferred generic recipes use this only
  // when they must materialize a temporary evaluator input; ordinary lookup
  // remains zero-copy through find_binding.
  void append_missing_bindings_to(ConstantTable &destination) const;

private:
  struct AppendOnlyOverlayTag {};
  ConstantTable(AppendOnlyOverlayTag, const ConstantTable &base);

  const ConstantTable *base_ = nullptr;
};

struct EvaluatedConstant {
  ConstantValue value;
  TypeId type;
};

// The common terminal/wait vocabulary for one independently scheduled
// compile-time value or branch choice. Blocked always carries explicit graph
// prerequisites; WaitingForSynthesis is the sole provider suspension state.
enum class CompileTimeProductStatus {
  Complete,
  Blocked,
  WaitingForSynthesis,
  Error,
};

// ConstantProductAttempt is task-owned. The caller supplies a private semantic
// package snapshot because successful evaluation may infer the root Symbol's
// type, intern an exact structural type value, or discover synthesis metadata.
// Only Complete may be published. Blocked separately names unfinished
// declaration classifications, constant values, and exact type facets. Its
// provisional diagnostics are discarded because a missing prerequisite may
// make an otherwise diagnostic expression valid; Error alone publishes the
// task-owned source diagnostics.
struct ConstantProductAttempt {
  CompileTimeProductStatus status = CompileTimeProductStatus::Error;
  std::optional<EvaluatedConstant> result;
  std::vector<SymbolId> declaration_dependencies;
  std::vector<SymbolId> constant_dependencies;
  std::vector<TypeFacetDependency> type_dependencies;
  std::vector<SymbolId> compile_time_procedures;
};

// IntegerExpressionProductAttempt is the task-owned result of evaluating one
// declaration-owned integer recipe, such as an array count, enum value, or
// `@align` argument. The recipe is deliberately part of its owning declaration
// product rather than a graph node per expression. Blocked names the exact
// declaration, constant, and type-facet products which must publish before the
// owner can be attempted again. Complete retains the interpreter's exact Draft
// type so the type resolver can distinguish, for example, usize from u64 even
// when the selected target gives them the same representation.
struct IntegerExpressionProductAttempt {
  CompileTimeProductStatus status = CompileTimeProductStatus::Error;
  std::optional<EvaluatedConstant> result;
  std::vector<SymbolId> declaration_dependencies;
  std::vector<SymbolId> constant_dependencies;
  std::vector<TypeFacetDependency> type_dependencies;
  // Every concrete procedure entered while evaluating the recipe, including
  // calls which completed against a provisional task snapshot. The enclosing
  // declaration resolver compares these IDs with its explicitly completed
  // graph prerequisites so an already-populated signature cannot hide an edge.
  std::vector<SymbolId> reached_procedures;
  std::vector<SymbolId> compile_time_procedures;
};

// ConditionalProductAttempt is the task-owned result for one package or member
// `when` site. Complete publishes selected_true. Blocked names every unfinished
// declaration classification, local ConstantValue, and exact type facet
// required by the condition. Waiting does not select either branch, and Error
// alone publishes the source-located condition diagnostic from the task sink.
struct ConditionalProductAttempt {
  CompileTimeProductStatus status = CompileTimeProductStatus::Error;
  bool selected_true = false;
  std::vector<SymbolId> declaration_dependencies;
  std::vector<SymbolId> constant_dependencies;
  std::vector<TypeFacetDependency> type_dependencies;
  std::vector<SymbolId> compile_time_procedures;
};

// Interface discovery is the only phase allowed to stop constant execution at
// a source synthesis site. Ordinary semantic analysis rejects that same site:
// by the time a complete program is checked, resolution must already have
// replaced it with ordinary Draft source. Keeping this as an enum prevents a
// permissive discovery rule from becoming an ambiguous boolean at call sites.
enum class CompileTimeSynthesisMode {
  Reject,
  Discover,
};

// A required expression inside a concrete parametric procedure may query the
// layout of a symbolic TypeParameter. This phase-local binding maps that unique
// parameter TypeId to its concrete instantiation. It is never serialized or
// added to the package TypeStore; both IDs already belong to that store.
struct ConstantTypeBinding {
  TypeId parameter;
  TypeId replacement;
};

// A concrete static argument pack has no ConstantValue or runtime TypeId. The
// constant evaluator nevertheless needs its exact length for len(pack) in
// `when`, static_assert, and lexical compile-time declarations. This narrow
// overlay exposes only that specified operation and lets every other attempted
// value use fail instead of masquerading as an array, string, or erased object.
struct ConstantStaticPackBinding {
  SymbolId binding;
  std::uint64_t length = 0;
};

struct CompileTimeRoundResult {
  ConstantTable constants;
  std::size_t new_selections = 0;
  std::size_t unresolved_conditionals = 0;
  // Package procedure bodies reached while evaluating constants and `when`
  // conditions. In Discover mode, the compiler body-checks exactly this set to
  // obtain typed synthesis obligations before dependent interface work
  // continues. IDs belong to the returned SemanticPackage round and must not
  // survive a clean semantic rebuild.
  std::vector<SymbolId> compile_time_procedures;
};

// Discovery result for one type/layout integer recipe. A blocked expression is
// not a failed constant: interface resolution must replace the direct site or
// the recorded procedure-body site before the recipe can be evaluated. The
// procedure IDs belong to the supplied SemanticPackage round.
struct CompileTimeExpressionDiscoveryResult {
  std::optional<EvaluatedConstant> value;
  bool blocked_by_synthesis = false;
  std::vector<SymbolId> compile_time_procedures;
};

// Evaluates package constants needed by visible declaration-level `when` sites.
// Ready boolean conditions append to selections; already selected sites are
// skipped. When diagnose_unready is false, missing dependencies remain pending
// for another semantic round. When true, every still-pending condition receives
// a source-located diagnostic, which terminates a no-progress fixed point.
[[nodiscard]] CompileTimeRoundResult evaluate_compile_time_round(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    ConditionalSelections &selections,
    CompileTimeSynthesisMode synthesis_mode,
    bool diagnose_unready,
    DiagnosticSink &diagnostics);

// Rechecks package `when` conditions and the complete named-constant set using
// only already published ConstantValue products. Missing local constants stay
// pending and receive the ordinary required-constant diagnostic; the validator
// never recursively computes a second copy of a product result.
[[nodiscard]] CompileTimeRoundResult validate_compile_time_products(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    ConditionalSelections &selections,
    const ConstantTable &published_constants,
    CompileTimeSynthesisMode synthesis_mode,
    bool diagnose_unready,
    DiagnosticSink &diagnostics);

// Evaluates exactly root. References to other local package constants consume
// published_constants or become explicit blockers; they are never evaluated
// recursively by this task. Imported ready constants remain ordinary immutable
// interface inputs. root must belong to task_package and denote Constant or
// UnresolvedDeclaration source. The function does not mutate published_constants.
[[nodiscard]] ConstantProductAttempt evaluate_package_constant_product(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &task_package,
    const TargetFacts &target,
    SymbolId root,
    const ConstantTable &published_constants,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics);

// Evaluates one integer-valued syntax site as part of root's declaration-type
// product. References to unfinished package declarations, unpublished local
// constants, or incomplete type facets are returned as explicit blockers. The
// function never recursively completes another graph product and publishes no
// mutation by itself. A blocked attempt contributes no diagnostics because its
// private evaluation may have observed only a provisional semantic snapshot.
[[nodiscard]] IntegerExpressionProductAttempt
evaluate_integer_expression_product(
    const SourceManager &sources, const LoadedPackage &loaded,
    SemanticPackage &task_package, const TargetFacts &target,
    const SyntaxTree &tree, NodeId expression, ScopeId scope, TypeId expected,
    SymbolId root, const ConstantTable &published_constants,
    CompileTimeSynthesisMode synthesis_mode, DiagnosticSink &diagnostics);

// Evaluates exactly one declaration/member `when` condition against published
// local constants and ready imported-interface values. It neither appends a
// ConditionalSelection nor materializes a branch. Missing local constants and
// type facets become explicit task blockers instead of recursive evaluation.
[[nodiscard]] ConditionalProductAttempt evaluate_conditional_product(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &task_package,
    const TargetFacts &target,
    const SemanticSite &site,
    const ConstantTable &published_constants,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics);

// Evaluates one required scalar expression in the already resolved semantic
// graph. Body-level compile-time intrinsics use the same evaluator as package
// constants and `when`, avoiding a second arithmetic or target-fact model.
// local_constants is an optional, caller-owned overlay for an instantiated
// procedure's compile-time value parameters. Overlay bindings take precedence
// over declarations. local_types performs the analogous substitution for
// layout queries such as `size_of(T)`. Both remain valid only for this call.
[[nodiscard]] std::optional<ConstantValue> evaluate_constant_expression(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    DiagnosticSink &diagnostics,
    const ConstantTable *local_constants = nullptr,
    const std::vector<ConstantTypeBinding> *local_types = nullptr,
    const std::vector<ConstantStaticPackBinding> *local_packs = nullptr);

// Typed form used by static storage and aggregate evaluation. Scalar callers
// that need only the mathematical value use evaluate_constant_expression.
[[nodiscard]] std::optional<EvaluatedConstant> evaluate_typed_constant_expression(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    DiagnosticSink &diagnostics,
    const ConstantTable *local_constants = nullptr,
    const std::vector<ConstantTypeBinding> *local_types = nullptr,
    TypeId expected = {},
    const std::vector<ConstantStaticPackBinding> *local_packs = nullptr);

// Non-diagnosing interface-discovery form for a required scalar expression.
// It uses the same interpreter as the rejecting API above, but returns the
// synthesis dependency instead of treating unresolved source as a constant
// evaluation error.
[[nodiscard]] CompileTimeExpressionDiscoveryResult
discover_typed_constant_expression(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    const ConstantTable *local_constants = nullptr,
    const std::vector<ConstantTypeBinding> *local_types = nullptr,
    TypeId expected = {},
    const std::vector<ConstantStaticPackBinding> *local_packs = nullptr);

} // namespace draft
