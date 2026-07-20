// Focused contracts for the dynamic semantic product coordinator.
//
// These tests exercise only scheduling representation and state transitions:
// canonical ready waves, dependency discovery between waves, synthesis waits,
// deterministic diagnostic publication, failure propagation, and uncollapsed
// cycle detection. Compiler integration tests separately prove that concrete
// package/type/body products use this coordinator rather than legacy loops.

#include "compile/semantic_work_graph.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestState {
  int failures = 0;
};

#define EXPECT(state, condition)                                               \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << __FILE__ << ':' << __LINE__                                 \
                << ": expectation failed: " #condition << '\n';                \
      ++(state).failures;                                                      \
    }                                                                          \
  } while (false)

draft::SemanticProductId
append(TestState &state, draft::SemanticProductGraph &graph,
       draft::SemanticProductKind kind,
       std::vector<draft::SemanticProductId> dependencies = {}) {
  std::string reason;
  const draft::SemanticProductId result =
      draft::append_semantic_product(graph, kind, dependencies, reason);
  EXPECT(state, result.is_valid());
  EXPECT(state, reason.empty());
  return result;
}

bool publish(TestState &state, draft::SemanticProductGraph &graph,
             const draft::SemanticReadyWave &wave,
             std::vector<draft::SemanticProductOutcome> outcomes,
             draft::DiagnosticSink &diagnostics) {
  std::string reason;
  const bool ok = draft::publish_semantic_ready_wave(graph, wave, outcomes,
                                                     diagnostics, reason);
  EXPECT(state, ok);
  EXPECT(state, reason.empty());
  return ok;
}

void test_dependency_waves_are_canonical(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto target =
      append(state, graph, draft::SemanticProductKind::TargetProfile);
  const auto source =
      append(state, graph, draft::SemanticProductKind::SourceGeneration);
  const auto parsed =
      append(state, graph, draft::SemanticProductKind::ParsedFile,
             {source, target, source});
  const auto names = append(
      state, graph, draft::SemanticProductKind::PackageNameSet, {parsed});

  EXPECT(state, graph.products[parsed.value].dependencies.size() == 2);
  EXPECT(state, graph.products[parsed.value].dependencies[0] == target);
  EXPECT(state, graph.products[parsed.value].dependencies[1] == source);

  draft::DiagnosticSink diagnostics;
  draft::SemanticReadyWave wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.status == draft::SemanticReadyWaveStatus::Ready);
  EXPECT(state, wave.products ==
                    std::vector<draft::SemanticProductId>({target, source}));
  publish(state, graph, wave, std::vector<draft::SemanticProductOutcome>(2),
          diagnostics);

  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state,
         wave.products == std::vector<draft::SemanticProductId>({parsed}));
  publish(state, graph, wave, std::vector<draft::SemanticProductOutcome>(1),
          diagnostics);

  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state,
         wave.products == std::vector<draft::SemanticProductId>({names}));
  publish(state, graph, wave, std::vector<draft::SemanticProductOutcome>(1),
          diagnostics);

  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.status == draft::SemanticReadyWaveStatus::Complete);
  EXPECT(state, diagnostics.diagnostics().empty());
}

void test_discovered_dependency_requeues_product(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto body =
      append(state, graph, draft::SemanticProductKind::ProcedureInstanceBody);
  draft::SemanticReadyWave wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.products == std::vector<draft::SemanticProductId>({body}));

  // Workers have joined. The coordinator canonicalizes their discovered type
  // request into a new product before publishing the body outcome that names
  // it as a blocker.
  const auto layout =
      append(state, graph, draft::SemanticProductKind::TypeNaturalLayout);
  draft::SemanticProductOutcome blocked;
  blocked.kind = draft::SemanticProductOutcomeKind::Blocked;
  blocked.dependencies = {layout};
  draft::DiagnosticSink diagnostics;
  publish(state, graph, wave, {std::move(blocked)}, diagnostics);
  EXPECT(state, graph.products[body.value].state ==
                    draft::SemanticProductState::Waiting);

  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state,
         wave.products == std::vector<draft::SemanticProductId>({layout}));
  publish(state, graph, wave, std::vector<draft::SemanticProductOutcome>(1),
          diagnostics);
  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.products == std::vector<draft::SemanticProductId>({body}));
  publish(state, graph, wave, std::vector<draft::SemanticProductOutcome>(1),
          diagnostics);
  EXPECT(state, draft::freeze_semantic_ready_wave(graph).status ==
                    draft::SemanticReadyWaveStatus::Complete);
}

