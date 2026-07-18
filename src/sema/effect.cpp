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

void add_flow_slot(
    ProcedureValueSummary &value, ProcedureFlowSlot slot) {
  if (std::find(
          value.flow_slots.begin(), value.flow_slots.end(), slot) ==
      value.flow_slots.end()) {
    value.flow_slots.push_back(std::move(slot));
  }
}

void merge_value(
    ProcedureValueSummary &destination,
    const ProcedureValueSummary &source) {
  for (SymbolId target : source.targets) add_target(destination, target);
  for (const ProcedureFlowSlot &slot : source.flow_slots) {
    add_flow_slot(destination, slot);
  }
  for (const SemanticEffect &effect : source.contract_effects) {
    add_effect(destination.contract_effects, effect);
  }
  destination.unknown = destination.unknown || source.unknown;
}

[[nodiscard]] bool value_is_empty(const ProcedureValueSummary &value) {
  return value.targets.empty() && value.flow_slots.empty() &&
      value.contract_effects.empty() &&
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
      const TargetProfile *target,
      std::span<const ForeignProviderAudit> provider_audits)
      : package_(package), hir_(hir), target_(target),
        provider_audits_(provider_audits) {}

  [[nodiscard]] EffectSummaryResult run() {
    // Install every local row before discovering bodies. Returned procedure
    // values can then refer forward or recursively without source order
    // changing their meaning.
    for (const HirProcedure &procedure : hir_.procedures()) {
      ProcedureEffectSummary summary;
      summary.procedure = procedure.symbol;
      result_.procedures.push_back(std::move(summary));
    }

    // Foreign declarations have no HIR body, but a lexical denial may enclose
    // a call to one directly. Give every foreign procedure its own composed
    // row so that both ordinary callers and the denial walker consult exactly
    // the same target/artifact-bound contract. Procedure-valued parameters are
    // seeded as symbolic flow slots; composing the row at a real call site then
    // substitutes the actual callback just like a Draft procedure does.
    for (const NativeBinding &binding : package_.native_bindings) {
      if (binding.kind != NativeBindingKind::ForeignImport) continue;
      ProcedureEffectSummary summary;
      summary.procedure = binding.symbol;
      const Symbol &symbol = package_.symbols.symbol(binding.symbol);
      std::vector<ProcedureArgumentSummary> arguments;
      if (symbol.type.is_valid()) {
        const Type &type = package_.types.type(symbol.type);
        if (type.kind == TypeKind::Procedure && !type.members.empty()) {
          arguments.resize(type.members.size() - 1);
          for (std::size_t index = 0; index < arguments.size(); ++index) {
            const Type &parameter_type =
                package_.types.type(type.members[index]);
            const bool follow_pointer =
                parameter_type.kind == TypeKind::Pointer ||
                parameter_type.kind == TypeKind::MultiPointer;
            for (const std::vector<std::string> &path :
                 procedure_paths(type.members[index], follow_pointer)) {
              ProcedureValueSummary value;
              value.flow_slots.push_back({
                  static_cast<std::uint32_t>(index), path, false});
              arguments[index].fields.push_back({path, std::move(value)});
            }
          }
        }
      }
      [[maybe_unused]] const bool added =
          compose_native_call(summary, binding.symbol, binding, arguments);
      summary.direct_effects = summary.effects;
      result_.procedures.push_back(std::move(summary));
    }

    // Replay source bodies until every returned procedure leaf is stable.
    // Each replay also refreshes call arguments that may themselves be the
    // result of a factory call. Local environments union every semantic write,
    // so branches remain conservative and optimization can never narrow the
    // selected may-target set.
    bool returns_changed = false;
    do {
      returns_changed = false;
      for (std::size_t index = 0; index < hir_.procedures().size(); ++index) {
        const HirProcedure &procedure = hir_.procedures()[index];
        ProcedureEffectSummary discovered;
        discovered.procedure = procedure.symbol;
        current_ = &discovered;
        current_procedure_ = procedure.symbol;
        current_result_type_ = {};
        const Symbol &symbol = package_.symbols.symbol(procedure.symbol);
        if (symbol.type.is_valid()) {
          const Type &type = package_.types.type(symbol.type);
          if (type.kind == TypeKind::Procedure && !type.members.empty()) {
            current_result_type_ = type.members.back();
          }
        }
        procedure_values_.clear();
        seed_parameter_slots(procedure.symbol);
        visit_block(procedure.body);
        if (result_.procedures[index].return_values !=
                discovered.return_values ||
            result_.procedures[index].field_writes !=
                discovered.field_writes) {
          returns_changed = true;
        }
        result_.procedures[index] = std::move(discovered);
      }
    } while (returns_changed);

    for (std::size_t index = 0; index < hir_.procedures().size(); ++index) {
      result_.procedures[index].effects =
          result_.procedures[index].direct_effects;
    }
    current_ = nullptr;
    current_procedure_ = {};
    current_result_type_ = {};
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

    // Recompose each source call once against the closed procedure summaries.
    // The denial walker consumes these exact rows, so a typed field selected at
    // one call site cannot degrade back into an unknown edge during its second
    // lexical-policy check.
    for (const ProcedureEffectSummary &summary : result_.procedures) {
      for (const ProcedureInvocationSummary &invocation :
           summary.direct_invocations) {
        ProcedureEffectSummary site;
        [[maybe_unused]] const bool added = compose_named_call(
            site, invocation.callee, invocation.arguments);
        result_.call_sites.push_back(
            {invocation.expression, std::move(site.effects)});
      }
      for (const ProcedureFlowInvocationSummary &invocation :
           summary.direct_flow_calls) {
        ProcedureEffectSummary site;
        [[maybe_unused]] const bool added = compose_value_call(
            site, invocation.callee, invocation.arguments);
        result_.call_sites.push_back(
            {invocation.expression, std::move(site.effects)});
      }
    }
    return std::move(result_);
  }

