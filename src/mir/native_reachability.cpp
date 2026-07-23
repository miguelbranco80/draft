// Direct native-reference extraction and artifact-rooted graph closure.
//
// See native_reachability.h for the phase boundary and ownership contract. The
// implementation intentionally uses sorted flat lookup tables and explicit
// queues. Entity counts are compiler data, not user-visible hash identities;
// stable sorting makes duplicate detection and traversal reproducible across
// worker counts and host libraries.

#include "mir/native_reachability.h"

#include "sema/constant_value.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool identity_less(
    const NativeSymbolIdentity &left,
    const NativeSymbolIdentity &right) {
  if (left.package.root_identity != right.package.root_identity) {
    return left.package.root_identity < right.package.root_identity;
  }
  if (left.package.root_relative_path != right.package.root_relative_path) {
    return left.package.root_relative_path < right.package.root_relative_path;
  }
  return left.name < right.name;
}

[[nodiscard]] std::string display_identity(
    const NativeSymbolIdentity &identity) {
  return display_package_identity(identity.package) + ":" + identity.name;
}

template <typename Value>
void append_unique(std::vector<Value> &values, Value value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(std::move(value));
  }
}

enum class ResolvedReferenceKind {
  None,
  Procedure,
  Global,
  Foreign,
};

// ResolvedReference is a short-lived classification of one package-local HIR
// SymbolId. Only the field selected by kind is meaningful. Native identities
// own their strings because imported symbol rows remain package-local compiler
// state and must not leak a borrowed view into reachability products.
struct ResolvedReference {
  ResolvedReferenceKind kind = ResolvedReferenceKind::None;
  NativeSymbolIdentity identity;
  NativeForeignReference foreign;
};

[[nodiscard]] std::string local_linkage_name(const Symbol &symbol) {
  return symbol.linkage_name.empty() ? symbol.name : symbol.linkage_name;
}

[[nodiscard]] const ImportedSymbol *find_imported_symbol(
    const SemanticPackage &semantic,
    SymbolId symbol) {
  for (const ImportedSymbol &imported : semantic.imported_symbols_for_read()) {
    if (imported.proxy == symbol) return &imported;
  }
  return nullptr;
}

[[nodiscard]] const NativeBinding *find_foreign_binding(
    const SemanticPackage &semantic,
    SymbolId symbol) {
  for (const NativeBinding &binding : semantic.native_bindings) {
    if (binding.symbol == symbol &&
        binding.kind == NativeBindingKind::ForeignImport) {
      return &binding;
    }
  }
  return nullptr;
}

// Resolves one HIR symbol without consulting LLVM naming. Imported Draft
// declarations use their canonical public/interface name; a native provider
// import terminates as Foreign even though its consumer-local proxy has
// SymbolKind::Procedure.
[[nodiscard]] ResolvedReference resolve_reference(
    const PackageIdentity &package,
    const SemanticPackage &semantic,
    SymbolId symbol_id) {
  ResolvedReference result;
  if (!symbol_id.is_valid() ||
      symbol_id.value >= semantic.symbols.symbol_count()) {
    return result;
  }
  if (const ImportedSymbol *imported =
          find_imported_symbol(semantic, symbol_id)) {
    if (!imported->native_provider.empty()) {
      result.kind = ResolvedReferenceKind::Foreign;
      result.foreign.provider = imported->native_provider;
      result.foreign.linker_name_spelling =
          imported->native_linker_name_spelling.empty()
          ? imported->public_name
          : imported->native_linker_name_spelling;
      return result;
    }
    const Symbol &symbol = semantic.symbols.symbol(symbol_id);
    if (symbol.kind != SymbolKind::Procedure &&
        symbol.kind != SymbolKind::Variable) {
      return result;
    }
    result.kind = symbol.kind == SymbolKind::Procedure
        ? ResolvedReferenceKind::Procedure
        : ResolvedReferenceKind::Global;
    result.identity.package = {
        imported->root_identity, imported->root_relative_path};
    result.identity.name = imported->public_name;
    return result;
  }

  const Symbol &symbol = semantic.symbols.symbol(symbol_id);
  if (const NativeBinding *binding =
          find_foreign_binding(semantic, symbol_id)) {
    result.kind = ResolvedReferenceKind::Foreign;
    result.foreign.provider = binding->provider;
    result.foreign.linker_name_spelling = binding->linker_name_spelling;
    return result;
  }
  if (symbol.kind == SymbolKind::Procedure) {
    result.kind = ResolvedReferenceKind::Procedure;
    result.identity.package = package;
    result.identity.name = local_linkage_name(symbol);
  } else if (symbol.kind == SymbolKind::Variable && !symbol.flags.foreign) {
    result.kind = ResolvedReferenceKind::Global;
    result.identity.package = package;
    result.identity.name = symbol.name;
  }
  return result;
}

