// Deterministic semantic fixed-point driver.

#include "sema/semantic.h"

#include "sema/body_checker.h"
#include "sema/global_initializer.h"
#include "sema/runtime_context.h"
#include "sema/target_validation.h"
#include "sema/type_resolver.h"

#include <algorithm>
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

// A pending constant or `when` condition may name declarations or members
// supplied by the package's current opaque interface set. Discovery must return
// that structural set without diagnosing the dependent expression yet. Once
// the set is overlaid, the next clean semantic round either makes progress or
// emits the ordinary precise unready diagnostic.
[[nodiscard]] bool has_structural_synthesis_site(
    const SemanticPackage &package) {
  for (const SemanticSite &site : package.sites) {
    if (site.kind == SemanticSiteKind::SynthesisDeclaration ||
        site.kind == SemanticSiteKind::SynthesisMember) {
      return true;
    }
  }
  return false;
}

// Runs only after one complete declaration/signature/type pass. The full
// interpreter can now execute calls and conditionals which the intentionally
// small early layout evaluator cannot. Successful values are source-keyed and
// consumed only by the next clean rebuild; no partially laid-out Type row is
// mutated in place.
struct RequiredIntegerResolutionResult {
  std::size_t resolved = 0;
  std::size_t newly_blocked = 0;
  std::vector<SymbolId> compile_time_procedures;
};

void remember_blocked_integer(
    std::vector<SyntaxReference> &blocked,
    SyntaxReference syntax,
    std::size_t &newly_blocked) {
  if (std::find(blocked.begin(), blocked.end(), syntax) != blocked.end()) {
    return;
  }
  blocked.push_back(syntax);
  ++newly_blocked;
}

void remember_compile_time_procedures(
    std::vector<SymbolId> &destination,
    const std::vector<SymbolId> &source) {
  for (SymbolId procedure : source) {
    if (std::find(destination.begin(), destination.end(), procedure) ==
        destination.end()) {
      destination.push_back(procedure);
    }
  }
}

