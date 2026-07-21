// Deterministic publication of isolated semantic task output.
//
// A semantic worker allocates TypeId, ScopeId, SymbolId, and side-table indices in
// the domain formed by its frozen canonical prefix plus its private suffix.
// Another result from the same frozen wave may be published first, so those
// suffix numbers are not canonical positions. This module translates every
// process-local reference, interns equal structural types, appends the owned
// rows. Procedure publication additionally rewrites local HIR and discovered
// work roots to the resulting canonical package generation.
//
// The operation is deliberately a plain deterministic publisher, not a query
// system or a concurrent mutation layer. Workers never call it. The package
// coordinator invokes it in stable product order after all work in a frozen
// wave has joined. It depends on semantic representations and HIR, but never on
// parsing, workspace scheduling, MIR, LLVM, or provider behavior.

#pragma once

#include "sema/body_checker.h"

namespace draft {

// SemanticTaskPublication is the coordinator-owned translation produced while
// publishing one SemanticTaskAppend. Prefix IDs remain unchanged; each vector
// maps a task-local suffix row to its canonical package identity. Structural
// type interning may map several source rows to one TypeId, while symbols and
// scopes normally receive fresh IDs unless equal specializations are merged.
// published_types lists only newly installed canonical rows. The other two
// result vectors retain exact body-product routes before the append is moved.
struct SemanticTaskPublication {
  SemanticTaskPrefix prefix;
  std::vector<TypeId> types;
  std::vector<ScopeId> scopes;
  std::vector<SymbolId> symbols;
  std::size_t deferred_element_count_base = 0;
  std::size_t deferred_value_expression_base = 0;
  std::size_t deferred_type_application_base = 0;
  std::vector<TypeId> published_types;
  std::vector<ImportedProcedureInstance> imported_procedure_instances;
  std::vector<std::size_t> semantic_site_indices;

  // Translates one ID retained by the task result. Invalid and frozen-prefix
  // IDs pass through unchanged; suffix IDs must lie in this exact packet.
  [[nodiscard]] TypeId canonical_type(TypeId id) const;
  [[nodiscard]] ScopeId canonical_scope(ScopeId id) const;
  [[nodiscard]] SymbolId canonical_symbol(SymbolId id) const;
};

// Publishes one generic semantic suffix and returns its complete translation.
// Workers never call this operation. The coordinator invokes it in stable
// semantic-product order after a frozen wave joins.
[[nodiscard]] bool publish_semantic_task_append(
    SemanticPackage &package,
    ConstantTable &constants,
    SemanticTaskAppend &semantic,
    SemanticTaskPublication &publication,
    DiagnosticSink &diagnostics);

// Publishes task.semantic into package/constants and rewrites every semantic ID
// retained by task.program and task.discovered_work. The packet may have been
// produced from an earlier prefix of the same package generation. A malformed
// or non-prefix packet is diagnosed and leaves the caller with an invalid
// compilation result; source errors are not reported at this layer.
[[nodiscard]] bool publish_body_task_semantics(
    SemanticPackage &package,
    ConstantTable &constants,
    ProcedureBodyTaskResult &task,
    DiagnosticSink &diagnostics);

} // namespace draft