void collect_constant_procedures(
    const PackageIdentity &package,
    const SemanticPackage &semantic,
    const ConstantValue &value,
    std::vector<NativeSymbolIdentity> &procedures) {
  if (value.kind == ConstantKind::Procedure) {
    if (value.symbol_index != std::numeric_limits<std::uint32_t>::max()) {
      const ResolvedReference reference = resolve_reference(
          package, semantic, SymbolId{value.symbol_index});
      if (reference.kind == ResolvedReferenceKind::Procedure) {
        append_unique(procedures, reference.identity);
      }
      return;
    }
    if (!value.text.empty()) {
      NativeSymbolIdentity identity;
      identity.package = value.root_identity.empty()
          ? package
          : PackageIdentity{
                value.root_identity, value.root_relative_path};
      identity.name = value.text;
      append_unique(procedures, std::move(identity));
    }
    return;
  }
  if (value.kind != ConstantKind::Aggregate &&
      value.kind != ConstantKind::EnumLabel) {
    return;
  }
  for (const ConstantValue &element : value.elements) {
    collect_constant_procedures(package, semantic, element, procedures);
  }
}

void collect_argument_targets(
    const PackageIdentity &package,
    const SemanticPackage &semantic,
    const std::vector<ProcedureArgumentSummary> &arguments,
    std::vector<NativeSymbolIdentity> &targets) {
  for (const ProcedureArgumentSummary &argument : arguments) {
    for (const ProcedureFieldValueSummary &field : argument.fields) {
      for (SymbolId symbol : field.value.targets) {
        const ResolvedReference reference =
            resolve_reference(package, semantic, symbol);
        if (reference.kind == ResolvedReferenceKind::Procedure) {
          append_unique(targets, reference.identity);
        }
      }
    }
  }
}

// SortedIdentityIndex keeps binary-search order separate from compiler
// publication order. result indices always address the original summary
// vectors, while identity comparison never depends on insertion order.
struct SortedIdentityIndex {
  NativeSymbolIdentity identity;
  std::size_t index = 0;
};

[[nodiscard]] bool sorted_index_less(
    const SortedIdentityIndex &left,
    const SortedIdentityIndex &right) {
  return identity_less(left.identity, right.identity);
}

