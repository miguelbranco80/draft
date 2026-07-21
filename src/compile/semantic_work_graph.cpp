// Deterministic state transitions for the dynamic semantic product graph.
//
// See semantic_work_graph.h for the phase, ownership, and concurrency contract.
// This implementation performs no compiler work itself. It validates and
// canonicalizes dependency rows, freezes complete ready sets, propagates failed
// prerequisites, detects uncollapsed cycles, and publishes already-joined
// task-local outcomes. All traversal order is ascending SemanticProductId.

#include "compile/semantic_work_graph.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool id_in_graph(const SemanticProductGraph &graph,
                               SemanticProductId id) {
  return id.is_valid() &&
         static_cast<std::size_t>(id.value) < graph.products.size();
}

[[nodiscard]] bool product_id_less(SemanticProductId left,
                                   SemanticProductId right) {
  return left.value < right.value;
}

// Canonicalizes a dependency packet without mutating graph state. self is
// absent while a new row is being constructed and present while a Running task
// reports newly discovered blockers.
[[nodiscard]] bool canonical_dependencies(
    const SemanticProductGraph &graph,
    std::span<const SemanticProductId> dependencies, SemanticProductId self,
    std::vector<SemanticProductId> &result, std::string &reason) {
  result.assign(dependencies.begin(), dependencies.end());
  std::sort(result.begin(), result.end(), product_id_less);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  for (SemanticProductId dependency : result) {
    if (!id_in_graph(graph, dependency)) {
      reason = "semantic product depends on an out-of-range product";
      return false;
    }
    if (self.is_valid() && dependency == self) {
      reason = "semantic product depends on itself";
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool is_failed(SemanticProductState state) {
  return state == SemanticProductState::Error ||
         state == SemanticProductState::DependencyFailed;
}

// Failed dependencies may appear after their consumers in product-ID order.
// Repeated ascending scans therefore propagate failure to a fixed point. Every
// changing scan terminalizes at least one row, which bounds the loop by the
// finite product count without an arbitrary retry limit.
void propagate_dependency_failures(SemanticProductGraph &graph) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t product_index = 0; product_index < graph.products.size();
         ++product_index) {
      SemanticProduct &product = graph.products[product_index];
      if (product.state != SemanticProductState::Waiting)
        continue;
      for (SemanticProductId dependency : product.dependencies) {
        const SemanticProduct &required = graph.products[dependency.value];
        if (!is_failed(required.state))
          continue;
        product.state = SemanticProductState::DependencyFailed;
        product.failure = "dependency semantic product " +
                          std::to_string(dependency.value) + " failed";
        changed = true;
        break;
      }
    }
  }
}

[[nodiscard]] bool all_dependencies_complete(const SemanticProductGraph &graph,
                                             const SemanticProduct &product) {
  return std::all_of(product.dependencies.begin(), product.dependencies.end(),
                     [&](SemanticProductId dependency) {
                       return graph.products[dependency.value].state ==
                              SemanticProductState::Complete;
                     });
}

// Detects a cycle entirely within Waiting rows. Edges to Complete,
// WaitingForSynthesis, or terminal failure rows cannot participate. color is
// zero for unseen, one for the active path, and two for a completed search.
[[nodiscard]] bool waiting_graph_has_cycle(const SemanticProductGraph &graph) {
  std::vector<std::uint8_t> color(graph.products.size(), 0);
  struct Frame {
    SemanticProductId product;
    std::size_t next_dependency = 0;
  };

  for (std::size_t root_index = 0; root_index < graph.products.size();
       ++root_index) {
    if (graph.products[root_index].state != SemanticProductState::Waiting ||
        color[root_index] != 0) {
      continue;
    }
    std::vector<Frame> stack;
    stack.push_back(
        {SemanticProductId{static_cast<std::uint32_t>(root_index)}});
    color[root_index] = 1;
    while (!stack.empty()) {
      Frame &frame = stack.back();
      const SemanticProduct &product = graph.products[frame.product.value];
      if (frame.next_dependency == product.dependencies.size()) {
        color[frame.product.value] = 2;
        stack.pop_back();
        continue;
      }
      const SemanticProductId dependency =
          product.dependencies[frame.next_dependency++];
      if (graph.products[dependency.value].state !=
          SemanticProductState::Waiting) {
        continue;
      }
      if (color[dependency.value] == 1)
        return true;
      if (color[dependency.value] == 2)
        continue;
      color[dependency.value] = 1;
      stack.push_back({dependency});
    }
  }
  return false;
}

[[nodiscard]] bool outcome_has_error(const SemanticProductOutcome &outcome) {
  return outcome.diagnostics.has_errors();
}

} // namespace

SemanticProductId
append_semantic_product(SemanticProductGraph &graph, SemanticProductKind kind,
                        std::span<const SemanticProductId> dependencies,
                        std::string &reason) {
  reason.clear();
  if (graph.products.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    reason = "semantic product graph exceeds its ID domain";
    return {};
  }
  std::vector<SemanticProductId> canonical;
  if (!canonical_dependencies(graph, dependencies, {}, canonical, reason)) {
    return {};
  }
  const SemanticProductId id{static_cast<std::uint32_t>(graph.products.size())};
  SemanticProduct product;
  product.kind = kind;
  product.dependencies = std::move(canonical);
  graph.products.push_back(std::move(product));
  return id;
}

