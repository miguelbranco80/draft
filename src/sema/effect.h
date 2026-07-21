// Procedure effect, concrete flow-graph, and SCC summaries for denials.
//
// Denials are transitive: a region denying `assert`, assembly, unchecked access,
// or a package global must also reject a helper that reaches that entity. This
// pass walks checked HIR, records direct effects and concrete finite callees,
// identifies strongly connected components in that graph, and closes those
// components in dependency-first order. Unknown procedure-pointer or bodyless
// calls stay explicit instead of being assumed harmless.
//
// These summaries are semantic contracts, not optimization call graphs. Later
// package-interface and link composition extends the same rows with imported
// audited summaries and procedure-pointer flow slots. No LLVM fact is used.
//
// Relevant specification: docs/specification/05-denials-validation.md section 13 and
// docs/specification/06-compiler.md, "Native lowering and summaries".

#pragma once

#include "sema/analyzer.h"
#include "sema/body_checker.h"
#include "sema/foreign_summary.h"
#include "sema/hir.h"
#include "target/profile.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace draft {

struct SemanticEffect;

// One path-shaped input slot for a procedure value. Parameter slots begin at a
// zero-based formal parameter and then select named aggregate fields. Context
// slots use the same field path against the hidden runtime Context instead.
struct ProcedureFlowSlot {
  std::uint32_t parameter = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::string> path;
  bool context = false;

  bool operator==(const ProcedureFlowSlot &) const = default;
};

// One conservative value set for a procedure-typed expression. targets are
// statically named procedures; flow_slots identify typed inputs whose eventual
// values flow here. unknown is sticky when type erasure, an uninitialized
// value, or an unsupported storage path can contribute another target. Vectors
// retain deterministic semantic order.
struct ProcedureValueSummary {
  std::vector<SymbolId> targets;
  std::vector<ProcedureFlowSlot> flow_slots;
  // Imported returned procedures may not have a consumer-local SymbolId. Their
  // exact call contract is retained directly and composed when the value is
  // eventually called. FlowCall rows here refer to that returned procedure's
  // own arguments, not to the factory procedure that returned it.
  std::vector<SemanticEffect> contract_effects;
  bool unknown = false;

  bool operator==(const ProcedureValueSummary &) const;
};

// A call argument can contain several procedure leaves. Each row is relative
// to the argument root, so a callee slot `allocator.procedure` performs one
// exact lookup without treating the rest of the aggregate as type-erased.
struct ProcedureFieldValueSummary {
  std::vector<std::string> path;
  ProcedureValueSummary value;

  bool operator==(const ProcedureFieldValueSummary &) const = default;
};

struct ProcedureArgumentSummary {
  std::vector<ProcedureFieldValueSummary> fields;

  bool operator==(const ProcedureArgumentSummary &) const = default;
};

// SemanticEffect identifies one reachable entity. symbol is meaningful for a
// package global; text names a context field or gives a stable reason for an
// unknown edge. A FlowCall also retains the typed procedure arguments supplied
// to the invoked callback. This recursive value is finite because it follows
// the finite source-level procedure type. Equality is semantic and is used for
// deterministic set union.
struct SemanticEffect {
  EffectKind kind = EffectKind::UnknownCall;
  SymbolId symbol;
  std::string text;
  std::string root_identity;
  std::string root_relative_path;
  std::string declaration;
  std::uint32_t flow_parameter = std::numeric_limits<std::uint32_t>::max();
  // FlowCall rows may select a procedure field nested inside a typed argument.
  // An empty path retains the original direct procedure-parameter meaning.
  // Context-rooted paths have no parameter and describe the hidden Context.
  std::vector<std::string> flow_path;
  bool flow_context = false;
  std::vector<ProcedureArgumentSummary> flow_arguments;

