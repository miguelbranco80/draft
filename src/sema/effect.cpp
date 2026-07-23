// Direct HIR effect discovery and conservative procedure-slot composition.

#include "sema/effect.h"

#include "syntax/literal.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

// EffectPhaseTimer is the semantic subsystem's narrow diagnostic boundary. A
// null destination performs no clock reads. The explicit destination keeps
// timing out of EffectSummaryResult and makes the ordinary semantic path
// identical except for the requested observation.
class EffectPhaseTimer {
public:
  explicit EffectPhaseTimer(std::uint64_t *destination)
      : destination_(destination) {
    if (destination_ != nullptr) started_ = Clock::now();
  }

  EffectPhaseTimer(const EffectPhaseTimer &) = delete;
  EffectPhaseTimer &operator=(const EffectPhaseTimer &) = delete;

  ~EffectPhaseTimer() { finish(); }

  void finish() {
    if (destination_ == nullptr) return;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started_)
            .count();
    assert(elapsed >= 0 && "effect phase duration must be nonnegative");
    *destination_ = static_cast<std::uint64_t>(elapsed);
    destination_ = nullptr;
  }

private:
  using Clock = std::chrono::steady_clock;

  std::uint64_t *destination_ = nullptr;
  Clock::time_point started_;
};

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

// Builds the concrete local call/value-flow condensation graph without using
// hash-table or traversal-order identity. Procedure rows are the stable node
// domain. The first depth-first pass records finish order in the caller-to-
// callee graph; the second walks reversed edges to recover SCCs. Standard
// Kosaraju order yields caller components first for this edge direction, so the
// final reversal makes every callee component complete before its consumers.
//
// Iterative stacks avoid making a large source call graph consume the C++ call
// stack. Sorting each component by procedure-row index makes publication
// independent from the exact DFS path used to discover the same SCC.
[[nodiscard]] std::vector<ClosedEffectComponent> build_effect_components(
    const std::vector<DirectProcedureEffectSummary> &procedures) {
  std::size_t symbol_domain = 0;
  for (const DirectProcedureEffectSummary &procedure : procedures) {
    if (procedure.procedure.is_valid()) {
      symbol_domain = std::max(
          symbol_domain,
          static_cast<std::size_t>(procedure.procedure.value) + 1U);
    }
  }
  const std::size_t absent = procedures.size();
  std::vector<std::size_t> row_by_symbol(symbol_domain, absent);
  for (std::size_t index = 0; index < procedures.size(); ++index) {
    const SymbolId symbol = procedures[index].procedure;
    if (symbol.is_valid()) row_by_symbol[symbol.value] = index;
  }

  std::vector<std::vector<std::size_t>> edges(procedures.size());
  std::vector<std::vector<std::size_t>> reverse_edges(procedures.size());
  for (std::size_t caller = 0; caller < procedures.size(); ++caller) {
    for (SymbolId callee_symbol : procedures[caller].direct_calls) {
      if (!callee_symbol.is_valid() ||
          callee_symbol.value >= row_by_symbol.size()) {
        continue;
      }
      const std::size_t callee = row_by_symbol[callee_symbol.value];
      if (callee == absent) continue;
      edges[caller].push_back(callee);
    }
    std::sort(edges[caller].begin(), edges[caller].end());
    edges[caller].erase(
        std::unique(edges[caller].begin(), edges[caller].end()),
        edges[caller].end());
    for (std::size_t callee : edges[caller]) {
      reverse_edges[callee].push_back(caller);
    }
  }

  struct DfsFrame {
    std::size_t procedure = 0;
    std::size_t next_edge = 0;
  };
  std::vector<bool> visited(procedures.size(), false);
  std::vector<std::size_t> finish_order;
  finish_order.reserve(procedures.size());
  for (std::size_t root = 0; root < procedures.size(); ++root) {
    if (visited[root]) continue;
    visited[root] = true;
    std::vector<DfsFrame> stack{{root, 0}};
    while (!stack.empty()) {
      DfsFrame &frame = stack.back();
      if (frame.next_edge < edges[frame.procedure].size()) {
        const std::size_t callee =
            edges[frame.procedure][frame.next_edge++];
        if (!visited[callee]) {
          visited[callee] = true;
          stack.push_back({callee, 0});
        }
        continue;
      }
      finish_order.push_back(frame.procedure);
      stack.pop_back();
    }
  }

  std::fill(visited.begin(), visited.end(), false);
  std::vector<ClosedEffectComponent> caller_first;
  for (auto position = finish_order.rbegin();
       position != finish_order.rend(); ++position) {
    const std::size_t root = *position;
    if (visited[root]) continue;
    ClosedEffectComponent component;
    visited[root] = true;
    std::vector<std::size_t> stack{root};
    while (!stack.empty()) {
      const std::size_t procedure = stack.back();
      stack.pop_back();
      component.procedure_indices.push_back(procedure);
      // Push reversed so the smaller canonical row is visited first after the
      // LIFO pop. The final sort below is still the publication authority.
      for (auto edge = reverse_edges[procedure].rbegin();
           edge != reverse_edges[procedure].rend(); ++edge) {
        if (visited[*edge]) continue;
        visited[*edge] = true;
        stack.push_back(*edge);
      }
    }
    std::sort(
        component.procedure_indices.begin(),
        component.procedure_indices.end());
    caller_first.push_back(std::move(component));
  }
  std::vector<std::size_t> component_by_procedure(
      procedures.size(), caller_first.size());
  for (std::size_t component = 0; component < caller_first.size(); ++component) {
    for (std::size_t procedure :
         caller_first[component].procedure_indices) {
      component_by_procedure[procedure] = component;
    }
  }
  std::vector<std::vector<std::size_t>> component_dependencies(
      caller_first.size());
  std::vector<std::vector<std::size_t>> consumers(caller_first.size());
  std::vector<std::size_t> remaining_dependencies(caller_first.size(), 0);
  for (std::size_t caller = 0; caller < procedures.size(); ++caller) {
    const std::size_t caller_component = component_by_procedure[caller];
    for (std::size_t callee : edges[caller]) {
      const std::size_t callee_component = component_by_procedure[callee];
      if (callee_component != caller_component) {
        component_dependencies[caller_component].push_back(callee_component);
      }
    }
  }
  for (std::size_t component = 0; component < caller_first.size(); ++component) {
    std::vector<std::size_t> &dependencies =
        component_dependencies[component];
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(
        std::unique(dependencies.begin(), dependencies.end()),
        dependencies.end());
    for (std::size_t dependency : dependencies) {
      consumers[dependency].push_back(component);
    }
  }
  for (std::size_t dependency = 0; dependency < consumers.size(); ++dependency) {
    std::sort(consumers[dependency].begin(), consumers[dependency].end());
    consumers[dependency].erase(
        std::unique(
            consumers[dependency].begin(), consumers[dependency].end()),
        consumers[dependency].end());
    for (std::size_t consumer : consumers[dependency]) {
      ++remaining_dependencies[consumer];
    }
  }

  // A component is ready after all components it calls are published. The
  // smallest procedure-row index is the stable tie-break for independent SCCs.
  using ReadyComponent = std::pair<std::size_t, std::size_t>;
  std::priority_queue<
      ReadyComponent,
      std::vector<ReadyComponent>,
      std::greater<ReadyComponent>>
      ready;
  for (std::size_t component = 0; component < caller_first.size(); ++component) {
    if (remaining_dependencies[component] == 0) {
      ready.push({
          caller_first[component].procedure_indices.front(), component});
    }
  }
  std::vector<ClosedEffectComponent> dependency_first;
  dependency_first.reserve(caller_first.size());
  std::vector<std::size_t> final_index_by_component(
      caller_first.size(), caller_first.size());
  while (!ready.empty()) {
    const std::size_t component = ready.top().second;
    ready.pop();
    final_index_by_component[component] = dependency_first.size();
    dependency_first.push_back(std::move(caller_first[component]));
    for (std::size_t consumer : consumers[component]) {
      if (--remaining_dependencies[consumer] == 0) {
        ready.push({
            caller_first[consumer].procedure_indices.front(), consumer});
      }
    }
  }
  for (std::size_t component = 0;
       component < component_dependencies.size(); ++component) {
    const std::size_t final_component = final_index_by_component[component];
    assert(final_component < dependency_first.size());
    for (std::size_t dependency : component_dependencies[component]) {
      const std::size_t final_dependency =
          final_index_by_component[dependency];
      assert(final_dependency < final_component);
      dependency_first[final_component].dependencies.push_back(
          final_dependency);
    }
    std::sort(
        dependency_first[final_component].dependencies.begin(),
        dependency_first[final_component].dependencies.end());
  }
  return dependency_first;
}

