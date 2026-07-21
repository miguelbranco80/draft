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

// BodyCheckResult owns the complete body-derived semantic state for one
// package. package and constants begin as copies of the declaration phase's
// stable baseline, then receive lexical scopes, local symbols, concrete
// procedure instances, body agent sites, denials, and lexical compile-time
// values while program is constructed. Keeping those mutations beside HIR is
// the phase boundary: declaration semantics remain immutable and may be reused
// by another source generation or demand set without truncating append-only
// tables or replaying a checker over already-enriched state.
//
// Every SymbolId and TypeId in program addresses package in this same result.
// constants likewise addresses those body-owned symbols. The three values must
// therefore move and live together; none may be paired with a declaration
// baseline or a result from another body check.
struct BodyCheckResult {
  bool ok = false;
  SemanticPackage package;
  ConstantTable constants;
  HirProgram program;
  // Number of exact authored/template/instance roots which produced a
  // HirProcedure row. Nested procedures are roots in their own right; foreign
  // declarations and procedure types have no body and do not contribute.
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
// every operand. The operation checks a private copy: no expression is
// executed, no HIR escapes, and the declaration baseline remains unchanged.
[[nodiscard]] bool validate_package_compile_time_expression_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Checks every package procedure definition in stable declaration order, then
// every concrete specialization in deterministic discovery order. Each
// package-level or concrete root receives a fresh checker reconstructed from
// the retained body-owned semantic state; a root may discover more instance
// records but never checks their bodies recursively through a hidden growing
// loop. A lexically nested declaration publishes another root with an exact
// snapshot of any enclosing concrete substitutions and pack-capture boundary;
// its body is checked only after the enclosing root completes. Foreign
// declarations and standalone procedure types have no body and are skipped.
// The declaration package and constants are immutable baselines. Successfully
// evaluated lexical `::` values append only to the returned result, so later
// agent obligations observe the same values as body expressions without
// contaminating a reusable declaration graph. Errors in one body do not prevent
// independent bodies from producing recoverable HIR.
[[nodiscard]] BodyCheckResult check_package_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &seeds = {});

// Adds only newly requested concrete generic instances to a successful body
// generation whose declaration source is unchanged. previous is consumed and
// returned with stable existing SymbolIds/HIR IDs plus append-only rows for the
// new instances. Every added specialization is reconstructed from its durable
// instance record in a fresh checker, including substitutions inherited by a
// nested generic procedure. Callers must supply only demands absent from the
// generation's recorded work key; removing a demand requires a clean check from
// the declaration baseline so stale executable bodies cannot survive.
[[nodiscard]] BodyCheckResult check_additional_package_instances(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    BodyCheckResult previous,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &additional_seeds);

// Checks only package procedures reached by compile-time constant evaluation.
// The selection uses stable SymbolIds from the same SemanticPackage and is
// applied in package declaration order, independent of evaluator call order.
// The returned HIR is disposable during interface discovery; callers consume
// the returned package and constants only while constructing early typed agent
// obligations. The declaration inputs remain a clean reusable baseline.
[[nodiscard]] BodyCheckResult check_compile_time_procedure_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    std::span<const SymbolId> procedures,
    DiagnosticSink &diagnostics);

} // namespace draft
