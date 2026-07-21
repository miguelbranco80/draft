// Deterministic ID remapping and canonical publication for procedure products.
//
// Inputs are one canonical SemanticPackage/ConstantTable generation and one
// worker-owned ProcedureBodyTaskResult produced from a frozen prefix of that
// generation. The worker packet owns only suffix rows. This file maps those
// rows into the package's current canonical domains, interns structural types,
// appends every table in dependency order, and rewrites HIR/discovered roots.
// The package and task are mutated only after the prefix contract is validated.
//
// The central invariant is simple: an ID below the worker's recorded prefix is
// already canonical; an ID at or above that prefix must be translated through
// the map for that exact table. No arithmetic offset is allowed to escape this
// module because structural type interning can collapse rows while preceding
// results can grow every other table.
//
// This module depends only on sema representations. It intentionally knows
// nothing about workspace product IDs, thread scheduling, MIR, or LLVM.

#include "sema/body_publication.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace draft {
namespace {

// PublicationMaps owns the translation from one worker's combined ID domains
// to the canonical package after earlier results in the wave. Scope and symbol
// rows are identity-bearing and therefore receive fresh canonical positions.
// Type rows are filled later because structural interning may map several local
// rows to one canonical TypeId.
struct PublicationMaps {
  ProcedureBodySemanticPrefix prefix;
  std::vector<TypeId> types;
  std::vector<ScopeId> scopes;
  std::vector<SymbolId> symbols;

