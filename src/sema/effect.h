// Procedure effect and direct-call summaries for denial composition.
//
// Denials are transitive: a region denying `assert`, assembly, unchecked access,
// or a package global must also reject a helper that reaches that entity. This
// pass walks checked HIR, records direct effects and statically named callees,
// and computes a deterministic fixed point over local procedures. Unknown
// procedure-pointer or bodyless calls stay explicit instead of being assumed
// harmless.
//
// These summaries are semantic contracts, not optimization call graphs. Later
// package-interface and link composition extends the same rows with imported
// audited summaries and procedure-pointer flow slots. No LLVM fact is used.
//
// Relevant specification: 05-denials-validation.md section 13 and
// 06-compiler.md, "Native lowering and summaries".

#pragma once

#include "sema/analyzer.h"
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

// SemanticEffect identifies one reachable entity. symbol is meaningful for a
// package global; text names a context field or gives a stable reason for an
// unknown edge. Equality is semantic and used for deterministic set union.
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
      bool effect_flow_context = false)
      : kind(effect_kind), symbol(effect_symbol), text(std::move(effect_text)),
        root_identity(std::move(effect_root_identity)),
        root_relative_path(std::move(effect_root_relative_path)),
        declaration(std::move(effect_declaration)),
        flow_parameter(effect_flow_parameter),
        flow_path(std::move(effect_flow_path)),
        flow_context(effect_flow_context) {}

  bool operator==(const SemanticEffect &) const = default;
};

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
  bool unknown = false;
};

// A call argument can contain several procedure leaves. Each row is relative
// to the argument root, so a callee slot `allocator.procedure` performs one
// exact lookup without treating the rest of the aggregate as type-erased.
struct ProcedureFieldValueSummary {
  std::vector<std::string> path;
  ProcedureValueSummary value;
};

struct ProcedureArgumentSummary {
  std::vector<ProcedureFieldValueSummary> fields;
};

// A direct named invocation retains the procedure-valued actual arguments
// needed to substitute the callee's FlowCall effects. Non-procedure arguments
// have empty known value sets and are never consulted by a valid FlowCall row.
struct ProcedureInvocationSummary {
  HirExpressionId expression;
  SymbolId callee;
  std::vector<ProcedureArgumentSummary> arguments;
};

struct ProcedureFlowInvocationSummary {
  HirExpressionId expression;
  ProcedureValueSummary callee;
  std::vector<ProcedureArgumentSummary> arguments;
};

// Lexical denials need the effects of the exact call expression, after typed
// callback substitution, rather than a second less-informed HIR walk. These
// rows are derived after the procedure fixed point and remain process-local.
struct CallSiteEffectSummary {
  HirExpressionId expression;
  std::vector<SemanticEffect> effects;
};

// ProcedureEffectSummary keeps direct facts separate from the closed local
// fixed point. direct_calls are SymbolIds in the same consumer-local table and
// source discovery order. effects begin with direct_effects and then append
// callee effects in procedure/declaration order without duplicates.
struct ProcedureEffectSummary {
  SymbolId procedure;
  std::vector<SemanticEffect> direct_effects;
  std::vector<SymbolId> direct_calls;
  std::vector<ProcedureInvocationSummary> direct_invocations;
  std::vector<ProcedureFlowInvocationSummary> direct_flow_calls;
  std::vector<SemanticEffect> effects;
};

struct EffectSummaryResult {
  std::vector<ProcedureEffectSummary> procedures;
  std::vector<CallSiteEffectSummary> call_sites;

  [[nodiscard]] const ProcedureEffectSummary *find(SymbolId procedure) const;
  [[nodiscard]] const CallSiteEffectSummary *find_call_site(
      HirExpressionId expression) const;
};

[[nodiscard]] EffectSummaryResult summarize_package_effects(
    const SemanticPackage &package,
    const HirProgram &hir,
    const TargetProfile *target = nullptr,
    std::span<const ForeignProviderAudit> provider_audits = {});

[[nodiscard]] std::string_view effect_kind_name(EffectKind kind);

} // namespace draft