private:
  struct StoragePath {
    SymbolId symbol;
    std::vector<std::string> fields;
    bool context = false;
    // Dereference adds one and address-of removes one. A positive balance
    // rooted at a parameter reaches storage owned by the caller; zero or less
    // names storage local to this procedure even when its address is passed to
    // another procedure.
    int indirection = 0;
  };

  struct StoredProcedureValue {
    SymbolId symbol;
    std::vector<std::string> path;
    bool context = false;
    ProcedureValueSummary value;
  };

  struct CompositionFrame {
    SymbolId callee;
    std::vector<ProcedureArgumentSummary> arguments;
  };

  [[nodiscard]] bool procedure_type(TypeId type) const {
    return type.is_valid() &&
        package_.types.type(type).kind == TypeKind::Procedure;
  }

  [[nodiscard]] std::optional<SymbolId> type_owner(TypeId type) const {
    for (const OwnedSemanticScope &owned : package_.owned_scopes) {
      if (package_.symbols.scope(owned.scope).kind == ScopeKind::Type &&
          package_.symbols.symbol(owned.owner).type == type) {
        return owned.owner;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<std::pair<std::string, TypeId>> fields(
      TypeId type) const {
    std::vector<std::pair<std::string, TypeId>> result;
    if (!type.is_valid()) return result;
    const Type &row = package_.types.type(type);
    if (row.kind == TypeKind::Tuple) {
      for (std::size_t index = 0; index < row.members.size(); ++index) {
        result.push_back({std::to_string(index), row.members[index]});
      }
      return result;
    }
    const std::optional<SymbolId> owner = type_owner(type);
    if (!owner.has_value()) return result;
    for (const AggregateMember &member : package_.aggregate_members) {
      if (member.owner != *owner) continue;
      const Symbol &symbol = package_.symbols.symbol(member.member);
      if (symbol.kind == SymbolKind::Field) {
        result.push_back({symbol.name, symbol.type});
      }
    }
    return result;
  }

  void collect_procedure_paths(
      TypeId type,
      std::vector<std::string> &path,
      std::vector<TypeId> &active,
      bool follow_pointer,
      std::vector<std::vector<std::string>> &result) const {
    if (!type.is_valid()) return;
    const Type &row = package_.types.type(type);
    if (row.kind == TypeKind::Procedure) {
      result.push_back(path);
      return;
    }
    if ((row.kind == TypeKind::Pointer ||
         row.kind == TypeKind::MultiPointer) && follow_pointer) {
      if (std::find(active.begin(), active.end(), type) != active.end()) return;
      active.push_back(type);
      collect_procedure_paths(row.element, path, active, true, result);
      active.pop_back();
      return;
    }
    if (row.kind == TypeKind::Distinct) {
      collect_procedure_paths(row.element, path, active, follow_pointer, result);
      return;
    }
    if (row.kind != TypeKind::Struct && row.kind != TypeKind::Tuple) return;
    if (std::find(active.begin(), active.end(), type) != active.end()) return;
    active.push_back(type);
    for (const auto &[name, member_type] : fields(type)) {
      path.push_back(name);
      collect_procedure_paths(member_type, path, active, true, result);
      path.pop_back();
    }
    active.pop_back();
  }

  [[nodiscard]] std::vector<std::vector<std::string>> procedure_paths(
      TypeId type, bool follow_pointer = false) const {
    std::vector<std::vector<std::string>> result;
    std::vector<std::string> path;
    std::vector<TypeId> active;
    collect_procedure_paths(type, path, active, follow_pointer, result);
    return result;
  }

  [[nodiscard]] std::optional<std::string> member_name(
      const HirExpression &expression) const {
    if (expression.symbol.is_valid()) {
      return package_.symbols.symbol(expression.symbol).name;
    }
    if (expression.constant.kind == ConstantKind::Integer) {
      return expression.constant.integer.to_decimal();
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<StoragePath> storage_path(
      HirExpressionId id) const {
    if (!id.is_valid()) return std::nullopt;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Context) {
      StoragePath result;
      result.context = true;
      return result;
    }
    if (expression.kind == HirExpressionKind::Symbol &&
        expression.symbol.is_valid()) {
      const SymbolKind kind =
          package_.symbols.symbol(expression.symbol).kind;
      if (kind == SymbolKind::Variable || kind == SymbolKind::Local ||
          kind == SymbolKind::Parameter) {
        StoragePath result;
        result.symbol = expression.symbol;
        return result;
      }
      return std::nullopt;
    }
    if (expression.kind == HirExpressionKind::Address &&
        !expression.operands.empty()) {
      std::optional<StoragePath> result =
          storage_path(expression.operands.front());
      if (result.has_value()) --result->indirection;
      return result;
    }
    if (expression.kind == HirExpressionKind::Dereference &&
        !expression.operands.empty()) {
      std::optional<StoragePath> result =
          storage_path(expression.operands.front());
      if (result.has_value()) ++result->indirection;
      return result;
    }
    if (expression.kind != HirExpressionKind::Member ||
        expression.operands.empty()) {
      return std::nullopt;
    }
    std::optional<StoragePath> result =
        storage_path(expression.operands.front());
    const std::optional<std::string> name = member_name(expression);
    if (!result.has_value() || !name.has_value()) return std::nullopt;
    result->fields.push_back(*name);
    return result;
  }

  [[nodiscard]] std::optional<std::uint32_t> parameter_ordinal(
      SymbolId parameter) const {
    for (const OwnedSemanticScope &owned : package_.owned_scopes) {
      if (owned.owner != current_procedure_ ||
          package_.symbols.scope(owned.scope).kind != ScopeKind::Procedure) {
        continue;
      }
      std::uint32_t ordinal = 0;
      for (SymbolId symbol : package_.symbols.scope(owned.scope).symbols) {
        if (package_.symbols.symbol(symbol).kind != SymbolKind::Parameter) {
          continue;
        }
        if (symbol == parameter) return ordinal;
        ++ordinal;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] ProcedureValueSummary *stored_value(
      SymbolId symbol,
      const std::vector<std::string> &path,
      bool context = false) {
    for (StoredProcedureValue &stored : procedure_values_) {
      if (stored.symbol == symbol && stored.path == path &&
          stored.context == context) {
        return &stored.value;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const ProcedureValueSummary *stored_value(
      SymbolId symbol,
      const std::vector<std::string> &path,
      bool context = false) const {
    for (const StoredProcedureValue &stored : procedure_values_) {
      if (stored.symbol == symbol && stored.path == path &&
          stored.context == context) {
        return &stored.value;
      }
    }
    return nullptr;
  }

  void merge_stored_value(
      SymbolId symbol,
      std::vector<std::string> path,
      const ProcedureValueSummary &value,
      bool context = false) {
    if (ProcedureValueSummary *stored = stored_value(symbol, path, context)) {
      merge_value(*stored, value);
      return;
    }
    procedure_values_.push_back({symbol, std::move(path), context, value});
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
          value.flow_slots.push_back({ordinal, {}, false});
          procedure_values_.push_back(
              {symbol_id, {}, false, std::move(value)});
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

  [[nodiscard]] const ForeignProviderAudit *provider_audit(
      std::string_view provider) const {
    for (const ForeignProviderAudit &audit : provider_audits_) {
      if (audit.provider == provider) return &audit;
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

  [[nodiscard]] const ProcedureValueSummary *argument_field(
      const std::vector<ProcedureArgumentSummary> &arguments,
      std::uint32_t parameter,
      const std::vector<std::string> &path) const {
    if (parameter >= arguments.size()) return nullptr;
    for (const ProcedureFieldValueSummary &field : arguments[parameter].fields) {
      if (field.path == path) return &field.value;
    }
    return nullptr;
  }

  [[nodiscard]] ProcedureValueSummary substitute_return_value(
      const ProcedureValueSummary &source,
      const std::vector<ProcedureArgumentSummary> &arguments) const {
    ProcedureValueSummary result;
    result.targets = source.targets;
    result.contract_effects = source.contract_effects;
    result.unknown = source.unknown;
    for (const ProcedureFlowSlot &slot : source.flow_slots) {
      if (slot.context) {
        add_flow_slot(result, slot);
        continue;
      }
      const ProcedureValueSummary *argument =
          argument_field(arguments, slot.parameter, slot.path);
      if (argument == nullptr) {
        result.unknown = true;
      } else {
        merge_value(result, *argument);
      }
    }
    return result;
  }

  [[nodiscard]] std::vector<ProcedureArgumentSummary> substitute_arguments(
      const std::vector<ProcedureArgumentSummary> &source,
      const std::vector<ProcedureArgumentSummary> &arguments) const {
    std::vector<ProcedureArgumentSummary> result;
    result.reserve(source.size());
    for (const ProcedureArgumentSummary &argument : source) {
      ProcedureArgumentSummary substituted;
      for (const ProcedureFieldValueSummary &field : argument.fields) {
        substituted.fields.push_back({
            field.path,
            substitute_return_value(field.value, arguments)});
      }
      result.push_back(std::move(substituted));
    }
    return result;
  }

  [[nodiscard]] ProcedureValueSummary imported_flow_value(
      const ImportedFlowValue &source) const {
    ProcedureValueSummary result;
    result.unknown = source.unknown;
    for (const ImportedReturnFlowSlot &slot : source.flow_slots) {
      result.flow_slots.push_back(
          {slot.parameter, slot.path, slot.context});
    }
    for (const ImportedEffect &effect : source.contract_effects) {
      add_effect(result.contract_effects, semantic_effect(effect));
    }
    return result;
  }

  [[nodiscard]] SemanticEffect semantic_effect(
      const ImportedEffect &source) const {
    std::vector<ProcedureArgumentSummary> arguments;
    for (const ImportedFlowArgument &argument : source.flow_arguments) {
      ProcedureArgumentSummary semantic_argument;
      for (const ImportedFlowField &field : argument.fields) {
        semantic_argument.fields.push_back(
            {field.path, imported_flow_value(field.value)});
      }
      arguments.push_back(std::move(semantic_argument));
    }
    return {
        source.kind,
        {},
        source.detail,
        source.root_identity,
        source.root_relative_path,
        source.declaration,
        source.flow_parameter,
        source.flow_path,
        source.flow_context,
        std::move(arguments)};
  }

  [[nodiscard]] ProcedureValueSummary imported_return_value(
      SymbolId callee,
      const std::vector<std::string> &path,
      const std::vector<ProcedureArgumentSummary> &arguments) const {
    ProcedureValueSummary canonical;
    bool found = false;
    for (const ImportedProcedureReturn &returned : package_.imported_returns) {
      if (returned.procedure_proxy != callee || returned.path != path) continue;
      found = true;
      canonical.unknown = returned.unknown;
      for (const ImportedReturnFlowSlot &slot : returned.flow_slots) {
        canonical.flow_slots.push_back(
            {slot.parameter, slot.path, slot.context});
      }
      for (const ImportedEffect &effect : returned.contract_effects) {
        add_effect(canonical.contract_effects, semantic_effect(effect));
      }
    }
    if (!found) canonical.unknown = true;
    return substitute_return_value(canonical, arguments);
  }

  [[nodiscard]] ProcedureValueSummary imported_write_value(
      const ImportedProcedureWrite &write,
      const std::vector<ProcedureArgumentSummary> &arguments) const {
    ProcedureValueSummary canonical;
    canonical.unknown = write.value_unknown;
    for (const ImportedReturnFlowSlot &slot : write.value_flow_slots) {
      canonical.flow_slots.push_back(
          {slot.parameter, slot.path, slot.context});
    }
    for (const ImportedEffect &effect : write.value_contract_effects) {
      add_effect(canonical.contract_effects, semantic_effect(effect));
    }
    return substitute_return_value(canonical, arguments);
  }

  void apply_value_write(
      StoragePath destination,
      const std::vector<std::string> &relative_path,
      const ProcedureValueSummary &value) {
    destination.fields.insert(
        destination.fields.end(), relative_path.begin(), relative_path.end());
    // A call can itself be the statement that makes a pointer write escape
    // from the current procedure. Publish that transitive write before
    // updating the current procedure's local value environment.
    merge_field_write(destination, destination.fields, value);
    if (stored_value(
            destination.symbol,
            destination.fields,
            destination.context) == nullptr) {
      ProcedureValueSummary original;
      if (destination.context) {
        original.flow_slots.push_back({
            std::numeric_limits<std::uint32_t>::max(),
            destination.fields,
            true});
      } else if (destination.symbol.is_valid() &&
                 package_.symbols.symbol(destination.symbol).kind ==
                     SymbolKind::Parameter) {
        const std::optional<std::uint32_t> ordinal =
            parameter_ordinal(destination.symbol);
        if (ordinal.has_value()) {
          original.flow_slots.push_back(
              {*ordinal, destination.fields, false});
        }
      }
      if (!value_is_empty(original)) {
        merge_stored_value(
            destination.symbol,
            destination.fields,
            original,
            destination.context);
      }
    }
    merge_stored_value(
        destination.symbol,
        std::move(destination.fields),
        value,
        destination.context);
  }

  void apply_call_writes(
      const HirExpression &call,
      SymbolId callee,
      const std::vector<ProcedureArgumentSummary> &arguments) {
    auto apply = [&](std::uint32_t parameter,
                     std::uint32_t indirection,
                     const std::vector<std::string> &path,
                     const ProcedureValueSummary &value) {
      if (call.operands.empty() ||
          parameter >= call.operands.size() - 1U) {
        return;
      }
      std::optional<StoragePath> destination =
          storage_path(call.operands[parameter + 1U]);
      if (!destination.has_value()) return;
      // Apply every dereference performed from the callee's formal parameter.
      // Explicit address operations already contributed negative balances.
      destination->indirection += static_cast<int>(indirection);
      apply_value_write(
          std::move(*destination),
          path,
          substitute_return_value(value, arguments));
    };

    if (const ProcedureEffectSummary *summary = result_.find(callee)) {
      for (const ProcedureFieldWriteSummary &write : summary->field_writes) {
        apply(
            write.parameter,
            write.indirection,
            write.path,
            write.value);
      }
      return;
    }
    for (const ImportedProcedureWrite &write : package_.imported_writes) {
      if (write.procedure_proxy != callee) continue;
      if (call.operands.empty() ||
          write.parameter >= call.operands.size() - 1U) {
        continue;
      }
      std::optional<StoragePath> destination =
          storage_path(call.operands[write.parameter + 1U]);
      if (!destination.has_value()) continue;
      destination->indirection += static_cast<int>(write.indirection);
      apply_value_write(
          std::move(*destination),
          write.path,
          imported_write_value(write, arguments));
    }
  }

  [[nodiscard]] ProcedureValueSummary call_return_value(
      HirExpressionId call_id,
      const HirExpression &call,
      const std::vector<std::string> &path) const {
    ProcedureValueSummary result;
    if (call.operands.empty()) {
      result.unknown = true;
      return result;
    }
    const HirExpression &callee = hir_.expression(call.operands.front());
    // record_call captured source-ordered arguments before the call's own
    // write-back became visible. Reuse that immutable snapshot when deriving a
    // returned callback instead of rereading possibly mutated storage here.
    const std::vector<ProcedureArgumentSummary> *arguments = nullptr;
    if (current_ != nullptr) {
      for (const ProcedureInvocationSummary &invocation :
           current_->direct_invocations) {
        if (invocation.expression == call_id) {
          arguments = &invocation.arguments;
          break;
        }
      }
    }
    std::vector<ProcedureArgumentSummary> fallback_arguments;
    if (arguments == nullptr) {
      fallback_arguments = call_arguments(call, 1);
      arguments = &fallback_arguments;
    }
    if (callee.kind != HirExpressionKind::Symbol ||
        !callee.symbol.is_valid() ||
        package_.symbols.symbol(callee.symbol).kind != SymbolKind::Procedure) {
      result.unknown = true;
      return result;
    }
    if (const ProcedureEffectSummary *summary = result_.find(callee.symbol)) {
      for (const ProcedureFieldValueSummary &returned :
           summary->return_values) {
        if (returned.path == path) {
          return substitute_return_value(returned.value, *arguments);
        }
      }
      // The compiler-owned default_context bridge returns a snapshot of the
      // current hidden Context. Its typed procedure leaves therefore retain
      // their Context-rooted paths without becoming arbitrary native targets.
      if (const NativeBinding *binding = native_binding(callee.symbol)) {
        const std::optional<std::string> linker_name =
            decode_linker_name(binding->linker_name_spelling);
        if (binding->provider == "draft_runtime" &&
            linker_name.has_value() &&
            *linker_name == "__draft.runtime.default_context") {
          result.flow_slots.push_back({
              std::numeric_limits<std::uint32_t>::max(), path, true});
          return result;
        }
      }
      result.unknown = true;
      return result;
    }
    if (imported_symbol(callee.symbol) != nullptr) {
      return imported_return_value(callee.symbol, path, *arguments);
    }
    result.unknown = true;
    return result;
  }

  [[nodiscard]] ProcedureValueSummary procedure_value_at(
      HirExpressionId id,
      const std::vector<std::string> &relative_path) const {
    ProcedureValueSummary value;
    if (!id.is_valid()) {
      value.unknown = true;
      return value;
    }
    const HirExpression &expression = hir_.expression(id);

    if (std::optional<StoragePath> location = storage_path(id)) {
      location->fields.insert(
          location->fields.end(), relative_path.begin(), relative_path.end());
      if (const ProcedureValueSummary *stored = stored_value(
              location->symbol, location->fields, location->context)) {
        return *stored;
      }
      if (!location->context) {
        const Symbol &root = package_.symbols.symbol(location->symbol);
        if (root.kind == SymbolKind::Parameter) {
          const std::optional<std::uint32_t> ordinal =
              parameter_ordinal(location->symbol);
          if (ordinal.has_value()) {
            value.flow_slots.push_back(
                {*ordinal, std::move(location->fields), false});
            return value;
          }
        }
      } else {
        value.flow_slots.push_back({
            std::numeric_limits<std::uint32_t>::max(),
            std::move(location->fields),
            true});
        return value;
      }
      value.unknown = true;
      return value;
    }

    if (relative_path.empty() &&
        expression.kind == HirExpressionKind::Symbol &&
        expression.symbol.is_valid()) {
      const Symbol &symbol = package_.symbols.symbol(expression.symbol);
      if (symbol.kind == SymbolKind::Procedure) {
        value.targets.push_back(expression.symbol);
        return value;
      }
    }

    // Conditional and denial expressions preserve values from their result
    // operands. Operands with non-procedure types (conditions, for example)
    // contribute nothing to this union.
    if (expression.kind == HirExpressionKind::Conditional ||
        expression.kind == HirExpressionKind::Denial) {
      for (HirExpressionId operand : expression.operands) {
        merge_value(value, procedure_value_at(operand, relative_path));
      }
      if (value_is_empty(value)) value.unknown = true;
      return value;
    }

    // Composite operands retain their selected field SymbolId. Positional
    // operands use the aggregate's canonical member order. Following one row
    // at a time makes keyed and positional aggregate construction contribute
    // the same typed callback path.
    if (expression.kind == HirExpressionKind::Composite &&
        !relative_path.empty()) {
      const std::vector<std::pair<std::string, TypeId>> members =
          fields(expression.type);
      for (std::size_t index = 0; index < expression.operands.size(); ++index) {
        std::string name;
        if (index < expression.operand_members.size() &&
            expression.operand_members[index].is_valid()) {
          name = package_.symbols.symbol(
              expression.operand_members[index]).name;
        } else if (index < members.size()) {
          name = members[index].first;
        }
        if (name != relative_path.front()) continue;
        const std::vector<std::string> remainder(
            relative_path.begin() + 1, relative_path.end());
        return procedure_value_at(expression.operands[index], remainder);
      }
      value.unknown = true;
      return value;
    }

    if (expression.kind == HirExpressionKind::Call) {
      return call_return_value(id, expression, relative_path);
    }

    // Erased raw storage and dynamically selected factories remain explicit
    // unknowns rather than being guessed from optimization or pointer bits.
    value.unknown = true;
    return value;
  }

  [[nodiscard]] ProcedureValueSummary procedure_value(
      HirExpressionId id) const {
    return procedure_value_at(id, {});
  }

  void merge_field_write(
      const StoragePath &destination,
      const std::vector<std::string> &path,
      const ProcedureValueSummary &value) {
    if (destination.context || destination.indirection <= 0 ||
        !destination.symbol.is_valid() ||
        package_.symbols.symbol(destination.symbol).kind !=
            SymbolKind::Parameter) {
      return;
    }
    const std::optional<std::uint32_t> ordinal =
        parameter_ordinal(destination.symbol);
    if (!ordinal.has_value()) return;
    for (ProcedureFieldWriteSummary &write : current_->field_writes) {
      if (write.parameter == *ordinal &&
          write.indirection ==
              static_cast<std::uint32_t>(destination.indirection) &&
          write.path == path) {
        merge_value(write.value, value);
        return;
      }
    }
    current_->field_writes.push_back({
        *ordinal,
        static_cast<std::uint32_t>(destination.indirection),
        path,
        value});
  }

  void merge_assignment_value(
      StoragePath destination,
      TypeId destination_type,
      HirExpressionId source) {
    for (const std::vector<std::string> &relative :
         procedure_paths(destination_type)) {
      std::vector<std::string> complete = destination.fields;
      complete.insert(complete.end(), relative.begin(), relative.end());
      if (stored_value(
              destination.symbol, complete, destination.context) == nullptr) {
        ProcedureValueSummary original;
        if (destination.context) {
          original.flow_slots.push_back({
              std::numeric_limits<std::uint32_t>::max(),
              complete,
              true});
        } else if (package_.symbols.symbol(destination.symbol).kind ==
                   SymbolKind::Parameter) {
          const std::optional<std::uint32_t> ordinal =
              parameter_ordinal(destination.symbol);
          if (ordinal.has_value()) {
            original.flow_slots.push_back({*ordinal, complete, false});
          }
        }
        if (!value_is_empty(original)) {
          merge_stored_value(
              destination.symbol,
              complete,
              original,
              destination.context);
        }
      }
      const ProcedureValueSummary assigned =
          procedure_value_at(source, relative);
      merge_field_write(destination, complete, assigned);
      merge_stored_value(
          destination.symbol,
          std::move(complete),
          assigned,
          destination.context);
    }
  }

  void merge_binding_value(
      SymbolId binding, TypeId type, HirExpressionId source) {
    StoragePath destination;
    destination.symbol = binding;
    merge_assignment_value(
        std::move(destination), type, source);
  }

  void merge_return_value(
      const std::vector<std::string> &path,
      const ProcedureValueSummary &value) {
    for (ProcedureFieldValueSummary &returned : current_->return_values) {
      if (returned.path == path) {
        merge_value(returned.value, value);
        return;
      }
    }
    current_->return_values.push_back({path, value});
  }

  [[nodiscard]] ProcedureArgumentSummary call_argument(
      HirExpressionId id) const {
    ProcedureArgumentSummary result;
    if (!id.is_valid()) return result;
    const HirExpression &expression = hir_.expression(id);
    const Type &type = package_.types.type(expression.type);
    const bool follow_pointer = type.kind == TypeKind::Pointer ||
        type.kind == TypeKind::MultiPointer;
    for (const std::vector<std::string> &path :
         procedure_paths(expression.type, follow_pointer)) {
      result.fields.push_back({path, procedure_value_at(id, path)});
    }
    return result;
  }

  [[nodiscard]] std::vector<ProcedureArgumentSummary> call_arguments(
      const HirExpression &call, std::size_t first) const {
    std::vector<ProcedureArgumentSummary> result;
    for (std::size_t index = first; index < call.operands.size(); ++index) {
      result.push_back(call_argument(call.operands[index]));
    }
    return result;
  }

  void record_call(
      HirExpressionId call_id,
      const HirExpression &call,
      ProcedureValueSummary callee_value,
      std::vector<ProcedureArgumentSummary> arguments) {
    if (call.operands.empty()) return;
    const HirExpression &callee = hir_.expression(call.operands.front());
    if (callee.kind == HirExpressionKind::Symbol &&
        callee.symbol.is_valid() &&
        package_.symbols.symbol(callee.symbol).kind == SymbolKind::Procedure) {
      if (has_local_body(callee.symbol)) {
        add_call(current_->direct_calls, callee.symbol);
      }
      current_->direct_invocations.push_back(
          {call_id, callee.symbol, arguments});
      apply_call_writes(call, callee.symbol, arguments);
      return;
    }
    ProcedureFlowInvocationSummary invocation{
        call_id, std::move(callee_value), std::move(arguments)};
    for (SymbolId target : invocation.callee.targets) {
      if (has_local_body(target)) add_call(current_->direct_calls, target);
    }
    current_->direct_flow_calls.push_back(std::move(invocation));
  }

  // runtime.call_with_context keeps its callback at operand one and its
  // callback arguments after that. It participates in the same finite-target
  // composition as an ordinary indirect call.
  void record_context_call(
      HirExpressionId expression_id,
      ProcedureValueSummary callee,
      std::vector<ProcedureArgumentSummary> arguments) {
    ProcedureFlowInvocationSummary invocation;
    invocation.expression = expression_id;
    invocation.callee = std::move(callee);
    invocation.arguments = std::move(arguments);
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
      const std::vector<ProcedureArgumentSummary> &arguments) {
    if (effect.kind != EffectKind::FlowCall) {
      return add_composed_effect(destination, effect);
    }
    const std::vector<ProcedureArgumentSummary> nested_arguments =
        substitute_arguments(effect.flow_arguments, arguments);
    if (effect.flow_context) {
      ProcedureValueSummary value;
      value.flow_slots.push_back({
          std::numeric_limits<std::uint32_t>::max(),
          effect.flow_path,
          true});
      return compose_value_call(destination, value, nested_arguments);
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
    for (const ProcedureFieldValueSummary &field :
         arguments[effect.flow_parameter].fields) {
      if (field.path == effect.flow_path) {
        return compose_value_call(
            destination, field.value, nested_arguments);
      }
    }
    return add_composed_effect(
        destination,
        {EffectKind::UnknownCall,
         {},
         "typed callback path is absent at call site",
         {},
         {},
         {}});
  }

  [[nodiscard]] bool compose_imported_call(
      ProcedureEffectSummary &destination,
      SymbolId callee,
      const ImportedSymbol &imported,
      const std::vector<ProcedureArgumentSummary> &arguments) {
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
          destination, semantic_effect(effect),
          arguments) || changed;
    }
    return changed;
  }

  [[nodiscard]] bool compose_native_call(
      ProcedureEffectSummary &destination,
      SymbolId callee,
      const NativeBinding &binding,
      const std::vector<ProcedureArgumentSummary> &arguments) {
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
      const ForeignProviderAudit *audit = provider_audit(binding.provider);
      const ForeignAuditSymbol *symbol =
          audit != nullptr && linker_name.has_value()
          ? audit->find_symbol(*linker_name)
          : nullptr;
      if (symbol != nullptr) {
        bool changed = false;
        for (const ForeignAuditEffect &effect : symbol->effects) {
          changed = compose_effect(
              destination,
              {effect.kind,
               {},
               effect.detail,
               effect.root_identity,
               effect.root_relative_path,
               effect.declaration,
               effect.flow_parameter,
               effect.flow_path,
               effect.flow_context},
              arguments) || changed;
        }
        return changed;
      }
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
        changed = compose_effect(
            destination,
            {EffectKind::FlowCall,
             {},
             "system callback parameter",
             {},
             {},
             {},
             parameter},
            arguments) || changed;
      }
    }
    return changed;
  }

  [[nodiscard]] bool compose_named_call(
      ProcedureEffectSummary &destination,
      SymbolId callee,
      const std::vector<ProcedureArgumentSummary> &arguments) {
    bool changed = add_composed_effect(
        destination,
        {EffectKind::Declaration,
         callee,
         "procedure call",
         {},
         {},
         {}});
    for (const CompositionFrame &frame : composition_stack_) {
      if (frame.callee != callee) continue;
      if (frame.arguments == arguments) {
        // This exact typed call contract is already being expanded. Any direct
        // effects were copied by the outer frame, so following the same edge
        // again cannot add a new reachable effect.
        return changed;
      }
      return add_composed_effect(
          destination,
          {EffectKind::UnknownCall,
           callee,
           "recursive higher-order call changes its callback contract",
           {},
           {},
           {}}) || changed;
    }
    composition_stack_.push_back({callee, arguments});
    if (const ProcedureEffectSummary *summary = result_.find(callee)) {
      // Copy the row before adding to destination: recursive calls may make
      // summary and destination the same vector.
      const std::vector<SemanticEffect> effects = summary->effects;
      for (const SemanticEffect &effect : effects) {
        changed = compose_effect(destination, effect, arguments) || changed;
      }
    } else if (const ImportedSymbol *imported = imported_symbol(callee)) {
      changed = compose_imported_call(
          destination, callee, *imported, arguments) || changed;
    } else if (const NativeBinding *binding = native_binding(callee)) {
      changed = compose_native_call(
          destination, callee, *binding, arguments) || changed;
    } else {
      changed = add_composed_effect(
          destination,
          {EffectKind::UnknownCall,
           callee,
           "callee has no composed summary",
           {},
           {},
           {}}) || changed;
    }
    composition_stack_.pop_back();
    return changed;
  }

  [[nodiscard]] bool compose_value_call(
      ProcedureEffectSummary &destination,
      const ProcedureValueSummary &value,
      const std::vector<ProcedureArgumentSummary> &arguments) {
    bool changed = false;
    for (SymbolId target : value.targets) {
      changed = compose_named_call(destination, target, arguments) || changed;
    }
    for (const SemanticEffect &effect : value.contract_effects) {
      changed = compose_effect(destination, effect, arguments) || changed;
    }
    for (const ProcedureFlowSlot &slot : value.flow_slots) {
      changed = add_composed_effect(
          destination,
          {EffectKind::FlowCall,
           {},
           "procedure parameter callback",
           {},
           {},
           {},
           slot.parameter,
           slot.path,
           slot.context,
           arguments}) || changed;
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

    // Calls evaluate the callee and then each argument in source order. Capture
    // each procedure value immediately after its own evaluation: a later
    // argument may call an initializer that mutates callback-bearing storage,
    // but that cannot retroactively change an earlier argument or callee.
    if (expression.kind == HirExpressionKind::Call) {
      if (expression.operands.empty()) return;
      visit_expression(expression.operands.front());
      ProcedureValueSummary callee =
          procedure_value(expression.operands.front());
      std::vector<ProcedureArgumentSummary> arguments;
      for (std::size_t index = 1; index < expression.operands.size(); ++index) {
        visit_expression(expression.operands[index]);
        arguments.push_back(call_argument(expression.operands[index]));
      }
      record_call(
          id, expression, std::move(callee), std::move(arguments));
      return;
    }
    if (expression.kind == HirExpressionKind::Intrinsic &&
        expression.constant.kind == ConstantKind::String &&
        expression.constant.text == "call_with_context") {
      if (expression.operands.size() < 2) return;
      visit_expression(expression.operands.front());
      visit_expression(expression.operands[1]);
      ProcedureValueSummary callee = procedure_value(expression.operands[1]);
      std::vector<ProcedureArgumentSummary> arguments;
      for (std::size_t index = 2; index < expression.operands.size(); ++index) {
        visit_expression(expression.operands[index]);
        arguments.push_back(call_argument(expression.operands[index]));
      }
      record_context_call(
          id, std::move(callee), std::move(arguments));
      return;
    }

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
    if (statement.kind == HirStatementKind::Return &&
        statement.expressions.size() == 1) {
      for (const std::vector<std::string> &path :
           procedure_paths(current_result_type_)) {
        merge_return_value(
            path, procedure_value_at(statement.expressions.front(), path));
      }
    } else if (statement.kind == HirStatementKind::LocalDeclaration &&
        statement.bindings.size() == 1 &&
        statement.expressions.size() == 1) {
      const SymbolId binding = statement.bindings.front();
      merge_binding_value(
          binding,
          package_.symbols.symbol(binding).type,
          statement.expressions.front());
    } else if (statement.kind == HirStatementKind::Assignment &&
               statement.operation == HirOperation::Assign) {
      const std::size_t left_count = statement.expressions.size() / 2;
      for (std::size_t index = 0; index < left_count; ++index) {
        const HirExpression &left = hir_.expression(statement.expressions[index]);
        std::optional<StoragePath> destination =
            storage_path(statement.expressions[index]);
        if (!destination.has_value()) continue;
        merge_assignment_value(
            std::move(*destination),
            left.type,
            statement.expressions[left_count + index]);
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
  std::span<const ForeignProviderAudit> provider_audits_;
  EffectSummaryResult result_;
  ProcedureEffectSummary *current_ = nullptr;
  SymbolId current_procedure_;
  TypeId current_result_type_;
  std::vector<StoredProcedureValue> procedure_values_;
  std::vector<CompositionFrame> composition_stack_;
};

} // namespace

const ProcedureEffectSummary *EffectSummaryResult::find(
    SymbolId procedure) const {
  for (const ProcedureEffectSummary &summary : procedures) {
    if (summary.procedure == procedure) return &summary;
  }
  return nullptr;
}

const CallSiteEffectSummary *EffectSummaryResult::find_call_site(
    HirExpressionId expression) const {
  for (const CallSiteEffectSummary &summary : call_sites) {
    if (summary.expression == expression) return &summary;
  }
  return nullptr;
}

EffectSummaryResult summarize_package_effects(
    const SemanticPackage &package,
    const HirProgram &hir,
    const TargetProfile *target,
    std::span<const ForeignProviderAudit> provider_audits) {
  EffectCollector collector(package, hir, target, provider_audits);
  return collector.run();
}

std::string_view effect_kind_name(EffectKind kind) {
  switch (kind) {
  case EffectKind::Declaration: return "declaration";
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
