// Direct HIR effect discovery and conservative procedure-slot composition.

#include "sema/effect.h"

#include "syntax/literal.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

void add_target(ProcedureValueSummary &value, SymbolId target) {
  if (std::find(value.targets.begin(), value.targets.end(), target) ==
      value.targets.end()) {
    value.targets.push_back(target);
  }
}

void add_parameter_slot(ProcedureValueSummary &value, std::uint32_t slot) {
  if (std::find(
          value.parameter_slots.begin(), value.parameter_slots.end(), slot) ==
      value.parameter_slots.end()) {
    value.parameter_slots.push_back(slot);
  }
}

void merge_value(
    ProcedureValueSummary &destination,
    const ProcedureValueSummary &source) {
  for (SymbolId target : source.targets) add_target(destination, target);
  for (std::uint32_t slot : source.parameter_slots) {
    add_parameter_slot(destination, slot);
  }
  destination.unknown = destination.unknown || source.unknown;
}

[[nodiscard]] bool value_is_empty(const ProcedureValueSummary &value) {
  return value.targets.empty() && value.parameter_slots.empty() &&
      !value.unknown;
}

[[nodiscard]] std::optional<std::string> decode_linker_name(
    std::string_view spelling) {
  if (spelling.empty()) return std::nullopt;
  if (spelling.front() == '"') {
    return decode_string_literal(spelling, TokenKind::StringLiteral);
  }
  return std::string(spelling);
}

class EffectCollector {
public:
  EffectCollector(
      const SemanticPackage &package,
      const HirProgram &hir,
      const TargetProfile *target)
      : package_(package), hir_(hir), target_(target) {}

  [[nodiscard]] EffectSummaryResult run() {
    // Discovery records direct source effects and symbolic call values. Local
    // procedure-value environments deliberately union every semantic write:
    // this may over-approximate a branch, but optimization can never erase a
    // possible denial edge from the contract.
    for (const HirProcedure &procedure : hir_.procedures()) {
      ProcedureEffectSummary summary;
      summary.procedure = procedure.symbol;
      current_ = &summary;
      procedure_values_.clear();
      seed_parameter_slots(procedure.symbol);
      visit_block(procedure.body);
      summary.effects = summary.direct_effects;
      result_.procedures.push_back(std::move(summary));
    }
    current_ = nullptr;
    procedure_values_.clear();

    // Every progress step adds one member of a finite source-derived set. The
    // deterministic procedure/call/effect traversal makes recursion converge
    // without a work-queue ordering dependency.
    while (true) {
      bool changed = false;
      for (ProcedureEffectSummary &summary : result_.procedures) {
        for (const ProcedureInvocationSummary &invocation :
             summary.direct_invocations) {
          changed = compose_named_call(
              summary, invocation.callee, invocation.arguments) || changed;
        }
        for (const ProcedureFlowInvocationSummary &invocation :
             summary.direct_flow_calls) {
          changed = compose_value_call(
              summary, invocation.callee, invocation.arguments) || changed;
        }
      }
      if (!changed) break;
    }
    return std::move(result_);
  }

private:
  struct StoredProcedureValue {
    SymbolId symbol;
    ProcedureValueSummary value;
  };

  [[nodiscard]] bool procedure_type(TypeId type) const {
    return type.is_valid() &&
        package_.types.type(type).kind == TypeKind::Procedure;
  }

  [[nodiscard]] ProcedureValueSummary *stored_value(SymbolId symbol) {
    for (StoredProcedureValue &stored : procedure_values_) {
      if (stored.symbol == symbol) return &stored.value;
    }
    return nullptr;
  }

  [[nodiscard]] const ProcedureValueSummary *stored_value(
      SymbolId symbol) const {
    for (const StoredProcedureValue &stored : procedure_values_) {
      if (stored.symbol == symbol) return &stored.value;
    }
    return nullptr;
  }

  void merge_stored_value(
      SymbolId symbol, const ProcedureValueSummary &value) {
    if (ProcedureValueSummary *stored = stored_value(symbol)) {
      merge_value(*stored, value);
      return;
    }
    procedure_values_.push_back({symbol, value});
  }

