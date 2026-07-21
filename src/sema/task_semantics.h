// Frozen semantic task views and their append-only publication packets.
//
// A semantic worker reads one immutable package/constant prefix through the
// overlay views owned by SemanticPackage, TypeStore, SymbolTable, and
// ConstantTable. It owns every row appended after the explicit counts below.
// The coordinator later translates those task-local IDs and publishes the
// suffix in stable product order. Procedure bodies and interface-level generic
// owner products use this same representation; neither returns or replaces a
// complete package snapshot.
//
// The source package and constants must outlive every view created from them.
// Body and generic workers may not mutate a retained prefix row. Declaration
// workers can refine only TypeStore/SymbolTable rows through an explicit
// patch-enabled view; the canonical prefix itself always remains immutable
// during the wave. This module owns no syntax, product graph, HIR, provider,
// target, or backend policy.

#pragma once

#include "sema/analyzer.h"
#include "sema/constant.h"

#include <cstddef>
#include <vector>

namespace draft {

// SemanticTaskPrefix records every append-only table boundary visible to one
// task. Counts are table-domain sizes, not byte offsets. Publication accepts an
// older prefix after earlier siblings have appended canonical rows, but rejects
// a prefix which is not present in the current package generation.
struct SemanticTaskPrefix {
  std::size_t type_count = 0;
  std::size_t scope_count = 0;
  std::size_t symbol_count = 0;
  std::size_t owned_scope_count = 0;
  std::size_t aggregate_member_count = 0;
  std::size_t enum_member_value_count = 0;
  std::size_t parametric_parameter_count = 0;
  std::size_t static_argument_pack_count = 0;
  std::size_t parametric_instance_count = 0;
  std::size_t parametric_type_instance_count = 0;
  std::size_t imported_symbol_count = 0;
  std::size_t imported_procedure_instance_count = 0;
  std::size_t imported_type_instantiation_request_count = 0;
  std::size_t imported_type_count = 0;
  std::size_t imported_effect_count = 0;
  std::size_t imported_return_count = 0;
  std::size_t imported_write_count = 0;
  std::size_t declaration_denial_count = 0;
  std::size_t site_count = 0;
  std::size_t required_integer_expression_count = 0;
  std::size_t deferred_element_count_count = 0;
  std::size_t deferred_value_expression_count = 0;
  std::size_t deferred_type_application_count = 0;
  std::size_t constant_count = 0;

  bool operator==(const SemanticTaskPrefix &) const = default;
};

// SemanticTaskAppend owns exactly the semantic rows created or refined by one
// task. IDs below prefix already name the canonical input; IDs at or above a
// count belong to this private suffix and require deterministic translation at
// publication. Declaration tasks may additionally replace individual prefix
// type/symbol rows while retaining their stable IDs. Body and generic-owner
// tasks cannot create such patches because their task views are append-only. No
// complete predecessor or successor package is retained in this value.
struct SemanticTaskAppend {
  SemanticTaskPrefix prefix;
  TypeStoreAppend types;
  std::vector<TypeStorePatch> type_patches;
  SymbolTableAppend symbols;
  std::vector<SymbolTablePatch> symbol_patches;
  std::vector<OwnedSemanticScope> owned_scopes;
  std::vector<AggregateMember> aggregate_members;
  std::vector<EnumMemberValue> enum_member_values;
  std::vector<ParametricParameterRecord> parametric_parameters;
  std::vector<StaticArgumentPack> static_argument_packs;
  std::vector<ParametricInstanceRecord> parametric_instances;
  std::vector<ParametricTypeInstanceRecord> parametric_type_instances;
  std::vector<ImportedSymbol> imported_symbols;
  std::vector<ImportedProcedureInstance> imported_procedure_instances;
  std::vector<ImportedTypeInstantiationRequest>
      imported_type_instantiation_requests;
  std::vector<ImportedType> imported_types;
  std::vector<ImportedEffect> imported_effects;
  std::vector<ImportedProcedureReturn> imported_returns;
  std::vector<ImportedProcedureWrite> imported_writes;
  std::vector<DeclarationDenial> declaration_denials;
  std::vector<SemanticSite> sites;
  std::vector<RequiredIntegerExpression> required_integer_expressions;
  std::vector<DeferredElementCount> deferred_element_counts;
  std::vector<DeferredValueExpression> deferred_value_expressions;
  std::vector<DeferredTypeApplication> deferred_type_applications;
  std::vector<ConstantBinding> constants;
};

// Captures the immutable table boundary from which a worker view will fork.
// Adding a new task-mutable semantic table requires adding its count here, its
// suffix below, and its remapping/publication rule in semantic publication.
[[nodiscard]] SemanticTaskPrefix capture_semantic_task_prefix(
    const SemanticPackage &package,
    const ConstantTable &constants);

// Extracts a worker view's owned suffix. package and constants must be overlays
// forked from the exact prefix; their overlay implementations enforce that a
// worker could mutate retained TypeStore or SymbolTable rows only through
// explicit declaration patches. ConstantTable and every other mutable vector
// remain suffix-only by construction.
[[nodiscard]] SemanticTaskAppend extract_semantic_task_append(
    const SemanticTaskPrefix &prefix,
    const SemanticPackage &package,
    const ConstantTable &constants);

} // namespace draft
