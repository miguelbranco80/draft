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
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace draft {

// ProcedureBodyHirResult permanently owns the typed HIR arena produced by one
// exact authored/template/instance root. HIR-local IDs begin at zero in every
// row and never address another row. Semantic IDs address the package generation
// in the containing BodyCheckResult. ok mirrors that root's recoverable validity;
// an invalid row remains available for diagnostics but cannot reach lowering.
struct ProcedureBodyHirResult {
  bool ok = false;
  SymbolId symbol;
  HirProgram program;
};

// BodyCheckResult owns the complete body-derived semantic state for one
// package. package and constants begin as copies of the declaration phase's
// stable baseline, then receive lexical scopes, local symbols, concrete
// procedure instances, body agent sites, denials, and lexical compile-time
// values. Each exact root permanently owns its HIR in procedures.
//
// program is a deterministic compatibility projection built once by offsetting
// and concatenating those procedure-local arenas. Effects, denials, and MIR
// consume it until their own product migrations complete; body workers never
// append into it. Every SymbolId and TypeId in either representation addresses
// package in this same result, and constants addresses those body-owned symbols.
// These values must therefore move and live together.
struct BodyCheckResult {
  bool ok = false;
  SemanticPackage package;
  ConstantTable constants;
  std::vector<ProcedureBodyHirResult> procedures;
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

// ProcedureBodyEnvironment is the exact concrete outer environment retained
// by a lexically nested procedure body. Package-level roots and roots nested in
// a symbolic template have no environment. A root discovered while checking a
// concrete outer instance copies its type/value substitutions and static-pack
// binding here so a later checker can reproduce the same compile-time names
// and capture boundary without retaining the discovering BodyChecker.
//
// Every ID addresses the PackageBodyWorkState which owns this value. The value
// is command-local, is never serialized, and is not specialization identity;
// ParametricInstanceRecord owns canonical concrete instance identity.
struct ProcedureBodyEnvironment {
  SymbolId source;
  SymbolId symbol;
  std::vector<ConcreteProcedureTypeSubstitution> type_substitutions;
  std::vector<ConcreteProcedureValueSubstitution> value_substitutions;
  std::vector<TypeId> pack_types;
  std::vector<SymbolId> pack_parameters;
  SymbolId pack_binding;
};

// One ProcedureBodyWorkItem names one exact independently scheduled body.
// parametric_template distinguishes symbolic checking from executable concrete
// checking. prerequisite is the index of the enclosing or discovering work
// item when this row was found dynamically; initial authored roots and
// externally materialized seeds have no procedure prerequisite and depend only
// on the package interface at workspace scheduling time.
struct ProcedureBodyWorkItem {
  SymbolId symbol;
  bool parametric_template = false;
  std::optional<ProcedureBodyEnvironment> enclosing_environment;
  std::optional<std::size_t> prerequisite;
};

// ProcedureBodyTaskInput transfers exclusive ownership of the current
// sequential semantic publication prefix to one worker. Moving rather than
// copying the package and constants keeps this transitional oracle linear in
// semantic state size. HIR is not an input: every task starts one new local
// arena. work is the exact root and next_instance partitions already published
// concrete records from any suffix discovered by this task.
struct ProcedureBodyTaskInput {
  bool valid = false;
  std::size_t work_index = 0;
  ProcedureBodyWorkItem work;
  std::size_t next_instance = 0;
  SemanticPackage package;
  ConstantTable constants;
};

// ProcedureBodyTaskResult is one worker-owned body attempt. package and
// constants are the successor to the exclusively owned prefix supplied in
// ProcedureBodyTaskInput; program is this root's new local HIR arena. The
// worker never aliases PackageBodyWorkState. discovered_work contains nested
// procedures and concrete instances found by this root. work_index ties the
// result to the exact state row it consumed, while next_instance records the
// published ParametricInstanceRecord prefix.
//
// This full semantic successor is intentionally simple and correct. A later
// procedure-local semantic arena will narrow package and constants after their
// ID domains and deterministic interning boundary are explicit; callers must
// not infer that unrelated package state is semantically owned by the
// procedure merely because it travels through this temporary packet.
struct ProcedureBodyTaskResult {
  bool ok = false;
  std::size_t work_index = 0;
  SymbolId symbol;
  std::size_t checked_procedures = 0;
  std::size_t next_instance = 0;
  SemanticPackage package;
  ConstantTable constants;
  // One local arena containing only this root's recoverable HIR.
  HirProgram program;
  std::vector<ProcedureBodyWorkItem> discovered_work;
};

// PackageBodyWorkState is the explicit sequential publication oracle for body
// products. It owns the current append-only package prefix, lexical constants,
// already published procedure-local HIR results, and the dynamic work list.
// `next_work` partitions completed work from the current ready suffix.
// Publishing one task may append nested procedures and concrete specializations
// to that suffix, but no task checks them recursively.
//
// The state is deliberately public phase data rather than a callback-driven
// executor: workspace orchestration freezes product waves, invokes one exact
// item at a time, then creates product rows for the newly appended suffix.
// Today this state is the deterministic full-successor publication oracle while
// procedure-local arenas are introduced. The coordinator moves its payload
// into one task, and only the matching publication operation moves a successor
// back. Independent tasks cannot yet share one frozen prefix because that
// prefix still has one exclusive owner.
struct PackageBodyWorkState {
  bool ok = false;
  SemanticPackage package;
  ConstantTable constants;
  std::vector<ProcedureBodyHirResult> procedures;
  std::vector<ProcedureBodyWorkItem> work;
  std::size_t next_work = 0;
  std::size_t next_instance = 0;
  std::size_t checked_procedures = 0;
  // Present only after the coordinator transfers the package prefix into a
  // ProcedureBodyTaskInput and before it adopts the matching result. No second
  // task may be dispatched while this row is present.
  std::optional<std::size_t> active_work;
};

// Starts a clean body generation from immutable declaration inputs. Runtime
// context construction and externally requested seed materialization happen
// once here; no procedure body is checked. Authored roots precede seed roots in
// stable declaration/seed order.
[[nodiscard]] PackageBodyWorkState begin_package_body_work(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &seeds = {});

// Starts an append-only extension from one successful body generation. This is
// the temporary source-transition bridge while procedure products replace the
// retained package body generation. Only roots introduced by additional_seeds
// enter the new work suffix; already checked HIR is never placed on the work
// list by this operation.
[[nodiscard]] PackageBodyWorkState begin_additional_package_body_work(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    BodyCheckResult previous,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &additional_seeds);

// Transfers the current package prefix and exact next root into a task-owned
// input. This is a coordinator operation: it marks one active work index and
// leaves no semantic payload available for another dispatch until publication.
[[nodiscard]] ProcedureBodyTaskInput take_next_procedure_body_work(
    PackageBodyWorkState &state,
    DiagnosticSink &diagnostics);

// Checks exactly input.work and returns its successor. The worker owns every
// mutable input and the caller owns diagnostics; no coordinator state is
// reachable through this contract.
[[nodiscard]] ProcedureBodyTaskResult check_procedure_body_work(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const TargetFacts &target,
    ProcedureBodyTaskInput input,
    DiagnosticSink &diagnostics);

// Adopts one task-owned successor into the sequential publication state. The
// work index and root symbol must match the next pending item. Discovered roots
// receive a prerequisite on that item and become visible only after adoption.
// A contract mismatch diagnoses an internal scheduling error and leaves state
// unchanged.
[[nodiscard]] bool publish_procedure_body_work(
    PackageBodyWorkState &state,
    ProcedureBodyTaskResult result,
    DiagnosticSink &diagnostics);

// Completes package-wide invariants after every dynamic root has run, then
// transfers the state into the compatibility aggregate consumed by later
// phases. Calling this with unfinished work is a compiler contract error and
// produces an invalid result rather than silently discarding body products.
[[nodiscard]] BodyCheckResult finish_package_body_work(
    const LoadedPackage &loaded,
    const TargetFacts &target,
    PackageBodyWorkState state,
    DiagnosticSink &diagnostics);

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