  // Parameter order in the owning Procedure scope is signature order. A
  // procedure-typed parameter begins as one symbolic slot; ordinary parameters
  // occupy their ordinal but need no value row.
  void seed_parameter_slots(SymbolId procedure) {
    for (const OwnedSemanticScope &owned : package_.owned_scopes) {
      if (owned.owner != procedure ||
          package_.symbols.scope(owned.scope).kind != ScopeKind::Procedure) {
        continue;
      }
      std::uint32_t ordinal = 0;
      for (SymbolId symbol_id : package_.symbols.scope(owned.scope).symbols) {
        const Symbol &symbol = package_.symbols.symbol(symbol_id);
        if (symbol.kind != SymbolKind::Parameter) continue;
        if (procedure_type(symbol.type)) {
          ProcedureValueSummary value;
          value.parameter_slots.push_back(ordinal);
          procedure_values_.push_back({symbol_id, std::move(value)});
        }
        ++ordinal;
      }
      return;
    }
  }

  [[nodiscard]] bool is_imported(SymbolId symbol) const {
    for (const ImportedSymbol &imported : package_.imported_symbols) {
      if (imported.proxy == symbol) return true;
    }
    return false;
  }

  [[nodiscard]] const ImportedSymbol *imported_symbol(SymbolId symbol) const {
    for (const ImportedSymbol &imported : package_.imported_symbols) {
      if (imported.proxy == symbol) return &imported;
    }
    return nullptr;
  }

  [[nodiscard]] bool has_local_body(SymbolId symbol) const {
    for (const HirProcedure &procedure : hir_.procedures()) {
      if (procedure.symbol == symbol) return true;
    }
    return false;
  }

