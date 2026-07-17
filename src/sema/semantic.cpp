// Deterministic semantic fixed-point driver.

#include "sema/semantic.h"

#include "sema/type_resolver.h"

#include <cstddef>
#include <utility>

namespace draft {

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  SemanticAnalysisResult result;
  const std::size_t initial_error_count = diagnostics.error_count();

  // Discover selections without copying provisional diagnostics into the user
  // sink. Each progress round adds at least one distinct SyntaxReference, and a
  // finite parsed tree therefore guarantees termination without an arbitrary
  // iteration limit.
  while (true) {
    DiagnosticSink provisional_diagnostics;
    SemanticPackage provisional = collect_package_declarations(
        sources, loaded, result.selections, provisional_diagnostics);
    resolve_package_types(
        sources, loaded, provisional, result.selections, provisional_diagnostics);
    const CompileTimeRoundResult round = evaluate_compile_time_round(
        sources,
        loaded,
        provisional,
        target,
        result.selections,
        false,
        provisional_diagnostics);
    if (round.new_selections == 0) {
      break;
    }
  }

  // Rebuild once from the complete known selection set. This is the only round
  // that contributes semantic diagnostics and the only graph returned to later
  // HIR construction, so stable IDs cannot refer into discarded rounds.
  result.package = collect_package_declarations(
      sources, loaded, result.selections, diagnostics);
  resolve_package_types(
      sources, loaded, result.package, result.selections, diagnostics);
  CompileTimeRoundResult final_round = evaluate_compile_time_round(
      sources,
      loaded,
      result.package,
      target,
      result.selections,
      true,
      diagnostics);
  result.constants = std::move(final_round.constants);
  result.ok = diagnostics.error_count() == initial_error_count &&
              final_round.unresolved_conditionals == 0;
  return result;
}

} // namespace draft
