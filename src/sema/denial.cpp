// Denial selector resolution and HIR/effect enforcement.

#include "sema/denial.h"

#include "syntax/token.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool token_is_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordAsm ||
      kind == TokenKind::KeywordUnchecked || kind == TokenKind::KeywordC ||
      kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
      kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
      kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory;
}

// Selector resolution is deliberately independent from HIR walking. The same
// resolved identities drive denial enforcement and synthesis-context pruning;
// neither consumer is allowed to reinterpret a selector by spelling alone.
class DenialSelectorResolver {
public:
  DenialSelectorResolver(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      const SemanticPackage &package,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), package_(package),
        diagnostics_(diagnostics) {}

  [[nodiscard]] std::vector<ResolvedDenialSelector> resolve(
      SyntaxReference syntax, ScopeId scope) {
    std::vector<ResolvedDenialSelector> result;
    const SyntaxTree *tree = find_tree(syntax.file);
    if (tree == nullptr || !syntax.node.is_valid()) return result;
    const SyntaxNode &denial = tree->node(syntax.node);
    if (denial.children.empty()) return result;
    // The final child is always the governed declaration/member list, block,
    // or expression. Every preceding child is one selector.
    for (std::size_t index = 0; index + 1 < denial.children.size(); ++index) {
      resolve_selector(*tree, denial.children[index], scope, result);
    }
    return result;
  }

private:
  [[nodiscard]] const SyntaxTree *find_tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) return &*entry.syntax;
    }
    return nullptr;
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
      std::vector<ResolvedDenialSelector> &result) {
    const SyntaxNode &selector = tree.node(selector_id);
    const auto selector_names = names(tree, selector);
    if (selector_names.empty()) {
      diagnostics_.error(selector.range, "deny selector does not resolve to an entity");
      return;
    }
    const std::string &first_name = selector_names.front().first;
    if (first_name == "asm") {
      result.push_back(
          {ResolvedDenialKind::Assembly, {}, {}, {}, {}, selector.range});
      return;
    }
    if (first_name == "unchecked") {
      result.push_back(
          {ResolvedDenialKind::Unchecked, {}, {}, {}, {}, selector.range});
      return;
    }
    if (first_name == "assert") {
      if (selector_names.size() != 1) {
        diagnostics_.error(
            selector.range,
            "assert deny selector does not accept a member path");
        return;
      }
      result.push_back(
          {ResolvedDenialKind::RuntimeAssert, {}, {}, {}, {}, selector.range});
      return;
    }
    if (first_name == "raw_data") {
      if (selector_names.size() != 1) {
        diagnostics_.error(
            selector.range,
            "raw_data deny selector does not accept a member path");
        return;
      }
      result.push_back(
          {ResolvedDenialKind::RawStringData, {}, {}, {}, {}, selector.range});
      return;
    }
    if (first_name == "context") {
      if (selector_names.size() == 1) {
        result.push_back(
            {ResolvedDenialKind::Context, {}, {}, {}, {}, selector.range});
      } else {
        result.push_back({
            ResolvedDenialKind::ContextField,
            {},
            {},
            {},
            selector_names.back().first,
            selector.range,
        });
      }
      return;
    }

    const std::optional<SymbolId> first =
        package_.symbols.lookup(scope, first_name);
    if (!first.has_value()) {
      diagnostics_.error(selector_names.front().second, "unknown deny selector name");
      return;
    }
    const Symbol &symbol = package_.symbols.symbol(*first);
    if (symbol.kind == SymbolKind::Import) {
      if (selector_names.size() == 1) {
        const ImportBinding *binding = import_binding(*first);
        if (binding == nullptr || binding->root_identity.empty()) {
          diagnostics_.error(
              selector.range, "import deny selector has no package identity");
          return;
        }
        result.push_back({
            ResolvedDenialKind::ImportedPackage,
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
        diagnostics_.error(
            selector.range, "imported deny selector is not public");
        return;
      }
      result.push_back({
          ResolvedDenialKind::Symbol, *member, {}, {}, {}, selector.range});
      return;
    }
    if (selector_names.size() != 1) {
      diagnostics_.error(
          selector.range, "deny selector has unsupported member path");
      return;
    }
    result.push_back(
        {ResolvedDenialKind::Symbol, *first, {}, {}, {}, selector.range});
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const SemanticPackage &package_;
  DiagnosticSink &diagnostics_;
};

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
      std::vector<ResolvedDenialSelector> active;
      for (const DeclarationDenial &contract : package_.declaration_denials) {
        if (contract.declaration == declaration_source(procedure.symbol)) {
          append_denial(contract.denial, file_scope(contract.denial.file), active);
        }
      }
      visit_block(procedure.body, active);
    }
    return diagnostics_.error_count() == initial_errors;
  }