  [[nodiscard]] const NativeBinding *native_binding(SymbolId symbol) const {
    for (const NativeBinding &binding : package_.native_bindings) {
      if (binding.symbol == symbol &&
          binding.kind == NativeBindingKind::ForeignImport) {
        return &binding;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const SystemForeignSummary *system_summary(
      std::string_view provider, std::string_view linker_name) const {
    if (target_ == nullptr) return nullptr;
    for (const SystemForeignSummary &summary :
         target_->system_foreign_summaries) {
      if (summary.provider == provider &&
          summary.linker_name == linker_name) {
        return &summary;
      }
    }
    return nullptr;
  }

  // Returns the first source-visible field selected from the built-in context.
  // A deeper access such as context.allocator.procedure is still governed by
  // the top-level context.allocator denial boundary.
  [[nodiscard]] std::optional<std::string_view> context_field(
      HirExpressionId id) const {
    if (!id.is_valid()) return std::nullopt;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Context) return "*";
    if (expression.kind != HirExpressionKind::Member ||
        expression.operands.empty()) {
      return std::nullopt;
    }
    const std::optional<std::string_view> base =
        context_field(expression.operands.front());
    if (!base.has_value()) return std::nullopt;
    if (*base == "*" && expression.symbol.is_valid()) {
      return package_.symbols.symbol(expression.symbol).name;
    }
    return base;
  }

  [[nodiscard]] ProcedureValueSummary procedure_value(
      HirExpressionId id) const {
    ProcedureValueSummary value;
    if (!id.is_valid()) {
      value.unknown = true;
      return value;
    }
    const HirExpression &expression = hir_.expression(id);
    if (!procedure_type(expression.type)) return value;

    if (expression.kind == HirExpressionKind::Symbol &&
        expression.symbol.is_valid()) {
      const Symbol &symbol = package_.symbols.symbol(expression.symbol);
      if (symbol.kind == SymbolKind::Procedure) {
        value.targets.push_back(expression.symbol);
        return value;
      }
      if (const ProcedureValueSummary *stored =
              stored_value(expression.symbol)) {
        return *stored;
      }
      value.unknown = true;
      return value;
    }

    // Conditional and denial expressions preserve values from their result
    // operands. Operands with non-procedure types (conditions, for example)
    // contribute nothing to this union.
    if (expression.kind == HirExpressionKind::Conditional ||
        expression.kind == HirExpressionKind::Denial) {
      for (HirExpressionId operand : expression.operands) {
        if (procedure_type(hir_.expression(operand).type)) {
          merge_value(value, procedure_value(operand));
        }
      }
      if (value_is_empty(value)) value.unknown = true;
      return value;
    }

    // Member paths, pointer-reached fields, returned callbacks, and erased raw
    // storage need path-sensitive slots. They remain explicit unknowns until
    // those richer slot shapes are composed rather than being guessed here.
    value.unknown = true;
    return value;
  }

  [[nodiscard]] std::vector<ProcedureValueSummary> call_arguments(
      const HirExpression &call, std::size_t first) const {
    std::vector<ProcedureValueSummary> result;
    for (std::size_t index = first; index < call.operands.size(); ++index) {
      result.push_back(procedure_value(call.operands[index]));
    }
    return result;
  }

  void record_call(const HirExpression &call) {
    if (call.operands.empty()) return;
    const HirExpression &callee = hir_.expression(call.operands.front());
    const std::vector<ProcedureValueSummary> arguments =
        call_arguments(call, 1);
    if (callee.kind == HirExpressionKind::Symbol &&
        callee.symbol.is_valid() &&
        package_.symbols.symbol(callee.symbol).kind == SymbolKind::Procedure) {
      if (has_local_body(callee.symbol)) {
        add_call(current_->direct_calls, callee.symbol);
      }
      current_->direct_invocations.push_back({callee.symbol, arguments});
      return;
    }
    ProcedureFlowInvocationSummary invocation{
        procedure_value(call.operands.front()), arguments};
    for (SymbolId target : invocation.callee.targets) {
      if (has_local_body(target)) add_call(current_->direct_calls, target);
    }
    current_->direct_flow_calls.push_back(std::move(invocation));
  }

  // runtime.call_with_context keeps its callback at operand one and its
  // callback arguments after that. It participates in the same finite-target
  // composition as an ordinary indirect call.
  void record_context_call(const HirExpression &expression) {
    if (expression.operands.size() < 2) return;
    ProcedureFlowInvocationSummary invocation;
    invocation.callee = procedure_value(expression.operands[1]);
    invocation.arguments = call_arguments(expression, 2);
    for (SymbolId target : invocation.callee.targets) {
      if (has_local_body(target)) add_call(current_->direct_calls, target);
    }
    current_->direct_flow_calls.push_back(std::move(invocation));
  }

  [[nodiscard]] bool add_composed_effect(
      ProcedureEffectSummary &destination, const SemanticEffect &effect) {
    const std::size_t before = destination.effects.size();
    add_effect(destination.effects, effect);
    return destination.effects.size() != before;
  }

  [[nodiscard]] bool compose_effect(
      ProcedureEffectSummary &destination,
      const SemanticEffect &effect,
      const std::vector<ProcedureValueSummary> &arguments) {
    if (effect.kind != EffectKind::FlowCall) {
      return add_composed_effect(destination, effect);
    }
    if (effect.flow_parameter >= arguments.size()) {
      return add_composed_effect(
          destination,
          {EffectKind::UnknownCall,
           {},
           "flow-through callback slot is absent at call site",
           {},
           {},
           {}});
    }
    return compose_value_call(destination, arguments[effect.flow_parameter], {});
  }

  [[nodiscard]] bool compose_imported_call(
      ProcedureEffectSummary &destination,
      SymbolId callee,
      const ImportedSymbol &imported,
      const std::vector<ProcedureValueSummary> &arguments) {
    if (!imported.has_effect_summary) {
      return add_composed_effect(
          destination,
          {EffectKind::UnknownCall,
           callee,
           "imported callee has no audited summary",
           {},
           {},
           {}});
    }
    bool changed = false;
    for (const ImportedEffect &effect : package_.imported_effects) {
      if (effect.procedure_proxy != callee) continue;
      changed = compose_effect(
          destination,
          {effect.kind,
           {},
           effect.detail,
           effect.root_identity,
           effect.root_relative_path,
           effect.declaration,
           effect.flow_parameter},
          arguments) || changed;
    }
    return changed;
  }

  [[nodiscard]] bool compose_native_call(
      ProcedureEffectSummary &destination,
      SymbolId callee,
      const NativeBinding &binding,
      const std::vector<ProcedureValueSummary> &arguments) {
    if (binding.provider == "package_assembly") {
      return add_composed_effect(
          destination,
          {EffectKind::Assembly, callee, "package assembly", {}, {}, {}});
    }
    // Compiler/runtime bridge bodies are part of the compiler content identity.
    // call_with_context is represented as a dedicated HIR intrinsic; the other
    // fixed bridges do not call source-supplied procedure values.
    if (binding.provider == "draft_runtime") return false;

    const std::optional<std::string> linker_name =
        decode_linker_name(binding.linker_name_spelling);
    const SystemForeignSummary *summary = linker_name.has_value()
        ? system_summary(binding.provider, *linker_name)
        : nullptr;
    if (summary == nullptr) {
      return add_composed_effect(
          destination,
          {EffectKind::UnknownCall,
           callee,
           "foreign callee has no artifact-bound denial summary",
           {},
           {},
           {}});
    }
    bool changed = false;
    for (std::uint32_t parameter : summary->callback_parameters) {
      if (parameter >= arguments.size()) {
        changed = add_composed_effect(
            destination,
            {EffectKind::UnknownCall,
             callee,
             "system callback slot is absent at call site",
             {},
             {},
             {}}) || changed;
      } else {
        changed = compose_value_call(
            destination, arguments[parameter], {}) || changed;
      }
    }
    return changed;
  }

  [[nodiscard]] bool compose_named_call(
      ProcedureEffectSummary &destination,
      SymbolId callee,
      const std::vector<ProcedureValueSummary> &arguments) {
    if (const ProcedureEffectSummary *summary = result_.find(callee)) {
      bool changed = false;
      // Copy the row before adding to destination: recursive calls may make
      // summary and destination the same vector.
      const std::vector<SemanticEffect> effects = summary->effects;
      for (const SemanticEffect &effect : effects) {
        changed = compose_effect(destination, effect, arguments) || changed;
      }
      return changed;
    }
    if (const ImportedSymbol *imported = imported_symbol(callee)) {
      return compose_imported_call(destination, callee, *imported, arguments);
    }
    if (const NativeBinding *binding = native_binding(callee)) {
      return compose_native_call(destination, callee, *binding, arguments);
    }
    return add_composed_effect(
        destination,
        {EffectKind::UnknownCall,
         callee,
         "callee has no composed summary",
         {},
         {},
         {}});
  }

  [[nodiscard]] bool compose_value_call(
      ProcedureEffectSummary &destination,
      const ProcedureValueSummary &value,
      const std::vector<ProcedureValueSummary> &arguments) {
    bool changed = false;
    for (SymbolId target : value.targets) {
      changed = compose_named_call(destination, target, arguments) || changed;
    }
    for (std::uint32_t slot : value.parameter_slots) {
      changed = add_composed_effect(
          destination,
          {EffectKind::FlowCall,
           {},
           "procedure parameter callback",
           {},
           {},
           {},
           slot}) || changed;
      // Higher-order callback arguments require nested slot paths. Preserve
      // safety until that shape is expressible instead of silently dropping
      // the callback's own procedure-valued inputs.
      for (const ProcedureValueSummary &argument : arguments) {
        if (!value_is_empty(argument)) {
          changed = add_composed_effect(
              destination,
              {EffectKind::UnknownCall,
               {},
               "higher-order callback argument needs a nested flow slot",
               {},
               {},
               {}}) || changed;
          break;
        }
      }
    }
    if (value.unknown || value_is_empty(value)) {
      changed = add_composed_effect(
          destination,
          {EffectKind::UnknownCall,
           {},
           "indirect procedure target is not finite",
           {},
           {},
           {}}) || changed;
    }
    return changed;
  }

  void visit_expression(HirExpressionId id) {
    if (!id.is_valid()) return;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Symbol &&
        expression.symbol.is_valid()) {
      const Symbol &symbol = package_.symbols.symbol(expression.symbol);
      if (symbol.kind == SymbolKind::Variable &&
          (symbol.scope == package_.package_scope ||
           is_imported(expression.symbol))) {
        add_effect(
            current_->direct_effects,
            {EffectKind::PackageGlobal, expression.symbol, {}, {}, {}, {}});
      }
    } else if (expression.kind == HirExpressionKind::Context) {
      add_effect(
          current_->direct_effects,
          {EffectKind::ContextField, {}, "*", {}, {}, {}});
    } else if (expression.kind == HirExpressionKind::Member) {
      const std::optional<std::string_view> field = context_field(id);
      if (field.has_value() && *field != "*") {
        add_effect(
            current_->direct_effects,
            {EffectKind::ContextField,
             {},
             std::string(*field),
             {},
             {},
             {}});
        return;
      }
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "call_with_context") {
      record_context_call(expression);
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "assert") {
      add_effect(
          current_->direct_effects,
          {EffectKind::RuntimeAssert, {}, "assert", {}, {}, {}});
      add_effect(
          current_->direct_effects,
          {EffectKind::ContextField,
           {},
           "assertion_failure_proc",
           {},
           {},
           {}});
    } else if (expression.kind == HirExpressionKind::Assembly) {
      add_effect(
          current_->direct_effects,
          {EffectKind::Assembly, {}, "asm", {}, {}, {}});
    } else if (expression.kind == HirExpressionKind::Index &&
               !expression.operands.empty()) {
      const TypeId base_type = hir_.expression(expression.operands.front()).type;
      if (base_type.is_valid() &&
          package_.types.type(base_type).kind == TypeKind::MultiPointer) {
        add_effect(
            current_->direct_effects,
            {EffectKind::Unchecked,
             {},
             "multi-pointer index",
             {},
             {},
             {}});
      }
    }

    if (expression.kind == HirExpressionKind::Call) record_call(expression);
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
      add_effect(
          current_->direct_effects,
          {EffectKind::Assembly, {}, "asm", {}, {}, {}});
    }

    // Evaluate source expressions before their resulting bindings/writes
    // become visible to later statements.
    for (HirExpressionId expression : statement.expressions) {
      visit_expression(expression);
    }
    if (statement.kind == HirStatementKind::LocalDeclaration &&
        statement.bindings.size() == 1 &&
        statement.expressions.size() == 1) {
      const SymbolId binding = statement.bindings.front();
      if (procedure_type(package_.symbols.symbol(binding).type)) {
        merge_stored_value(
            binding, procedure_value(statement.expressions.front()));
      }
    } else if (statement.kind == HirStatementKind::Assignment &&
               statement.operation == HirOperation::Assign) {
      const std::size_t left_count = statement.expressions.size() / 2;
      for (std::size_t index = 0; index < left_count; ++index) {
        const HirExpression &left = hir_.expression(statement.expressions[index]);
        if (left.kind != HirExpressionKind::Symbol ||
            !left.symbol.is_valid() || !procedure_type(left.type)) {
          continue;
        }
        merge_stored_value(
            left.symbol,
            procedure_value(statement.expressions[left_count + index]));
      }
    }
    for (HirStatementId header : statement.header_statements) {
      visit_statement(header);
    }
    for (HirBlockId block : statement.blocks) visit_block(block);
  }