void test_synthesis_wait_suspends_consumers(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto opaque =
      append(state, graph, draft::SemanticProductKind::OpaqueSynthesisSet);
  const auto names = append(
      state, graph, draft::SemanticProductKind::PackageNameSet, {opaque});
  draft::SemanticReadyWave wave = draft::freeze_semantic_ready_wave(graph);
  draft::SemanticProductOutcome waiting;
  waiting.kind = draft::SemanticProductOutcomeKind::WaitingForSynthesis;
  draft::DiagnosticSink diagnostics;
  publish(state, graph, wave, {std::move(waiting)}, diagnostics);

  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state,
         wave.status == draft::SemanticReadyWaveStatus::WaitingForSynthesis);
  EXPECT(state, graph.products[opaque.value].state ==
                    draft::SemanticProductState::WaitingForSynthesis);
  EXPECT(state, graph.products[names.value].state ==
                    draft::SemanticProductState::Waiting);
}

void test_diagnostics_publish_in_product_order(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto first =
      append(state, graph, draft::SemanticProductKind::ProcedureTemplateBody);
  const auto second =
      append(state, graph, draft::SemanticProductKind::ProcedureInstanceBody);
  const auto consumer = append(
      state, graph, draft::SemanticProductKind::DirectEffectSummary, {first});
  draft::SemanticReadyWave wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.products ==
                    std::vector<draft::SemanticProductId>({first, second}));

  std::vector<draft::SemanticProductOutcome> outcomes(2);
  outcomes[0].kind = draft::SemanticProductOutcomeKind::Error;
  outcomes[0].diagnostics.error(draft::SourceRange::invalid(),
                                "first product failed");
  outcomes[1].kind = draft::SemanticProductOutcomeKind::Error;
  outcomes[1].diagnostics.error(draft::SourceRange::invalid(),
                                "second product failed");
  draft::DiagnosticSink diagnostics;
  publish(state, graph, wave, std::move(outcomes), diagnostics);
  EXPECT(state, diagnostics.diagnostics().size() == 2);
  EXPECT(state, diagnostics.diagnostics()[0].message == "first product failed");
  EXPECT(state,
         diagnostics.diagnostics()[1].message == "second product failed");

  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.status == draft::SemanticReadyWaveStatus::Failed);
  EXPECT(state, graph.products[consumer.value].state ==
                    draft::SemanticProductState::DependencyFailed);
  EXPECT(state, graph.products[consumer.value].failure ==
                    "dependency semantic product 0 failed");
}

void test_uncollapsed_cycle_stalls(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto first =
      append(state, graph, draft::SemanticProductKind::ConstantValue);
  const auto second =
      append(state, graph, draft::SemanticProductKind::ConstantValue, {first});
  draft::SemanticReadyWave wave = draft::freeze_semantic_ready_wave(graph);
  draft::SemanticProductOutcome blocked;
  blocked.kind = draft::SemanticProductOutcomeKind::Blocked;
  blocked.dependencies = {second};
  draft::DiagnosticSink diagnostics;
  publish(state, graph, wave, {std::move(blocked)}, diagnostics);

  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.status == draft::SemanticReadyWaveStatus::Stalled);
  EXPECT(state,
         wave.failure ==
             "semantic product graph contains an uncollapsed dependency cycle");
}

void test_invalid_outcome_is_atomic(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto product =
      append(state, graph, draft::SemanticProductKind::TypeMembers);
  const draft::SemanticReadyWave wave =
      draft::freeze_semantic_ready_wave(graph);
  draft::SemanticProductOutcome invalid;
  invalid.kind = draft::SemanticProductOutcomeKind::Blocked;
  std::vector<draft::SemanticProductOutcome> outcomes;
  outcomes.push_back(std::move(invalid));
  draft::DiagnosticSink diagnostics;
  std::string reason;
  EXPECT(state, !draft::publish_semantic_ready_wave(graph, wave, outcomes,
                                                    diagnostics, reason));
  EXPECT(state, reason == "blocked semantic product reported no dependency");
  EXPECT(state, graph.products[product.value].state ==
                    draft::SemanticProductState::Running);
  EXPECT(state, diagnostics.diagnostics().empty());
}

