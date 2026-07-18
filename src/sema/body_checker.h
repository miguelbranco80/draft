// Type checking and typed-HIR construction for procedure bodies.
//
// This phase consumes the final selected package semantic graph: package names,
// signatures, package constants, and layouts are already stable. It appends
// lexical block/local symbols and lexical compile-time constants, checks runtime
// expressions and statements with expected types, records body-level
// judgment/synthesis sites, and emits structured HIR. It does not perform ABI
// lowering, storage placement, or LLVM construction.

#pragma once

#include "sema/analyzer.h"
#include "sema/constant.h"
#include "sema/hir.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstddef>
#include <string>
#include <vector>

namespace draft {

struct BodyCheckResult {
  bool ok = false;
  HirProgram program;
  std::size_t checked_procedures = 0;
};

// Compiler orchestration creates these rows from imported generic calls found
// in packages earlier in the acyclic consumer-to-dependency order. arguments
// have already been translated into the defining package's TypeStore. The
// requested instance_name is shared with every consumer proxy and therefore is
// the exact package-local symbol spelling used by native emission.
struct ProcedureInstantiationSeed {
  std::string public_template_name;
  std::string instance_name;
  std::vector<ParametricArgument> arguments;
};

// Validates package constant and global-initializer expressions with the same
// operator and expected-type rules used by procedure bodies. Both branches of
// short-circuit and conditional expressions are checked, but no expression is
// executed and no HIR escapes this validation pass.
[[nodiscard]] bool validate_package_initializer_expression_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    SemanticPackage &package,
    ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Checks every package procedure definition in stable declaration order.
// Foreign declarations and standalone procedure types have no body and are
// skipped. Successfully evaluated lexical `::` values append to constants so
// later agent obligations observe the same values as body expressions. Errors
// in one body do not prevent independent bodies from producing recoverable HIR.
[[nodiscard]] BodyCheckResult check_package_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    SemanticPackage &package,
    ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &seeds = {});

} // namespace draft