class EffectCollector {
public:
  EffectCollector(
      const SemanticPackage &package,
      std::span<const HirProgram *const> programs,
      const ImportedProcedureContracts &imported,
      const TargetProfile *target,
      std::span<const ForeignProviderAudit> provider_audits)
      : package_(package), programs_(programs), imported_(imported),
        target_(target),
        provider_audits_(provider_audits) {}

  // Direct workers and closure borrow one package context prepared before the
  // ready wave. The context owns all package-sized lookup construction; this
  // collector owns only body-local traversal state until closure explicitly
  // copies lookup_rows into its mutable fixed-point table.
  explicit EffectCollector(const ProcedureEffectAnalysis &analysis)
      : package_(*analysis.package), imported_(*analysis.imported),
        target_(analysis.target), provider_audits_(analysis.provider_audits),
        direct_lookup_(&analysis.lookup_rows),
        row_by_symbol_(&analysis.row_by_symbol),
        procedure_paths_(&analysis.procedure_paths),
        pointer_procedure_paths_(&analysis.pointer_procedure_paths),
        source_procedure_count_(analysis.source_procedures.size()) {
    assert(analysis.package != nullptr && analysis.imported != nullptr);
  }

  [[nodiscard]] ProcedureEffectAnalysis prepare_analysis() {
    ProcedureEffectAnalysis analysis;
    analysis.package = &package_;
    analysis.imported = &imported_;
    analysis.target = target_;
    analysis.provider_audits = provider_audits_;
    analysis.source_procedures = selected_source_procedures();
    initialize_direct_rows(analysis.source_procedures);
    analysis.lookup_rows = std::move(direct_);
    const std::size_t absent = analysis.lookup_rows.procedures.size();
    analysis.row_by_symbol.assign(package_.symbols.symbol_count(), absent);
    for (std::size_t row = 0;
         row < analysis.lookup_rows.procedures.size(); ++row) {
      const SymbolId symbol = analysis.lookup_rows.procedures[row].procedure;
      assert(symbol.is_valid() && symbol.value < analysis.row_by_symbol.size());
      assert(analysis.row_by_symbol[symbol.value] == absent);
      analysis.row_by_symbol[symbol.value] = row;
    }
    analysis.procedure_paths.resize(package_.types.size());
    analysis.pointer_procedure_paths.resize(package_.types.size());
    for (std::size_t index = 0; index < package_.types.size(); ++index) {
      const TypeId type{static_cast<std::uint32_t>(index)};
      analysis.procedure_paths[index] =
          compute_procedure_paths(type, false);
      analysis.pointer_procedure_paths[index] =
          compute_procedure_paths(type, true);
    }
    direct_lookup_ = nullptr;
    return analysis;
  }

  [[nodiscard]] DirectProcedureEffectSummary collect_direct_at(
      std::span<const EffectSourceProcedure> source_procedures,
      std::size_t selected_index) {
    assert(direct_lookup_ != nullptr);
    assert(selected_index < source_procedures.size());
    return discover_source_procedure(source_procedures[selected_index]);
  }