  SemanticEffect() = default;
  SemanticEffect(
      EffectKind effect_kind,
      SymbolId effect_symbol,
      std::string effect_text,
      std::string effect_root_identity,
      std::string effect_root_relative_path,
      std::string effect_declaration,
      std::uint32_t effect_flow_parameter =
          std::numeric_limits<std::uint32_t>::max(),
      std::vector<std::string> effect_flow_path = {},
      bool effect_flow_context = false,
      std::vector<ProcedureArgumentSummary> effect_flow_arguments = {})
      : kind(effect_kind), symbol(effect_symbol), text(std::move(effect_text)),
        root_identity(std::move(effect_root_identity)),
        root_relative_path(std::move(effect_root_relative_path)),
        declaration(std::move(effect_declaration)),
        flow_parameter(effect_flow_parameter),
        flow_path(std::move(effect_flow_path)),
        flow_context(effect_flow_context),
        flow_arguments(std::move(effect_flow_arguments)) {}

  bool operator==(const SemanticEffect &) const = default;
};

inline bool ProcedureValueSummary::operator==(
    const ProcedureValueSummary &) const = default;

// One consumer-local imported procedure's audit status. proxy is the SymbolId
// already installed by interface binding. has_effect_summary distinguishes an
// audited empty contract from an unavailable contract, which must close as an
// UnknownCall rather than being treated as harmless.
struct ImportedProcedureContractStatus {
  SymbolId proxy;
  bool has_effect_summary = false;
};

// ImportedProcedureContracts is the immutable dependency-interface input to
// local procedure-flow discovery and effect closure. Rows use the existing
// consumer-local imported representations, but live outside SemanticPackage:
// final dependency effects arrive after body checking and must not clear or
// rewrite the semantic generation retained by procedure products.
struct ImportedProcedureContracts {
  std::vector<ImportedProcedureContractStatus> procedures;
  std::vector<ImportedEffect> effects;
  std::vector<ImportedProcedureReturn> returns;
  std::vector<ImportedProcedureWrite> writes;

  [[nodiscard]] const ImportedProcedureContractStatus *find(
      SymbolId procedure) const;
};

// Copies the contracts already present in a standalone SemanticPackage. This
// is used by direct subsystem callers which bind a completed interface before
// body checking. Workspace compilation instead constructs this payload from
// the current final dependency interfaces and never mutates the package.
[[nodiscard]] ImportedProcedureContracts imported_procedure_contracts(
    const SemanticPackage &package);

// One procedure leaf written through a typed pointer parameter. indirection is
// the number of dereferences from that parameter and path is relative to the
// reached object. Value origins use the writing procedure's formal parameters
// and are substituted at its call site.
struct ProcedureFieldWriteSummary {
  std::uint32_t parameter = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t indirection = 0;
  std::vector<std::string> path;
  ProcedureValueSummary value;

  bool operator==(const ProcedureFieldWriteSummary &) const = default;
};

// A direct named invocation retains the procedure-valued actual arguments
// needed to substitute the callee's FlowCall effects. Non-procedure arguments
// have empty known value sets and are never consulted by a valid FlowCall row.
struct ProcedureInvocationSummary {
  HirExpressionId expression;
  SymbolId callee;
  std::vector<ProcedureArgumentSummary> arguments;

  bool operator==(const ProcedureInvocationSummary &) const = default;
};

struct ProcedureFlowInvocationSummary {
  HirExpressionId expression;
  ProcedureValueSummary callee;
  std::vector<ProcedureArgumentSummary> arguments;

  bool operator==(const ProcedureFlowInvocationSummary &) const = default;
};

// Lexical denials need the effects of the exact call expression, after typed
// callback substitution, rather than a second less-informed HIR walk. These
// rows are derived after the procedure fixed point and remain process-local.
struct CallSiteEffectSummary {
  SymbolId procedure;
  HirExpressionId expression;
  std::vector<SemanticEffect> effects;
};

// One legal recursive component in the concrete procedure call/value-flow
// graph. procedure_indices address EffectSummaryResult::procedures and are
// sorted in canonical procedure-row order. components themselves are stored in
// dependency-first condensation order: every local callee component appears
// before a component which calls it. A singleton with no self-edge is retained
// because the same explicit closure representation covers recursive and
// acyclic procedures without a hidden second scheduling path.
struct ClosedEffectComponent {
  std::vector<std::size_t> procedure_indices;
};

