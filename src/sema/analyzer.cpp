// Deterministic first semantic pass over one parsed folder package.
//
// The collector follows source-file and syntax-child order. That order controls
// stable SymbolId allocation and duplicate diagnostics, so no filesystem scan,
// hash-table iteration, or pointer address can affect the result.

#include "sema/analyzer.h"

#include "syntax/token.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

struct SourceName {
  std::string text;
  SourceRange range;
};

struct CollectionContext {
  SymbolFlags flags;
  std::string foreign_provider;
  std::vector<SyntaxReference> denials;
};

[[nodiscard]] bool token_is_contextual_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
         kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
         kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
         kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory;
}

[[nodiscard]] bool node_is_type_syntax(NodeKind kind) {
  switch (kind) {
  case NodeKind::NamedType:
  case NodeKind::PointerType:
  case NodeKind::MultiPointerType:
  case NodeKind::SliceType:
  case NodeKind::ArrayType:
  case NodeKind::SimdType:
  case NodeKind::TupleType:
  case NodeKind::ProcedureType:
  case NodeKind::DistinctType:
  case NodeKind::StructType:
  case NodeKind::EnumType:
  case NodeKind::TaggedUnionType:
  case NodeKind::RawUnionType:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] std::optional<TypeKind> nominal_type_kind(NodeKind kind) {
  switch (kind) {
  case NodeKind::StructType: return TypeKind::Struct;
  case NodeKind::EnumType: return TypeKind::Enum;
  case NodeKind::TaggedUnionType: return TypeKind::TaggedUnion;
  case NodeKind::RawUnionType: return TypeKind::RawUnion;
  default: return std::nullopt;
  }
}

class DeclarationCollector {
public:
  DeclarationCollector(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      const ConditionalSelections &selections,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), selections_(selections),
        diagnostics_(diagnostics) {
    semantic_.short_name = loaded_.short_name;
  }

  [[nodiscard]] SemanticPackage collect() {
    // The package range is used only as a broad owner range. Prefer the first
    // parsed file so the scope remains source-located even when assembly sorts
    // before Draft source. A valid package is guaranteed to have such a file.
    SourceRange package_range = SourceRange::invalid();
    for (const LoadedPackageFile &file : loaded_.files) {
      if (file.syntax.has_value() && file.syntax->root().is_valid()) {
        package_range = file.syntax->node(file.syntax->root()).range;
        break;
      }
    }
    semantic_.package_scope =
        semantic_.symbols.add_scope(ScopeKind::Package, {}, package_range);

    // Allocate every file scope before collecting names. This makes the scope
    // graph complete independently of semantic errors in an earlier file.
    for (const LoadedPackageFile &file : loaded_.files) {
      if (!file.syntax.has_value() || !file.syntax->root().is_valid()) {
        continue;
      }
      const SourceRange range = file.syntax->node(file.syntax->root()).range;
      const ScopeId scope = semantic_.symbols.add_scope(
          ScopeKind::File, semantic_.package_scope, range);
      semantic_.files.push_back({file.source, scope});
    }

    std::size_t semantic_file_index = 0;
    for (const LoadedPackageFile &file : loaded_.files) {
      if (!file.syntax.has_value() || !file.syntax->root().is_valid()) {
        continue;
      }
      assert(semantic_file_index < semantic_.files.size());
      collect_file(*file.syntax, semantic_.files[semantic_file_index].scope);
      ++semantic_file_index;
    }
    return std::move(semantic_);
  }

private:
  [[nodiscard]] SourceName token_name(const SyntaxTree &tree, std::uint32_t index) const {
    const Token &token = tree.token(index);
    return {std::string(sources_.text(token.range)), token.range};
  }

  [[nodiscard]] std::vector<SourceName> names_in_pattern(
      const SyntaxTree &tree, NodeId pattern_id) const {
    const SyntaxNode &pattern = tree.node(pattern_id);
    std::vector<SourceName> names;
    for (NodeId child_id : pattern.children) {
      const SyntaxNode &child = tree.node(child_id);
      if (child.kind != NodeKind::NameList) {
        continue;
      }
      for (std::uint32_t index = child.token_begin; index < child.token_end; ++index) {
        if (token_is_contextual_name(tree.token(index).kind)) {
          names.push_back(token_name(tree, index));
        }
      }
    }
    return names;
  }

  [[nodiscard]] bool node_has_token_before(
      const SyntaxTree &tree,
      const SyntaxNode &node,
      std::uint32_t end,
      TokenKind wanted) const {
    const std::uint32_t bounded_end = end < node.token_end ? end : node.token_end;
    for (std::uint32_t index = node.token_begin; index < bounded_end; ++index) {
      if (tree.token(index).kind == wanted) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::optional<NodeId> declaration_payload(
      const SyntaxTree &tree, const SyntaxNode &declaration) const {
    if (declaration.children.size() < 2) {
      return std::nullopt;
    }
    const NodeId candidate = declaration.children.back();
    const NodeKind kind = tree.node(candidate).kind;
    if (kind == NodeKind::BindingPattern || kind == NodeKind::TuplePattern ||
        kind == NodeKind::ParametricParameterList) {
      return std::nullopt;
    }
    return candidate;
  }

  [[nodiscard]] SymbolKind declaration_kind(
      const SyntaxTree &tree,
      const SyntaxNode &declaration,
      const CollectionContext &context,
      const std::optional<NodeId> &payload) const {
    const std::uint32_t header_end = payload.has_value()
        ? tree.node(*payload).token_begin
        : declaration.token_end;
    const bool immutable = node_has_token_before(
        tree, declaration, header_end, TokenKind::ColonColon);
    if (!immutable) {
      return SymbolKind::Variable;
    }
    if (!payload.has_value()) {
      return SymbolKind::UnresolvedDeclaration;
    }

    const NodeKind payload_kind = tree.node(*payload).kind;
    if (payload_kind == NodeKind::Procedure) {
      return SymbolKind::Procedure;
    }
    // A bodyless `c proc` is a type in ordinary source but denotes an imported
    // procedure inside a foreign provider block.
    if (payload_kind == NodeKind::ProcedureType && context.flags.foreign) {
      return SymbolKind::Procedure;
    }
    if (node_is_type_syntax(payload_kind)) {
      return SymbolKind::Type;
    }
    // A bare name can denote either a type alias or a constant value. Preserve
    // that ambiguity until the complete package declaration set is available.
    if (payload_kind == NodeKind::NameExpression) {
      return SymbolKind::UnresolvedDeclaration;
    }
    return SymbolKind::Constant;
  }

  [[nodiscard]] std::string explicit_linker_name(
      const SyntaxTree &tree, const std::optional<NodeId> &payload) const {
    if (!payload.has_value()) {
      return {};
    }
    const SyntaxNode &node = tree.node(*payload);
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      const TokenKind kind = tree.token(index).kind;
      if (kind == TokenKind::KeywordProc) {
        break;
      }
      if (kind == TokenKind::StringLiteral) {
        return std::string(sources_.text(tree.token(index).range));
      }
    }
    return {};
  }

  [[nodiscard]] std::vector<SymbolId> collect_declaration(
      const SyntaxTree &tree,
      NodeId declaration_id,
      ScopeId scope,
      const CollectionContext &context) {
    const SyntaxNode &declaration = tree.node(declaration_id);
    if (declaration.children.empty()) {
      return {};
    }
    const NodeId pattern_id = declaration.children.front();
    const SyntaxNode &pattern = tree.node(pattern_id);
    if (pattern.kind != NodeKind::BindingPattern && pattern.kind != NodeKind::TuplePattern) {
      return {};
    }

    const std::optional<NodeId> payload = declaration_payload(tree, declaration);
    const SymbolKind kind = declaration_kind(tree, declaration, context, payload);
    SymbolFlags flags = context.flags;
    for (NodeId child_id : declaration.children) {
      if (tree.node(child_id).kind == NodeKind::ParametricParameterList) {
        flags.parametric = true;
      }
    }
    flags.is_thread_local = node_has_token_before(
        tree, declaration, pattern.token_begin, TokenKind::KeywordThreadLocal);
    const Visibility visibility = node_has_token_before(
        tree, declaration, pattern.token_begin, TokenKind::KeywordPub)
        ? Visibility::Public
        : Visibility::Private;

    const std::vector<SourceName> names = names_in_pattern(tree, pattern_id);
    if ((kind == SymbolKind::Type || kind == SymbolKind::Procedure) && names.size() > 1) {
      diagnostics_.error(
          pattern.range,
          std::string(symbol_kind_name(kind)) + " declaration requires one binding name");
    }

    std::vector<SymbolId> declared;
    for (const SourceName &name : names) {
      if (name.text == "_") {
        continue;
      }

      Symbol symbol;
      symbol.name = name.text;
      symbol.kind = kind;
      symbol.visibility = visibility;
      symbol.flags = flags;
      symbol.scope = scope;
      symbol.syntax = {tree.file(), declaration_id};
      symbol.name_range = name.range;

      const SymbolId id = semantic_.symbols.declare(std::move(symbol), diagnostics_);
      if (!id.is_valid()) {
        continue;
      }
      for (SyntaxReference denial : context.denials) {
        semantic_.declaration_denials.push_back({id, denial});
      }
      declared.push_back(id);

      // Allocate nominal identity only after the binding succeeds. A duplicate
      // declaration must not leave an unreachable type row behind and thereby
      // perturb later stable TypeId allocation.
      if (kind == SymbolKind::Type && payload.has_value()) {
        const std::optional<TypeKind> nominal = nominal_type_kind(tree.node(*payload).kind);
        if (nominal.has_value()) {
          semantic_.symbols.symbol_mut(id).type =
              semantic_.types.begin_nominal(*nominal, name.text, name.range);
        }
      }

      if (flags.foreign || flags.exported) {
        std::string linker_name = explicit_linker_name(tree, payload);
        if (linker_name.empty()) {
          linker_name = name.text;
        }
        NativeBinding binding;
        binding.kind = flags.foreign
            ? NativeBindingKind::ForeignImport
            : NativeBindingKind::CExport;
        binding.symbol = id;
        binding.provider = context.foreign_provider;
        binding.linker_name_spelling = std::move(linker_name);
        binding.syntax = {tree.file(), declaration_id};
        semantic_.native_bindings.push_back(std::move(binding));
      }
    }
    return declared;
  }

  [[nodiscard]] std::string foreign_provider_name(
      const SyntaxTree &tree, const SyntaxNode &foreign) const {
    for (std::uint32_t index = foreign.token_begin + 1; index < foreign.token_end; ++index) {
      if (token_is_contextual_name(tree.token(index).kind)) {
        return std::string(sources_.text(tree.token(index).range));
      }
    }
    return {};
  }

  void add_site(
      SemanticSiteKind kind, const SyntaxTree &tree, NodeId node, ScopeId scope) {
    semantic_.sites.push_back({kind, {tree.file(), node}, scope, {}, {}});
  }

  [[nodiscard]] std::vector<SymbolId> collect_item(
      const SyntaxTree &tree,
      NodeId node_id,
      ScopeId scope,
      const CollectionContext &context) {
    const SyntaxNode &node = tree.node(node_id);
    switch (node.kind) {
    case NodeKind::Declaration:
      return collect_declaration(tree, node_id, scope, context);

    case NodeKind::ForeignBlock: {
      CollectionContext nested = context;
      nested.flags.foreign = true;
      nested.foreign_provider = foreign_provider_name(tree, node);
      if (!node.children.empty()) {
        collect_declaration_list(tree, node.children.back(), scope, nested);
      }
      return {};
    }

    case NodeKind::ExportDeclaration: {
      CollectionContext nested = context;
      nested.flags.exported = true;
      if (!node.children.empty()) {
        return collect_item(tree, node.children.front(), scope, nested);
      }
      return {};
    }

    case NodeKind::DenyDeclaration:
      add_site(SemanticSiteKind::DenialDeclaration, tree, node_id, scope);
      if (!node.children.empty()) {
        CollectionContext nested = context;
        nested.denials.push_back({tree.file(), node_id});
        collect_declaration_list(tree, node.children.back(), scope, nested);
      }
      return {};

    case NodeKind::WhenDeclaration: {
      add_site(SemanticSiteKind::ConditionalDeclaration, tree, node_id, scope);
      const ConditionalSelection *selection =
          selections_.find({tree.file(), node_id});
      if (selection == nullptr) {
        return {};
      }
      if (selection->select_true) {
        if (node.children.size() >= 2) {
          collect_declaration_list(tree, node.children[1], scope, context);
        }
        return {};
      }
      if (node.children.size() >= 3) {
        const NodeId alternative = node.children[2];
        if (tree.node(alternative).kind == NodeKind::WhenDeclaration) {
          (void)collect_item(tree, alternative, scope, context);
        } else {
          collect_declaration_list(tree, alternative, scope, context);
        }
      }
      return {};
    }

    case NodeKind::SynthesisDeclaration:
      add_site(SemanticSiteKind::SynthesisDeclaration, tree, node_id, scope);
      return {};

    case NodeKind::Judgment:
      add_site(SemanticSiteKind::Judgment, tree, node_id, scope);
      return {};

    case NodeKind::Documentation:
      // Lists handle documentation so they can associate it with the following
      // declaration. This fallback covers recovered or directly nested trees.
      add_site(SemanticSiteKind::Documentation, tree, node_id, scope);
      return {};

    case NodeKind::DeclarationList:
      collect_declaration_list(tree, node_id, scope, context);
      return {};

    default:
      return {};
    }
  }

  void collect_declaration_list(
      const SyntaxTree &tree,
      NodeId list_id,
      ScopeId scope,
      const CollectionContext &context) {
    const SyntaxNode &list = tree.node(list_id);
    if (list.kind != NodeKind::DeclarationList) {
      return;
    }

    std::vector<std::size_t> pending_documentation;
    for (NodeId child_id : list.children) {
      const SyntaxNode &child = tree.node(child_id);
      if (child.kind == NodeKind::Documentation) {
        add_site(SemanticSiteKind::Documentation, tree, child_id, scope);
        pending_documentation.push_back(semantic_.sites.size() - 1);
        continue;
      }

      const std::vector<SymbolId> declared = collect_item(tree, child_id, scope, context);
      if (!declared.empty()) {
        for (std::size_t site_index : pending_documentation) {
          semantic_.sites[site_index].anchor = declared.front();
        }
      }
      // Documentation attaches only across uninterrupted adjacency. A judgment,
      // synthesis site, or conditional with no immediate declaration leaves it
      // unanchored for the later placement diagnostic rather than attaching it
      // to an unrelated declaration farther down the file.
      pending_documentation.clear();
    }
  }

  void collect_import(const SyntaxTree &tree, NodeId import_id, ScopeId file_scope) {
    const SyntaxNode &import = tree.node(import_id);
    if (import.children.empty()) {
      return;
    }
    const SyntaxNode &path = tree.node(import.children.front());
    std::string package_path;
    SourceName final_component;
    bool have_component = false;
    for (std::uint32_t index = path.token_begin; index < path.token_end; ++index) {
      const Token &token = tree.token(index);
      if (!token_is_contextual_name(token.kind)) {
        continue;
      }
      const SourceName component = token_name(tree, index);
      if (!package_path.empty()) {
        package_path.push_back('/');
      }
      package_path += component.text;
      final_component = component;
      have_component = true;
    }
    if (!have_component) {
      return;
    }

    SourceName alias = final_component;
    for (std::uint32_t index = path.token_end; index < import.token_end; ++index) {
      if (tree.token(index).kind != TokenKind::KeywordAs) {
        continue;
      }
      if (index + 1 < import.token_end &&
          token_is_contextual_name(tree.token(index + 1).kind)) {
        alias = token_name(tree, index + 1);
      }
      break;
    }

    Symbol symbol;
    symbol.name = alias.text;
    symbol.kind = SymbolKind::Import;
    symbol.scope = file_scope;
    symbol.syntax = {tree.file(), import_id};
    symbol.name_range = alias.range;
    const SymbolId id = semantic_.symbols.declare(std::move(symbol), diagnostics_);
    if (id.is_valid()) {
      semantic_.imports.push_back(
          {id, std::move(package_path), {tree.file(), import_id}, {}, {}});
    }
  }

  void collect_file(const SyntaxTree &tree, ScopeId file_scope) {
    const SyntaxNode &root = tree.node(tree.root());
    CollectionContext context;
    for (NodeId child_id : root.children) {
      const SyntaxNode &child = tree.node(child_id);
      if (child.kind == NodeKind::ImportClause) {
        collect_import(tree, child_id, file_scope);
      } else if (child.kind == NodeKind::Documentation) {
        // Parser placement makes direct root documentation package metadata.
        add_site(SemanticSiteKind::Documentation, tree, child_id, semantic_.package_scope);
      } else if (child.kind == NodeKind::DeclarationList) {
        collect_declaration_list(tree, child_id, semantic_.package_scope, context);
      }
    }
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const ConditionalSelections &selections_;
  DiagnosticSink &diagnostics_;
  SemanticPackage semantic_;
};

} // namespace

SemanticPackage collect_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &package,
    DiagnosticSink &diagnostics) {
  const ConditionalSelections selections;
  DeclarationCollector collector(sources, package, selections, diagnostics);
  return collector.collect();
}

SemanticPackage collect_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &package,
    const ConditionalSelections &selections,
    DiagnosticSink &diagnostics) {
  DeclarationCollector collector(sources, package, selections, diagnostics);
  return collector.collect();
}

