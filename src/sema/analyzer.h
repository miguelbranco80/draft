// Package declaration collection and provider-independent semantic metadata.
//
// This pass is deliberately narrower than full type checking. It establishes
// every package declaration that can be known without evaluating expressions,
// creates file-local import scopes, assigns nominal type identities, and keeps
// the source sites needed by later target selection, denial checking, judging,
// and synthesis. Later passes enrich these stable rows instead of rebuilding a
// second, disconnected view of the package.
//
// `when` branches are parsed but not collected here because selecting both
// branches would create false duplicate declarations. Constant evaluation will
// select one branch and feed that declaration region back through the same
// collection rules. `deny` does not select a branch, so its contents are
// collected immediately while the denial site is retained as policy metadata.
//
// Relevant specification: 01-core-language.md sections 3-4, 03-agent-synthesis.md,
// 04-native-interop.md, and 05-denials-validation.md.

#pragma once

#include "sema/constant_value.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// EffectKind lives with semantic package records because imported interface
// summaries must retain these tags before the HIR effect-composition pass runs.
enum class EffectKind {
  PackageGlobal,
  RuntimeAssert,
  ContextField,
  Assembly,
  Unchecked,
  UnknownCall,
};

// FileSemanticScope connects one parsed file to the scope containing its import
// aliases. Ordinary declarations live in package_scope and are therefore
// visible through every file scope's parent link.
struct FileSemanticScope {
  FileId file;
  ScopeId scope;
};

// OwnedSemanticScope records the lexical scope introduced by a declaration.
// Parametric declarations can own both a Parametric scope and a nested Type or
// Procedure scope, so the same owner may appear more than once with distinct
// ScopeKind values.
struct OwnedSemanticScope {
  SymbolId owner;
  ScopeId scope;
};

// AggregateMember connects nominal type identity to the member symbol and its
// byte offset. An invalid/unknown layout uses offset zero until instantiation or
// compile-time selection completes it; callers must consult the owning Type's
// layout.known bit before treating the offset as physical.
struct AggregateMember {
  SymbolId owner;
  SymbolId member;
  std::uint64_t offset = 0;
};

enum class TypeConstraintKind {
  AnyType,
  Integer,
  Float,
  Number,
  CompileTimeValue,
};

// ParametricParameterRecord preserves the closed constraint vocabulary. Value
// parameters use CompileTimeValue and carry their required value type on the
// parameter Symbol. Type parameters carry a unique TypeParameter TypeId.
struct ParametricParameterRecord {
  SymbolId owner;
  SymbolId parameter;
  TypeConstraintKind constraint = TypeConstraintKind::AnyType;
};

// Concrete procedure symbols are created during body checking, after the
// declaration graph is stable. Retaining their source template relationship
// lets denial and diagnostic passes apply declaration contracts to every
// monomorphized body without copying policy records.
struct ParametricInstanceRecord {
  SymbolId source;
  SymbolId instance;
};

// ImportBinding retains the canonical source spelling of the imported package
// path separately from the local alias symbol. Workspace resolution later maps
// package_path to a root-qualified package identity and fills dependency edges.
struct ImportBinding {
  SymbolId symbol;
  std::string package_path;
  SyntaxReference syntax;
  // Filled by interface binding from WorkspaceGraph. Source collection leaves
  // these empty because physical import spelling alone is not semantic identity.
  std::string root_identity;
  std::string root_relative_path;
};

// ImportedSymbol connects a consumer-local proxy SymbolId to the file-local
// import alias and the dependency's public declaration identity. proxy is used
// in HIR exactly like a local symbol; later package/MIR lowering consults this
// row to emit an inter-package reference instead of allocating local storage.
// Ready public constants carry their value so `when` and runtime constant
// folding do not reevaluate dependency source.
struct ImportedSymbol {
  SymbolId import_symbol;
  SymbolId proxy;
  std::string root_identity;
  std::string root_relative_path;
  std::string public_name;
  bool has_constant = false;
  ConstantValue constant;
  bool has_effect_summary = false;
  std::string native_provider;
  std::string native_linker_name_spelling;
};

// ImportedEffect is one canonical dependency effect attached to a public
// procedure proxy. Origin fields identify referenced dependency declarations
// without importing their private SymbolIds into the consumer.
struct ImportedEffect {
  SymbolId procedure_proxy;
  EffectKind kind = EffectKind::UnknownCall;
  std::string root_identity;
  std::string root_relative_path;
  std::string declaration;
  std::string detail;
};

