// Construction of frozen semantic-task boundaries and suffix packets.
//
// See task_semantics.h for ownership and ID-domain contracts. Keeping every
// table named in two direct lists is intentional: a new mutable table cannot
// silently bypass task isolation or deterministic publication.

#include "sema/task_semantics.h"

namespace draft {

SemanticTaskPrefix capture_semantic_task_prefix(
    const SemanticPackage &package,
    const ConstantTable &constants) {
  SemanticTaskPrefix prefix;
  prefix.type_count = package.types.size();
  prefix.scope_count = package.symbols.scope_count();
  prefix.symbol_count = package.symbols.symbol_count();
  prefix.owned_scope_count = package.owned_scopes_for_read().size();
  prefix.aggregate_member_count = package.aggregate_members_for_read().size();
  prefix.enum_member_value_count =
      package.enum_member_values_for_read().size();
  prefix.parametric_parameter_count =
      package.parametric_parameters_for_read().size();
  prefix.static_argument_pack_count =
      package.static_argument_packs_for_read().size();
  prefix.parametric_instance_count =
      package.parametric_instances_for_read().size();
  prefix.parametric_type_instance_count =
      package.parametric_type_instances_for_read().size();
  prefix.imported_symbol_count = package.imported_symbols_for_read().size();
  prefix.imported_procedure_instance_count =
      package.imported_procedure_instances_for_read().size();
  prefix.imported_type_instantiation_request_count =
      package.imported_type_instantiation_requests_for_read().size();
  prefix.imported_type_count = package.imported_types_for_read().size();
  prefix.imported_effect_count = package.imported_effects_for_read().size();
  prefix.imported_return_count = package.imported_returns_for_read().size();
  prefix.imported_write_count = package.imported_writes_for_read().size();
  prefix.declaration_denial_count =
      package.declaration_denials_for_read().size();
  prefix.site_count = package.sites_for_read().size();
  prefix.required_integer_expression_count =
      package.required_integer_expressions_for_read().size();
  prefix.deferred_element_count_count =
      package.deferred_element_counts_for_read().size();
  prefix.deferred_value_expression_count =
      package.deferred_value_expressions_for_read().size();
  prefix.deferred_type_application_count =
      package.deferred_type_applications_for_read().size();
  prefix.constant_count = constants.size();
  return prefix;
}

SemanticTaskAppend extract_semantic_task_append(
    const SemanticTaskPrefix &prefix,
    const SemanticPackage &package,
    const ConstantTable &constants) {
  SemanticTaskAppend appended;
  appended.prefix = prefix;
  appended.types = package.types.appended_since(prefix.type_count);
  appended.symbols = package.symbols.appended_since(
      prefix.scope_count, prefix.symbol_count);
  appended.owned_scopes = package.owned_scopes;
  appended.aggregate_members = package.aggregate_members;
  appended.enum_member_values = package.enum_member_values;
  appended.parametric_parameters = package.parametric_parameters;
  appended.static_argument_packs = package.static_argument_packs;
  appended.parametric_instances = package.parametric_instances;
  appended.parametric_type_instances = package.parametric_type_instances;
  appended.imported_symbols = package.imported_symbols;
  appended.imported_procedure_instances =
      package.imported_procedure_instances;
  appended.imported_type_instantiation_requests =
      package.imported_type_instantiation_requests;
  appended.imported_types = package.imported_types;
  appended.imported_effects = package.imported_effects;
  appended.imported_returns = package.imported_returns;
  appended.imported_writes = package.imported_writes;
  appended.declaration_denials = package.declaration_denials;
  appended.sites = package.sites;
  appended.required_integer_expressions =
      package.required_integer_expressions;
  appended.deferred_element_counts = package.deferred_element_counts;
  appended.deferred_value_expressions = package.deferred_value_expressions;
  appended.deferred_type_applications = package.deferred_type_applications;
  appended.constants = constants.appended_since(prefix.constant_count);
  return appended;
}

} // namespace draft