  [[nodiscard]] EffectSummaryResult close(
      std::span<const EffectSourceProcedure> source_procedures,
      const DirectEffectSummaryResult &direct,
      ProcedureEffectClosureTimings *timings) {
    EffectPhaseTimer setup_timing(
        timings == nullptr
            ? nullptr
            : &timings->contract_table_setup_nanoseconds);
    assert(direct_lookup_ != nullptr);
    direct_ = *direct_lookup_;
    direct_lookup_ = &direct_;
    assert(direct.procedures.size() == source_procedure_count_);
    for (std::size_t index = 0; index < source_procedure_count_; ++index) {
      assert(
          direct.procedures[index].procedure ==
          direct_.procedures[index].procedure);
      direct_.procedures[index] = direct.procedures[index];
    }
    setup_timing.finish();

    // Returned procedure values and caller-visible pointer writes are closed
    // only after the independent body-local products exist. Concrete target
    // discovery may refine the graph; each refinement rebuilds SCCs before the
    // affected components continue.
    {
      EffectPhaseTimer flow_timing(
          timings == nullptr
              ? nullptr
              : &timings->procedure_flow_nanoseconds);
      close_source_flow(source_procedures, false);
      local_returns_complete_ = true;
      close_source_flow(source_procedures, true);
    }

    closed_.procedures.reserve(direct_.procedures.size());
    for (const DirectProcedureEffectSummary &source : direct_.procedures) {
      ProcedureEffectSummary summary;
      summary.procedure = source.procedure;
      summary.return_values = source.return_values;
      summary.field_writes = source.field_writes;
      summary.effects = source.direct_effects;
      closed_.procedures.push_back(std::move(summary));
    }

    // The direct summary graph is now immutable. Collapse legal recursive
    // call/value-flow cycles explicitly, then close the condensation graph from
    // callees to callers. An acyclic singleton executes once. A recursive SCC
    // repeats only its own rows; each changing pass adds one member of a finite
    // source-derived effect set, so no arbitrary iteration bound is required.
    {
      EffectPhaseTimer component_timing(
          timings == nullptr
              ? nullptr
              : &timings->scc_construction_nanoseconds);
      closed_.components = build_effect_components(direct_.procedures);
    }
    EffectPhaseTimer propagation_timing(
        timings == nullptr
            ? nullptr
            : &timings->effect_propagation_nanoseconds);
    for (const ClosedEffectComponent &component : closed_.components) {
      bool changed = false;
      do {
        changed = false;
        for (std::size_t procedure_index : component.procedure_indices) {
          ProcedureEffectSummary &summary =
              closed_.procedures[procedure_index];
          const DirectProcedureEffectSummary &source =
              direct_.procedures[procedure_index];
          for (const ProcedureInvocationSummary &invocation :
               source.direct_invocations) {
            changed = compose_named_call(
                summary, invocation.callee, invocation.arguments) || changed;
          }
          for (const ProcedureFlowInvocationSummary &invocation :
               source.direct_flow_calls) {
            changed = compose_value_call(
                summary, invocation.callee, invocation.arguments) || changed;
          }
        }
      } while (changed);
    }
    propagation_timing.finish();

    // Recompose each source call once against the closed procedure summaries.
    // The denial walker consumes these exact rows, so a typed field selected at
    // one call site cannot degrade back into an unknown edge during its second
    // lexical-policy check.
    EffectPhaseTimer call_site_timing(
        timings == nullptr
            ? nullptr
            : &timings->call_site_composition_nanoseconds);
    for (const DirectProcedureEffectSummary &summary : direct_.procedures) {
      for (const ProcedureInvocationSummary &invocation :
           summary.direct_invocations) {
        ProcedureEffectSummary site;
        [[maybe_unused]] const bool added = compose_named_call(
            site, invocation.callee, invocation.arguments);
        closed_.call_sites.push_back(
            {summary.procedure, invocation.expression, std::move(site.effects)});
      }
      for (const ProcedureFlowInvocationSummary &invocation :
           summary.direct_flow_calls) {
        ProcedureEffectSummary site;
        [[maybe_unused]] const bool added = compose_value_call(
            site, invocation.callee, invocation.arguments);
        closed_.call_sites.push_back(
            {summary.procedure, invocation.expression, std::move(site.effects)});
      }
    }
    call_site_timing.finish();
    return std::move(closed_);
  }

private:
  [[nodiscard]] std::vector<EffectSourceProcedure>
  selected_source_procedures() const {
    std::vector<EffectSourceProcedure> result;
    for (const HirProgram *program : programs_) {
      for (const HirProcedure &procedure : program->procedures()) {
        result.push_back({program, &procedure});
      }
    }
    return result;
  }

  // Installs the lookup domain shared by independent direct discovery and SCC
  // closure. Source rows begin as bottom placeholders; native and imported rows
  // are immutable terminal contracts. No HIR body is visited here.
  void initialize_direct_rows(
      std::span<const EffectSourceProcedure> source_procedures) {
    assert(direct_.procedures.empty());
    source_procedure_count_ = source_procedures.size();
    for (const EffectSourceProcedure &source : source_procedures) {
      DirectProcedureEffectSummary summary;
      summary.procedure = source.procedure->symbol;
      direct_.procedures.push_back(std::move(summary));
    }

    // Foreign declarations have no HIR body, but a lexical denial may enclose
    // a call to one directly. Give every foreign procedure its own composed
    // row so that both ordinary callers and the denial walker consult exactly
    // the same target/artifact-bound contract. Procedure-valued parameters are
    // seeded as symbolic flow slots; composing the row at a real call site then
    // substitutes the actual callback just like a Draft procedure does.
    for (const NativeBinding &binding : package_.native_bindings) {
      if (binding.kind != NativeBindingKind::ForeignImport) continue;
      ProcedureEffectSummary composed;
      composed.procedure = binding.symbol;
      const Symbol &symbol = package_.symbols.symbol(binding.symbol);
      std::vector<ProcedureArgumentSummary> arguments;
      if (symbol.type.is_valid()) {
        const Type &type = package_.types.type(symbol.type);
        if (type.kind == TypeKind::Procedure && !type.members.empty()) {
          arguments.resize(type.members.size() - 1);
          for (std::size_t index = 0; index < arguments.size(); ++index) {
            const Type &parameter_type = package_.types.type(type.members[index]);
            const bool follow_pointer =
                parameter_type.kind == TypeKind::Pointer ||
                parameter_type.kind == TypeKind::MultiPointer;
            for (const std::vector<std::string> &path :
                 procedure_paths(type.members[index], follow_pointer)) {
              ProcedureValueSummary value;
              value.flow_slots.push_back(
                  {static_cast<std::uint32_t>(index), path, false});
              arguments[index].fields.push_back({path, std::move(value)});
            }
          }
        }
      }
      [[maybe_unused]] const bool added =
          compose_native_call(composed, binding.symbol, binding, arguments);
      DirectProcedureEffectSummary direct;
      direct.procedure = binding.symbol;
      direct.direct_effects = std::move(composed.effects);
      direct_.procedures.push_back(std::move(direct));
    }

    // Final dependency contracts are already closed in their owning packages.
    // Install them as immutable leaf rows so direct source discovery and later
    // SCC closure perform the same exact return/write lookup.
    for (const ImportedProcedureContractStatus &status : imported_.procedures) {
      DirectProcedureEffectSummary direct;
      direct.procedure = status.proxy;
      if (!status.has_effect_summary) {
        direct.direct_effects.push_back({
            EffectKind::UnknownCall,
            status.proxy,
            "imported callee has no audited summary",
            {},
            {},
            {}});
      } else {
        for (const ImportedEffect &effect : imported_.effects) {
          if (effect.procedure_proxy == status.proxy) {
            add_effect(direct.direct_effects, semantic_effect(effect));
          }
        }
      }
      for (const ImportedProcedureReturn &returned : imported_.returns) {
        if (returned.procedure_proxy != status.proxy) continue;
        ProcedureValueSummary value;
        value.unknown = returned.unknown;
        for (const ImportedReturnFlowSlot &slot : returned.flow_slots) {
          value.flow_slots.push_back({slot.parameter, slot.path, slot.context});
        }
        for (const ImportedEffect &effect : returned.contract_effects) {
          add_effect(value.contract_effects, semantic_effect(effect));
        }
        direct.return_values.push_back({returned.path, std::move(value)});
      }
      for (const ImportedProcedureWrite &write : imported_.writes) {
        if (write.procedure_proxy != status.proxy) continue;
        ProcedureValueSummary value;
        value.unknown = write.value_unknown;
        for (const ImportedReturnFlowSlot &slot : write.value_flow_slots) {
          value.flow_slots.push_back({slot.parameter, slot.path, slot.context});
        }
        for (const ImportedEffect &effect : write.value_contract_effects) {
          add_effect(value.contract_effects, semantic_effect(effect));
        }
        direct.field_writes.push_back(
            {write.parameter, write.indirection, write.path, std::move(value)});
      }
      direct_.procedures.push_back(std::move(direct));
    }
    direct_lookup_ = &direct_;
  }

