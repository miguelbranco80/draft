// Denial selector resolution and HIR/effect enforcement.

#include "sema/denial.h"

#include "syntax/token.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

enum class DeniedKind {
  Symbol,
  ImportedPackage,
  RuntimeAssert,
  Context,
  ContextField,
  Assembly,
  Unchecked,
};

struct DeniedEntity {
  DeniedKind kind = DeniedKind::Symbol;
  SymbolId symbol;
  std::string root_identity;
  std::string root_relative_path;
  std::string field;
  SourceRange range;
};

[[nodiscard]] bool token_is_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordAsm ||
      kind == TokenKind::KeywordUnchecked || kind == TokenKind::KeywordC ||
      kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
      kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber;
}

class DenialChecker {
public:
  DenialChecker(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      const SemanticPackage &package,
      const HirProgram &hir,
      const EffectSummaryResult &effects,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), package_(package), hir_(hir),
        effects_(effects), diagnostics_(diagnostics) {}

  [[nodiscard]] bool run() {
    const std::size_t initial_errors = diagnostics_.error_count();
    for (const HirProcedure &procedure : hir_.procedures()) {
      std::vector<DeniedEntity> active;
      for (const DeclarationDenial &contract : package_.declaration_denials) {
        if (contract.declaration == procedure.symbol) {
          append_denial(contract.denial, file_scope(contract.denial.file), active);
        }
      }
      visit_block(procedure.body, active);
    }
    return diagnostics_.error_count() == initial_errors;
  }

private:
  [[nodiscard]] const SyntaxTree *find_tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) return &*entry.syntax;
    }
    return nullptr;
  }

  [[nodiscard]] ScopeId file_scope(FileId file) const {
    for (const FileSemanticScope &entry : package_.files) {
      if (entry.file == file) return entry.scope;
    }
    return package_.package_scope;
  }

  [[nodiscard]] std::vector<std::pair<std::string, SourceRange>> names(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    std::vector<std::pair<std::string, SourceRange>> result;
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      const Token &token = tree.token(index);
      if (token_is_name(token.kind)) {
        result.push_back({std::string(sources_.text(token.range)), token.range});
      }
    }
    return result;
  }

  [[nodiscard]] const ImportBinding *import_binding(SymbolId symbol) const {
    for (const ImportBinding &binding : package_.imports) {
      if (binding.symbol == symbol) return &binding;
    }
    return nullptr;
  }

  [[nodiscard]] std::optional<SymbolId> imported_member(
      SymbolId import, std::string_view name) const {
    for (const OwnedSemanticScope &owned : package_.owned_scopes) {
      if (owned.owner == import &&
          package_.symbols.scope(owned.scope).kind == ScopeKind::ImportedPackage) {
        return package_.symbols.lookup_direct(owned.scope, name);
      }
    }
    return std::nullopt;
  }

  void resolve_selector(
      const SyntaxTree &tree,
      NodeId selector_id,
      ScopeId scope,
      std::vector<DeniedEntity> &active) {
    const SyntaxNode &selector = tree.node(selector_id);
    const auto selector_names = names(tree, selector);
    if (selector_names.empty()) {
      diagnostics_.error(selector.range, "deny selector does not resolve to an entity");
      return;
    }
    const std::string &first_name = selector_names.front().first;
    if (first_name == "asm") {
      active.push_back({DeniedKind::Assembly, {}, {}, {}, {}, selector.range});
      return;
    }
    if (first_name == "unchecked") {
      active.push_back({DeniedKind::Unchecked, {}, {}, {}, {}, selector.range});
      return;
    }
    if (first_name == "assert") {
      active.push_back({DeniedKind::RuntimeAssert, {}, {}, {}, {}, selector.range});
      return;
    }
    if (first_name == "context") {
      if (selector_names.size() == 1) {
        active.push_back({DeniedKind::Context, {}, {}, {}, {}, selector.range});
      } else {
        active.push_back({
            DeniedKind::ContextField,
            {},
            {},
            {},
            selector_names.back().first,
            selector.range,
        });
      }
      return;
    }

    const std::optional<SymbolId> first = package_.symbols.lookup(scope, first_name);
    if (!first.has_value()) {
      diagnostics_.error(selector_names.front().second, "unknown deny selector name");
      return;
    }
    const Symbol &symbol = package_.symbols.symbol(*first);
    if (symbol.kind == SymbolKind::Import) {
      if (selector_names.size() == 1) {
        const ImportBinding *binding = import_binding(*first);
        if (binding == nullptr || binding->root_identity.empty()) {
          diagnostics_.error(selector.range, "import deny selector has no package identity");
          return;
        }
        active.push_back({
            DeniedKind::ImportedPackage,
            {},
            binding->root_identity,
            binding->root_relative_path,
            {},
            selector.range,
        });
        return;
      }
      const std::optional<SymbolId> member =
          imported_member(*first, selector_names.back().first);
      if (!member.has_value()) {
        diagnostics_.error(selector.range, "imported deny selector is not public");
        return;
      }
      active.push_back({DeniedKind::Symbol, *member, {}, {}, {}, selector.range});
      return;
    }
    if (selector_names.size() != 1) {
      diagnostics_.error(selector.range, "deny selector has unsupported member path");
      return;
    }
    active.push_back({DeniedKind::Symbol, *first, {}, {}, {}, selector.range});
  }

  void append_denial(
      SyntaxReference syntax, ScopeId scope, std::vector<DeniedEntity> &active) {
    const SyntaxTree *tree = find_tree(syntax.file);
    if (tree == nullptr || !syntax.node.is_valid()) return;
    const SyntaxNode &denial = tree->node(syntax.node);
    if (denial.children.empty()) return;
    for (std::size_t index = 0; index + 1 < denial.children.size(); ++index) {
      resolve_selector(*tree, denial.children[index], scope, active);
    }
  }

  [[nodiscard]] bool symbol_from_package(
      SymbolId symbol, const DeniedEntity &denied) const {
    for (const ImportedSymbol &imported : package_.imported_symbols) {
      if (imported.proxy == symbol) {
        return imported.root_identity == denied.root_identity &&
            imported.root_relative_path == denied.root_relative_path;
      }
    }
    return false;
  }

  [[nodiscard]] bool matches_effect(
      const DeniedEntity &denied, const SemanticEffect &effect) const {
    if (effect.kind == EffectKind::UnknownCall) return true;
    switch (denied.kind) {
    case DeniedKind::Symbol:
      return effect.symbol.is_valid() && effect.symbol == denied.symbol;
    case DeniedKind::ImportedPackage:
      if (!effect.root_identity.empty()) {
        return effect.root_identity == denied.root_identity &&
            effect.root_relative_path == denied.root_relative_path;
      }
      return effect.symbol.is_valid() && symbol_from_package(effect.symbol, denied);
    case DeniedKind::RuntimeAssert:
      return effect.kind == EffectKind::RuntimeAssert;
    case DeniedKind::Context:
      return effect.kind == EffectKind::ContextField;
    case DeniedKind::ContextField:
      return effect.kind == EffectKind::ContextField && effect.text == denied.field;
    case DeniedKind::Assembly:
      return effect.kind == EffectKind::Assembly;
    case DeniedKind::Unchecked:
      return effect.kind == EffectKind::Unchecked;
    }
    return false;
  }

  [[nodiscard]] bool matches_symbol(
      const DeniedEntity &denied, SymbolId symbol) const {
    if (denied.kind == DeniedKind::Symbol) return denied.symbol == symbol;
    if (denied.kind == DeniedKind::ImportedPackage) {
      return symbol_from_package(symbol, denied);
    }
    return false;
  }

  void report_violation(
      const DeniedEntity &denied, SourceRange range, std::string entity) {
    diagnostics_.error(range, "operation reaches denied " + std::move(entity));
    diagnostics_.note(denied.range, "denial is established here");
  }

  void check_effect(
      const SemanticEffect &effect,
      SourceRange range,
      const std::vector<DeniedEntity> &active) {
    for (const DeniedEntity &denied : active) {
      if (matches_effect(denied, effect)) {
        report_violation(denied, range, std::string(effect_kind_name(effect.kind)));
      }
    }
  }

  void check_call(
      SymbolId callee, SourceRange range, const std::vector<DeniedEntity> &active) {
    for (const DeniedEntity &denied : active) {
      if (matches_symbol(denied, callee)) {
        report_violation(denied, range, "declaration call");
      }
    }
    const ProcedureEffectSummary *summary = effects_.find(callee);
    if (summary == nullptr) {
      SemanticEffect unknown{
          EffectKind::UnknownCall,
          callee,
          "callee has no composed summary",
          {},
          {},
          {},
      };
      // Imported summary effects were copied into the caller's summary, but a
      // nested denial needs the callee-specific facts. Reconstruct them here.
      bool imported_known = false;
      for (const ImportedSymbol &imported : package_.imported_symbols) {
        if (imported.proxy == callee) imported_known = imported.has_effect_summary;
      }
      if (imported_known) {
        for (const ImportedEffect &effect : package_.imported_effects) {
          if (effect.procedure_proxy != callee) continue;
          check_effect(
              {effect.kind,
               {},
               effect.detail,
               effect.root_identity,
               effect.root_relative_path,
               effect.declaration},
              range,
              active);
        }
      } else {
        check_effect(unknown, range, active);
      }
      return;
    }
    for (const SemanticEffect &effect : summary->effects) {
      check_effect(effect, range, active);
    }
  }

  void visit_expression(
      HirExpressionId id, const std::vector<DeniedEntity> &active) {
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Denial) {
      std::vector<DeniedEntity> nested = active;
      append_denial(expression.syntax, expression.scope, nested);
      for (HirExpressionId operand : expression.operands) visit_expression(operand, nested);
      return;
    }
    if (expression.kind == HirExpressionKind::Call && !expression.operands.empty()) {
      const HirExpression &callee = hir_.expression(expression.operands.front());
      if (callee.kind == HirExpressionKind::Symbol && callee.symbol.is_valid()) {
        check_call(callee.symbol, expression.range, active);
      } else {
        check_effect(
            {EffectKind::UnknownCall, {}, "indirect procedure target", {}, {}, {}},
            expression.range,
            active);
      }
    } else if (expression.kind == HirExpressionKind::Symbol &&
               expression.symbol.is_valid()) {
      const Symbol &symbol = package_.symbols.symbol(expression.symbol);
      if (symbol.kind == SymbolKind::Variable &&
          (symbol.scope == package_.package_scope ||
           std::any_of(
               package_.imported_symbols.begin(),
               package_.imported_symbols.end(),
               [&expression](const ImportedSymbol &imported) {
                 return imported.proxy == expression.symbol;
               }))) {
        check_effect(
            {EffectKind::PackageGlobal, expression.symbol, {}, {}, {}, {}},
            expression.range,
            active);
      }
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "assert") {
      check_effect(
          {EffectKind::RuntimeAssert, {}, "assert", {}, {}, {}}, expression.range, active);
      check_effect(
          {EffectKind::ContextField, {}, "assertion_failure_proc", {}, {}, {}},
          expression.range,
          active);
    } else if (expression.kind == HirExpressionKind::Assembly) {
      check_effect(
          {EffectKind::Assembly, {}, "asm", {}, {}, {}}, expression.range, active);
    } else if (expression.kind == HirExpressionKind::Index &&
               !expression.operands.empty()) {
      const TypeId base = hir_.expression(expression.operands.front()).type;
      if (base.is_valid() && package_.types.type(base).kind == TypeKind::MultiPointer) {
        check_effect(
            {EffectKind::Unchecked, {}, "multi-pointer index", {}, {}, {}},
            expression.range,
            active);
      }
    }
    for (HirExpressionId operand : expression.operands) visit_expression(operand, active);
  }

  void visit_statement(
      HirStatementId id, const std::vector<DeniedEntity> &active) {
    const HirStatement &statement = hir_.statement(id);
    if (statement.kind == HirStatementKind::Denial) {
      std::vector<DeniedEntity> nested = active;
      append_denial(statement.syntax, hir_.block(statement.blocks.front()).scope, nested);
      for (HirBlockId block : statement.blocks) visit_block(block, nested);
      return;
    }
    if (statement.kind == HirStatementKind::Unchecked) {
      check_effect(
          {EffectKind::Unchecked, {}, "unchecked region", {}, {}, {}}, statement.range, active);
    }
    if (statement.kind == HirStatementKind::Assembly) {
      check_effect({EffectKind::Assembly, {}, "asm", {}, {}, {}}, statement.range, active);
    }
    for (HirExpressionId expression : statement.expressions) visit_expression(expression, active);
    for (HirStatementId header : statement.header_statements) visit_statement(header, active);
    for (HirBlockId block : statement.blocks) visit_block(block, active);
  }

  void visit_block(HirBlockId id, const std::vector<DeniedEntity> &active) {
    for (HirStatementId statement : hir_.block(id).statements) {
      visit_statement(statement, active);
    }
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const SemanticPackage &package_;
  const HirProgram &hir_;
  const EffectSummaryResult &effects_;
  DiagnosticSink &diagnostics_;
};

} // namespace

bool check_package_denials(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const HirProgram &hir,
    const EffectSummaryResult &effects,
    DiagnosticSink &diagnostics) {
  DenialChecker checker(sources, loaded, package, hir, effects, diagnostics);
  return checker.run();
}

} // namespace draft