void test_partial_wave_is_rejected(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto first =
      append(state, graph, draft::SemanticProductKind::TypeIdentity);
  (void)append(state, graph, draft::SemanticProductKind::TypeMembers);
  draft::SemanticReadyWave wave = draft::freeze_semantic_ready_wave(graph);
  wave.products = {first};
  std::vector<draft::SemanticProductOutcome> outcomes(1);
  draft::DiagnosticSink diagnostics;
  std::string reason;
  EXPECT(state, !draft::publish_semantic_ready_wave(graph, wave, outcomes,
                                                    diagnostics, reason));
  EXPECT(state,
         reason == "semantic wave does not contain every running product");
  EXPECT(state,
         graph.products[0].state == draft::SemanticProductState::Running);
  EXPECT(state,
         graph.products[1].state == draft::SemanticProductState::Running);
}

void test_completed_inputs_and_superseded_generation(TestState &state) {
  draft::SemanticProductGraph graph;
  std::string reason;
  const auto target = draft::append_completed_semantic_product(
      graph, draft::SemanticProductKind::TargetProfile, {}, reason);
  EXPECT(state, target.is_valid());
  const auto surface_source = draft::append_completed_semantic_product(
      graph, draft::SemanticProductKind::SourceGeneration, {}, reason);
  const auto surface_file = draft::append_completed_semantic_product(
      graph, draft::SemanticProductKind::ParsedFile,
      std::vector<draft::SemanticProductId>{surface_source}, reason);
  EXPECT(state, surface_file.is_valid());
  const auto surface_names =
      append(state, graph, draft::SemanticProductKind::PackageNameSet,
             {target, surface_file});
  draft::SemanticReadyWave wave = draft::freeze_semantic_ready_wave(graph);
  draft::SemanticProductOutcome waiting;
  waiting.kind = draft::SemanticProductOutcomeKind::WaitingForSynthesis;
  draft::DiagnosticSink diagnostics;
  publish(state, graph, wave, {std::move(waiting)}, diagnostics);
  EXPECT(state, draft::freeze_semantic_ready_wave(graph).status ==
                    draft::SemanticReadyWaveStatus::WaitingForSynthesis);

  EXPECT(state, draft::supersede_semantic_products(
                    graph, std::vector<draft::SemanticProductId>{surface_names},
                    reason));
  const auto resolved_source = draft::append_completed_semantic_product(
      graph, draft::SemanticProductKind::SourceGeneration, {}, reason);
  const auto resolved_file = draft::append_completed_semantic_product(
      graph, draft::SemanticProductKind::ParsedFile,
      std::vector<draft::SemanticProductId>{resolved_source}, reason);
  const auto resolved_names =
      append(state, graph, draft::SemanticProductKind::PackageNameSet,
             {target, resolved_file});
  wave = draft::freeze_semantic_ready_wave(graph);
  EXPECT(state, wave.products ==
                    std::vector<draft::SemanticProductId>({resolved_names}));
  publish(state, graph, wave, std::vector<draft::SemanticProductOutcome>(1),
          diagnostics);
  EXPECT(state, draft::freeze_semantic_ready_wave(graph).status ==
                    draft::SemanticReadyWaveStatus::Complete);
}

void test_completed_product_rejects_incomplete_dependency(TestState &state) {
  draft::SemanticProductGraph graph;
  const auto waiting =
      append(state, graph, draft::SemanticProductKind::TypeIdentity);
  std::string reason;
  const auto invalid = draft::append_completed_semantic_product(
      graph, draft::SemanticProductKind::TypeMembers,
      std::vector<draft::SemanticProductId>{waiting}, reason);
  EXPECT(state, !invalid.is_valid());
  EXPECT(state,
         reason == "completed semantic product has an incomplete dependency");
  EXPECT(state, graph.products.size() == 1);
}

} // namespace

int main() {
  TestState state;
  test_dependency_waves_are_canonical(state);
  test_discovered_dependency_requeues_product(state);
  test_synthesis_wait_suspends_consumers(state);
  test_diagnostics_publish_in_product_order(state);
  test_uncollapsed_cycle_stalls(state);
  test_invalid_outcome_is_atomic(state);
  test_partial_wave_is_rejected(state);
  test_completed_inputs_and_superseded_generation(state);
  test_completed_product_rejects_incomplete_dependency(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " semantic work graph expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all semantic work graph tests passed\n";
  return EXIT_SUCCESS;
}