const ConditionalSelection *ConditionalSelections::find(SyntaxReference site) const {
  for (const ConditionalSelection &entry : entries) {
    if (entry.site == site) {
      return &entry;
    }
  }
  return nullptr;
}

std::string_view semantic_site_kind_name(SemanticSiteKind kind) {
  switch (kind) {
  case SemanticSiteKind::Documentation: return "documentation";
  case SemanticSiteKind::Judgment: return "judgment";
  case SemanticSiteKind::SynthesisDeclaration: return "declaration synthesis";
  case SemanticSiteKind::SynthesisMember: return "member synthesis";
  case SemanticSiteKind::SynthesisStatement: return "statement synthesis";
  case SemanticSiteKind::SynthesisExpression: return "expression synthesis";
  case SemanticSiteKind::SynthesisAssembly: return "assembly synthesis";
  case SemanticSiteKind::ConditionalDeclaration: return "conditional declaration";
  case SemanticSiteKind::ConditionalMember: return "conditional member";
  case SemanticSiteKind::ConditionalStatement: return "conditional statement";
  case SemanticSiteKind::DenialDeclaration: return "declaration denial";
  case SemanticSiteKind::DenialMember: return "member denial";
  case SemanticSiteKind::DenialStatement: return "statement denial";
  case SemanticSiteKind::DenialExpression: return "expression denial";
  }
  return "unknown semantic site";
}

} // namespace draft