  void visit_block(HirBlockId id) {
    if (!id.is_valid()) return;
    for (HirStatementId statement : hir_.block(id).statements) {
      visit_statement(statement);
    }
  }

  const SemanticPackage &package_;
  const HirProgram &hir_;
  const TargetProfile *target_ = nullptr;
  EffectSummaryResult result_;
  ProcedureEffectSummary *current_ = nullptr;
  std::vector<StoredProcedureValue> procedure_values_;
};

} // namespace

const ProcedureEffectSummary *EffectSummaryResult::find(
    SymbolId procedure) const {
  for (const ProcedureEffectSummary &summary : procedures) {
    if (summary.procedure == procedure) return &summary;
  }
  return nullptr;
}

EffectSummaryResult summarize_package_effects(
    const SemanticPackage &package,
    const HirProgram &hir,
    const TargetProfile *target) {
  EffectCollector collector(package, hir, target);
  return collector.run();
}

std::string_view effect_kind_name(EffectKind kind) {
  switch (kind) {
  case EffectKind::PackageGlobal: return "package global";
  case EffectKind::RuntimeAssert: return "assert";
  case EffectKind::ContextField: return "context field";
  case EffectKind::Assembly: return "assembly";
  case EffectKind::Unchecked: return "unchecked";
  case EffectKind::FlowCall: return "procedure flow slot";
  case EffectKind::UnknownCall: return "unknown call";
  }
  return "unknown effect";
}

} // namespace draft