private:
  [[nodiscard]] SymbolId declaration_source(SymbolId procedure) const {
    for (const ParametricInstanceRecord &instance :
         package_.parametric_instances) {
      if (instance.instance == procedure) return instance.source;
    }
    return procedure;
  }

  [[nodiscard]] ScopeId file_scope(FileId file) const {
    for (const FileSemanticScope &entry : package_.files) {
      if (entry.file == file) return entry.scope;
    }
    return package_.package_scope;
  }

  void append_denial(
      SyntaxReference syntax,
      ScopeId scope,
      std::vector<ResolvedDenialSelector> &active) {
    std::vector<ResolvedDenialSelector> resolved = resolve_denial_selectors(
        sources_, loaded_, package_, syntax, scope, diagnostics_);
    active.insert(
        active.end(),
        std::make_move_iterator(resolved.begin()),
        std::make_move_iterator(resolved.end()));
  }

  [[nodiscard]] bool symbol_from_package(
      SymbolId symbol, const ResolvedDenialSelector &denied) const {
    for (const ImportedSymbol &imported : package_.imported_symbols) {
      if (imported.proxy == symbol) {
        return imported.root_identity == denied.root_identity &&
            imported.root_relative_path == denied.root_relative_path;
      }
    }
    return false;
  }

  [[nodiscard]] bool origin_is_symbol(
      const SemanticEffect &effect, SymbolId symbol) const {
    for (const ImportedSymbol &imported : package_.imported_symbols) {
      if (imported.proxy != symbol) continue;
      return imported.root_identity == effect.root_identity &&
          imported.root_relative_path == effect.root_relative_path &&
          imported.public_name == effect.declaration;
    }
    const Symbol &local = package_.symbols.symbol(symbol);
    return effect.root_identity.empty() &&
        effect.root_relative_path.empty() && effect.declaration == local.name;
  }

  [[nodiscard]] bool matches_effect(
      const ResolvedDenialSelector &denied,
      const SemanticEffect &effect) const {
    if (effect.kind == EffectKind::UnknownCall) return true;
    // A flow slot is a placeholder, not an effectful target. The enclosing
    // call substitutes its actual procedure value before matching denials.
    if (effect.kind == EffectKind::FlowCall) return false;
    switch (denied.kind) {
    case ResolvedDenialKind::Symbol:
      return (effect.symbol.is_valid() && effect.symbol == denied.symbol) ||
          (!effect.root_identity.empty() &&
           origin_is_symbol(effect, denied.symbol));
    case ResolvedDenialKind::ImportedPackage:
      if (!effect.root_identity.empty()) {
        return effect.root_identity == denied.root_identity &&
            effect.root_relative_path == denied.root_relative_path;
      }
      return effect.symbol.is_valid() && symbol_from_package(effect.symbol, denied);
    case ResolvedDenialKind::RuntimeAssert:
      return effect.kind == EffectKind::RuntimeAssert;
    case ResolvedDenialKind::RawStringData:
      return effect.kind == EffectKind::RawStringData;
    case ResolvedDenialKind::Context:
      return effect.kind == EffectKind::ContextField;
    case ResolvedDenialKind::ContextField:
      return effect.kind == EffectKind::ContextField && effect.text == denied.field;
    case ResolvedDenialKind::Assembly:
      return effect.kind == EffectKind::Assembly;
    case ResolvedDenialKind::Unchecked:
      return effect.kind == EffectKind::Unchecked;
    }
    return false;
  }

  void report_violation(
      const ResolvedDenialSelector &denied,
      SourceRange range,
      std::string entity) {
    diagnostics_.error(range, "operation reaches denied " + std::move(entity));
    diagnostics_.note(denied.range, "denial is established here");
  }

  void check_effect(
      const SemanticEffect &effect,
      SourceRange range,
      const std::vector<ResolvedDenialSelector> &active) {
    for (const ResolvedDenialSelector &denied : active) {
      if (matches_effect(denied, effect)) {
        report_violation(denied, range, std::string(effect_kind_name(effect.kind)));
      }
    }
  }

  void check_call_site(
      HirExpressionId expression,
      SourceRange range,
      const std::vector<ResolvedDenialSelector> &active) {
    const CallSiteEffectSummary *summary =
        effects_.find_call_site(expression);
    if (summary == nullptr) {
      check_effect(
          {EffectKind::UnknownCall,
           {},
           "call site has no composed summary",
           {},
           {},
           {}},
          range,
          active);
      return;
    }
    for (const SemanticEffect &effect : summary->effects) {
      check_effect(effect, range, active);
    }
  }

  // Denials name top-level Context fields. A nested member chain retains the
  // first selector after `context`, matching both the source syntax and the
  // effect summaries exported through package interfaces.
  [[nodiscard]] std::optional<std::string> context_field(
      HirExpressionId id) const {
    if (!id.is_valid()) return std::nullopt;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Context) return std::string("*");
    if (expression.kind != HirExpressionKind::Member ||
        expression.operands.empty()) {
      return std::nullopt;
    }
    const std::optional<std::string> base =
        context_field(expression.operands.front());
    if (!base.has_value()) return std::nullopt;
    if (*base == "*" && expression.symbol.is_valid()) {
      return package_.symbols.symbol(expression.symbol).name;
    }
    return base;
  }

  void visit_expression(
      HirExpressionId id,
      const std::vector<ResolvedDenialSelector> &active) {
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Denial) {
      std::vector<ResolvedDenialSelector> nested = active;
      append_denial(expression.syntax, expression.scope, nested);
      for (HirExpressionId operand : expression.operands) visit_expression(operand, nested);
      return;
    }
    if (expression.kind == HirExpressionKind::Call && !expression.operands.empty()) {
      check_call_site(id, expression.range, active);
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
    } else if (expression.kind == HirExpressionKind::Context) {
      check_effect(
          {EffectKind::ContextField, {}, "*", {}, {}, {}},
          expression.range,
          active);
    } else if (expression.kind == HirExpressionKind::Member) {
      const std::optional<std::string> field = context_field(id);
      if (field.has_value()) {
        check_effect(
            {EffectKind::ContextField, {}, *field, {}, {}, {}},
            expression.range,
            active);
        return;
      }
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "call_with_context") {
      check_call_site(id, expression.range, active);
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "assert") {
      check_effect(
          {EffectKind::RuntimeAssert, {}, "assert", {}, {}, {}}, expression.range, active);
      check_effect(
          {EffectKind::ContextField, {}, "assertion_failure_proc", {}, {}, {}},
          expression.range,
          active);
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "raw_data") {
      // The exact intrinsic range is retained even though transitive calls use
      // their call-site range. This gives a direct denial a diagnostic on the
      // representation escape itself and lets the same effect row flow through
      // ordinary local and imported procedure summaries.
      check_effect(
          {EffectKind::RawStringData, {}, "raw_data", {}, {}, {}},
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
      HirStatementId id,
      const std::vector<ResolvedDenialSelector> &active) {
    const HirStatement &statement = hir_.statement(id);
    if (statement.kind == HirStatementKind::Denial) {
      std::vector<ResolvedDenialSelector> nested = active;
      // The governed block owns a fresh scope. Selectors precede that block and
      // resolve in its parent, so a same-named local inside the block cannot
      // retarget the denial.
      const ScopeId block_scope = hir_.block(statement.blocks.front()).scope;
      append_denial(
          statement.syntax, package_.symbols.scope(block_scope).parent, nested);
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

  void visit_block(
      HirBlockId id,
      const std::vector<ResolvedDenialSelector> &active) {
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

std::vector<ResolvedDenialSelector> resolve_denial_selectors(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    SyntaxReference denial,
    ScopeId scope,
    DiagnosticSink &diagnostics) {
  DenialSelectorResolver resolver(sources, loaded, package, diagnostics);
  return resolver.resolve(denial, scope);
}

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
