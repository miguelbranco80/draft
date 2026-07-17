// Compile-time scalar values, target facts, constant declaration evaluation,
// and discovery of ready declaration-level `when` selections.
//
// This module is the first executable part of Draft's staged semantic dependency
// graph. It evaluates source expressions only when every referenced input is
// available, records successfully computed constants by stable SymbolId, and
// appends deterministic branch decisions without mutating syntax. The caller may
// then rebuild declaration collection with those decisions and repeat.
//
// The current value representation covers booleans, arbitrary-precision
// integers, exact decimal rationals, strings, categorical target labels, and the
// built-in target object. Aggregates, procedural constant evaluation, and
// type/layout queries extend this same table before the semantic core is
// considered complete; unsupported values remain Pending and are never silently
// folded with host behavior.
//
// Relevant specification: 01-core-language.md "Constants and compile-time
// evaluation" and "when"; 02-types-memory-runtime.md "Target profile".

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
#include <vector>

namespace draft {

// TargetFacts is the semantic, LLVM-independent view of a selected profile.
// Categorical fields contain their source enum alternative name without a dot.
// known_features and features are in canonical bytewise order; the former is
// the architecture vocabulary and the latter its enabled subset. This lets
// target.has_feature distinguish disabled from misspelled feature names.
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
};

struct ConstantBinding {
  SymbolId symbol;
  ConstantValue value;
};

// ConstantTable contains only successfully evaluated package constants in
// stable SymbolId order. A missing entry means not requested, not yet ready, or
// invalid; diagnostics and CompileTimeRoundResult distinguish those cases.
struct ConstantTable {
  std::vector<ConstantBinding> bindings;

  [[nodiscard]] const ConstantValue *find(SymbolId symbol) const;
};

struct CompileTimeRoundResult {
  ConstantTable constants;
  std::size_t new_selections = 0;
  std::size_t unresolved_conditionals = 0;
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
    bool diagnose_unready,
    DiagnosticSink &diagnostics);

// Evaluates one required scalar expression in the already resolved semantic
// graph. Body-level compile-time intrinsics use the same evaluator as package
// constants and `when`, avoiding a second arithmetic or target-fact model.
// local_constants is an optional, caller-owned overlay for an instantiated
// procedure's compile-time value parameters. Overlay bindings take precedence
// over declarations and remain valid only for the duration of this call.
[[nodiscard]] std::optional<ConstantValue> evaluate_constant_expression(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    DiagnosticSink &diagnostics,
    const ConstantTable *local_constants = nullptr);

} // namespace draft
