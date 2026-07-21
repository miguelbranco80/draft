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
#include "sema/task_semantics.h"
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
// row and never address another row. Semantic IDs address the canonical package
// tables in the containing PackageBodyWorkState. ok mirrors that root's
// recoverable validity; an invalid row remains available for diagnostics but
// cannot reach lowering.
struct ProcedureBodyHirResult {
  bool ok = false;
  SymbolId symbol;
  HirProgram program;
  // Outbound requests are copied after deterministic publication has
  // translated every task-local ID into the canonical package tables.
  // Site indices address the containing PackageBodyWorkState's append-only
  // semantic-site table, whose rows may later receive loop-range enrichment.
  // Together these fields record exactly which body discovered each item, so
  // workspace selection can ignore a completed but currently unselected body.
  std::vector<ImportedProcedureInstance> imported_procedure_instances;
  std::vector<std::size_t> semantic_site_indices;
  // Canonical TypeIds first appended while this product's semantic suffix was
  // published. Equal structural or nominal types which reuse an earlier ID do
  // not appear. The workspace graph uses these rows to attach later type facets
  // to their exact producing body rather than a package-wide body barrier.
  std::vector<TypeId> published_types;
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

// ProcedureBodyWorkOrigin records why the scheduler first created one body
// row. Authored roots are always selected. Discovered roots are selected with
// their prerequisite. ExternalDemand roots are selected only while a current
// cross-package demand names them. A later external demand may reuse a locally
// discovered row; in that case the original Discovered origin remains correct.
enum class ProcedureBodyWorkOrigin {
  Authored,
  Discovered,
  ExternalDemand,
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
  ProcedureBodyWorkOrigin origin = ProcedureBodyWorkOrigin::Discovered;
};

// ProcedureBodyTaskInput owns the private semantic view frozen at prefix.
// TypeStore, SymbolTable, and ConstantTable are append-only overlays whose
// non-owning bases live in PackageBodyWorkState. Declaration-closed files,
// imports, imported documentation, native bindings, and conditional regions
// are also read through that base and cannot appear in a body append packet.
// Owned scopes, aggregate/enum member metadata, parametric-parameter metadata,
// static argument packs, and procedure/type specialization records expose the
// same base followed by a task-owned suffix. Imported symbols, types, concrete
// procedures, outbound type requests, and effect/return/write contracts use the
// same boundary, as do required-integer and deferred dependent-type recipes,
// semantic sites, and declaration denials. No body-mutable table prefix is
// copied into the task. HIR is not an input: every task starts one new local
// arena. work is the exact root.
struct ProcedureBodyTaskInput {
  bool valid = false;
  std::size_t work_index = 0;
  ProcedureBodyWorkItem work;
  SemanticTaskPrefix prefix;
  SemanticPackage package;
  ConstantTable constants;
};

// ProcedureBodyTaskResult is one worker-owned body attempt. semantic contains
// only this task's append packet; program is this root's local HIR arena. The
// worker never aliases or replaces PackageBodyWorkState. discovered_work
// contains nested procedures and concrete instances found by this root.
// work_index ties the result to the exact state row it consumed.
struct ProcedureBodyTaskResult {
  bool ok = false;
  std::size_t work_index = 0;
  SymbolId symbol;
  std::size_t checked_procedures = 0;
  SemanticTaskAppend semantic;
  // One local arena containing only this root's recoverable HIR.
  HirProgram program;
  std::vector<ProcedureBodyWorkItem> discovered_work;
  // Filled by deterministic publication after remapping task-local semantic
  // IDs. Workers leave these empty because their IDs are not yet canonical.
  std::vector<ImportedProcedureInstance> imported_procedure_instances;
  std::vector<std::size_t> semantic_site_indices;
  std::vector<TypeId> published_types;
};

// PackageBodyWorkState is the explicit deterministic publication state for
// body products. It owns the current append-only package prefix, lexical
// constants, already published procedure-local HIR results, and the dynamic
// work list. Every semantic ID in procedures addresses package in this same
// value, and constants may address body-owned symbols, so those fields must
// move and live together. This is the sole body-phase result for workspace and
// direct subsystem compilation; there is no reduced transfer representation.
// `next_work` partitions completed work from the current ready suffix. One
// dispatch freezes that complete suffix as a wave; publication happens only
// after every worker has returned. Publishing the wave may append nested
// procedures and concrete specializations to the next suffix, but no task
// checks them recursively.
//
// The state is deliberately public phase data rather than a callback-driven
// executor: workspace orchestration freezes product waves, invokes one exact
// ready wave, then creates product rows for the newly appended suffix. The
// state retains canonical data while every task reads one shared prefix;
// publication remaps task-local IDs and interns types in stable work order.
struct PackageBodyWorkState {
  bool ok = false;
  // True only after every current root has published and target-wide type
  // invariants have run over the canonical package. Appending an external seed
  // clears this bit without invalidating earlier products.
  bool finalized = false;
  SemanticPackage package;
  ConstantTable constants;
  std::vector<ProcedureBodyHirResult> procedures;
  std::vector<ProcedureBodyWorkItem> work;
  std::size_t next_work = 0;
  // Number of exact authored/template/instance roots which produced a
  // HirProcedure row. Nested procedures are roots in their own right; foreign
  // declarations and procedure types have no body and do not contribute.
  std::size_t checked_procedures = 0;
  // Present after dispatch and before the complete matching result vector is
  // published. It is the exclusive end of the frozen [next_work, end) wave.
  std::optional<std::size_t> active_wave_end;
};

// Starts a clean body work state from immutable declaration inputs. Runtime
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

// Materializes additional external procedure instances directly into one live
// command-local package body state. Only newly created concrete instances are
// appended to the work suffix; completed roots and their procedure-owned HIR
// remain untouched. The operation is invalid during an active worker wave.
// Even when a seed promotes an already local instance rather than creating a
// root, finalization is cleared because public instance metadata may change.
[[nodiscard]] bool append_package_body_seeds(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    PackageBodyWorkState &state,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &additional_seeds);

// Freezes every currently ready root and returns one task-owned input per row in
// canonical work order. Every input reads the same retained package prefix.
// The coordinator must finish all checks before publication and cannot dispatch
// another wave while this one is active.
[[nodiscard]] std::vector<ProcedureBodyTaskInput>
take_ready_procedure_body_wave(
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

// Adopts one complete frozen wave in canonical work order. Every packet must
// name the shared dispatch prefix and matching root. Publication remaps IDs,
// interns structural types, then exposes discovered roots for the next wave.
// A contract mismatch diagnoses an internal scheduling error.
[[nodiscard]] bool publish_procedure_body_wave(
    PackageBodyWorkState &state,
    std::vector<ProcedureBodyTaskResult> results,
    DiagnosticSink &diagnostics);

// Completes target-wide type invariants after every dynamic root has run while
// retaining the live scheduler state for later command-local discoveries.
// Procedure-local initialization and agent-flow checks have already completed
// in their worker-owned arenas. Calling this with unfinished work is a compiler
// contract error and leaves the state invalid rather than silently discarding
// body products.
[[nodiscard]] bool finalize_package_body_work(
    const TargetFacts &target,
    PackageBodyWorkState &state,
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
[[nodiscard]] PackageBodyWorkState check_package_bodies(
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
// During interface discovery the exact stopped product owns the returned
// package/constants long enough to construct its provider constraint; its HIR
// is disposable and no sibling packet is merged or replayed. The declaration
// inputs remain a clean reusable baseline.
[[nodiscard]] PackageBodyWorkState check_compile_time_procedure_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    std::span<const SymbolId> procedures,
    DiagnosticSink &diagnostics);

} // namespace draft