  std::size_t deferred_element_count_base = 0;
  std::size_t deferred_value_expression_base = 0;
  std::size_t deferred_type_application_base = 0;
};

// TypePublicationPlan separates canonical TypeId selection from mutation of
// TypeStore. Procedure-instance canonicalization needs those TypeIds to compare
// argument keys, but may then compact provisional symbols before symbolic
// integer expressions receive their final parameter IDs.
struct TypePublicationPlan {
  TypeStoreAppend append;
  std::vector<Type> source_types;
  std::vector<TypeCompletion> source_completions;
  std::vector<std::size_t> published_source;
};

[[nodiscard]] bool fits_u32(std::size_t value) {
  return value <
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] TypeId remap_type(TypeId id, const PublicationMaps &maps) {
  if (!id.is_valid() || id.value < maps.prefix.type_count) return id;
  const std::size_t local = id.value - maps.prefix.type_count;
  assert(local < maps.types.size());
  assert(maps.types[local].is_valid());
  return maps.types[local];
}

[[nodiscard]] ScopeId remap_scope(ScopeId id, const PublicationMaps &maps) {
  if (!id.is_valid() || id.value < maps.prefix.scope_count) return id;
  const std::size_t local = id.value - maps.prefix.scope_count;
  assert(local < maps.scopes.size());
  return maps.scopes[local];
}

[[nodiscard]] SymbolId remap_symbol(SymbolId id, const PublicationMaps &maps) {
  if (!id.is_valid() || id.value < maps.prefix.symbol_count) return id;
  const std::size_t local = id.value - maps.prefix.symbol_count;
  assert(local < maps.symbols.size());
  return maps.symbols[local];
}

[[nodiscard]] std::uint32_t remap_table_index(
    std::uint32_t index,
    std::size_t prefix,
    std::size_t canonical_base) {
  if (index == std::numeric_limits<std::uint32_t>::max() || index < prefix) {
    return index;
  }
  const std::size_t mapped = canonical_base + (index - prefix);
  assert(fits_u32(mapped));
  return static_cast<std::uint32_t>(mapped);
}

void remap_constant(ConstantValue &value, const PublicationMaps &maps) {
  if (value.kind == ConstantKind::Procedure &&
      value.symbol_index != std::numeric_limits<std::uint32_t>::max()) {
    const SymbolId mapped = remap_symbol({value.symbol_index}, maps);
    value.symbol_index = mapped.value;
  }
  if (value.kind == ConstantKind::Type &&
      value.type_index != std::numeric_limits<std::uint32_t>::max()) {
    const TypeId mapped = remap_type({value.type_index}, maps);
    value.type_index = mapped.value;
  }
  for (ConstantValue &element : value.elements) {
    remap_constant(element, maps);
  }
}

void remap_integer_expression(
    IntegerExpression &expression, const PublicationMaps &maps) {
  for (IntegerExpressionNode &node : expression.nodes) {
    if (node.operation != IntegerExpressionOperation::Parameter ||
        node.parameter == std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    node.parameter = remap_symbol({node.parameter}, maps).value;
  }
}

void remap_argument(ParametricArgument &argument, const PublicationMaps &maps) {
  argument.type = remap_type(argument.type, maps);
  argument.value_type = remap_type(argument.value_type, maps);
  remap_constant(argument.value, maps);
  remap_integer_expression(argument.value_expression, maps);
  argument.deferred_value_index = remap_table_index(
      argument.deferred_value_index,
      maps.prefix.deferred_value_expression_count,
      maps.deferred_value_expression_base);
}

void remap_arguments(
    std::vector<ParametricArgument> &arguments,
    const PublicationMaps &maps) {
  for (ParametricArgument &argument : arguments) {
    remap_argument(argument, maps);
  }
}

[[nodiscard]] bool type_references_are_ready(
    const Type &type, const PublicationMaps &maps) {
  const auto ready = [&](TypeId id) {
    if (!id.is_valid() || id.value < maps.prefix.type_count) return true;
    const std::size_t local = id.value - maps.prefix.type_count;
    return local < maps.types.size() && maps.types[local].is_valid();
  };
  if (!ready(type.element)) return false;
  return std::all_of(type.members.begin(), type.members.end(), ready);
}

// Nominal, distinct, type-parameter, and owner-evaluated rows carry source or
// recipe identity which structural equality cannot recover. They are allocated
// once for this task even when their physical fields happen to match another
// row. All other body-created rows use Draft's ordinary structural identity.
[[nodiscard]] bool type_has_local_identity(const Type &type) {
  if (type.owner_evaluated_element_count ||
      type.owner_evaluated_type_application ||
      integer_expression_has_parameters(type.element_count_expression)) {
    return true;
  }
  switch (type.kind) {
  case TypeKind::Struct:
  case TypeKind::Enum:
  case TypeKind::TaggedUnion:
  case TypeKind::RawUnion:
  case TypeKind::Distinct:
  case TypeKind::TypeParameter:
    return true;
  default:
    return false;
  }
}

void remap_type_row(Type &type, const PublicationMaps &maps) {
  type.element = remap_type(type.element, maps);
  for (TypeId &member : type.members) member = remap_type(member, maps);
  remap_integer_expression(type.element_count_expression, maps);
  type.deferred_element_count_index = remap_table_index(
      type.deferred_element_count_index,
      maps.prefix.deferred_element_count_count,
      maps.deferred_element_count_base);
  type.deferred_type_application_index = remap_table_index(
      type.deferred_type_application_index,
      maps.prefix.deferred_type_application_count,
      maps.deferred_type_application_base);
}

[[nodiscard]] bool same_structural_type(
    const Type &left, const Type &right) {
  if (left.kind != right.kind || left.owner_evaluated_element_count ||
      right.owner_evaluated_element_count ||
      left.owner_evaluated_type_application ||
      right.owner_evaluated_type_application) {
    return false;
  }
  switch (left.kind) {
  case TypeKind::Pointer:
  case TypeKind::MultiPointer:
  case TypeKind::Slice:
    return left.element == right.element;
  case TypeKind::Array:
  case TypeKind::Simd:
    return left.element == right.element &&
        left.element_count == right.element_count &&
        left.element_count_expression == right.element_count_expression;
  case TypeKind::Tuple:
    return left.members == right.members;
  case TypeKind::Procedure:
    return left.members == right.members &&
        left.c_calling_convention == right.c_calling_convention;
  default:
    return false;
  }
}

[[nodiscard]] std::optional<TypeId> find_structural_type(
    const SemanticPackage &package,
    const TypeStoreAppend &pending,
    const Type &type) {
  for (std::size_t index = 0; index < package.types.size(); ++index) {
    if (same_structural_type(package.types.type(
                                 {static_cast<std::uint32_t>(index)}),
                             type)) {
      return TypeId{static_cast<std::uint32_t>(index)};
    }
  }
  for (std::size_t index = 0; index < pending.types.size(); ++index) {
    if (same_structural_type(pending.types[index], type)) {
      const std::size_t canonical = pending.base_size + index;
      assert(fits_u32(canonical));
      return TypeId{static_cast<std::uint32_t>(canonical)};
    }
  }
  return std::nullopt;
}

// Builds the complete TypeId translation before publishing any dependent row.
// Identity-bearing rows receive positions first so a structural pointer can
// refer to a nominal declared later in the worker's local vector. Structural
// rows then complete in dependency-ready passes and intern against both the
// retained store and rows selected earlier from this packet.
[[nodiscard]] bool plan_type_publication(
    SemanticPackage &package,
    ProcedureBodySemanticAppend &semantic,
    PublicationMaps &maps,
    TypePublicationPlan &plan,
    DiagnosticSink &diagnostics,
    const std::vector<TypeId> *forced_types = nullptr) {
  plan.source_types = semantic.types.types;
  plan.source_completions = semantic.types.completions;
  if (plan.source_types.size() != plan.source_completions.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body type append has mismatched row and completion counts");
    return false;
  }

  plan.append.base_size = package.types.size();
  maps.types.assign(plan.source_types.size(), TypeId{});
  if (forced_types != nullptr) {
    if (forced_types->size() != maps.types.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "procedure body type publication has an invalid forced map");
      return false;
    }
    maps.types = *forced_types;
  }

  const auto reserve_new_type = [&](
                                    std::size_t source_index,
                                    Type selected) {
    const std::size_t canonical =
        plan.append.base_size + plan.append.types.size();
    assert(fits_u32(canonical));
    maps.types[source_index] = {static_cast<std::uint32_t>(canonical)};
    plan.append.types.push_back(std::move(selected));
    plan.append.completions.push_back(plan.source_completions[source_index]);
    plan.published_source.push_back(source_index);
  };

  for (std::size_t index = 0; index < plan.source_types.size(); ++index) {
    if (!maps.types[index].is_valid() &&
        type_has_local_identity(plan.source_types[index])) {
      reserve_new_type(index, plan.source_types[index]);
    }
  }

  const std::size_t mapped_count = static_cast<std::size_t>(std::count_if(
      maps.types.begin(), maps.types.end(), [](TypeId type) {
        return type.is_valid();
      }));
  std::size_t remaining = plan.source_types.size() - mapped_count;
  while (remaining != 0) {
    bool made_progress = false;
    for (std::size_t index = 0; index < plan.source_types.size(); ++index) {
      if (maps.types[index].is_valid() ||
          !type_references_are_ready(plan.source_types[index], maps)) {
        continue;
      }
      Type remapped = plan.source_types[index];
      remap_type_row(remapped, maps);
      const std::optional<TypeId> existing =
          find_structural_type(package, plan.append, remapped);
      if (existing.has_value()) {
        maps.types[index] = *existing;
      } else {
        reserve_new_type(index, std::move(remapped));
      }
      --remaining;
      made_progress = true;
    }
    if (!made_progress) {
      diagnostics.error(
          SourceRange::invalid(),
          "procedure body type append contains an unresolved structural cycle");
      return false;
    }
  }

  return true;
}

void publish_planned_types(
    SemanticPackage &package,
    TypePublicationPlan plan,
    const PublicationMaps &maps) {
  // Identity rows were assigned before every dependency was translated, and
  // procedure-instance merging may have compacted SymbolIds since structural
  // matching. Rebuild every selected row from its worker source now that all
  // mappings are final.
  for (std::size_t output = 0; output < plan.append.types.size(); ++output) {
    Type remapped = plan.source_types[plan.published_source[output]];
    remap_type_row(remapped, maps);
    plan.append.types[output] = std::move(remapped);
  }
  package.types.append_exact(std::move(plan.append));
}

void remap_symbol_append(
    SymbolTableAppend &symbols, const PublicationMaps &maps) {
  for (Scope &scope : symbols.scopes) {
    scope.parent = remap_scope(scope.parent, maps);
    for (SymbolId &symbol : scope.symbols) {
      symbol = remap_symbol(symbol, maps);
    }
  }
  for (Symbol &symbol : symbols.symbols) {
    symbol.scope = remap_scope(symbol.scope, maps);
    symbol.type = remap_type(symbol.type, maps);
  }
  for (ExistingScopeSymbolAppend &addition :
       symbols.existing_scope_symbols) {
    addition.scope = remap_scope(addition.scope, maps);
    for (SymbolId &symbol : addition.symbols) {
      symbol = remap_symbol(symbol, maps);
    }
  }
}

[[nodiscard]] ParametricInstanceRecord remapped_instance_identity(
    const ParametricInstanceRecord &source,
    const PublicationMaps &maps) {
  ParametricInstanceRecord identity;
  identity.source = remap_symbol(source.source, maps);
  identity.instance = remap_symbol(source.instance, maps);
  identity.arguments = source.arguments;
  remap_arguments(identity.arguments, maps);
  identity.pack_types = source.pack_types;
  for (TypeId &type : identity.pack_types) type = remap_type(type, maps);
  identity.externally_requested = source.externally_requested;
  return identity;
}

[[nodiscard]] ParametricTypeInstanceRecord remapped_type_instance_identity(
    const ParametricTypeInstanceRecord &source,
    const PublicationMaps &maps) {
  ParametricTypeInstanceRecord identity;
  identity.source = remap_symbol(source.source, maps);
  identity.instance = remap_symbol(source.instance, maps);
  identity.arguments = source.arguments;
  remap_arguments(identity.arguments, maps);
  return identity;
}

// InstanceMerge describes one worker-private specialization whose semantic
// identity was published by an earlier result from the same frozen wave. Both
// procedure and nominal type specializations own a root symbol plus one or more
// scopes, so the same compaction operation can redirect the entire private
// object graph to its canonical predecessor.
struct InstanceMerge {
  SymbolId provisional;
  SymbolId canonical;
};

// Finds nominal type specializations already retained by the package. The
// provisional nominal TypeId is a dependency of structural rows created by the
// same worker, so this operation also records a forced TypeId translation. The
// caller replans the complete type suffix after every discovery; that fixed
// point lets an outer instance key become equal only after an inner instance in
// its arguments has itself become canonical.
[[nodiscard]] bool discover_type_instance_merges(
    const SemanticPackage &package,
    const ProcedureBodySemanticAppend &semantic,
    PublicationMaps &maps,
    std::vector<TypeId> &forced_types,
    std::vector<InstanceMerge> &merges,
    bool &discovered,
    DiagnosticSink &diagnostics) {
  discovered = false;
  for (const ParametricTypeInstanceRecord &row :
       semantic.parametric_type_instances) {
    const bool already_merged = std::any_of(
        merges.begin(), merges.end(), [&](const InstanceMerge &merge) {
          return merge.provisional == row.instance;
        });
    if (already_merged) continue;

    const ParametricTypeInstanceRecord identity =
        remapped_type_instance_identity(row, maps);
    for (const ParametricTypeInstanceRecord &candidate :
         package.parametric_type_instances_for_read()) {
      if (candidate.source != identity.source ||
          candidate.arguments != identity.arguments) {
        continue;
      }

      if (!row.instance.is_valid() ||
          row.instance.value < maps.prefix.symbol_count) {
        diagnostics.error(
            SourceRange::invalid(),
            "procedure body nominal instance is not task-local");
        return false;
      }
      const std::size_t symbol_local =
          row.instance.value - maps.prefix.symbol_count;
      if (symbol_local >= semantic.symbols.symbols.size()) {
        diagnostics.error(
            SourceRange::invalid(),
            "procedure body nominal instance symbol is outside its append");
        return false;
      }

      const TypeId provisional_type =
          semantic.symbols.symbols[symbol_local].type;
      const TypeId canonical_type =
          package.symbols.symbol(candidate.instance).type;
      if (!provisional_type.is_valid() ||
          provisional_type.value < maps.prefix.type_count ||
          !canonical_type.is_valid()) {
        diagnostics.error(
            SourceRange::invalid(),
            "procedure body nominal instance has an invalid type identity");
        return false;
      }
      const std::size_t type_local =
          provisional_type.value - maps.prefix.type_count;
      if (type_local >= forced_types.size()) {
        diagnostics.error(
            SourceRange::invalid(),
            "procedure body nominal instance type is outside its append");
        return false;
      }

      forced_types[type_local] = canonical_type;
      maps.symbols[symbol_local] = candidate.instance;
      merges.push_back({row.instance, candidate.instance});
      discovered = true;
      break;
    }
  }
  return true;
}

// Equal procedure and nominal type specializations can be discovered
// independently by several callers in one wave. The first result in canonical
// work order owns the instance. Later packets redirect their provisional root,
// scope, parameter/member, and TypeIds to that canonical structure, remove the
// duplicate rows, then compact every unrelated local ID in source order. No
// orphan semantic storage or duplicate package binding survives publication.
[[nodiscard]] bool canonicalize_instances(
    SemanticPackage &package,
    ProcedureBodySemanticAppend &semantic,
    PublicationMaps &maps,
    std::vector<InstanceMerge> merges,
    DiagnosticSink &diagnostics) {
  for (std::size_t row_index = 0;
       row_index < semantic.parametric_instances.size(); ++row_index) {
    const ParametricInstanceRecord identity = remapped_instance_identity(
        semantic.parametric_instances[row_index], maps);
    const AppendOnlyTableView<ParametricInstanceRecord> retained =
        package.parametric_instances_for_read();
    for (std::size_t retained_index = 0;
         retained_index < retained.size(); ++retained_index) {
      const ParametricInstanceRecord &candidate = retained[retained_index];
      if (candidate.source != identity.source ||
          candidate.arguments != identity.arguments ||
          candidate.pack_types != identity.pack_types) {
        continue;
      }

      const SymbolId provisional =
          semantic.parametric_instances[row_index].instance;
      assert(provisional.is_valid());
      assert(provisional.value >= maps.prefix.symbol_count);
      const std::size_t local =
          provisional.value - maps.prefix.symbol_count;
      assert(local < maps.symbols.size());
      maps.symbols[local] = candidate.instance;
      merges.push_back({provisional, candidate.instance});

      if (identity.externally_requested && !candidate.externally_requested) {
        package.parametric_instance_mut(retained_index)
            .externally_requested = true;
        const Symbol &provisional_symbol =
            semantic.symbols.symbols[local];
        if (!provisional_symbol.linkage_name.empty()) {
          package.symbols.symbol_mut(candidate.instance).linkage_name =
              provisional_symbol.linkage_name;
        }
      }
      break;
    }
  }
  if (merges.empty()) return true;

  std::vector<bool> drop_symbols(semantic.symbols.symbols.size(), false);
  std::vector<bool> drop_scopes(semantic.symbols.scopes.size(), false);
  for (const InstanceMerge &merge : merges) {
    const std::size_t instance_local =
        merge.provisional.value - maps.prefix.symbol_count;
    drop_symbols[instance_local] = true;

    // A procedure owns a Procedure scope; a nominal type owns a Type scope.
    // Match by owner and kind rather than depending on owned-scope row order.
    for (const OwnedSemanticScope &owned : semantic.owned_scopes) {
      if (owned.owner != merge.provisional ||
          owned.scope.value < maps.prefix.scope_count) {
        continue;
      }
      const std::size_t local_scope =
          owned.scope.value - maps.prefix.scope_count;
      if (local_scope >= semantic.symbols.scopes.size()) continue;
      const ScopeKind kind = semantic.symbols.scopes[local_scope].kind;
      std::optional<ScopeId> canonical_scope;
      for (const OwnedSemanticScope &candidate :
           package.owned_scopes_for_read()) {
        if (candidate.owner == merge.canonical &&
            package.symbols.scope(candidate.scope).kind == kind) {
          canonical_scope = candidate.scope;
          break;
        }
      }
      if (!canonical_scope.has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "canonical specialization has no matching owned scope");
        return false;
      }
      maps.scopes[local_scope] = *canonical_scope;
      drop_scopes[local_scope] = true;

      // Procedure parameters, pack markers, and aggregate members have the
      // same deterministic names and kinds as the first canonical instance.
      for (std::size_t symbol_index = 0;
           symbol_index < semantic.symbols.symbols.size(); ++symbol_index) {
        const Symbol &symbol = semantic.symbols.symbols[symbol_index];
        if (symbol.scope != owned.scope) continue;
        const std::optional<SymbolId> canonical_symbol =
            package.symbols.lookup_direct(*canonical_scope, symbol.name);
        if (!canonical_symbol.has_value() ||
            package.symbols.symbol(*canonical_symbol).kind != symbol.kind) {
          diagnostics.error(
              SourceRange::invalid(),
              "canonical specialization has inconsistent owned symbols");
          return false;
        }
        maps.symbols[symbol_index] = *canonical_symbol;
        drop_symbols[symbol_index] = true;
      }
    }
  }

