// Direct HIR effect discovery and transitive local summary fixed point.

#include "sema/effect.h"

#include <algorithm>
#include <string_view>

namespace draft {
namespace {

void add_effect(std::vector<SemanticEffect> &effects, SemanticEffect effect) {
  if (std::find(effects.begin(), effects.end(), effect) == effects.end()) {
    effects.push_back(std::move(effect));
  }
}

void add_call(std::vector<SymbolId> &calls, SymbolId call) {
  if (std::find(calls.begin(), calls.end(), call) == calls.end()) {
    calls.push_back(call);
  }
}

class EffectCollector {
public:
  EffectCollector(const SemanticPackage &package, const HirProgram &hir)
      : package_(package), hir_(hir) {}

  [[nodiscard]] EffectSummaryResult run() {
    for (const HirProcedure &procedure : hir_.procedures()) {
      ProcedureEffectSummary summary;
      summary.procedure = procedure.symbol;
      current_ = &summary;
      visit_block(procedure.body);
      summary.effects = summary.direct_effects;
      result_.procedures.push_back(std::move(summary));
    }
    current_ = nullptr;

    // Each progress step adds at least one finite direct effect to one summary.
    // Source/declaration order controls union order, so recursion and mutual
    // recursion converge without nondeterministic work queues.
    while (true) {
      bool changed = false;
      for (ProcedureEffectSummary &summary : result_.procedures) {
        for (SymbolId callee : summary.direct_calls) {
          const ProcedureEffectSummary *callee_summary = result_.find(callee);
          if (callee_summary == nullptr) continue;
          for (const SemanticEffect &effect : callee_summary->effects) {
            const std::size_t before = summary.effects.size();
            add_effect(summary.effects, effect);
            changed = changed || summary.effects.size() != before;
          }
        }
      }
      if (!changed) break;
    }
    return std::move(result_);
  }

private:
  [[nodiscard]] bool is_imported(SymbolId symbol) const {
    for (const ImportedSymbol &imported : package_.imported_symbols) {
      if (imported.proxy == symbol) return true;
    }
    return false;
  }

  [[nodiscard]] bool has_local_body(SymbolId symbol) const {
    for (const HirProcedure &procedure : hir_.procedures()) {
      if (procedure.symbol == symbol) return true;
    }
    return false;
  }

  void visit_expression(HirExpressionId id) {
    if (!id.is_valid()) return;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Symbol && expression.symbol.is_valid()) {
      const Symbol &symbol = package_.symbols.symbol(expression.symbol);
      if (symbol.kind == SymbolKind::Variable &&
          (symbol.scope == package_.package_scope || is_imported(expression.symbol))) {
        add_effect(
            current_->direct_effects,
            {EffectKind::PackageGlobal, expression.symbol, {}, {}, {}, {}});
      }
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "assert") {
      add_effect(
          current_->direct_effects,
          {EffectKind::RuntimeAssert, {}, "assert", {}, {}, {}});
      add_effect(
          current_->direct_effects,
          {EffectKind::ContextField, {}, "assertion_failure_proc", {}, {}, {}});
    } else if (expression.kind == HirExpressionKind::Assembly) {
      add_effect(current_->direct_effects, {EffectKind::Assembly, {}, "asm", {}, {}, {}});
    } else if (expression.kind == HirExpressionKind::Index &&
               !expression.operands.empty()) {
      const TypeId base_type = hir_.expression(expression.operands.front()).type;
      if (base_type.is_valid() &&
          package_.types.type(base_type).kind == TypeKind::MultiPointer) {
        add_effect(
            current_->direct_effects,
            {EffectKind::Unchecked, {}, "multi-pointer index", {}, {}, {}});
      }
    }

    if (expression.kind == HirExpressionKind::Call && !expression.operands.empty()) {
      const HirExpression &callee = hir_.expression(expression.operands.front());
      if (callee.kind == HirExpressionKind::Symbol && callee.symbol.is_valid()) {
        if (has_local_body(callee.symbol)) {
          add_call(current_->direct_calls, callee.symbol);
        } else if (is_imported(callee.symbol)) {
          bool known_summary = false;
          for (const ImportedSymbol &imported : package_.imported_symbols) {
            if (imported.proxy == callee.symbol) {
              known_summary = imported.has_effect_summary;
              break;
            }
          }
          if (known_summary) {
            for (const ImportedEffect &effect : package_.imported_effects) {
              if (effect.procedure_proxy != callee.symbol) continue;
              add_effect(
                  current_->direct_effects,
                  {effect.kind,
                   {},
                   effect.detail,
                   effect.root_identity,
                   effect.root_relative_path,
                   effect.declaration});
            }
          } else {
            add_effect(
                current_->direct_effects,
                {EffectKind::UnknownCall,
                 callee.symbol,
                 "imported callee has no audited summary",
                 {},
                 {},
                 {}});
          }
        } else {
          add_effect(
              current_->direct_effects,
              {EffectKind::UnknownCall,
               callee.symbol,
               "callee has no composed summary",
               {},
               {},
               {}});
        }
      } else {
        add_effect(
            current_->direct_effects,
            {EffectKind::UnknownCall, {}, "indirect procedure target", {}, {}, {}});
      }
    }
    for (HirExpressionId operand : expression.operands) {
      visit_expression(operand);
    }
  }

  void visit_statement(HirStatementId id) {
    const HirStatement &statement = hir_.statement(id);
    if (statement.kind == HirStatementKind::Unchecked) {
      add_effect(
          current_->direct_effects,
          {EffectKind::Unchecked, {}, "unchecked region", {}, {}, {}});
    }
    if (statement.kind == HirStatementKind::Assembly) {
      add_effect(current_->direct_effects, {EffectKind::Assembly, {}, "asm", {}, {}, {}});
    }
    for (HirExpressionId expression : statement.expressions) {
      visit_expression(expression);
    }
    for (HirStatementId header : statement.header_statements) {
      visit_statement(header);
    }
    for (HirBlockId block : statement.blocks) {
      visit_block(block);
    }
  }

  void visit_block(HirBlockId id) {
    if (!id.is_valid()) return;
    for (HirStatementId statement : hir_.block(id).statements) {
      visit_statement(statement);
    }
  }

  const SemanticPackage &package_;
  const HirProgram &hir_;
  EffectSummaryResult result_;
  ProcedureEffectSummary *current_ = nullptr;
};

} // namespace

const ProcedureEffectSummary *EffectSummaryResult::find(SymbolId procedure) const {
  for (const ProcedureEffectSummary &summary : procedures) {
    if (summary.procedure == procedure) return &summary;
  }
  return nullptr;
}

EffectSummaryResult summarize_package_effects(
    const SemanticPackage &package,
    const HirProgram &hir) {
  EffectCollector collector(package, hir);
  return collector.run();
}

std::string_view effect_kind_name(EffectKind kind) {
  switch (kind) {
  case EffectKind::PackageGlobal: return "package global";
  case EffectKind::RuntimeAssert: return "assert";
  case EffectKind::ContextField: return "context field";
  case EffectKind::Assembly: return "assembly";
  case EffectKind::Unchecked: return "unchecked";
  case EffectKind::UnknownCall: return "unknown call";
  }
  return "unknown effect";
}

} // namespace draft
