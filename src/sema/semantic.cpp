// Deterministic semantic fixed-point driver.

#include "sema/semantic.h"

#include "sema/body_checker.h"
#include "sema/global_initializer.h"
#include "sema/runtime_context.h"
#include "sema/target_validation.h"
#include "sema/type_resolver.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace draft {
namespace {

// Finds the immutable parsed tree named by a cross-round SyntaxReference.
// LoadedPackage owns every returned tree for the complete semantic operation;
// a missing/non-Draft file is a recoverable failed lookup rather than an
// assertion because malformed package inputs may still reach diagnostics.
[[nodiscard]] const SyntaxTree *find_tree(
    const LoadedPackage &loaded, FileId file) {
  for (const LoadedPackageFile &entry : loaded.files) {
    if (entry.source == file && entry.syntax.has_value()) {
      return &*entry.syntax;
    }
  }
  return nullptr;
}

// Source syntax, rather than a discarded round's SymbolId or TypeId, is the
// equality key for interpreter progress. The vector is deliberately scanned in
// insertion order: required layout sites are normally few, and preserving the
// direct deterministic representation is more valuable than a second index.
[[nodiscard]] bool already_resolved(
    const std::vector<ResolvedIntegerExpression> &resolved,
    SyntaxReference syntax) {
  for (const ResolvedIntegerExpression &entry : resolved) {
    if (entry.syntax == syntax) return true;
  }
  return false;
}

// Converts the interpreter's concrete result TypeId into the round-independent
// descriptor used at a later value-parameter boundary. An untyped integer has
// a present default descriptor so contextual conversion remains legal; a
// non-integer TypeId returns no descriptor and is rejected by the consumer.
[[nodiscard]] std::optional<IntegerExpressionType> integer_expression_type(
    const SemanticPackage &package, TypeId type_id) {
  if (!type_id.is_valid()) return std::nullopt;
  const Type &type = package.types.type(type_id);
  IntegerExpressionType result;
  if (type.kind == TypeKind::UntypedInteger) return result;
  result.bit_width = type.bit_width;
  if (type.kind == TypeKind::SignedInteger) {
    result.representation = IntegerExpressionRepresentation::Signed;
    result.identity = type.name;
  } else if (type.kind == TypeKind::UnsignedInteger) {
    result.representation = IntegerExpressionRepresentation::Unsigned;
    result.identity = type.name;
  } else {
    return std::nullopt;
  }
  return result;
}

// Runs only after one complete declaration/signature/type pass. The full
// interpreter can now execute calls and conditionals which the intentionally
// small early layout evaluator cannot. Successful values are source-keyed and
// consumed only by the next clean rebuild; no partially laid-out Type row is
// mutated in place.
[[nodiscard]] std::size_t resolve_required_integer_expressions(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const ConstantTable &constants,
    std::vector<ResolvedIntegerExpression> &resolved,
    DiagnosticSink &diagnostics) {
  std::size_t progress = 0;
  // Copy each row before invoking the interpreter. Constant evaluation is
  // allowed to append semantic tables, so no reference into an append-only
  // vector may survive that call. Re-reading size also lets a newly discovered
  // required site run later in this same deterministic source-order pass.
  for (std::size_t index = 0;
       index < package.required_integer_expressions.size();
       ++index) {
    const RequiredIntegerExpression required =
        package.required_integer_expressions[index];
    if (already_resolved(resolved, required.syntax)) continue;
    const SyntaxTree *tree = find_tree(loaded, required.syntax.file);
    if (tree == nullptr || !required.syntax.node.is_valid()) continue;
    const std::optional<EvaluatedConstant> value =
        evaluate_typed_constant_expression(
        sources,
        loaded,
        package,
        target,
        *tree,
        required.syntax.node,
        required.scope,
        diagnostics,
        &constants);
    if (!value.has_value() ||
        value->value.kind != ConstantKind::Integer) {
      continue;
    }
    resolved.push_back({
        required.syntax,
        value->value.integer,
        integer_expression_type(package, value->type),
    });
    ++progress;
  }
  return progress;
}

} // namespace

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  const AvailablePackageImports imports;
  return analyze_package_semantics(sources, loaded, target, imports, diagnostics);
}

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics) {
  SemanticAnalysisResult result;
  const std::size_t initial_error_count = diagnostics.error_count();
  std::vector<ResolvedIntegerExpression> resolved_integers;

  // Discover selections without copying provisional diagnostics into the user
  // sink. Each progress round adds at least one distinct SyntaxReference, and a
  // finite parsed tree therefore guarantees termination without an arbitrary
  // iteration limit.
  while (true) {
    DiagnosticSink provisional_diagnostics;
    SemanticPackage provisional = collect_package_declarations(
        sources, loaded, result.selections, provisional_diagnostics);
    bind_package_interfaces(provisional, imports, provisional_diagnostics);
    resolve_package_types(
        sources,
        loaded,
        provisional,
        result.selections,
        resolved_integers,
        provisional_diagnostics);
    const CompileTimeRoundResult round = evaluate_compile_time_round(
        sources,
        loaded,
        provisional,
        target,
        result.selections,
        false,
        provisional_diagnostics);
    const std::size_t new_integers = resolve_required_integer_expressions(
        sources,
        loaded,
        provisional,
        target,
        round.constants,
        resolved_integers,
        provisional_diagnostics);
    if (round.new_selections == 0 && new_integers == 0) {
      break;
    }
  }

  // Rebuild once from the complete known selection set. This is the only round
  // that contributes semantic diagnostics and the only graph returned to later
  // HIR construction, so stable IDs cannot refer into discarded rounds.
  result.package = collect_package_declarations(
      sources, loaded, result.selections, diagnostics);
  bind_package_interfaces(result.package, imports, diagnostics);
  resolve_package_types(
      sources,
      loaded,
      result.package,
      result.selections,
      resolved_integers,
      diagnostics);
  // Declaration/member synthesis runs before bodies. Install the built-in
  // Context now so those early requests receive the same typed field set as
  // later statement/expression synthesis.
  ensure_runtime_context_type(result.package, diagnostics);
  CompileTimeRoundResult final_round = evaluate_compile_time_round(
      sources,
      loaded,
      result.package,
      target,
      result.selections,
      true,
      diagnostics);
  result.constants = std::move(final_round.constants);
  (void)check_global_initializers(
      sources,
      loaded,
      result.package,
      target,
      result.constants,
      result.global_initializers,
      diagnostics);
  (void)validate_package_initializer_expression_types(
      sources,
      loaded,
      result.selections,
      result.package,
      result.constants,
      target,
      diagnostics);
  (void)validate_target_types(result.package.types, target, diagnostics);
  result.ok = diagnostics.error_count() == initial_error_count &&
              final_round.unresolved_conditionals == 0;
  return result;
}

} // namespace draft