  const std::size_t scope_base = package.symbols.scope_count();
  std::size_t kept_scopes = 0;
  for (std::size_t index = 0; index < maps.scopes.size(); ++index) {
    if (drop_scopes[index]) continue;
    const std::size_t canonical = scope_base + kept_scopes;
    assert(fits_u32(canonical));
    maps.scopes[index] = {static_cast<std::uint32_t>(canonical)};
    ++kept_scopes;
  }
  const std::size_t symbol_base = package.symbols.symbol_count();
  std::size_t kept_symbols = 0;
  for (std::size_t index = 0; index < maps.symbols.size(); ++index) {
    if (drop_symbols[index]) continue;
    const std::size_t canonical = symbol_base + kept_symbols;
    assert(fits_u32(canonical));
    maps.symbols[index] = {static_cast<std::uint32_t>(canonical)};
    ++kept_symbols;
  }

  const auto symbol_is_dropped = [&](SymbolId symbol) {
    return symbol.is_valid() && symbol.value >= maps.prefix.symbol_count &&
        symbol.value - maps.prefix.symbol_count < drop_symbols.size() &&
        drop_symbols[symbol.value - maps.prefix.symbol_count];
  };
  const auto scope_is_dropped = [&](ScopeId scope) {
    return scope.is_valid() && scope.value >= maps.prefix.scope_count &&
        scope.value - maps.prefix.scope_count < drop_scopes.size() &&
        drop_scopes[scope.value - maps.prefix.scope_count];
  };

