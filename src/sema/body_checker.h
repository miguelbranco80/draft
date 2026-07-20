// Type checking and typed-HIR construction for procedure bodies.
//
// The complete phase consumes the final selected package semantic graph:
// package names, signatures, package constants, and layouts are already stable.
// Interface discovery may also invoke the same checker for the narrow procedure
// dependency closure blocked by synthesis; all semantic inputs reached before
// the site are stable, while dependent constants remain intentionally absent.
// The checker appends lexical block/local symbols and lexical compile-time
// constants, checks runtime expressions and statements with expected types,
// records body-level judgment/synthesis sites, and emits structured HIR. It does
// not perform ABI lowering, storage placement, or LLVM construction.

#pragma once

#include "sema/analyzer.h"
#include "sema/constant.h"
#include "sema/hir.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstddef>
#include <span>
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
// and pack_types have already been translated into the defining package's
// TypeStore. The ordered pack tail follows the named argument packet in
// specialization identity. The requested instance_name is shared with every
// consumer proxy and therefore is the exact package-local symbol spelling used
// by native emission.
struct ProcedureInstantiationSeed {
  std::string public_template_name;
  std::string instance_name;
  std::vector<ParametricArgument> arguments;
  std::vector<TypeId> pack_types;
};

// Validates package constant/global initializers and selected structural
// `when` conditions which contain a non-evaluating or value-selecting form,
// using the same expression checker as procedure bodies. This catches operands
// hidden by type_of, short-circuiting, or a dead conditional branch without
// rechecking ordinary constant expressions whose evaluator already visited
// every operand. No expression is executed and no HIR escapes this pass.
[[nodiscard]] bool validate_package_compile_time_expression_types(
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

// Checks only package procedures reached by compile-time constant evaluation.
// The selection uses stable SymbolIds from the same SemanticPackage and is
// applied in package declaration order, independent of evaluator call order.
// Its HIR is disposable; the lasting output is the typed body-agent sites
// appended to package for early interface-synthesis obligation construction.
[[nodiscard]] BodyCheckResult check_compile_time_procedure_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    SemanticPackage &package,
    ConstantTable &constants,
    const TargetFacts &target,
    std::span<const SymbolId> procedures,
    DiagnosticSink &diagnostics);

} // namespace draft
