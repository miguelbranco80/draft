// Direct append-only Draft scopes, symbol lookup, and frozen task suffixes.
//
// Direct scope lookup is currently linear in declarations in that scope. This
// preserves declaration order and keeps duplicate behavior obvious. Large real
// packages may justify a secondary deterministic index later; the vector remains
// canonical and lookup semantics do not change. A procedure task reads
// existing Symbol rows through a read-only prefix, records new rows plus
// additions to old scopes, and lets the coordinator publish only that append
// packet. The canonical table is never copied or mutated by the task.

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

SymbolTable::SymbolTable(
    AppendOnlyOverlayTag,
    const SymbolTable &base,
    bool permits_prefix_patches)
    : base_(&base), base_scope_count_(base.scope_count()),
      base_symbol_count_(base.symbol_count()),
      permits_prefix_patches_(permits_prefix_patches) {
  assert(base.base_ == nullptr);
}

bool ScopeId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

bool SymbolId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

ScopeId SymbolTable::add_scope(ScopeKind kind, ScopeId parent, SourceRange range) {
  assert(scope_count() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  if (parent.is_valid()) {
    assert(static_cast<std::size_t>(parent.value) < scope_count());
  } else {
    assert(kind == ScopeKind::Package);
  }
  const ScopeId id{static_cast<std::uint32_t>(scope_count())};
  scopes_.push_back({kind, parent, range, {}});
  return id;
}

ExistingScopeSymbolAppend *SymbolTable::base_scope_append(ScopeId scope_id) {
  for (ExistingScopeSymbolAppend &appended : base_scope_symbols_) {
    if (appended.scope == scope_id) return &appended;
  }
  base_scope_symbols_.push_back({scope_id, {}});
  return &base_scope_symbols_.back();
}

SymbolId SymbolTable::declare(Symbol new_symbol, DiagnosticSink &diagnostics) {
  assert(new_symbol.scope.is_valid());
  const ScopeKind owner_kind = scope(new_symbol.scope).kind;
  if (const std::optional<SymbolId> previous = lookup_direct(new_symbol.scope, new_symbol.name)) {
    diagnostics.error(
        new_symbol.name_range,
        "duplicate declaration of '" + new_symbol.name + "' in the same " +
            std::string(scope_kind_name(owner_kind)) + " scope");
    diagnostics.note(symbol(*previous).name_range, "previous declaration is here");
    return {};
  }

  assert(symbol_count() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const SymbolId id{static_cast<std::uint32_t>(symbol_count())};
  symbols_.push_back(std::move(new_symbol));
  const ScopeId owner_scope = symbols_.back().scope;
  if (static_cast<std::size_t>(owner_scope.value) < base_scope_count_) {
    base_scope_append(owner_scope)->symbols.push_back(id);
  } else {
    scope_mut(owner_scope).symbols.push_back(id);
  }
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
  if (static_cast<std::size_t>(scope_id.value) < base_scope_count_) {
    for (const ExistingScopeSymbolAppend &appended : base_scope_symbols_) {
      if (appended.scope != scope_id) continue;
      for (SymbolId id : appended.symbols) {
        if (symbol(id).name == name) return id;
      }
      break;
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
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < scope_count());
  if (index < base_scope_count_) {
    assert(base_ != nullptr);
    return base_->scope(id);
  }
  return scopes_[index - base_scope_count_];
}

Scope &SymbolTable::scope_mut(ScopeId id) {
  assert(id.is_valid());
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < scope_count());
  assert(index >= base_scope_count_);
  return scopes_[index - base_scope_count_];
}

std::vector<SymbolId> SymbolTable::symbols_in_scope(ScopeId id) const {
  std::vector<SymbolId> result = scope(id).symbols;
  if (static_cast<std::size_t>(id.value) < base_scope_count_) {
    for (const ExistingScopeSymbolAppend &appended : base_scope_symbols_) {
      if (appended.scope == id) {
        result.insert(
            result.end(), appended.symbols.begin(), appended.symbols.end());
        break;
      }
    }
  }
  return result;
}

const Symbol &SymbolTable::symbol(SymbolId id) const {
  assert(id.is_valid());
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < symbol_count());
  if (index < base_symbol_count_) {
    assert(base_ != nullptr);
    if (const SymbolTablePatch *patch = find_prefix_patch(id)) {
      return patch->symbol;
    }
    return base_->symbol(id);
  }
  return symbols_[index - base_symbol_count_];
}

Symbol &SymbolTable::symbol_mut(SymbolId id) {
  assert(id.is_valid());
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < symbol_count());
  if (index < base_symbol_count_) {
    assert(base_ != nullptr);
    assert(permits_prefix_patches_);
    if (SymbolTablePatch *patch = find_prefix_patch(id)) {
      return patch->symbol;
    }
    prefix_patches_.push_back({id, base_->symbol(id)});
    return prefix_patches_.back().symbol;
  }
  return symbols_[index - base_symbol_count_];
}

std::size_t SymbolTable::scope_count() const {
  return base_scope_count_ + scopes_.size();
}

std::size_t SymbolTable::symbol_count() const {
  return base_symbol_count_ + symbols_.size();
}

SymbolTable SymbolTable::fork_append_only() const {
  return SymbolTable(AppendOnlyOverlayTag{}, *this, false);
}

SymbolTable SymbolTable::fork_with_prefix_patches() const {
  return SymbolTable(AppendOnlyOverlayTag{}, *this, true);
}

SymbolTableAppend SymbolTable::appended_since(
    std::size_t base_scope_count,
    std::size_t base_symbol_count) const {
  assert(base_ != nullptr);
  assert(base_scope_count == base_scope_count_);
  assert(base_symbol_count == base_symbol_count_);
  SymbolTableAppend appended;
  appended.base_scope_count = base_scope_count;
  appended.base_symbol_count = base_symbol_count;
  appended.scopes = scopes_;
  appended.symbols = symbols_;
  appended.existing_scope_symbols = base_scope_symbols_;
  return appended;
}

std::vector<SymbolTablePatch> SymbolTable::prefix_patches_since(
    std::size_t base_scope_count,
    std::size_t base_symbol_count) const {
  assert(base_ != nullptr);
  assert(base_scope_count == base_scope_count_);
  assert(base_symbol_count == base_symbol_count_);
  return prefix_patches_;
}

void SymbolTable::append_exact(SymbolTableAppend appended) {
  assert(base_ == nullptr);
  assert(scope_count() == appended.base_scope_count);
  assert(symbol_count() == appended.base_symbol_count);
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

void SymbolTable::apply_patch_exact(SymbolTablePatch patch) {
  assert(base_ == nullptr);
  assert(patch.id.is_valid());
  assert(static_cast<std::size_t>(patch.id.value) < symbol_count());
  symbols_[patch.id.value] = std::move(patch.symbol);
}

SymbolTablePatch *SymbolTable::find_prefix_patch(SymbolId id) {
  for (SymbolTablePatch &patch : prefix_patches_) {
    if (patch.id == id) return &patch;
  }
  return nullptr;
}

const SymbolTablePatch *SymbolTable::find_prefix_patch(SymbolId id) const {
  for (const SymbolTablePatch &patch : prefix_patches_) {
    if (patch.id == id) return &patch;
  }
  return nullptr;
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
  case SymbolKind::VariantAlternative: return "variant alternative";
  case SymbolKind::TypeParameter: return "type parameter";
  case SymbolKind::ValueParameter: return "value parameter";
  }
  return "unknown";
}

} // namespace draft