  semantic.parametric_instances.erase(
      std::remove_if(
          semantic.parametric_instances.begin(),
          semantic.parametric_instances.end(),
          [&](const ParametricInstanceRecord &row) {
            return symbol_is_dropped(row.instance);
          }),
      semantic.parametric_instances.end());
  semantic.parametric_type_instances.erase(
      std::remove_if(
          semantic.parametric_type_instances.begin(),
          semantic.parametric_type_instances.end(),
          [&](const ParametricTypeInstanceRecord &row) {
            return symbol_is_dropped(row.instance);
          }),
      semantic.parametric_type_instances.end());
  semantic.aggregate_members.erase(
      std::remove_if(
          semantic.aggregate_members.begin(),
          semantic.aggregate_members.end(),
          [&](const AggregateMember &row) {
            return symbol_is_dropped(row.owner) ||
                symbol_is_dropped(row.member);
          }),
      semantic.aggregate_members.end());
  semantic.enum_member_values.erase(
      std::remove_if(
          semantic.enum_member_values.begin(),
          semantic.enum_member_values.end(),
          [&](const EnumMemberValue &row) {
            return symbol_is_dropped(row.member);
          }),
      semantic.enum_member_values.end());
  semantic.parametric_parameters.erase(
      std::remove_if(
          semantic.parametric_parameters.begin(),
          semantic.parametric_parameters.end(),
          [&](const ParametricParameterRecord &row) {
            return symbol_is_dropped(row.owner) ||
                symbol_is_dropped(row.parameter);
          }),
      semantic.parametric_parameters.end());
  semantic.static_argument_packs.erase(
      std::remove_if(
          semantic.static_argument_packs.begin(),
          semantic.static_argument_packs.end(),
          [&](const StaticArgumentPack &row) {
            return symbol_is_dropped(row.owner) ||
                symbol_is_dropped(row.binding);
          }),
      semantic.static_argument_packs.end());
  semantic.declaration_denials.erase(
      std::remove_if(
          semantic.declaration_denials.begin(),
          semantic.declaration_denials.end(),
          [&](const DeclarationDenial &row) {
            return symbol_is_dropped(row.declaration);
          }),
      semantic.declaration_denials.end());
  semantic.sites.erase(
      std::remove_if(
          semantic.sites.begin(), semantic.sites.end(),
          [&](const SemanticSite &row) {
            return symbol_is_dropped(row.anchor) ||
                scope_is_dropped(row.scope);
          }),
      semantic.sites.end());
  semantic.required_integer_expressions.erase(
      std::remove_if(
          semantic.required_integer_expressions.begin(),
          semantic.required_integer_expressions.end(),
          [&](const RequiredIntegerExpression &row) {
            return symbol_is_dropped(row.anchor) ||
                scope_is_dropped(row.scope);
          }),
      semantic.required_integer_expressions.end());
  // Deferred recipe vectors are indexed by Type and ParametricArgument rows.
  // They therefore retain their exact task-local order here and are remapped
  // below. A duplicate specialization normally contributes no private recipe;
  // if it does, preserving the row is preferable to silently invalidating all
  // later suffix indices. Recipe liveness belongs to the eventual semantic
  // table compactor, not instance identity publication.
  semantic.constants.erase(
      std::remove_if(
          semantic.constants.begin(), semantic.constants.end(),
          [&](const ConstantBinding &row) {
            return symbol_is_dropped(row.symbol);
          }),
      semantic.constants.end());
  semantic.owned_scopes.erase(
      std::remove_if(
          semantic.owned_scopes.begin(),
          semantic.owned_scopes.end(),
          [&](const OwnedSemanticScope &row) {
            return symbol_is_dropped(row.owner) || scope_is_dropped(row.scope);
          }),
      semantic.owned_scopes.end());
  std::vector<Scope> retained_scopes;
  retained_scopes.reserve(kept_scopes);
  for (std::size_t index = 0;
       index < semantic.symbols.scopes.size(); ++index) {
    if (!drop_scopes[index]) {
      retained_scopes.push_back(std::move(semantic.symbols.scopes[index]));
    }
  }
  semantic.symbols.scopes = std::move(retained_scopes);
  std::vector<Symbol> retained_symbols;
  retained_symbols.reserve(kept_symbols);
  for (std::size_t index = 0;
       index < semantic.symbols.symbols.size(); ++index) {
    if (!drop_symbols[index]) {
      retained_symbols.push_back(std::move(semantic.symbols.symbols[index]));
    }
  }
  semantic.symbols.symbols = std::move(retained_symbols);
  for (ExistingScopeSymbolAppend &addition :
       semantic.symbols.existing_scope_symbols) {
    addition.symbols.erase(
        std::remove_if(
            addition.symbols.begin(),
            addition.symbols.end(),
            [&](SymbolId symbol) {
              return symbol_is_dropped(symbol);
            }),
        addition.symbols.end());
  }
  return true;
}

