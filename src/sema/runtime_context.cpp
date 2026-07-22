// Compiler-managed runtime Context type construction.

#include "sema/runtime_context.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] std::optional<TypeId> imported_runtime_context(
    const SemanticPackage &package) {
  for (const ImportBinding &binding : package.imports) {
    if (binding.package_path != "core/runtime") continue;
    for (const ImportedType &imported : package.imported_types_for_read()) {
      if (imported.root_identity == binding.root_identity &&
          imported.root_relative_path == binding.root_relative_path &&
          imported.public_name == "Context" && imported.arguments.empty()) {
        return imported.type;
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<TypeId> local_runtime_context(
    const SemanticPackage &package) {
  if (package.short_name != "runtime") return std::nullopt;
  const std::optional<SymbolId> symbol = package.symbols.lookup_direct(
      package.package_scope, "Context");
  if (!symbol.has_value()) return std::nullopt;
  const Symbol &context_symbol = package.symbols.symbol(*symbol);
  if (context_symbol.kind != SymbolKind::Type ||
      !context_symbol.type.is_valid()) {
    return std::nullopt;
  }
  const Type &context = package.types.type(context_symbol.type);
  const std::vector<std::uint64_t> expected_offsets = {
      0, 16, 32, 40, 56, 72, 80, 88};
  if (context.kind != TypeKind::Struct || !context.c_representation ||
      !context.layout.known || context.layout.size != 96 ||
      context.layout.alignment != 8 ||
      context.member_offsets != expected_offsets) {
    return std::nullopt;
  }
  return context_symbol.type;
}

} // namespace

void ensure_runtime_context_type(
    SemanticPackage &package, DiagnosticSink &diagnostics) {
  if (package.runtime_context_type.is_valid()) return;
  if (const std::optional<TypeId> imported = imported_runtime_context(package)) {
    package.runtime_context_type = *imported;
    return;
  }
  if (const std::optional<TypeId> local = local_runtime_context(package)) {
    package.runtime_context_type = *local;
    return;
  }

  // Packages that do not name core/runtime still receive the versioned
  // AArch64 macOS Context layout. Provider handles are structural two-pointer
  // records because their nominal source names are not in scope.
  const BuiltinTypes &builtins = package.types.builtins();
  const TypeId provider = package.types.tuple(
      {builtins.rawptr_type, builtins.rawptr_type});
  const TypeId assertion = package.types.procedure(
      {builtins.string_type,
       builtins.string_type,
       builtins.string_type,
       builtins.usize_type,
       builtins.usize_type},
      builtins.void_type,
      false);
  const TypeId context = package.types.begin_nominal(
      TypeKind::Struct, "<runtime-context>", SourceRange::invalid());
  package.types.type_mut(context).c_representation = true;
  const std::vector<TypeId> members = {
      provider,
      provider,
      assertion,
      provider,
      provider,
      builtins.rawptr_type,
      builtins.int_type,
      builtins.rawptr_type,
  };
  const std::vector<std::uint64_t> offsets = {
      0, 16, 32, 40, 56, 72, 80, 88};
  package.types.publish_nominal_member_types(context, members);
  package.types.publish_nominal_natural_layout(
      context, {true, 96, 8}, offsets);

  Symbol owner;
  owner.name = "<runtime-context>";
  owner.kind = SymbolKind::Type;
  owner.visibility = Visibility::Private;
  owner.scope = package.package_scope;
  owner.type = context;
  const SymbolId owner_id =
      package.symbols.declare(std::move(owner), diagnostics);
  if (!owner_id.is_valid()) {
    package.runtime_context_type = context;
    return;
  }
  const ScopeId member_scope = package.symbols.add_scope(
      ScopeKind::Type, package.package_scope, SourceRange::invalid());
  package.owned_scopes.push_back({owner_id, member_scope});
  const std::vector<std::string> names = {
      "allocator",
      "temp_allocator",
      "assertion_failure_proc",
      "logger",
      "random_generator",
      "user_ptr",
      "user_index",
      "_internal",
  };
  for (std::size_t index = 0; index < names.size(); ++index) {
    Symbol field;
    field.name = names[index];
    field.kind = SymbolKind::Field;
    field.visibility = Visibility::Public;
    field.scope = member_scope;
    field.type = members[index];
    const SymbolId field_id =
        package.symbols.declare(std::move(field), diagnostics);
    if (field_id.is_valid()) {
      package.aggregate_members.push_back(
          {owner_id, field_id, offsets[index]});
    }
  }
  package.types.publish_nominal_members(context);
  package.runtime_context_type = context;
}

} // namespace draft