SemanticProductId append_completed_semantic_product(
    SemanticProductGraph &graph, SemanticProductKind kind,
    std::span<const SemanticProductId> dependencies, std::string &reason) {
  const SemanticProductId id =
      append_semantic_product(graph, kind, dependencies, reason);
  if (!id.is_valid())
    return {};
  for (SemanticProductId dependency : graph.products[id.value].dependencies) {
    if (graph.products[dependency.value].state !=
        SemanticProductState::Complete) {
      graph.products.pop_back();
      reason = "completed semantic product has an incomplete dependency";
      return {};
    }
  }
  graph.products[id.value].state = SemanticProductState::Complete;
  return id;
}

bool supersede_semantic_products(SemanticProductGraph &graph,
                                 std::span<const SemanticProductId> products,
                                 std::string &reason) {
  reason.clear();
  for (SemanticProductId product : products) {
    if (!id_in_graph(graph, product)) {
      reason = "cannot supersede an out-of-range semantic product";
      return false;
    }
    if (graph.products[product.value].state == SemanticProductState::Running) {
      reason = "cannot supersede a running semantic product";
      return false;
    }
  }
  for (SemanticProductId product : products) {
    SemanticProduct &row = graph.products[product.value];
    row.state = SemanticProductState::Superseded;
    row.failure.clear();
  }
  return true;
}

SemanticReadyWave freeze_semantic_ready_wave(SemanticProductGraph &graph) {
  SemanticReadyWave wave;
  for (const SemanticProduct &product : graph.products) {
    if (product.state == SemanticProductState::Running) {
      wave.failure =
          "cannot freeze a semantic wave while another wave is running";
      return wave;
    }
  }

  propagate_dependency_failures(graph);
  for (std::size_t product_index = 0; product_index < graph.products.size();
       ++product_index) {
    SemanticProduct &product = graph.products[product_index];
    if (product.state == SemanticProductState::Waiting &&
        all_dependencies_complete(graph, product)) {
      product.state = SemanticProductState::Running;
      wave.products.push_back(
          SemanticProductId{static_cast<std::uint32_t>(product_index)});
    }
  }
  if (!wave.products.empty()) {
    wave.status = SemanticReadyWaveStatus::Ready;
    return wave;
  }

  bool has_waiting = false;
  bool has_synthesis = false;
  bool has_failure = false;
  for (const SemanticProduct &product : graph.products) {
    has_waiting = has_waiting || product.state == SemanticProductState::Waiting;
    has_synthesis = has_synthesis ||
                    product.state == SemanticProductState::WaitingForSynthesis;
    has_failure = has_failure || is_failed(product.state);
  }
  if (has_waiting && waiting_graph_has_cycle(graph)) {
    wave.status = SemanticReadyWaveStatus::Stalled;
    wave.failure =
        "semantic product graph contains an uncollapsed dependency cycle";
    return wave;
  }
  if (has_failure) {
    wave.status = SemanticReadyWaveStatus::Failed;
    return wave;
  }
  if (has_synthesis) {
    wave.status = SemanticReadyWaveStatus::WaitingForSynthesis;
    return wave;
  }
  if (has_waiting) {
    wave.status = SemanticReadyWaveStatus::Stalled;
    wave.failure = "semantic product graph has no ready product";
    return wave;
  }
  wave.status = SemanticReadyWaveStatus::Complete;
  return wave;
}