void remap_imported_effect(
    ImportedEffect &effect, const PublicationMaps &maps) {
  effect.procedure_proxy = remap_symbol(effect.procedure_proxy, maps);
  for (ImportedFlowArgument &argument : effect.flow_arguments) {
    for (ImportedFlowField &field : argument.fields) {
      for (ImportedEffect &nested : field.value.contract_effects) {
        remap_imported_effect(nested, maps);
      }
    }
  }
}

void remap_type_binding(
    DeferredElementCountTypeBinding &binding,
    const PublicationMaps &maps) {
  binding.parameter = remap_type(binding.parameter, maps);
  binding.replacement = remap_type(binding.replacement, maps);
}

void remap_value_binding(
    DeferredElementCountValueBinding &binding,
    const PublicationMaps &maps) {
  binding.parameter = remap_symbol(binding.parameter, maps);
  remap_constant(binding.value, maps);
  remap_integer_expression(binding.symbolic_expression, maps);
}

void remap_side_tables(
    ProcedureBodySemanticAppend &semantic,
    const PublicationMaps &maps) {
  for (OwnedSemanticScope &row : semantic.owned_scopes) {
    row.owner = remap_symbol(row.owner, maps);
    row.scope = remap_scope(row.scope, maps);
  }
  for (AggregateMember &row : semantic.aggregate_members) {
    row.owner = remap_symbol(row.owner, maps);
    row.member = remap_symbol(row.member, maps);
  }
  for (EnumMemberValue &row : semantic.enum_member_values) {
    row.member = remap_symbol(row.member, maps);
  }
  for (ParametricParameterRecord &row : semantic.parametric_parameters) {
    row.owner = remap_symbol(row.owner, maps);
    row.parameter = remap_symbol(row.parameter, maps);
  }
  for (StaticArgumentPack &row : semantic.static_argument_packs) {
    row.owner = remap_symbol(row.owner, maps);
    row.binding = remap_symbol(row.binding, maps);
    row.symbolic_element_type = remap_type(row.symbolic_element_type, maps);
  }
  for (ParametricInstanceRecord &row : semantic.parametric_instances) {
    row.source = remap_symbol(row.source, maps);
    row.instance = remap_symbol(row.instance, maps);
    remap_arguments(row.arguments, maps);
    for (TypeId &type : row.pack_types) type = remap_type(type, maps);
    for (ConcreteProcedureTypeSubstitution &binding :
         row.type_substitutions) {
      binding.parameter = remap_type(binding.parameter, maps);
      binding.replacement = remap_type(binding.replacement, maps);
    }
    for (ConcreteProcedureValueSubstitution &binding :
         row.value_substitutions) {
      binding.parameter = remap_symbol(binding.parameter, maps);
      remap_constant(binding.value, maps);
    }
  }
  for (ParametricTypeInstanceRecord &row :
       semantic.parametric_type_instances) {
    row.source = remap_symbol(row.source, maps);
    row.instance = remap_symbol(row.instance, maps);
    remap_arguments(row.arguments, maps);
  }
  for (ImportedSymbol &row : semantic.imported_symbols) {
    row.import_symbol = remap_symbol(row.import_symbol, maps);
    row.proxy = remap_symbol(row.proxy, maps);
    remap_constant(row.constant, maps);
  }
  for (ImportedProcedureInstance &row :
       semantic.imported_procedure_instances) {
    row.source_proxy = remap_symbol(row.source_proxy, maps);
    row.instance_proxy = remap_symbol(row.instance_proxy, maps);
    remap_arguments(row.arguments, maps);
    for (TypeId &type : row.pack_types) type = remap_type(type, maps);
  }
  for (ImportedTypeInstantiationRequest &row :
       semantic.imported_type_instantiation_requests) {
    row.source_proxy = remap_symbol(row.source_proxy, maps);
    remap_arguments(row.arguments, maps);
  }
  for (ImportedType &row : semantic.imported_types) {
    row.type = remap_type(row.type, maps);
    remap_arguments(row.arguments, maps);
  }
  for (ImportedEffect &row : semantic.imported_effects) {
    remap_imported_effect(row, maps);
  }
  for (ImportedProcedureReturn &row : semantic.imported_returns) {
    row.procedure_proxy = remap_symbol(row.procedure_proxy, maps);
    for (ImportedEffect &effect : row.contract_effects) {
      remap_imported_effect(effect, maps);
    }
  }
  for (ImportedProcedureWrite &row : semantic.imported_writes) {
    row.procedure_proxy = remap_symbol(row.procedure_proxy, maps);
    for (ImportedEffect &effect : row.value_contract_effects) {
      remap_imported_effect(effect, maps);
    }
  }
  for (DeclarationDenial &row : semantic.declaration_denials) {
    row.declaration = remap_symbol(row.declaration, maps);
  }
  for (SemanticSite &row : semantic.sites) {
    row.scope = remap_scope(row.scope, maps);
    row.anchor = remap_symbol(row.anchor, maps);
    row.expected_type = remap_type(row.expected_type, maps);
    for (SemanticBranchRefinement &branch : row.branch_refinements) {
      branch.subject_type = remap_type(branch.subject_type, maps);
    }
    for (SemanticLoopRange &range : row.loop_ranges) {
      range.binding = remap_symbol(range.binding, maps);
      range.upper_type = remap_type(range.upper_type, maps);
      for (SymbolId &symbol : range.upper_symbols) {
        symbol = remap_symbol(symbol, maps);
      }
    }
  }
  for (RequiredIntegerExpression &row :
       semantic.required_integer_expressions) {
    row.scope = remap_scope(row.scope, maps);
    row.anchor = remap_symbol(row.anchor, maps);
    row.expected_type = remap_type(row.expected_type, maps);
  }
  for (DeferredElementCount &row : semantic.deferred_element_counts) {
    row.type = remap_type(row.type, maps);
    row.scope = remap_scope(row.scope, maps);
    for (DeferredElementCountTypeBinding &binding : row.type_bindings) {
      remap_type_binding(binding, maps);
    }
    for (DeferredElementCountValueBinding &binding : row.value_bindings) {
      remap_value_binding(binding, maps);
    }
  }
  for (DeferredValueExpression &row : semantic.deferred_value_expressions) {
    row.scope = remap_scope(row.scope, maps);
    row.expected_type = remap_type(row.expected_type, maps);
    for (DeferredElementCountTypeBinding &binding : row.type_bindings) {
      remap_type_binding(binding, maps);
    }
    for (DeferredElementCountValueBinding &binding : row.value_bindings) {
      remap_value_binding(binding, maps);
    }
  }
  for (DeferredTypeApplication &row :
       semantic.deferred_type_applications) {
    row.type = remap_type(row.type, maps);
    row.source = remap_symbol(row.source, maps);
    remap_arguments(row.arguments, maps);
  }
  for (ConstantBinding &row : semantic.constants) {
    row.symbol = remap_symbol(row.symbol, maps);
    row.type = remap_type(row.type, maps);
    remap_constant(row.value, maps);
  }
}

