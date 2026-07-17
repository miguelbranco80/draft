// Stable lexical scopes and semantic symbols.
//
// SymbolTable is the name-identity authority for one resolved Draft program.
// Package declarations, file-local imports, types, procedures, parameters,
// locals, and members receive stable SymbolId values. Lookup walks explicit
// parent scopes; it never consults process globals, filesystem paths, or a hash
// table with nondeterministic iteration.
//
// The table initially records declarations before every type is known. Symbol
// type and classification may therefore be completed by later semantic passes,
// but name, declaration source, owning scope, and visibility never change.
// Duplicate declarations are diagnosed against the exact new range and return
// an invalid ID so later passes cannot accidentally treat the duplicate as the
// canonical binding.
//
// Relevant specification: 01-core-language.md sections 3-4 and file-local
// import rules.

#pragma once

#include "sema/type.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/syntax_tree.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

struct ScopeId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const ScopeId &) const = default;
};

struct SymbolId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const SymbolId &) const = default;
};

enum class ScopeKind {
  Package,
  File,
  ImportedPackage,
  Type,
  Procedure,
  Block,
  Parametric,
};

enum class SymbolKind {
  Import,
  UnresolvedDeclaration,
  Type,
  Constant,
  Variable,
  Procedure,
  Parameter,
  Local,
  Field,
  EnumMember,
  UnionAlternative,
  TypeParameter,
  ValueParameter,
};

enum class Visibility {
  Private,
  Public,
};

// SyntaxReference identifies a declaration node in one parsed file. NodeId is
// meaningful only in that file's SyntaxTree, so FileId is always carried with it.
// Generated source later follows the same rule and remains distinguishable by
// its own FileId and source-map metadata.
struct SyntaxReference {
  FileId file;
  NodeId node;

  bool operator==(const SyntaxReference &) const = default;
};

// Symbol flags describe source-level storage or linkage properties orthogonal to
// SymbolKind. A foreign procedure is still a procedure; exported controls its C
// linker definition, while public controls Draft package visibility.
struct SymbolFlags {
  bool is_thread_local = false;
  bool foreign = false;
  bool exported = false;
  bool parametric = false;
};

struct Symbol {
  std::string name;
  SymbolKind kind = SymbolKind::UnresolvedDeclaration;
  Visibility visibility = Visibility::Private;
  SymbolFlags flags;
  ScopeId scope;
  TypeId type;
  SyntaxReference syntax;
  SourceRange name_range;
};

// Scope owns bindings declared directly in one lexical region. symbol IDs remain
// in declaration order for canonical diagnostics and interface construction.
// parent is invalid only for the package scope.
struct Scope {
  ScopeKind kind = ScopeKind::Block;
  ScopeId parent;
  SourceRange range;
  std::vector<SymbolId> symbols;
};

class SymbolTable {
public:
  [[nodiscard]] ScopeId add_scope(ScopeKind kind, ScopeId parent, SourceRange range);

  // Declares a new binding and diagnoses a duplicate in the same scope. Shadowing
  // an ancestor is permitted here; language-specific restrictions can be checked
  // by the pass that understands the declaration category.
  [[nodiscard]] SymbolId declare(Symbol symbol, DiagnosticSink &diagnostics);

  [[nodiscard]] std::optional<SymbolId> lookup_direct(
      ScopeId scope, std::string_view name) const;
  [[nodiscard]] std::optional<SymbolId> lookup(ScopeId scope, std::string_view name) const;

  [[nodiscard]] const Scope &scope(ScopeId id) const;
  [[nodiscard]] Scope &scope_mut(ScopeId id);
  [[nodiscard]] const Symbol &symbol(SymbolId id) const;
  [[nodiscard]] Symbol &symbol_mut(SymbolId id);
  [[nodiscard]] std::size_t scope_count() const;
  [[nodiscard]] std::size_t symbol_count() const;

private:
  std::vector<Scope> scopes_;
  std::vector<Symbol> symbols_;
};

[[nodiscard]] std::string_view scope_kind_name(ScopeKind kind);
[[nodiscard]] std::string_view symbol_kind_name(SymbolKind kind);

} // namespace draft