// ImportedType preserves nominal identity after an interface type has been
// reconstructed in a consumer-local TypeStore. This prevents a downstream
// interface from rebaptizing `dep:T` as `consumer:T` when it merely exposes the
// dependency type in a public signature. Structural types need no provenance;
// their identity follows their recursively translated components.
struct ImportedType {
  TypeId type;
  std::string root_identity;
  std::string root_relative_path;
  std::string public_name;
};

// DeclarationDenial attaches a lexical `deny` contract to every declaration
// contributed by that declaration region. The selector syntax is resolved only
// after imports and declarations are complete; parametric instantiations retain
// the same contract through their owner SymbolId.
struct DeclarationDenial {
  SymbolId declaration;
  SyntaxReference denial;
};

enum class SemanticSiteKind {
  Documentation,
  Judgment,
  SynthesisDeclaration,
  SynthesisMember,
  SynthesisStatement,
  SynthesisExpression,
  SynthesisAssembly,
  ConditionalDeclaration,
  ConditionalMember,
  ConditionalStatement,
  DenialDeclaration,
  DenialMember,
};

// A semantic site is a zero-runtime source construct anchored in a lexical
// scope. Documentation may additionally anchor to the first symbol of the
// immediately following declaration. An invalid anchor means package-level or
// not-yet-selected metadata; it never means the site was discarded.
struct SemanticSite {
  SemanticSiteKind kind = SemanticSiteKind::Documentation;
  SyntaxReference syntax;
  ScopeId scope;
  SymbolId anchor;
  // Synthesis expressions receive their contextual result type during body
  // checking. Other categories leave this invalid until their grammar-specific
  // obligation builder computes a more detailed expected form.
  TypeId expected_type;
};

enum class NativeBindingKind {
  ForeignImport,
  CExport,
};

// NativeBinding records linkage facts visible during declaration collection.
// linker_name_spelling is the exact quoted token, including quotes and escapes,
// or the local Draft name when no explicit spelling is present. Literal decoding
// and ABI validation belong to the interop semantic pass.
struct NativeBinding {
  NativeBindingKind kind = NativeBindingKind::ForeignImport;
  SymbolId symbol;
  std::string provider;
  std::string linker_name_spelling;
  SyntaxReference syntax;
};

// SemanticPackage is the append-only semantic foundation for one folder
// package. Public fields are intentional: compiler passes operate on explicit
// table rows and stable IDs rather than a deep accessor/object hierarchy.
struct SemanticPackage {
  std::string short_name;
  TypeStore types;
  SymbolTable symbols;
  ScopeId package_scope;
  std::vector<FileSemanticScope> files;
  std::vector<OwnedSemanticScope> owned_scopes;
  std::vector<AggregateMember> aggregate_members;
  std::vector<ParametricParameterRecord> parametric_parameters;
  std::vector<ParametricInstanceRecord> parametric_instances;
  std::vector<ImportBinding> imports;
  std::vector<ImportedSymbol> imported_symbols;
  std::vector<ImportedType> imported_types;
  std::vector<ImportedEffect> imported_effects;
  std::vector<DeclarationDenial> declaration_denials;
  std::vector<SemanticSite> sites;
  std::vector<NativeBinding> native_bindings;
};

// One selection chooses the true or false branch of a particular parsed `when`.
// References are stable while the owning SyntaxTree is unchanged. Keeping the
// decision outside SyntaxTree permits deterministic semantic rounds without
// mutating or cloning parsed source.
struct ConditionalSelection {
  SyntaxReference site;
  bool select_true = false;
};

struct ConditionalSelections {
  std::vector<ConditionalSelection> entries;

  [[nodiscard]] const ConditionalSelection *find(SyntaxReference site) const;
};

// Collects declarations from every parsed Draft file in canonical package-file
// order. Assembly files have no Draft declarations and remain in LoadedPackage
// for the target assembly/link pass. Syntax errors may already exist; this pass
// continues over recovered trees and reports only semantic collection errors.
[[nodiscard]] SemanticPackage collect_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &package,
    DiagnosticSink &diagnostics);

// Selection-aware form used by staged semantic analysis. A selected branch is
// collected into the surrounding scope; an absent selection remains a pending
// ConditionalDeclaration site and contributes no declarations.
[[nodiscard]] SemanticPackage collect_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &package,
    const ConditionalSelections &selections,
    DiagnosticSink &diagnostics);

[[nodiscard]] std::string_view semantic_site_kind_name(SemanticSiteKind kind);

} // namespace draft