void remap_hir(
    HirProgram &program, const PublicationMaps &maps) {
  HirProgram remapped;
  for (std::size_t index = 0; index < program.expression_count(); ++index) {
    HirExpression expression = program.expression(
        {static_cast<std::uint32_t>(index)});
    expression.type = remap_type(expression.type, maps);
    expression.scope = remap_scope(expression.scope, maps);
    expression.symbol = remap_symbol(expression.symbol, maps);
    remap_constant(expression.constant, maps);
    for (SymbolId &member : expression.operand_members) {
      member = remap_symbol(member, maps);
    }
    (void)remapped.add_expression(std::move(expression));
  }
  for (std::size_t index = 0; index < program.statement_count(); ++index) {
    HirStatement statement = program.statement(
        {static_cast<std::uint32_t>(index)});
    for (SymbolId &binding : statement.bindings) {
      binding = remap_symbol(binding, maps);
    }
    for (HirSwitchCase &switch_case : statement.switch_cases) {
      switch_case.payload_alternative =
          remap_symbol(switch_case.payload_alternative, maps);
      switch_case.payload_binding =
          remap_symbol(switch_case.payload_binding, maps);
    }
    (void)remapped.add_statement(std::move(statement));
  }
  for (std::size_t index = 0; index < program.block_count(); ++index) {
    HirBlock block = program.block({static_cast<std::uint32_t>(index)});
    block.scope = remap_scope(block.scope, maps);
    (void)remapped.add_block(std::move(block));
  }
  for (HirProcedure procedure : program.procedures()) {
    procedure.symbol = remap_symbol(procedure.symbol, maps);
    procedure.type = remap_type(procedure.type, maps);
    remapped.add_procedure(std::move(procedure));
  }
  program = std::move(remapped);
}

