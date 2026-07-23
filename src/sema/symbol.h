// Stable lexical scopes and semantic symbols.
//
// SymbolTable is the name-identity authority for one resolved Draft program.
// Package declarations, file-local imports, types, procedures, parameters,
// locals, and members receive stable SymbolId values. Lookup walks explicit
// parent scopes; it never consults process globals, filesystem paths, or a hash
// table with nondeterministic iteration.
//
// The table initially records declarations before every type is known. Symbol
// type, classification, and procedure call metadata may therefore be completed
// by later semantic passes, but name, declaration source, owning scope, and
// visibility never change.
// Duplicate declarations are diagnosed against the exact new range and return
// an invalid ID so later passes cannot accidentally treat the duplicate as the
// canonical binding.
//
// Relevant specification: docs/specification/01-core-language.md sections 3-4 and file-local
// import rules.

#pragma once

#include "sema/constant_value.h"
#include "sema/type.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/syntax_tree.h"

#include <cstddef>
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
  VariantAlternative,
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

// ProcedureParameter records the source-level calling contract that is not
// part of a procedure TypeId. name is empty for `_`, which can be supplied only
// positionally. A source declaration initially carries default_syntax; the
// package-interface barrier evaluates that expression once and publishes the
// canonical value. Imported declarations carry only the ready value and never
// retain a source coordinate from another package.
//
// Rows are in physical signature order, including expansion of a grouped
// declaration such as `left, right: i64`. Static packs are intentionally absent:
// they name an open tail rather than one physical parameter. The vector must
// therefore have exactly one row per fixed procedure parameter whenever it is
// nonempty.
struct ProcedureParameter {
  std::string name;
  SourceRange name_range;
  bool has_default = false;
  SyntaxReference default_syntax;
  bool default_is_ready = false;
  ConstantValue default_value;
};

struct Symbol {
  std::string name;
  // Most symbols use their source name for both lookup and native linkage.
  // Nested procedures are different: the same short name may appear in two
  // unrelated lexical blocks, while both bodies still become package-level
  // machine functions. linkage_name gives those procedures a deterministic,
  // compiler-owned identity without changing the spelling used by lookup,
  // diagnostics, documentation, or package interfaces. An empty value means
  // "use name", which keeps ordinary declarations direct and unsurprising.
  std::string linkage_name;
  SymbolKind kind = SymbolKind::UnresolvedDeclaration;
  Visibility visibility = Visibility::Private;
  SymbolFlags flags;
  ScopeId scope;
  TypeId type;
  SyntaxReference syntax;
  SourceRange name_range;
  // Declaration-only call metadata. Procedure type values deliberately omit
  // it, which keeps assignment and ABI compatibility structural and makes
  // named/default calls available only when the callee declaration is known.
  std::vector<ProcedureParameter> procedure_parameters;
};

// Scope owns bindings declared directly in one lexical region. Symbol IDs
// remain in declaration order for canonical diagnostics and interface
// construction. An append-only task overlay reads a canonical prefix Scope
// without mutating this vector and records its new direct bindings in a
// separate ExistingScopeSymbolAppend. Call symbols_in_scope when the table may
// be such an overlay. parent is invalid only for the package scope.
struct Scope {
  ScopeKind kind = ScopeKind::Block;
  ScopeId parent;
  SourceRange range;
  std::vector<SymbolId> symbols;
};

// ExistingScopeSymbolAppend records bindings added to a scope which belongs to
// a frozen package prefix. New scopes carry their own complete symbol vectors;
// only pre-existing package/procedure/block scopes need these explicit rows.
struct ExistingScopeSymbolAppend {
  ScopeId scope;
  std::vector<SymbolId> symbols;
};

// SymbolTableAppend is one exact task-local append over a frozen symbol-table
// prefix. New scope and symbol rows retain their final IDs. Existing scope
// additions are separate because declaring a nested procedure or concrete
// instance can add a new symbol to a scope whose row predates the task.
struct SymbolTableAppend {
  std::size_t base_scope_count = 0;
  std::size_t base_symbol_count = 0;
  std::vector<Scope> scopes;
  std::vector<Symbol> symbols;
  std::vector<ExistingScopeSymbolAppend> existing_scope_symbols;
};

// SymbolTablePatch is one declaration task's completed replacement for a
// symbol below its frozen prefix. Declaration collection establishes names,
// scopes, source identities, and flags; later TypeIdentity or TypeMemberTypes
// products fill classification and TypeId without changing the SymbolId. The
// coordinator remaps type before installing the full prospective row.
struct SymbolTablePatch {
  SymbolId id;
  Symbol symbol;
};

class SymbolTable {
public:
  SymbolTable() = default;

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
  // Returns direct bindings in declaration order, including task-local
  // additions to a scope owned by an immutable overlay prefix.
  [[nodiscard]] std::vector<SymbolId> symbols_in_scope(ScopeId id) const;
  [[nodiscard]] const Symbol &symbol(SymbolId id) const;
  [[nodiscard]] Symbol &symbol_mut(SymbolId id);
  [[nodiscard]] std::size_t scope_count() const;
  [[nodiscard]] std::size_t symbol_count() const;

  // Creates an empty append-only overlay over this canonical table. Existing
  // scopes and symbols are read through base; declarations occupy local suffix
  // rows, with additions to base scopes recorded separately. base must outlive
  // the overlay and may not itself be an overlay.
  [[nodiscard]] SymbolTable fork_append_only() const;

  // Creates an overlay which may copy individual prefix Symbol rows on first
  // mutation. New scopes, symbols, and additions to existing scopes remain the
  // same append-only suffix used by body tasks. Keeping this entry point
  // separate makes prefix refinement an explicit declaration-phase authority.
  [[nodiscard]] SymbolTable fork_with_prefix_patches() const;

  // Extracts one overlay's local rows, including explicit binding additions to
  // canonical prefix scopes, or publishes those rows to the canonical table.
  [[nodiscard]] SymbolTableAppend appended_since(
      std::size_t base_scope_count,
      std::size_t base_symbol_count) const;
  [[nodiscard]] std::vector<SymbolTablePatch> prefix_patches_since(
      std::size_t base_scope_count,
      std::size_t base_symbol_count) const;
  void append_exact(SymbolTableAppend appended);
  void apply_patch_exact(SymbolTablePatch patch);

private:
  struct AppendOnlyOverlayTag {};
  SymbolTable(
      AppendOnlyOverlayTag,
      const SymbolTable &base,
      bool permits_prefix_patches);

  [[nodiscard]] ExistingScopeSymbolAppend *base_scope_append(ScopeId scope);
  [[nodiscard]] SymbolTablePatch *find_prefix_patch(SymbolId id);
  [[nodiscard]] const SymbolTablePatch *find_prefix_patch(SymbolId id) const;

  const SymbolTable *base_ = nullptr;
  std::size_t base_scope_count_ = 0;
  std::size_t base_symbol_count_ = 0;
  [[maybe_unused]] bool permits_prefix_patches_ = false;
  // One declaration product normally refines one root or a small member list.
  // Preserve first-mutation order and use a direct scan instead of introducing
  // a hash/index whose order would become another semantic invariant.
  std::vector<SymbolTablePatch> prefix_patches_;
  std::vector<Scope> scopes_;
  std::vector<Symbol> symbols_;
  std::vector<ExistingScopeSymbolAppend> base_scope_symbols_;
};

[[nodiscard]] std::string_view scope_kind_name(ScopeKind kind);
[[nodiscard]] std::string_view symbol_kind_name(SymbolKind kind);

} // namespace draft
