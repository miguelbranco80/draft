// Direct append-only Draft scopes, symbol lookup, and frozen task suffixes.
//
// Direct scope lookup is currently linear in declarations in that scope. This
// preserves declaration order and keeps duplicate behavior obvious. Large real
// packages may justify a secondary deterministic index later; the vector remains
// canonical and lookup semantics do not change. A procedure task freezes
// existing Symbol rows in its private snapshot, records new rows plus additions
// to old scopes, and lets the coordinator publish only that append packet.

#include "sema/symbol.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
  assert(static_cast<std::size_t>(id.value) >=
         immutable_symbol_prefix_size_);
  return symbols_[id.value];
}

std::size_t SymbolTable::scope_count() const {
  return scopes_.size();
}

std::size_t SymbolTable::symbol_count() const {
  return symbols_.size();
}

void SymbolTable::freeze_existing_rows() {
  immutable_symbol_prefix_size_ = symbols_.size();
}

SymbolTableAppend SymbolTable::appended_since(
    std::size_t base_scope_count,
    std::size_t base_symbol_count) const {
  assert(base_scope_count <= scopes_.size());
  assert(base_symbol_count <= symbols_.size());
  SymbolTableAppend appended;
  appended.base_scope_count = base_scope_count;
  appended.base_symbol_count = base_symbol_count;
  appended.scopes.assign(
      scopes_.begin() + static_cast<std::ptrdiff_t>(base_scope_count),
      scopes_.end());
  appended.symbols.assign(
      symbols_.begin() + static_cast<std::ptrdiff_t>(base_symbol_count),
      symbols_.end());

  // A new binding always receives a suffix SymbolId. Scanning only the
  // pre-existing scopes therefore isolates exactly the additions without
  // storing or comparing their complete prior symbol vectors.
  for (std::size_t index = 0; index < base_scope_count; ++index) {
    ExistingScopeSymbolAppend scope_append;
    scope_append.scope = ScopeId{static_cast<std::uint32_t>(index)};
    for (SymbolId symbol : scopes_[index].symbols) {
      if (static_cast<std::size_t>(symbol.value) >= base_symbol_count) {
        scope_append.symbols.push_back(symbol);
      }
    }
    if (!scope_append.symbols.empty()) {
      appended.existing_scope_symbols.push_back(std::move(scope_append));
    }
  }
  return appended;
}

void SymbolTable::append_exact(SymbolTableAppend appended) {
  assert(scopes_.size() == appended.base_scope_count);
  assert(symbols_.size() == appended.base_symbol_count);
  scopes_.insert(
      scopes_.end(),
      std::make_move_iterator(appended.scopes.begin()),
      std::make_move_iterator(appended.scopes.end()));
  symbols_.insert(
      symbols_.end(),
      std::make_move_iterator(appended.symbols.begin()),
      std::make_move_iterator(appended.symbols.end()));
  for (ExistingScopeSymbolAppend &scope_append :
       appended.existing_scope_symbols) {
    assert(scope_append.scope.is_valid());
    assert(static_cast<std::size_t>(scope_append.scope.value) <
           appended.base_scope_count);
    Scope &owner = scopes_[scope_append.scope.value];
    owner.symbols.insert(
        owner.symbols.end(),
        std::make_move_iterator(scope_append.symbols.begin()),
        std::make_move_iterator(scope_append.symbols.end()));
  }
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