void remap_environment(
    ProcedureBodyEnvironment &environment,
    const PublicationMaps &maps) {
  environment.source = remap_symbol(environment.source, maps);
  environment.symbol = remap_symbol(environment.symbol, maps);
  for (ConcreteProcedureTypeSubstitution &binding :
       environment.type_substitutions) {
    binding.parameter = remap_type(binding.parameter, maps);
    binding.replacement = remap_type(binding.replacement, maps);
  }
  for (ConcreteProcedureValueSubstitution &binding :
       environment.value_substitutions) {
    binding.parameter = remap_symbol(binding.parameter, maps);
    remap_constant(binding.value, maps);
  }
  for (TypeId &type : environment.pack_types) {
    type = remap_type(type, maps);
  }
  for (SymbolId &parameter : environment.pack_parameters) {
    parameter = remap_symbol(parameter, maps);
  }
  environment.pack_binding = remap_symbol(environment.pack_binding, maps);
}

void remap_discovered_work(
    std::vector<ProcedureBodyWorkItem> &work,
    const PublicationMaps &maps) {
  for (ProcedureBodyWorkItem &item : work) {
    item.symbol = remap_symbol(item.symbol, maps);
    if (item.enclosing_environment.has_value()) {
      remap_environment(*item.enclosing_environment, maps);
    }
  }
}

template <typename Value>
void append_rows(std::vector<Value> &destination, std::vector<Value> source) {
  destination.insert(
      destination.end(),
      std::make_move_iterator(source.begin()),
      std::make_move_iterator(source.end()));
}

[[nodiscard]] bool prefix_is_available(
    const SemanticPackage &package,
    const ConstantTable &constants,
    const ProcedureBodySemanticPrefix &prefix) {
  return package.types.size() >= prefix.type_count &&
      package.symbols.scope_count() >= prefix.scope_count &&
      package.symbols.symbol_count() >= prefix.symbol_count &&
      package.owned_scopes_for_read().size() >= prefix.owned_scope_count &&
      package.aggregate_members_for_read().size() >=
          prefix.aggregate_member_count &&
      package.enum_member_values_for_read().size() >=
          prefix.enum_member_value_count &&
      package.parametric_parameters_for_read().size() >=
          prefix.parametric_parameter_count &&
      package.static_argument_packs_for_read().size() >=
          prefix.static_argument_pack_count &&
      package.parametric_instances_for_read().size() >=
          prefix.parametric_instance_count &&
      package.parametric_type_instances_for_read().size() >=
          prefix.parametric_type_instance_count &&
      package.imported_symbols_for_read().size() >=
          prefix.imported_symbol_count &&
      package.imported_procedure_instances_for_read().size() >=
          prefix.imported_procedure_instance_count &&
      package.imported_type_instantiation_requests_for_read().size() >=
          prefix.imported_type_instantiation_request_count &&
      package.imported_types_for_read().size() >= prefix.imported_type_count &&
      package.imported_effects_for_read().size() >=
          prefix.imported_effect_count &&
      package.imported_returns_for_read().size() >=
          prefix.imported_return_count &&
      package.imported_writes_for_read().size() >= prefix.imported_write_count &&
      package.declaration_denials_for_read().size() >=
          prefix.declaration_denial_count &&
      package.sites_for_read().size() >= prefix.site_count &&
      package.required_integer_expressions_for_read().size() >=
          prefix.required_integer_expression_count &&
      package.deferred_element_counts_for_read().size() >=
          prefix.deferred_element_count_count &&
      package.deferred_value_expressions_for_read().size() >=
          prefix.deferred_value_expression_count &&
      package.deferred_type_applications_for_read().size() >=
          prefix.deferred_type_application_count &&
      constants.size() >= prefix.constant_count;
}

} // namespace

