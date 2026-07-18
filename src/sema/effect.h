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
#include "sema/hir.h"
#include "target/profile.h"

#include <cstdint>
#include <limits>
#include <string>
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

  bool operator==(const SemanticEffect &) const = default;
};

// One conservative value set for a procedure-typed expression. targets are
// statically named procedures; parameter_slots are zero-based parameters of
// the current procedure whose eventual values flow here. unknown is sticky
// when type erasure, an uninitialized value, or an unsupported storage path can
// contribute another target. Vectors retain deterministic semantic order.
struct ProcedureValueSummary {
  std::vector<SymbolId> targets;
  std::vector<std::uint32_t> parameter_slots;
  bool unknown = false;
};

// A direct named invocation retains the procedure-valued actual arguments
// needed to substitute the callee's FlowCall effects. Non-procedure arguments
// have empty known value sets and are never consulted by a valid FlowCall row.
struct ProcedureInvocationSummary {
  SymbolId callee;
  std::vector<ProcedureValueSummary> arguments;
};

struct ProcedureFlowInvocationSummary {
  ProcedureValueSummary callee;
  std::vector<ProcedureValueSummary> arguments;
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

  [[nodiscard]] const ProcedureEffectSummary *find(SymbolId procedure) const;
};

[[nodiscard]] EffectSummaryResult summarize_package_effects(
    const SemanticPackage &package,
    const HirProgram &hir,
    const TargetProfile *target = nullptr);

[[nodiscard]] std::string_view effect_kind_name(EffectKind kind);

} // namespace draft