  struct StoragePath {
    SymbolId symbol;
    std::vector<std::string> fields;
    bool context = false;
    // Dereference adds one and address-of removes one. A positive balance
    // rooted at a parameter reaches storage owned by the caller; zero or less
    // names storage local to this procedure even when its address is passed to
    // another procedure.
    int indirection = 0;

    bool operator==(const StoragePath &) const = default;
  };

  // A pointer-valued local may retain an address derived from a formal
  // parameter. Procedure-effect summaries must follow that origin through the
  // explicit local copy; otherwise `copy := parameter; write(copy)` would hide
  // a caller-visible write merely because the source used immutable parameter
  // bindings correctly. Multiple rows deliberately form a may-origin set for
  // assignments in different control-flow paths.
  struct StoredStorageOrigin {
    SymbolId binding;
    StoragePath origin;
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

  // Discovers one source procedure against the currently published direct
  // contracts of its callees. All body-local value and storage state is owned
  // by this invocation and discarded before returning. Repeating this
  // operation is therefore deterministic: only explicit callee contracts can
  // change its result.
  [[nodiscard]] DirectProcedureEffectSummary discover_source_procedure(
      const EffectSourceProcedure &source) {
    assert(source.hir != nullptr && source.procedure != nullptr);
    hir_ = source.hir;
    DirectProcedureEffectSummary discovered;
    discovered.procedure = source.procedure->symbol;
    current_ = &discovered;
    current_procedure_ = source.procedure->symbol;
    current_result_type_ = {};
    const Symbol &symbol = package_.symbols.symbol(source.procedure->symbol);
    if (symbol.type.is_valid()) {
      const Type &type = package_.types.type(symbol.type);
      if (type.kind == TypeKind::Procedure && !type.members.empty()) {
        current_result_type_ = type.members.back();
      }
    }
    procedure_values_.clear();
    storage_origins_.clear();
    seed_parameter_slots(source.procedure->symbol);
    visit_block(source.procedure->body);

    hir_ = nullptr;
    current_ = nullptr;
    current_procedure_ = {};
    current_result_type_ = {};
    procedure_values_.clear();
    storage_origins_.clear();
    return discovered;
  }

  // Closes procedure-return and pointer-write facts one concrete SCC at a
  // time. The condensation order makes every external dependency stable before
  // its callers. If a newly learned returned callback adds an edge, the outer
  // loop rebuilds the graph; the finite source procedure domain guarantees
  // termination. Existing edges may never disappear, which is the central
  // monotonicity invariant of this flow lattice.
  void close_source_flow(
      std::span<const EffectSourceProcedure> source_procedures,
      bool finalize_absent_returns) {
    bool graph_changed = false;
    do {
      // A changed callee contract wakes only consumers which recorded that
      // exact flow dependency. During the discovery pass every dependent row
      // runs once. During finalization, unresolved returned paths seed the
      // wave and changes propagate in dependency-first component order.
      std::vector<bool> flow_changed(source_procedure_count_, false);
      std::vector<std::vector<SymbolId>> previous_edges;
      previous_edges.reserve(source_procedure_count_);
      for (std::size_t index = 0; index < source_procedure_count_; ++index) {
        previous_edges.push_back(direct_.procedures[index].direct_calls);
      }

      const std::vector<ClosedEffectComponent> components =
          build_effect_components(direct_.procedures);
      for (const ClosedEffectComponent &component : components) {
        auto rediscover = [&](std::size_t procedure_index) {
          const DirectProcedureEffectSummary &previous =
              direct_.procedures[procedure_index];
          DirectProcedureEffectSummary discovered =
              discover_source_procedure(source_procedures[procedure_index]);
#ifndef NDEBUG
          for (SymbolId edge : previous.direct_calls) {
            // Losing a finite target would make termination and scheduling
            // depend on replay order. Such a loss is an internal compiler
            // error: source-local flow joins only add possible values.
            assert(
                std::find(
                    discovered.direct_calls.begin(),
                    discovered.direct_calls.end(),
                    edge) != discovered.direct_calls.end());
          }
#endif
          if (previous == discovered) return false;
          direct_.procedures[procedure_index] = std::move(discovered);
          flow_changed[procedure_index] = true;
          return true;
        };

        auto needs_rediscovery = [&](std::size_t procedure_index) {
          const DirectProcedureEffectSummary &procedure =
              direct_.procedures[procedure_index];
          if (procedure.flow_dependencies.empty()) return false;
          if (!finalize_absent_returns ||
              !procedure.unresolved_return_dependencies.empty()) {
            return true;
          }
          for (SymbolId dependency : procedure.flow_dependencies) {
            const std::size_t dependency_row = direct_row(dependency);
            if (dependency_row < flow_changed.size() &&
                flow_changed[dependency_row]) {
              return true;
            }
          }
          return false;
        };

        // A non-recursive singleton consumes only already-closed dependency
        // rows, so one rediscovery is its exact transfer operation. Repeating
        // it merely to observe the same result doubled every acyclic body walk.
        // A newly discovered outgoing edge is handled by the outer graph
        // rebuild below. Only a real recursive component needs an internal
        // fixed point.
        bool recursive = component.procedure_indices.size() > 1;
        if (!recursive && !component.procedure_indices.empty()) {
          const std::size_t procedure_index =
              component.procedure_indices.front();
          const SymbolId procedure =
              direct_.procedures[procedure_index].procedure;
          recursive = std::find(
              direct_.procedures[procedure_index].direct_calls.begin(),
              direct_.procedures[procedure_index].direct_calls.end(),
              procedure) !=
              direct_.procedures[procedure_index].direct_calls.end();
        }
        if (!recursive) {
          for (std::size_t procedure_index : component.procedure_indices) {
            if (procedure_index < source_procedure_count_ &&
                needs_rediscovery(procedure_index)) {
              rediscover(procedure_index);
            }
          }
          continue;
        }

        bool component_is_ready = false;
        for (std::size_t procedure_index : component.procedure_indices) {
          if (procedure_index < source_procedure_count_ &&
              needs_rediscovery(procedure_index)) {
            component_is_ready = true;
            break;
          }
        }
        if (!component_is_ready) continue;

        bool component_changed = false;
        do {
          component_changed = false;
          for (std::size_t procedure_index : component.procedure_indices) {
            if (procedure_index >= source_procedure_count_ ||
                direct_.procedures[procedure_index]
                    .flow_dependencies.empty()) {
              continue;
            }
            component_changed =
                rediscover(procedure_index) || component_changed;
          }
        } while (component_changed);
      }

      graph_changed = false;
      for (std::size_t index = 0; index < source_procedure_count_; ++index) {
        if (previous_edges[index] != direct_.procedures[index].direct_calls) {
          graph_changed = true;
          break;
        }
      }
    } while (graph_changed);
  }