bool publish_body_task_semantics(
    SemanticPackage &package,
    ConstantTable &constants,
    ProcedureBodyTaskResult &task,
    DiagnosticSink &diagnostics) {
  ProcedureBodySemanticAppend &semantic = task.semantic;
  if (!prefix_is_available(package, constants, semantic.prefix) ||
      semantic.types.base_size != semantic.prefix.type_count ||
      semantic.symbols.base_scope_count != semantic.prefix.scope_count ||
      semantic.symbols.base_symbol_count != semantic.prefix.symbol_count) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body task result does not belong to this semantic prefix");
    return false;
  }

  PublicationMaps maps;
  maps.prefix = semantic.prefix;
  maps.deferred_element_count_base =
      package.deferred_element_counts_for_read().size();
  maps.deferred_value_expression_base =
      package.deferred_value_expressions_for_read().size();
  maps.deferred_type_application_base =
      package.deferred_type_applications_for_read().size();

  const std::size_t scope_base = package.symbols.scope_count();
  maps.scopes.reserve(semantic.symbols.scopes.size());
  for (std::size_t index = 0; index < semantic.symbols.scopes.size(); ++index) {
    const std::size_t canonical = scope_base + index;
    if (!fits_u32(canonical)) {
      diagnostics.error(SourceRange::invalid(), "procedure body scope table overflow");
      return false;
    }
    maps.scopes.push_back({static_cast<std::uint32_t>(canonical)});
  }
  const std::size_t symbol_base = package.symbols.symbol_count();
  maps.symbols.reserve(semantic.symbols.symbols.size());
  for (std::size_t index = 0; index < semantic.symbols.symbols.size(); ++index) {
    const std::size_t canonical = symbol_base + index;
    if (!fits_u32(canonical)) {
      diagnostics.error(SourceRange::invalid(), "procedure body symbol table overflow");
      return false;
    }
    maps.symbols.push_back({static_cast<std::uint32_t>(canonical)});
  }

  // Select canonical type identities without mutating TypeStore. A nominal
  // instance can depend on another nominal instance created by the same task,
  // so discover retained identities to a fixed point and replan the suffix
  // after every newly forced TypeId. This is a small deterministic operation
  // over one task packet, not a semantic checker retry.
  std::vector<TypeId> forced_types(semantic.types.types.size(), TypeId{});
  std::vector<InstanceMerge> instance_merges;
  TypePublicationPlan type_plan;
  while (true) {
    type_plan = {};
    if (!plan_type_publication(
            package,
            semantic,
            maps,
            type_plan,
            diagnostics,
            &forced_types)) {
      return false;
    }
    bool discovered_type_instance = false;
    if (!discover_type_instance_merges(
            package,
            semantic,
            maps,
            forced_types,
            instance_merges,
            discovered_type_instance,
            diagnostics)) {
      return false;
    }
    if (!discovered_type_instance) break;
  }

  // Equal procedure keys now use the final TypeId map. Instance compaction may
  // still rewrite task-local parameter/member SymbolIds, so publish selected
  // type rows only after that rewrite is complete.
  if (!canonicalize_instances(
          package,
          semantic,
          maps,
          std::move(instance_merges),
          diagnostics)) {
    return false;
  }
  publish_planned_types(package, std::move(type_plan), maps);

  remap_symbol_append(semantic.symbols, maps);
  semantic.symbols.base_scope_count = scope_base;
  semantic.symbols.base_symbol_count = symbol_base;
  package.symbols.append_exact(std::move(semantic.symbols));

  remap_side_tables(semantic, maps);
  // Preserve exact outbound work and canonical agent-site indices on the body
  // product after IDs have become canonical but before the append packet is
  // consumed. The package tables remain an append-only interning/publication
  // substrate; current workspace selection follows these product-owned routes.
  task.imported_procedure_instances =
      semantic.imported_procedure_instances;
  const std::size_t site_base = package.sites_for_read().size();
  task.semantic_site_indices.reserve(semantic.sites.size());
  for (std::size_t index = 0; index < semantic.sites.size(); ++index) {
    task.semantic_site_indices.push_back(site_base + index);
  }
  append_rows(package.owned_scopes, std::move(semantic.owned_scopes));
  append_rows(package.aggregate_members, std::move(semantic.aggregate_members));
  append_rows(
      package.enum_member_values, std::move(semantic.enum_member_values));
  append_rows(
      package.parametric_parameters,
      std::move(semantic.parametric_parameters));
  append_rows(
      package.static_argument_packs,
      std::move(semantic.static_argument_packs));
  append_rows(
      package.parametric_instances, std::move(semantic.parametric_instances));
  append_rows(
      package.parametric_type_instances,
      std::move(semantic.parametric_type_instances));
  append_rows(package.imported_symbols, std::move(semantic.imported_symbols));
  append_rows(
      package.imported_procedure_instances,
      std::move(semantic.imported_procedure_instances));
  append_rows(
      package.imported_type_instantiation_requests,
      std::move(semantic.imported_type_instantiation_requests));
  append_rows(package.imported_types, std::move(semantic.imported_types));
  append_rows(package.imported_effects, std::move(semantic.imported_effects));
  append_rows(package.imported_returns, std::move(semantic.imported_returns));
  append_rows(package.imported_writes, std::move(semantic.imported_writes));
  append_rows(
      package.declaration_denials, std::move(semantic.declaration_denials));
  append_rows(package.sites, std::move(semantic.sites));
  append_rows(
      package.required_integer_expressions,
      std::move(semantic.required_integer_expressions));
  append_rows(
      package.deferred_element_counts,
      std::move(semantic.deferred_element_counts));
  append_rows(
      package.deferred_value_expressions,
      std::move(semantic.deferred_value_expressions));
  append_rows(
      package.deferred_type_applications,
      std::move(semantic.deferred_type_applications));
  constants.append_exact(constants.size(), std::move(semantic.constants));

  task.symbol = remap_symbol(task.symbol, maps);
  remap_hir(task.program, maps);
  remap_discovered_work(task.discovered_work, maps);
  return true;
}

} // namespace draft