bool publish_semantic_ready_wave(SemanticProductGraph &graph,
                                 const SemanticReadyWave &wave,
                                 std::span<SemanticProductOutcome> outcomes,
                                 DiagnosticSink &diagnostics,
                                 std::string &reason) {
  reason.clear();
  if (wave.status != SemanticReadyWaveStatus::Ready || wave.products.empty()) {
    reason = "only a non-empty ready semantic wave can be published";
    return false;
  }
  if (wave.products.size() != outcomes.size()) {
    reason = "semantic wave outcome count does not match its ready set";
    return false;
  }

  std::size_t running_count = 0;
  for (const SemanticProduct &product : graph.products) {
    if (product.state == SemanticProductState::Running)
      ++running_count;
  }
  if (running_count != wave.products.size()) {
    reason = "semantic wave does not contain every running product";
    return false;
  }

  std::vector<std::vector<SemanticProductId>> canonical_blockers(
      outcomes.size());
  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    const SemanticProductId product_id = wave.products[index];
    if (!id_in_graph(graph, product_id) ||
        graph.products[product_id.value].state !=
            SemanticProductState::Running) {
      reason = "semantic wave does not name its current running product";
      return false;
    }
    if (index != 0 && wave.products[index - 1].value >= product_id.value) {
      reason = "semantic wave products are not in canonical order";
      return false;
    }
    SemanticProductOutcome &outcome = outcomes[index];
    switch (outcome.kind) {
    case SemanticProductOutcomeKind::Complete:
      if (!outcome.dependencies.empty()) {
        reason = "completed semantic product reported new dependencies";
        return false;
      }
      break;
    case SemanticProductOutcomeKind::Error:
      if (outcome.failure.empty() && !outcome_has_error(outcome)) {
        reason = "failed semantic product has no failure or error diagnostic";
        return false;
      }
      if (!outcome.dependencies.empty()) {
        reason = "failed semantic product reported new dependencies";
        return false;
      }
      break;
    case SemanticProductOutcomeKind::Blocked:
      if (outcome.dependencies.empty()) {
        reason = "blocked semantic product reported no dependency";
        return false;
      }
      if (!canonical_dependencies(graph, outcome.dependencies, product_id,
                                  canonical_blockers[index], reason)) {
        return false;
      }
      break;
    case SemanticProductOutcomeKind::WaitingForSynthesis:
      if (!outcome.dependencies.empty() || !outcome.failure.empty() ||
          outcome_has_error(outcome)) {
        reason = "synthesis-waiting semantic product has an invalid outcome";
        return false;
      }
      break;
    }
  }

  // Validation above makes publication atomic with respect to API misuse. The
  // coordinator now merges diagnostics and transitions rows in ascending ID
  // order, which is the stable observable order even if workers completed in a
  // different sequence.
  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    const SemanticProductId product_id = wave.products[index];
    SemanticProduct &product = graph.products[product_id.value];
    SemanticProductOutcome &outcome = outcomes[index];
    for (const Diagnostic &diagnostic : outcome.diagnostics.diagnostics()) {
      diagnostics.report(diagnostic.severity, diagnostic.range,
                         diagnostic.message);
    }
    switch (outcome.kind) {
    case SemanticProductOutcomeKind::Complete:
      product.state = SemanticProductState::Complete;
      product.failure.clear();
      break;
    case SemanticProductOutcomeKind::Error:
      product.state = SemanticProductState::Error;
      product.failure = outcome.failure.empty()
                            ? "semantic product reported an error"
                            : std::move(outcome.failure);
      break;
    case SemanticProductOutcomeKind::Blocked: {
      std::vector<SemanticProductId> merged = product.dependencies;
      merged.insert(merged.end(), canonical_blockers[index].begin(),
                    canonical_blockers[index].end());
      std::sort(merged.begin(), merged.end(), product_id_less);
      merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
      product.dependencies = std::move(merged);
      product.state = SemanticProductState::Waiting;
      product.failure.clear();
      break;
    }
    case SemanticProductOutcomeKind::WaitingForSynthesis:
      product.state = SemanticProductState::WaitingForSynthesis;
      product.failure.clear();
      break;
    }
  }
  return true;
}

std::string_view semantic_product_kind_name(SemanticProductKind kind) {
  switch (kind) {
  case SemanticProductKind::TargetProfile:
    return "target profile";
  case SemanticProductKind::SourceGeneration:
    return "source generation";
  case SemanticProductKind::ParsedFile:
    return "parsed file";
  case SemanticProductKind::PackageImports:
    return "package imports";
  case SemanticProductKind::PackageNameSet:
    return "package name set";
  case SemanticProductKind::PackageInterface:
    return "package interface";
  case SemanticProductKind::OpaqueSynthesisSet:
    return "opaque synthesis set";
  case SemanticProductKind::ConstantValue:
    return "constant value";
  case SemanticProductKind::TypeIdentity:
    return "type identity";
  case SemanticProductKind::TypeMembers:
    return "type members";
  case SemanticProductKind::TypeMemberTypes:
    return "type member types";
  case SemanticProductKind::TypeNaturalLayout:
    return "type natural layout";
  case SemanticProductKind::TypeAbiClassification:
    return "type ABI classification";
  case SemanticProductKind::ProcedureTemplateBody:
    return "procedure template body";
  case SemanticProductKind::ProcedureInstanceBody:
    return "procedure instance body";
  case SemanticProductKind::DirectEffectSummary:
    return "direct effect summary";
  case SemanticProductKind::ClosedEffectScc:
    return "closed effect SCC";
  case SemanticProductKind::DenialResult:
    return "denial result";
  case SemanticProductKind::MirProcedure:
    return "MIR procedure";
  case SemanticProductKind::PackageAssembly:
    return "package assembly";
  case SemanticProductKind::PackageLlvmModule:
    return "package LLVM module";
  case SemanticProductKind::ArtifactLayout:
    return "artifact layout";
  }
  return "unknown semantic product";
}

std::string_view semantic_product_state_name(SemanticProductState state) {
  switch (state) {
  case SemanticProductState::Waiting:
    return "waiting";
  case SemanticProductState::Running:
    return "running";
  case SemanticProductState::Complete:
    return "complete";
  case SemanticProductState::Error:
    return "error";
  case SemanticProductState::DependencyFailed:
    return "dependency failed";
  case SemanticProductState::WaitingForSynthesis:
    return "waiting for synthesis";
  case SemanticProductState::Superseded:
    return "superseded";
  }
  return "unknown semantic product state";
}

} // namespace draft
