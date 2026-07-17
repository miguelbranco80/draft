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

#include "sema/symbol.h"
#include "sema/type.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <string>
#include <string_view>
#include <vector>

namespace draft {

// FileSemanticScope connects one parsed file to the scope containing its import
// aliases. Ordinary declarations live in package_scope and are therefore
// visible through every file scope's parent link.
struct FileSemanticScope {
  FileId file;
  ScopeId scope;
};

// ImportBinding retains the canonical source spelling of the imported package
// path separately from the local alias symbol. Workspace resolution later maps
// package_path to a root-qualified package identity and fills dependency edges.
struct ImportBinding {
  SymbolId symbol;
  std::string package_path;
  SyntaxReference syntax;
};

enum class SemanticSiteKind {
  Documentation,
  Judgment,
  SynthesisDeclaration,
  ConditionalDeclaration,
  DenialDeclaration,
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
  std::vector<ImportBinding> imports;
  std::vector<SemanticSite> sites;
  std::vector<NativeBinding> native_bindings;
};

// Collects declarations from every parsed Draft file in canonical package-file
// order. Assembly files have no Draft declarations and remain in LoadedPackage
// for the target assembly/link pass. Syntax errors may already exist; this pass
// continues over recovered trees and reports only semantic collection errors.
[[nodiscard]] SemanticPackage collect_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &package,
    DiagnosticSink &diagnostics);

[[nodiscard]] std::string_view semantic_site_kind_name(SemanticSiteKind kind);

} // namespace draft