// One independently discovered direct procedure contract. Every field is
// local to this body before transitive call/value-flow closure.
// direct_calls are concrete consumer-local SymbolIds with a known direct row
// and form the SCC/condensation graph; imported/native rows are terminal leaves.
// Invocation rows retain exact source-ordered argument value sets so closure
// can substitute callback contracts without revisiting HIR. Return and
// pointer-write rows contain only body-local facts at this boundary. Closed SCC
// products may derive additional targets, arguments, returns, and writes without
// mutating this direct product.
struct DirectProcedureEffectSummary {
  SymbolId procedure;
  std::vector<SemanticEffect> direct_effects;
  std::vector<SymbolId> direct_calls;
  std::vector<ProcedureInvocationSummary> direct_invocations;
  std::vector<ProcedureFlowInvocationSummary> direct_flow_calls;
  // Procedure leaves returned by this procedure, keyed relative to the result
  // root. A direct procedure result uses the empty path.
  std::vector<ProcedureFieldValueSummary> return_values;
  // Procedure leaves assigned into caller-owned typed storage.
  std::vector<ProcedureFieldWriteSummary> field_writes;

  bool operator==(const DirectProcedureEffectSummary &) const = default;
};

// DirectEffectSummaryResult is the immutable input to SCC discovery. Procedure
// rows retain the canonical selected body order; no package-wide HIR or closed
// transitive effect is stored here.
struct DirectEffectSummaryResult {
  std::vector<DirectProcedureEffectSummary> procedures;

  [[nodiscard]] const DirectProcedureEffectSummary *find(
      SymbolId procedure) const;
};

// One closed procedure contract. Return and write rows are copied from the
// corresponding direct summary because public interfaces consume the complete
// procedure contract from this one terminal result. effects begins with direct
// effects and adds transitive rows in deterministic SCC closure order.
struct ProcedureEffectSummary {
  SymbolId procedure;
  std::vector<ProcedureFieldValueSummary> return_values;
  std::vector<ProcedureFieldWriteSummary> field_writes;
  std::vector<SemanticEffect> effects;
};

struct EffectSummaryResult {
  std::vector<ProcedureEffectSummary> procedures;
  std::vector<ClosedEffectComponent> components;
  std::vector<CallSiteEffectSummary> call_sites;

  [[nodiscard]] const ProcedureEffectSummary *find(SymbolId procedure) const;
  [[nodiscard]] const CallSiteEffectSummary *find_call_site(
      SymbolId procedure, HirExpressionId expression) const;
};

// Discovers direct summaries from the selected immutable procedure products.
// selected_indices addresses procedures, must be strictly increasing, and
// preserves the body scheduler's canonical product order. Each HIR-local ID is
// interpreted only inside its owning ProcedureBodyHirResult.
[[nodiscard]] DirectEffectSummaryResult collect_direct_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target = nullptr,
    std::span<const ForeignProviderAudit> provider_audits = {});

// Worker form of direct discovery. selected_position addresses selected_indices
// rather than procedures, and the returned row depends only on that exact body,
// the selected source symbol domain, and immutable native/imported contracts.
[[nodiscard]] DirectProcedureEffectSummary collect_direct_procedure_effect(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    std::size_t selected_position,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target = nullptr,
    std::span<const ForeignProviderAudit> provider_audits = {});

// Standalone subsystem form which selects every supplied procedure product.
[[nodiscard]] DirectEffectSummaryResult collect_direct_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target = nullptr,
    std::span<const ForeignProviderAudit> provider_audits = {});

// Closes one immutable direct-summary set through explicit flow/effect SCCs.
// selected_indices must be the exact body projection used for direct discovery.
// The returned call-site rows are keyed by procedure plus HIR-local expression
// ID.
[[nodiscard]] EffectSummaryResult close_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const DirectEffectSummaryResult &direct,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target = nullptr,
    std::span<const ForeignProviderAudit> provider_audits = {});

// Standalone subsystem form which closes every supplied procedure product.
[[nodiscard]] EffectSummaryResult close_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    const DirectEffectSummaryResult &direct,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target = nullptr,
    std::span<const ForeignProviderAudit> provider_audits = {});

[[nodiscard]] std::string_view effect_kind_name(EffectKind kind);

} // namespace draft
