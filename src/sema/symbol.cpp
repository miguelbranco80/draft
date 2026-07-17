// Direct append-only implementation of Draft scopes and symbol lookup.
//
// Direct scope lookup is currently linear in declarations in that scope. This
// preserves declaration order and keeps duplicate behavior obvious. Large real
// packages may justify a secondary deterministic index later; the vector remains
// canonical and lookup semantics do not change.

#include "sema/symbol.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace draft {

bool ScopeId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

bool SymbolId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

ScopeId SymbolTable::add_scope(ScopeKind kind, ScopeId parent, SourceRange range) {
  assert(scopes_.size() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  if (parent.is_valid()) {
    assert(static_cast<std::size_t>(parent.value) < scopes_.size());
  } else {
    assert(kind == ScopeKind::Package);
  }
  const ScopeId id{static_cast<std::uint32_t>(scopes_.size())};
  scopes_.push_back({kind, parent, range, {}});
  return id;
}

SymbolId SymbolTable::declare(Symbol new_symbol, DiagnosticSink &diagnostics) {
  assert(new_symbol.scope.is_valid());
  Scope &owner = scope_mut(new_symbol.scope);
  if (const std::optional<SymbolId> previous = lookup_direct(new_symbol.scope, new_symbol.name)) {
    diagnostics.error(
        new_symbol.name_range,
        "duplicate declaration of '" + new_symbol.name + "' in the same " +
            std::string(scope_kind_name(owner.kind)) + " scope");
    diagnostics.note(symbol(*previous).name_range, "previous declaration is here");
    return {};
  }

  assert(symbols_.size() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const SymbolId id{static_cast<std::uint32_t>(symbols_.size())};
  symbols_.push_back(std::move(new_symbol));
  owner.symbols.push_back(id);
  return id;
}

std::optional<SymbolId> SymbolTable::lookup_direct(
    ScopeId scope_id, std::string_view name) const {
  const Scope &owner = scope(scope_id);
  for (SymbolId id : owner.symbols) {
    if (symbol(id).name == name) {
      return id;
    }
  }
  return std::nullopt;
}

std::optional<SymbolId> SymbolTable::lookup(ScopeId scope_id, std::string_view name) const {
  ScopeId current = scope_id;
  while (current.is_valid()) {
    if (const std::optional<SymbolId> found = lookup_direct(current, name)) {
      return found;
    }
    current = scope(current).parent;
  }
  return std::nullopt;
}

const Scope &SymbolTable::scope(ScopeId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < scopes_.size());
  return scopes_[id.value];
}

Scope &SymbolTable::scope_mut(ScopeId id) {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < scopes_.size());
  return scopes_[id.value];
}

const Symbol &SymbolTable::symbol(SymbolId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < symbols_.size());
  return symbols_[id.value];
}

Symbol &SymbolTable::symbol_mut(SymbolId id) {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < symbols_.size());
  return symbols_[id.value];
}

std::size_t SymbolTable::scope_count() const {
  return scopes_.size();
}

std::size_t SymbolTable::symbol_count() const {
  return symbols_.size();
}

std::string_view scope_kind_name(ScopeKind kind) {
  switch (kind) {
  case ScopeKind::Package: return "package";
  case ScopeKind::File: return "file";
  case ScopeKind::ImportedPackage: return "imported package";
  case ScopeKind::Type: return "type";
  case ScopeKind::Procedure: return "procedure";
  case ScopeKind::Block: return "block";
  case ScopeKind::Parametric: return "parametric";
  }
  return "unknown";
}

std::string_view symbol_kind_name(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Import: return "import";
  case SymbolKind::UnresolvedDeclaration: return "unresolved declaration";
  case SymbolKind::Type: return "type";
  case SymbolKind::Constant: return "constant";
  case SymbolKind::Variable: return "variable";
  case SymbolKind::Procedure: return "procedure";
  case SymbolKind::Parameter: return "parameter";
  case SymbolKind::Local: return "local";
  case SymbolKind::Field: return "field";
  case SymbolKind::EnumMember: return "enum member";
  case SymbolKind::UnionAlternative: return "union alternative";
  case SymbolKind::TypeParameter: return "type parameter";
  case SymbolKind::ValueParameter: return "value parameter";
  }
  return "unknown";
}

} // namespace draft