template <typename Summary, typename IdentityAccessor>
[[nodiscard]] bool build_sorted_index(
    const std::vector<Summary> &summaries,
    IdentityAccessor identity,
    std::vector<SortedIdentityIndex> &result,
    std::string &failure) {
  result.clear();
  result.reserve(summaries.size());
  for (std::size_t index = 0; index < summaries.size(); ++index) {
    result.push_back({identity(summaries[index]), index});
  }
  std::sort(result.begin(), result.end(), sorted_index_less);
  for (std::size_t index = 1; index < result.size(); ++index) {
    if (result[index - 1].identity == result[index].identity) {
      failure = "duplicate native definition '" +
          display_identity(result[index].identity) + "'";
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::size_t> find_identity(
    const std::vector<SortedIdentityIndex> &index,
    const NativeSymbolIdentity &identity) {
  const auto found = std::lower_bound(
      index.begin(), index.end(), SortedIdentityIndex{identity, 0},
      sorted_index_less);
  if (found == index.end() || found->identity != identity) {
    return std::nullopt;
  }
  return found->index;
}

enum class PendingEntityKind {
  Procedure,
  Global,
};

struct PendingEntity {
  PendingEntityKind kind = PendingEntityKind::Procedure;
  std::size_t index = 0;
};

} // namespace

NativeProcedureReferenceSummary collect_native_procedure_references(
    const PackageIdentity &package,
    const SemanticPackage &semantic,
    const ProcedureBodyHirResult &body,
    std::size_t body_index,
    const DirectProcedureEffectSummary &direct) {
  NativeProcedureReferenceSummary result;
  result.body_index = body_index;
  if (body.program.procedures().size() != 1) return result;
  const HirProcedure &procedure = body.program.procedures().front();
  result.local_symbol = procedure.symbol;
  const ResolvedReference owner =
      resolve_reference(package, semantic, procedure.symbol);
  if (owner.kind == ResolvedReferenceKind::Procedure) {
    result.procedure = owner.identity;
  }

  // Every direct Symbol/constant occurrence is a native relocation edge even
  // when it is not called. Scanning the procedure-owned HIR arena once captures
  // address-taking, storage, comparisons, returns, and branch-local uses
  // without reconstructing source syntax.
  for (std::size_t index = 0; index < body.program.expression_count(); ++index) {
    const HirExpression &expression = body.program.expression(
        HirExpressionId{static_cast<std::uint32_t>(index)});
    if (expression.kind == HirExpressionKind::Symbol &&
        expression.symbol.is_valid()) {
      const ResolvedReference reference =
          resolve_reference(package, semantic, expression.symbol);
      if (reference.kind == ResolvedReferenceKind::Procedure) {
        append_unique(result.procedure_values, reference.identity);
      } else if (reference.kind == ResolvedReferenceKind::Global) {
        append_unique(result.referenced_globals, reference.identity);
      }
    }
    if (expression.kind == HirExpressionKind::Constant) {
      collect_constant_procedures(
          package, semantic, expression.constant, result.procedure_values);
    }
  }

  // Effect discovery has already resolved finite indirect callees and
  // procedure-valued arguments through locals, aggregates, parameters, and
  // returned values. Reuse those exact direct facts instead of a less-informed
  // second value-flow implementation.
  for (const ProcedureInvocationSummary &invocation :
       direct.direct_invocations) {
    const ResolvedReference callee =
        resolve_reference(package, semantic, invocation.callee);
    if (callee.kind == ResolvedReferenceKind::Procedure) {
      append_unique(result.direct_calls, callee.identity);
    } else if (callee.kind == ResolvedReferenceKind::Foreign) {
      append_unique(result.foreign_calls, callee.foreign);
      collect_argument_targets(
          package,
          semantic,
          invocation.arguments,
          result.escaped_procedure_values);
    }
    collect_argument_targets(
        package, semantic, invocation.arguments, result.procedure_values);
  }
  for (const ProcedureFlowInvocationSummary &invocation :
       direct.direct_flow_calls) {
    for (SymbolId target : invocation.callee.targets) {
      const ResolvedReference reference =
          resolve_reference(package, semantic, target);
      if (reference.kind == ResolvedReferenceKind::Procedure) {
        append_unique(result.direct_calls, reference.identity);
        append_unique(result.procedure_values, reference.identity);
      } else if (reference.kind == ResolvedReferenceKind::Foreign) {
        append_unique(result.foreign_calls, reference.foreign);
      }
    }
    if (invocation.callee.unknown) {
      result.has_unknown_call_target = true;
      collect_argument_targets(
          package,
          semantic,
          invocation.arguments,
          result.escaped_procedure_values);
    }
    collect_argument_targets(
        package, semantic, invocation.arguments, result.procedure_values);
  }
  return result;
}

std::vector<NativeGlobalReferenceSummary> collect_native_global_references(
    const PackageIdentity &package,
    const SemanticPackage &semantic,
    const ConstantTable &global_initializers) {
  std::vector<NativeGlobalReferenceSummary> result;
  for (SymbolId symbol_id :
       semantic.symbols.symbols_in_scope(semantic.package_scope)) {
    const Symbol &symbol = semantic.symbols.symbol(symbol_id);
    if (symbol.kind != SymbolKind::Variable || symbol.flags.foreign) continue;
    NativeGlobalReferenceSummary summary;
    summary.global.package = package;
    summary.global.name = symbol.name;
    summary.local_symbol = symbol_id;
    if (const ConstantValue *initializer =
            global_initializers.find(symbol_id)) {
      collect_constant_procedures(
          package, semantic, *initializer, summary.procedure_values);
    }
    result.push_back(std::move(summary));
  }
  return result;
}

NativeReachabilityResult compute_native_reachability(
    const NativeReachabilityInput &input) {
  NativeReachabilityResult result;
  std::vector<SortedIdentityIndex> procedures;
  std::vector<SortedIdentityIndex> globals;
  if (!build_sorted_index(
          input.procedures,
          [](const NativeProcedureReferenceSummary &summary) {
            return summary.procedure;
          },
          procedures,
          result.failure) ||
      !build_sorted_index(
          input.globals,
          [](const NativeGlobalReferenceSummary &summary) {
            return summary.global;
          },
          globals,
          result.failure)) {
    return result;
  }

  std::vector<bool> live_procedures(input.procedures.size(), false);
  std::vector<bool> live_globals(input.globals.size(), false);
  std::vector<PendingEntity> pending;
  pending.reserve(input.procedures.size() + input.globals.size());

  const auto add_procedure = [&](
      const NativeSymbolIdentity &identity,
      std::string_view reason) -> bool {
    const std::optional<std::size_t> index =
        find_identity(procedures, identity);
    if (!index.has_value()) {
      result.failure = std::string(reason) + " procedure '" +
          display_identity(identity) + "' has no checked runtime definition";
      return false;
    }
    if (!live_procedures[*index]) {
      live_procedures[*index] = true;
      pending.push_back({PendingEntityKind::Procedure, *index});
    }
    return true;
  };
  const auto add_global = [&](
      const NativeSymbolIdentity &identity,
      std::string_view reason) -> bool {
    const std::optional<std::size_t> index = find_identity(globals, identity);
    if (!index.has_value()) {
      result.failure = std::string(reason) + " global '" +
          display_identity(identity) + "' has no checked storage definition";
      return false;
    }
    if (!live_globals[*index]) {
      live_globals[*index] = true;
      pending.push_back({PendingEntityKind::Global, *index});
    }
    return true;
  };

  for (const NativeSymbolIdentity &root : input.procedure_roots) {
    if (!add_procedure(root, "native root")) return result;
  }
  for (const NativeSymbolIdentity &root : input.global_roots) {
    if (!add_global(root, "native root")) return result;
  }

  // The queue grows monotonically and every entity is inserted once. Edges are
  // visited in the direct summary's deterministic discovery order; returned
  // live indices are rebuilt from input order below, so equivalent graphs have
  // identical products even if root order differs.
  for (std::size_t next = 0; next < pending.size(); ++next) {
    const PendingEntity entity = pending[next];
    if (entity.kind == PendingEntityKind::Procedure) {
      const NativeProcedureReferenceSummary &summary =
          input.procedures[entity.index];
      for (const NativeSymbolIdentity &callee : summary.direct_calls) {
        if (!add_procedure(callee, "live call")) return result;
      }
      for (const NativeSymbolIdentity &value : summary.procedure_values) {
        if (!add_procedure(value, "live procedure value")) return result;
      }
      for (const NativeSymbolIdentity &global : summary.referenced_globals) {
        if (!add_global(global, "live reference")) return result;
      }
      continue;
    }
    const NativeGlobalReferenceSummary &summary = input.globals[entity.index];
    for (const NativeSymbolIdentity &value : summary.procedure_values) {
      if (!add_procedure(value, "live global initializer")) return result;
    }
  }

  for (std::size_t index = 0; index < live_procedures.size(); ++index) {
    if (!live_procedures[index]) continue;
    result.live_procedures.push_back(index);
    if (input.procedures[index].has_unknown_call_target) {
      result.unknown_target_procedures.push_back(index);
    }
  }
  for (std::size_t index = 0; index < live_globals.size(); ++index) {
    if (live_globals[index]) result.live_globals.push_back(index);
  }
  result.ok = true;
  return result;
}

} // namespace draft