  [[nodiscard]] bool procedure_type(TypeId type) const {
    return type.is_valid() &&
        package_.types.type(type).kind == TypeKind::Procedure;
  }

  [[nodiscard]] std::optional<SymbolId> type_owner(TypeId type) const {
    for (const OwnedSemanticScope &owned : package_.owned_scopes_for_read()) {
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
    for (const AggregateMember &member :
         package_.aggregate_members_for_read()) {
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

  [[nodiscard]] std::vector<std::vector<std::string>> compute_procedure_paths(
      TypeId type, bool follow_pointer) const {
    std::vector<std::vector<std::string>> result;
    std::vector<std::string> path;
    std::vector<TypeId> active;
    collect_procedure_paths(type, path, active, follow_pointer, result);
    return result;
  }

  [[nodiscard]] const std::vector<std::vector<std::string>> &procedure_paths(
      TypeId type, bool follow_pointer = false) const {
    const auto *cache = follow_pointer
        ? pointer_procedure_paths_
        : procedure_paths_;
    if (type.is_valid() && cache != nullptr && type.value < cache->size()) {
      return (*cache)[type.value];
    }
    procedure_path_scratch_ =
        compute_procedure_paths(type, follow_pointer);
    return procedure_path_scratch_;
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
    const HirExpression &expression = hir_->expression(id);
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

  [[nodiscard]] bool data_pointer_type(TypeId type) const {
    if (!type.is_valid()) return false;
    const TypeKind kind = package_.types.type(type).kind;
    return kind == TypeKind::Pointer || kind == TypeKind::MultiPointer;
  }

  [[nodiscard]] std::vector<StoragePath> pointer_value_origins(
      HirExpressionId id) const {
    if (!id.is_valid()) return {};
    const HirExpression &expression = hir_->expression(id);

    // A local pointer symbol denotes the pointer stored in its slot, not the
    // slot itself. Recover every address that may have been assigned to it.
    if (expression.kind == HirExpressionKind::Symbol &&
        expression.symbol.is_valid() &&
        package_.symbols.symbol(expression.symbol).kind == SymbolKind::Local) {
      std::vector<StoragePath> result;
      for (const StoredStorageOrigin &stored : storage_origins_) {
        if (stored.binding == expression.symbol &&
            std::find(result.begin(), result.end(), stored.origin) ==
                result.end()) {
          result.push_back(stored.origin);
        }
      }
      if (!result.empty()) return result;
    }

    if (const std::optional<StoragePath> direct = storage_path(id)) {
      return {*direct};
    }

    // Pointer-preserving casts do not sever effect provenance.
    if (expression.kind == HirExpressionKind::Intrinsic &&
        expression.constant.kind == ConstantKind::String &&
        expression.constant.text == "cast" &&
        expression.operands.size() == 1 &&
        data_pointer_type(expression.type)) {
      return pointer_value_origins(expression.operands.front());
    }
    return {};
  }

  void merge_storage_origins(SymbolId binding, HirExpressionId source) {
    if (!binding.is_valid() ||
        !data_pointer_type(package_.symbols.symbol(binding).type)) {
      return;
    }
    for (const StoragePath &origin : pointer_value_origins(source)) {
      const StoredStorageOrigin candidate{binding, origin};
      const bool present = std::any_of(
          storage_origins_.begin(),
          storage_origins_.end(),
          [&](const StoredStorageOrigin &stored) {
            return stored.binding == candidate.binding &&
                stored.origin == candidate.origin;
          });
      if (!present) storage_origins_.push_back(candidate);
    }
  }

  void resolve_storage_path(
      StoragePath path,
      std::vector<SymbolId> &active,
      std::vector<StoragePath> &result) const {
    if (path.context || path.indirection <= 0 || !path.symbol.is_valid() ||
        package_.symbols.symbol(path.symbol).kind != SymbolKind::Local) {
      if (std::find(result.begin(), result.end(), path) == result.end()) {
        result.push_back(std::move(path));
      }
      return;
    }

    if (std::find(active.begin(), active.end(), path.symbol) != active.end()) {
      // A pure local cycle has no parameter-rooted write to publish. Other
      // non-cyclic rows in the same may-origin set are still retained.
      return;
    }
    active.push_back(path.symbol);
    bool found = false;
    for (const StoredStorageOrigin &stored : storage_origins_) {
      if (stored.binding != path.symbol) continue;
      found = true;
      StoragePath resolved = stored.origin;
      resolved.indirection += path.indirection;
      resolved.fields.insert(
          resolved.fields.end(), path.fields.begin(), path.fields.end());
      resolve_storage_path(std::move(resolved), active, result);
    }
    active.pop_back();

    // An untracked local remains local storage. It cannot produce a formal
    // parameter write summary, but preserving it lets procedure-value storage
    // tracking continue to operate normally.
    if (!found && std::find(result.begin(), result.end(), path) == result.end()) {
      result.push_back(std::move(path));
    }
  }

  [[nodiscard]] std::vector<StoragePath> resolved_storage_paths(
      StoragePath path) const {
    std::vector<StoragePath> result;
    std::vector<SymbolId> active;
    resolve_storage_path(std::move(path), active, result);
    return result;
  }

  [[nodiscard]] std::optional<std::uint32_t> parameter_ordinal(
      SymbolId parameter) const {
    for (const OwnedSemanticScope &owned : package_.owned_scopes_for_read()) {
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
    for (const OwnedSemanticScope &owned : package_.owned_scopes_for_read()) {
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
    for (const ImportedSymbol &imported : package_.imported_symbols_for_read()) {
      if (imported.proxy == symbol) return true;
    }
    return false;
  }

  [[nodiscard]] const ImportedSymbol *imported_symbol(SymbolId symbol) const {
    for (const ImportedSymbol &imported : package_.imported_symbols_for_read()) {
      if (imported.proxy == symbol) return &imported;
    }
    return nullptr;
  }

  [[nodiscard]] bool has_direct_summary(SymbolId symbol) const {
    return direct_summary(symbol) != nullptr;
  }

  // Source rows occupy the stable prefix installed before native and imported
  // terminal contracts. During flow closure, an absent return on one of these
  // rows means "not discovered yet"; absence on a terminal row is already a
  // final unknown contract.
  [[nodiscard]] bool has_source_body(SymbolId symbol) const {
    const std::size_t row = direct_row(symbol);
    return row < source_procedure_count_;
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
    const HirExpression &expression = hir_->expression(id);
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
    for (const ImportedProcedureReturn &returned : imported_.returns) {
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
      const ProcedureValueSummary substituted =
          substitute_return_value(value, arguments);
      for (StoragePath resolved :
           resolved_storage_paths(std::move(*destination))) {
        apply_value_write(std::move(resolved), path, substituted);
      }
    };

    if (const DirectProcedureEffectSummary *summary = direct_summary(callee)) {
      if (has_source_body(callee)) {
        const bool carries_procedure_value = std::any_of(
            arguments.begin(), arguments.end(),
            [](const ProcedureArgumentSummary &argument) {
              return !argument.fields.empty();
            });
        if (carries_procedure_value && current_ != nullptr) {
          add_call(current_->flow_dependencies, callee);
        }
      }
      for (const ProcedureFieldWriteSummary &write : summary->field_writes) {
        apply(
            write.parameter,
            write.indirection,
            write.path,
            write.value);
      }
      return;
    }
    for (const ImportedProcedureWrite &write : imported_.writes) {
      if (write.procedure_proxy != callee) continue;
      if (call.operands.empty() ||
          write.parameter >= call.operands.size() - 1U) {
        continue;
      }
      std::optional<StoragePath> destination =
          storage_path(call.operands[write.parameter + 1U]);
      if (!destination.has_value()) continue;
      destination->indirection += static_cast<int>(write.indirection);
      const ProcedureValueSummary value =
          imported_write_value(write, arguments);
      for (StoragePath resolved :
           resolved_storage_paths(std::move(*destination))) {
        apply_value_write(std::move(resolved), write.path, value);
      }
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
    const HirExpression &callee = hir_->expression(call.operands.front());
    // record_call captured arguments before the call's own write-back became
    // visible, then placed those immutable snapshots in physical parameter
    // order. Reuse them when deriving a returned callback instead of rereading
    // possibly mutated storage here.
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
    if (const DirectProcedureEffectSummary *summary =
            direct_summary(callee.symbol)) {
      if (has_source_body(callee.symbol) && current_ != nullptr) {
        add_call(current_->flow_dependencies, callee.symbol);
      }
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
      if (has_source_body(callee.symbol) && !local_returns_complete_) {
        // The dependency SCC has not yet published this local path. Bottom is
        // intentionally distinct from unknown: a later component pass may
        // still discover an exact finite procedure value.
        if (current_ != nullptr) {
          add_call(
              current_->unresolved_return_dependencies, callee.symbol);
        }
        return result;
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
    const HirExpression &expression = hir_->expression(id);

    if (std::optional<StoragePath> location = storage_path(id)) {
      for (StoragePath resolved :
           resolved_storage_paths(std::move(*location))) {
        resolved.fields.insert(
            resolved.fields.end(), relative_path.begin(), relative_path.end());
        if (const ProcedureValueSummary *stored = stored_value(
                resolved.symbol, resolved.fields, resolved.context)) {
          merge_value(value, *stored);
          continue;
        }
        if (resolved.context) {
          add_flow_slot(value, {
              std::numeric_limits<std::uint32_t>::max(),
              std::move(resolved.fields),
              true});
          continue;
        }
        const Symbol &root = package_.symbols.symbol(resolved.symbol);
        if (root.kind == SymbolKind::Parameter) {
          const std::optional<std::uint32_t> ordinal =
              parameter_ordinal(resolved.symbol);
          if (ordinal.has_value()) {
            add_flow_slot(
                value, {*ordinal, std::move(resolved.fields), false});
            continue;
          }
        }
        value.unknown = true;
      }
      if (value_is_empty(value)) value.unknown = true;
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

    if (expression.kind == HirExpressionKind::Tuple &&
        !relative_path.empty()) {
      const std::optional<BigInteger> parsed =
          BigInteger::parse_literal(relative_path.front());
      const std::optional<std::uint64_t> index =
          parsed.has_value() ? parsed->to_u64() : std::nullopt;
      if (!index.has_value() || *index >= expression.operands.size()) {
        value.unknown = true;
        return value;
      }
      const std::vector<std::string> remainder(
          relative_path.begin() + 1, relative_path.end());
      return procedure_value_at(
          expression.operands[static_cast<std::size_t>(*index)], remainder);
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

  void merge_assignment_value_from_path(
      StoragePath destination,
      TypeId destination_type,
      HirExpressionId source,
      const std::vector<std::string> &source_prefix) {
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
      std::vector<std::string> source_path = source_prefix;
      source_path.insert(
          source_path.end(), relative.begin(), relative.end());
      const ProcedureValueSummary assigned =
          procedure_value_at(source, source_path);
      merge_field_write(destination, complete, assigned);
      merge_stored_value(
          destination.symbol,
          std::move(complete),
          assigned,
          destination.context);
    }
  }

  void merge_assignment_value(
      StoragePath destination,
      TypeId destination_type,
      HirExpressionId source) {
    merge_assignment_value_from_path(
        std::move(destination), destination_type, source, {});
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
    const HirExpression &expression = hir_->expression(id);
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
    const std::size_t count = call.operands.size() - first;
    std::vector<ProcedureArgumentSummary> result(count);
    for (std::size_t index = first; index < call.operands.size(); ++index) {
      const std::size_t source_ordinal = index - first;
      const std::size_t physical = first == 1 &&
              call.call_parameter_indices.size() == count
          ? call.call_parameter_indices[source_ordinal]
          : source_ordinal;
      if (physical < count) {
        result[physical] = call_argument(call.operands[index]);
      }
    }
    return result;
  }

  void record_call(
      HirExpressionId call_id,
      const HirExpression &call,
      ProcedureValueSummary callee_value,
      std::vector<ProcedureArgumentSummary> arguments) {
    if (call.operands.empty()) return;
    const HirExpression &callee = hir_->expression(call.operands.front());
    if (callee.kind == HirExpressionKind::Symbol &&
        callee.symbol.is_valid() &&
        package_.symbols.symbol(callee.symbol).kind == SymbolKind::Procedure) {
      if (has_direct_summary(callee.symbol)) {
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
      if (has_direct_summary(target)) add_call(current_->direct_calls, target);
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
      if (has_direct_summary(target)) add_call(current_->direct_calls, target);
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
      const ImportedProcedureContractStatus *imported,
      const std::vector<ProcedureArgumentSummary> &arguments) {
    if (imported == nullptr || !imported->has_effect_summary) {
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
    for (const ImportedEffect &effect : imported_.effects) {
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
    if (const ProcedureEffectSummary *summary = closed_summary(callee)) {
      // Copy the row before adding to destination: recursive calls may make
      // summary and destination the same vector.
      const std::vector<SemanticEffect> effects = summary->effects;
      for (const SemanticEffect &effect : effects) {
        changed = compose_effect(destination, effect, arguments) || changed;
      }
    } else if (imported_symbol(callee) != nullptr) {
      changed = compose_imported_call(
          destination, callee, imported_.find(callee), arguments) || changed;
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
    const HirExpression &expression = hir_->expression(id);

    // Calls evaluate the callee and then each argument in source order. Capture
    // each procedure value immediately after its own evaluation: a later
    // argument may call an initializer that mutates callback-bearing storage,
    // but that cannot retroactively change an earlier argument or callee.
    if (expression.kind == HirExpressionKind::Call) {
      if (expression.operands.empty()) return;
      visit_expression(expression.operands.front());
      ProcedureValueSummary callee =
          procedure_value(expression.operands.front());
      const std::size_t argument_count = expression.operands.size() - 1;
      std::vector<ProcedureArgumentSummary> arguments(argument_count);
      for (std::size_t index = 1; index < expression.operands.size(); ++index) {
        visit_expression(expression.operands[index]);
        const std::size_t source_ordinal = index - 1;
        const std::size_t physical =
            expression.call_parameter_indices.size() == argument_count
            ? expression.call_parameter_indices[source_ordinal]
            : source_ordinal;
        if (physical < argument_count) {
          arguments[physical] = call_argument(expression.operands[index]);
        }
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
    } else if (expression.kind == HirExpressionKind::Intrinsic &&
               expression.constant.kind == ConstantKind::String &&
               expression.constant.text == "raw_data") {
      // Raw string-data extraction is a semantic representation escape, not an
      // unchecked memory access. Record it independently so callers can deny
      // this capability without rejecting every multi-pointer operation. The
      // ordinary fixed point and interface translation carry this row through
      // helpers and package boundaries.
      add_effect(
          current_->direct_effects,
          {EffectKind::RawStringData, {}, "raw_data", {}, {}, {}});
    } else if (expression.kind == HirExpressionKind::Assembly) {
      add_effect(
          current_->direct_effects,
          {EffectKind::Assembly, {}, "asm", {}, {}, {}});
    } else if (expression.kind == HirExpressionKind::Index &&
               !expression.operands.empty()) {
      const TypeId base_type = hir_->expression(expression.operands.front()).type;
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
    const HirStatement &statement = hir_->statement(id);
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
      merge_storage_origins(binding, statement.expressions.front());
    } else if (statement.kind == HirStatementKind::Assignment &&
               statement.operation == HirOperation::Assign) {
      const std::size_t left_count = std::min(
          statement.assignment_target_count, statement.expressions.size());
      if (statement.assignment_destructures_tuple &&
          statement.expressions.size() > left_count &&
          statement.assignment_member_indices.size() == left_count) {
        const HirExpressionId source = statement.expressions[left_count];
        for (std::size_t index = 0; index < left_count; ++index) {
          const HirExpression &left =
              hir_->expression(statement.expressions[index]);
          std::optional<StoragePath> destination =
              storage_path(statement.expressions[index]);
          if (!destination.has_value()) continue;
          for (StoragePath resolved :
               resolved_storage_paths(std::move(*destination))) {
            merge_assignment_value_from_path(
                std::move(resolved),
                left.type,
                source,
                {std::to_string(statement.assignment_member_indices[index])});
          }
        }
        return;
      }
      const std::size_t right_count = statement.expressions.size() - left_count;
      const std::size_t paired = std::min(left_count, right_count);
      for (std::size_t index = 0; index < paired; ++index) {
        const HirExpression &left = hir_->expression(statement.expressions[index]);
        std::optional<StoragePath> destination =
            storage_path(statement.expressions[index]);
        if (!destination.has_value()) continue;
        for (StoragePath resolved :
             resolved_storage_paths(std::move(*destination))) {
          merge_assignment_value(
              std::move(resolved),
              left.type,
              statement.expressions[left_count + index]);
        }
        if (left.kind == HirExpressionKind::Symbol &&
            left.symbol.is_valid() &&
            package_.symbols.symbol(left.symbol).kind == SymbolKind::Local) {
          merge_storage_origins(
              left.symbol,
              statement.expressions[left_count + index]);
        }
      }
    }
    for (HirStatementId header : statement.header_statements) {
      visit_statement(header);
    }
    for (HirBlockId block : statement.blocks) visit_block(block);
  }

  void visit_block(HirBlockId id) {
    if (!id.is_valid()) return;
    for (HirStatementId statement : hir_->block(id).statements) {
      visit_statement(statement);
    }
  }

  // Direct workers read the prepared package rows while closure redirects the
  // same lookup operation to its one mutable fixed-point copy.
  [[nodiscard]] const DirectEffectSummaryResult &direct_rows() const {
    assert(direct_lookup_ != nullptr);
    return *direct_lookup_;
  }

  [[nodiscard]] std::size_t direct_row(SymbolId symbol) const {
    const std::size_t absent = direct_rows().procedures.size();
    if (!symbol.is_valid() || row_by_symbol_ == nullptr ||
        symbol.value >= row_by_symbol_->size()) {
      return absent;
    }
    return (*row_by_symbol_)[symbol.value];
  }

  [[nodiscard]] const DirectProcedureEffectSummary *direct_summary(
      SymbolId symbol) const {
    const std::size_t row = direct_row(symbol);
    return row < direct_rows().procedures.size()
        ? &direct_rows().procedures[row]
        : nullptr;
  }

  [[nodiscard]] const ProcedureEffectSummary *closed_summary(
      SymbolId symbol) const {
    const std::size_t row = direct_row(symbol);
    return row < closed_.procedures.size()
        ? &closed_.procedures[row]
        : nullptr;
  }

  const SemanticPackage &package_;
  std::span<const HirProgram *const> programs_;
  const ImportedProcedureContracts &imported_;
  const HirProgram *hir_ = nullptr;
  const TargetProfile *target_ = nullptr;
  std::span<const ForeignProviderAudit> provider_audits_;
  DirectEffectSummaryResult direct_;
  const DirectEffectSummaryResult *direct_lookup_ = nullptr;
  const std::vector<std::size_t> *row_by_symbol_ = nullptr;
  const std::vector<std::vector<std::vector<std::string>>> *procedure_paths_ =
      nullptr;
  const std::vector<std::vector<std::vector<std::string>>> *
      pointer_procedure_paths_ = nullptr;
  mutable std::vector<std::vector<std::string>> procedure_path_scratch_;
  EffectSummaryResult closed_;
  // Source rows are a prefix of direct_.procedures. local_returns_complete_
  // changes only after concrete target/edge discovery reaches its fixed point;
  // it turns any remaining absent local return path into a final unknown fact.
  std::size_t source_procedure_count_ = 0;
  bool local_returns_complete_ = false;
  DirectProcedureEffectSummary *current_ = nullptr;
  SymbolId current_procedure_;
  TypeId current_result_type_;
  std::vector<StoredProcedureValue> procedure_values_;
  std::vector<StoredStorageOrigin> storage_origins_;
  std::vector<CompositionFrame> composition_stack_;
};

} // namespace

const ImportedProcedureContractStatus *ImportedProcedureContracts::find(
    SymbolId procedure) const {
  for (const ImportedProcedureContractStatus &status : procedures) {
    if (status.proxy == procedure) return &status;
  }
  return nullptr;
}

ImportedProcedureContracts imported_procedure_contracts(
    const SemanticPackage &package) {
  ImportedProcedureContracts result;
  for (const ImportedSymbol &imported : package.imported_symbols_for_read()) {
    if (!imported.proxy.is_valid() ||
        package.symbols.symbol(imported.proxy).kind != SymbolKind::Procedure) {
      continue;
    }
    result.procedures.push_back(
        {imported.proxy, imported.has_effect_summary});
  }
  for (const ImportedEffect &effect : package.imported_effects_for_read()) {
    result.effects.push_back(effect);
  }
  for (const ImportedProcedureReturn &returned :
       package.imported_returns_for_read()) {
    result.returns.push_back(returned);
  }
  for (const ImportedProcedureWrite &write :
       package.imported_writes_for_read()) {
    result.writes.push_back(write);
  }
  return result;
}

const DirectProcedureEffectSummary *DirectEffectSummaryResult::find(
    SymbolId procedure) const {
  for (const DirectProcedureEffectSummary &summary : procedures) {
    if (summary.procedure == procedure) return &summary;
  }
  return nullptr;
}

const ProcedureEffectSummary *EffectSummaryResult::find(
    SymbolId procedure) const {
  for (const ProcedureEffectSummary &summary : procedures) {
    if (summary.procedure == procedure) return &summary;
  }
  return nullptr;
}

const CallSiteEffectSummary *EffectSummaryResult::find_call_site(
    SymbolId procedure, HirExpressionId expression) const {
  for (const CallSiteEffectSummary &summary : call_sites) {
    if (summary.procedure == procedure && summary.expression == expression) {
      return &summary;
    }
  }
  return nullptr;
}

ProcedureEffectAnalysis prepare_procedure_effect_analysis(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target,
    std::span<const ForeignProviderAudit> provider_audits) {
  std::vector<const HirProgram *> programs;
  programs.reserve(selected_indices.size());
  for (std::size_t position = 0; position < selected_indices.size();
       ++position) {
    const std::size_t index = selected_indices[position];
    assert(index < procedures.size());
    assert(position == 0 || selected_indices[position - 1] < index);
    programs.push_back(&procedures[index].program);
  }
  EffectCollector collector(
      package, programs, imported, target, provider_audits);
  return collector.prepare_analysis();
}

DirectProcedureEffectSummary collect_direct_procedure_effect(
    const ProcedureEffectAnalysis &analysis,
    std::size_t selected_position) {
  EffectCollector collector(analysis);
  return collector.collect_direct_at(
      analysis.source_procedures, selected_position);
}

EffectSummaryResult close_procedure_effects(
    const ProcedureEffectAnalysis &analysis,
    const DirectEffectSummaryResult &direct,
    ProcedureEffectClosureTimings *timings) {
  EffectCollector collector(analysis);
  return collector.close(analysis.source_procedures, direct, timings);
}

DirectEffectSummaryResult collect_direct_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target,
    std::span<const ForeignProviderAudit> provider_audits) {
  const ProcedureEffectAnalysis analysis = prepare_procedure_effect_analysis(
      package,
      procedures,
      selected_indices,
      imported,
      target,
      provider_audits);
  DirectEffectSummaryResult result;
  result.procedures.reserve(analysis.source_procedures.size());
  for (std::size_t position = 0;
       position < analysis.source_procedures.size(); ++position) {
    result.procedures.push_back(
        collect_direct_procedure_effect(analysis, position));
  }
  return result;
}

DirectEffectSummaryResult collect_direct_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target,
    std::span<const ForeignProviderAudit> provider_audits) {
  std::vector<std::size_t> selected_indices(procedures.size());
  for (std::size_t index = 0; index < selected_indices.size(); ++index) {
    selected_indices[index] = index;
  }
  return collect_direct_procedure_effects(
      package,
      procedures,
      selected_indices,
      imported,
      target,
      provider_audits);
}

EffectSummaryResult close_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const DirectEffectSummaryResult &direct,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target,
    std::span<const ForeignProviderAudit> provider_audits) {
  const ProcedureEffectAnalysis analysis = prepare_procedure_effect_analysis(
      package,
      procedures,
      selected_indices,
      imported,
      target,
      provider_audits);
  return close_procedure_effects(analysis, direct);
}

EffectSummaryResult close_procedure_effects(
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    const DirectEffectSummaryResult &direct,
    const ImportedProcedureContracts &imported,
    const TargetProfile *target,
    std::span<const ForeignProviderAudit> provider_audits) {
  std::vector<std::size_t> selected_indices(procedures.size());
  for (std::size_t index = 0; index < selected_indices.size(); ++index) {
    selected_indices[index] = index;
  }
  return close_procedure_effects(
      package,
      procedures,
      selected_indices,
      direct,
      imported,
      target,
      provider_audits);
}

std::string_view effect_kind_name(EffectKind kind) {
  switch (kind) {
  case EffectKind::Declaration: return "declaration";
  case EffectKind::PackageGlobal: return "package global";
  case EffectKind::RuntimeAssert: return "assert";
  case EffectKind::RawStringData: return "raw_data";
  case EffectKind::ContextField: return "context field";
  case EffectKind::Assembly: return "assembly";
  case EffectKind::Unchecked: return "unchecked";
  case EffectKind::FlowCall: return "procedure flow slot";
  case EffectKind::UnknownCall: return "unknown call";
  }
  return "unknown effect";
}

} // namespace draft
