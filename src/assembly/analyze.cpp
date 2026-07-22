// Direct target dispatch for parsed assembly.

#include "assembly/analyze.h"

#include <cstddef>
#include <cstdint>

namespace draft {

AssemblyProgram analyze_target_assembly(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetProfile &target,
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics) {
  if (target.supports_parsed_assembly &&
      target.parsed_assembly_architecture == "aarch64") {
    return analyze_aarch64_assembly(
        sources, loaded, target, semantic, hir, diagnostics);
  }

  AssemblyProgram result;
  const std::size_t initial_errors = diagnostics.error_count();
  for (std::size_t index = 0; index < hir.expression_count(); ++index) {
    const HirExpression &expression =
        hir.expression(HirExpressionId{static_cast<std::uint32_t>(index)});
    if (expression.kind != HirExpressionKind::Assembly) continue;
    diagnostics.error(
        expression.range,
        "parsed assembly is not supported by target '" +
            target.facts.identity + "'");
  }
  for (std::size_t index = 0; index < hir.statement_count(); ++index) {
    const HirStatement &statement =
        hir.statement(HirStatementId{static_cast<std::uint32_t>(index)});
    if (statement.kind != HirStatementKind::Assembly) continue;
    diagnostics.error(
        statement.range,
        "parsed assembly is not supported by target '" +
            target.facts.identity + "'");
  }
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