[[nodiscard]] RequiredIntegerResolutionResult
resolve_required_integer_expressions(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const ConstantTable &constants,
    std::vector<ResolvedIntegerExpression> &resolved,
    CompileTimeSynthesisMode synthesis_mode,
    std::vector<SyntaxReference> &blocked_synthesis,
    DiagnosticSink &diagnostics) {
  RequiredIntegerResolutionResult result;
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
    std::optional<EvaluatedConstant> value;
    if (synthesis_mode == CompileTimeSynthesisMode::Discover) {
      const std::size_t original_site_count = package.sites.size();
      CompileTimeExpressionDiscoveryResult discovery =
          discover_typed_constant_expression(
              sources,
              loaded,
              package,
              target,
              *tree,
              required.syntax.node,
              required.scope,
              &constants,
              nullptr,
              required.expected_type);
      value = std::move(discovery.value);
      if (discovery.blocked_by_synthesis) {
        // Direct recipe synthesis is usually lexically in package scope (for
        // example `[... ]u8`), but semantically belongs to the declaration
        // whose type is incomplete. Procedure-body sites already carry their
        // own procedure anchor and are not changed here.
        for (std::size_t site_index = original_site_count;
             site_index < package.sites.size();
             ++site_index) {
          SemanticSite &site = package.sites[site_index];
          if (site.kind == SemanticSiteKind::SynthesisExpression &&
              !site.anchor.is_valid() && required.anchor.is_valid()) {
            site.anchor = required.anchor;
          }
        }
        remember_blocked_integer(
            blocked_synthesis,
            required.syntax,
            result.newly_blocked);
        remember_compile_time_procedures(
            result.compile_time_procedures,
            discovery.compile_time_procedures);
        continue;
      }
    } else {
      value = evaluate_typed_constant_expression(
          sources,
          loaded,
          package,
          target,
          *tree,
          required.syntax.node,
          required.scope,
          diagnostics,
          &constants,
          nullptr,
          required.expected_type);
    }
    if (!value.has_value() ||
        value->value.kind != ConstantKind::Integer) {
      continue;
    }
    resolved.push_back({
        required.syntax,
        value->value.integer,
        integer_expression_type(package, value->type),
    });
    ++result.resolved;
  }
  return result;
}

} // namespace

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  const AvailablePackageImports imports;
  return analyze_package_semantics(
      sources,
      loaded,
      target,
      imports,
      CompileTimeSynthesisMode::Reject,
      diagnostics);
}

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics) {
  return analyze_package_semantics(
      sources,
      loaded,
      target,
      imports,
      CompileTimeSynthesisMode::Reject,
      diagnostics);
}

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics) {
  SemanticAnalysisResult result;
  const std::size_t initial_error_count = diagnostics.error_count();
  std::vector<ResolvedIntegerExpression> resolved_integers;
  std::vector<SyntaxReference> blocked_integer_synthesis;

  // Discover selections without copying provisional diagnostics into the user
  // sink. Each progress round adds at least one distinct SyntaxReference, and a
  // finite parsed tree therefore guarantees termination without an arbitrary
  // iteration limit.
  while (true) {
    DiagnosticSink provisional_diagnostics;
    SemanticPackage provisional = collect_package_declarations(
        sources, loaded, result.selections, provisional_diagnostics);
    provisional.identity = imports.consumer_identity;
    bind_package_interfaces(provisional, imports, provisional_diagnostics);
    if (synthesis_mode == CompileTimeSynthesisMode::Discover) {
      resolve_package_types(
          sources,
          loaded,
          provisional,
          result.selections,
          resolved_integers,
          target,
          blocked_integer_synthesis,
          provisional_diagnostics);
    } else {
      resolve_package_types(
          sources,
          loaded,
          provisional,
          result.selections,
          resolved_integers,
          target,
          provisional_diagnostics);
    }
    const CompileTimeRoundResult round = evaluate_compile_time_round(
        sources,
        loaded,
        provisional,
        target,
        result.selections,
        synthesis_mode,
        false,
        provisional_diagnostics);
    const RequiredIntegerResolutionResult integer_round =
        resolve_required_integer_expressions(
            sources,
            loaded,
            provisional,
            target,
            round.constants,
            resolved_integers,
            synthesis_mode,
            blocked_integer_synthesis,
            provisional_diagnostics);
    if (round.new_selections == 0 && integer_round.resolved == 0 &&
        integer_round.newly_blocked == 0) {
      break;
    }
  }

  // Rebuild once from the complete known selection set. This is the only round
  // that contributes semantic diagnostics and the only graph returned to later
  // HIR construction, so stable IDs cannot refer into discarded rounds.
  result.package = collect_package_declarations(
      sources, loaded, result.selections, diagnostics);
  result.package.identity = imports.consumer_identity;
  bind_package_interfaces(result.package, imports, diagnostics);
  if (synthesis_mode == CompileTimeSynthesisMode::Discover) {
    resolve_package_types(
        sources,
        loaded,
        result.package,
        result.selections,
        resolved_integers,
        target,
        blocked_integer_synthesis,
        diagnostics);
  } else {
    resolve_package_types(
        sources,
        loaded,
        result.package,
        result.selections,
        resolved_integers,
        target,
        diagnostics);
  }
  // Declaration/member synthesis runs before bodies. Install the built-in
  // Context now so those early requests receive the same typed field set as
  // later statement/expression synthesis.
  ensure_runtime_context_type(result.package, diagnostics);
  const bool defer_unready_compile_time_dependencies =
      synthesis_mode == CompileTimeSynthesisMode::Discover &&
      (has_structural_synthesis_site(result.package) ||
       !blocked_integer_synthesis.empty());
  CompileTimeRoundResult final_round = evaluate_compile_time_round(
      sources,
      loaded,
      result.package,
      target,
      result.selections,
      synthesis_mode,
      !defer_unready_compile_time_dependencies,
      diagnostics);
  result.constants = std::move(final_round.constants);
  if (synthesis_mode == CompileTimeSynthesisMode::Discover) {
    result.compile_time_synthesis_procedures =
        std::move(final_round.compile_time_procedures);
    RequiredIntegerResolutionResult integer_dependencies =
        resolve_required_integer_expressions(
            sources,
            loaded,
            result.package,
            target,
            result.constants,
            resolved_integers,
            synthesis_mode,
            blocked_integer_synthesis,
            diagnostics);
    remember_compile_time_procedures(
        result.compile_time_synthesis_procedures,
        integer_dependencies.compile_time_procedures);
  }
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
      (final_round.unresolved_conditionals == 0 ||
       defer_unready_compile_time_dependencies);
  return result;
}

} // namespace draft
