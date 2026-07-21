// Append-only declaration selection and deterministic semantic readiness probes.

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

// Finds the immutable parsed tree named by a cross-probe SyntaxReference.
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

// Publishes one terminal task-local diagnostic packet in its deterministic
// insertion order. Discovery packets from earlier blocked attempts are never
// passed here: only the attempt whose SemanticPackage payload is retained may
// contribute diagnostics to the command.
void publish_diagnostics(
    const DiagnosticSink &source, DiagnosticSink &destination) {
  for (const Diagnostic &diagnostic : source.diagnostics()) {
    destination.report(
        diagnostic.severity, diagnostic.range, diagnostic.message);
  }
}

// Installs ready dependency-interface constants under their consumer-local
// proxy SymbolIds. Local declarations are graph-owned ConstantValue products;
// imported constants are already immutable PackageInterface inputs and must
// not acquire duplicate consumer products merely to enter the package's
// constant lookup table. Interface binding has already translated nested type
// values into this package's TypeStore, so publication is a sorted value copy,
// not evaluation or recursive dependency work.
void append_imported_constant_bindings(
    const SemanticPackage &package, ConstantTable &constants) {
  for (const ImportedSymbol &imported : package.imported_symbols) {
    if (!imported.has_constant) continue;
    const auto position = std::lower_bound(
        constants.bindings.begin(),
        constants.bindings.end(),
        imported.proxy.value,
        [](const ConstantBinding &binding, std::uint32_t value) {
          return binding.symbol.value < value;
        });
    if (position != constants.bindings.end() &&
        position->symbol == imported.proxy) {
      continue;
    }
    constants.bindings.insert(
        position, {imported.proxy, imported.constant});
  }
}

// Source syntax, rather than a private probe's SymbolId or TypeId, is the
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
// the set is overlaid, the successor source generation either makes progress or
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

// Returns true only for a package-level conditional branch recorded by the
// append-only declaration collector and not yet merged into the authoritative
// declaration generation. Conditional member and statement selections share
// ConditionalSelections but are consumed by their later owning phases.
[[nodiscard]] bool conditional_declaration_needs_materialization(
    const SemanticPackage &package, SyntaxReference site) {
  for (const ConditionalDeclarationRegion &region :
       package.conditional_declarations) {
    if (region.syntax == site) return !region.materialized;
  }
  return false;
}

// Runs only after one complete declaration/signature/type pass. The full
// interpreter can now execute calls and conditionals which the intentionally
// small early layout evaluator cannot. Successful values are source-keyed and
// consumed only by the next task-local attempt; no partially laid-out Type row
// from a blocked attempt is published.
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

PackageDeclarationDiscovery discover_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_error_count = diagnostics.error_count();
  PackageDeclarationDiscovery result = begin_package_declaration_discovery(
      sources, loaded, imports, diagnostics);
  std::vector<ResolvedIntegerExpression> resolved_integers;
  std::vector<SyntaxReference> blocked_integer_synthesis;

  // Collect and bind unconditional declarations exactly once. Conditional
  // declaration regions retain the lexical context required to append a branch
  // after its boolean product becomes ready. The authoritative declaration
  // table remains free of provisional type/member rows until the selections are
  // complete.
  SemanticPackage declarations = std::move(result.package);

  // The terminal discovery attempt is the authoritative type payload. Earlier
  // attempts may have observed an incomplete selected name set and are
  // discarded whole. Keeping the package and its type diagnostics together is
  // essential: publishing diagnostics from one attempt beside IDs from another
  // would make source ranges look correct while semantic anchors were stale.
  std::optional<SemanticPackage> terminal_package;
  DiagnosticSink terminal_type_diagnostics;
  bool materialization_failed = false;

  // Type and constant discovery still uses a private copy until those facts
  // become individual semantic products. Crucially, the copy begins from the
  // one authoritative declaration table: unconditional source is never
  // recollected, and each newly selected declaration branch is appended once.
  // Each progress wave adds a selection, a resolved integer recipe, or a new
  // synthesis blocker, so finite syntax guarantees termination without an
  // arbitrary iteration limit.
  while (true) {
    DiagnosticSink provisional_type_diagnostics;
    SemanticPackage provisional = declarations;
    if (synthesis_mode == CompileTimeSynthesisMode::Discover) {
      resolve_package_types(
          sources,
          loaded,
          provisional,
          result.selections,
          resolved_integers,
          target,
          blocked_integer_synthesis,
          provisional_type_diagnostics);
    } else {
      resolve_package_types(
          sources,
          loaded,
          provisional,
          result.selections,
          resolved_integers,
          target,
          provisional_type_diagnostics);
    }
    DiagnosticSink provisional_discovery_diagnostics;
    const std::size_t previous_selection_count =
        result.selections.entries.size();
    const CompileTimeRoundResult round = evaluate_compile_time_round(
        sources,
        loaded,
        provisional,
        target,
        result.selections,
        synthesis_mode,
        false,
        provisional_discovery_diagnostics);
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
            provisional_discovery_diagnostics);
    bool materialization_ok = true;
    for (std::size_t selection_index = previous_selection_count;
         selection_index < result.selections.entries.size();
         ++selection_index) {
      const ConditionalSelection &selection =
          result.selections.entries[selection_index];
      if (!conditional_declaration_needs_materialization(
              declarations, selection.site)) {
        continue;
      }
      materialization_ok = materialize_conditional_declaration(
          sources,
          loaded,
          result.selections,
          selection.site,
          declarations,
          diagnostics) && materialization_ok;
    }
    if (!materialization_ok) {
      materialization_failed = true;
      break;
    }
    if (round.new_selections == 0 && integer_round.resolved == 0 &&
        integer_round.newly_blocked == 0) {
      terminal_package = std::move(provisional);
      terminal_type_diagnostics = std::move(provisional_type_diagnostics);
      result.unresolved_conditionals = round.unresolved_conditionals;
      result.compile_time_synthesis_procedures =
          round.compile_time_procedures;
      break;
    }
  }

  if (materialization_failed || !terminal_package.has_value()) {
    // A failed append means no coherent selected name set exists. Preserve the
    // authoritative declarations for diagnostic inspection, but do not invent
    // a resolved type payload by replaying a pass after the structural error.
    result.package = std::move(declarations);
    result.discovery_ok = false;
    return result;
  }
  result.package = std::move(*terminal_package);
  publish_diagnostics(terminal_type_diagnostics, diagnostics);

  // Declaration/member synthesis runs before bodies. Install the built-in
  // Context now so those early requests receive the same typed field set as
  // later statement/expression synthesis.
  ensure_runtime_context_type(result.package, diagnostics);
  result.terminal = true;
  result.discovery_ok =
      diagnostics.error_count() == initial_error_count;
  result.resolved_integers = std::move(resolved_integers);
  result.blocked_integer_synthesis = std::move(blocked_integer_synthesis);
  return result;
}

PackageDeclarationDiscovery begin_package_declaration_discovery(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics) {
  PackageDeclarationDiscovery result;
  const std::size_t initial_error_count = diagnostics.error_count();
  result.package = collect_package_declarations(sources, loaded, diagnostics);
  result.package.identity = imports.consumer_identity;
  bind_package_interfaces(result.package, imports, diagnostics);
  result.discovery_ok =
      diagnostics.error_count() == initial_error_count;
  return result;
}

[[nodiscard]] static SemanticAnalysisResult finish_package_semantics_impl(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    CompileTimeSynthesisMode synthesis_mode,
    PackageDeclarationDiscovery discovery,
    bool constants_are_products,
    DiagnosticSink &diagnostics) {
  SemanticAnalysisResult result;
  result.package = std::move(discovery.package);
  result.selections = std::move(discovery.selections);
  if (!discovery.terminal) return result;

  const std::size_t initial_error_count = diagnostics.error_count();
  const bool defer_unready_compile_time_dependencies =
      synthesis_mode == CompileTimeSynthesisMode::Discover &&
      (has_structural_synthesis_site(result.package) ||
       !discovery.blocked_integer_synthesis.empty());
  CompileTimeRoundResult final_round;
  if (constants_are_products) {
    result.constants = std::move(discovery.published_constants);
    append_imported_constant_bindings(result.package, result.constants);
    final_round = validate_compile_time_products(
        sources,
        loaded,
        result.package,
        target,
        result.selections,
        result.constants,
        synthesis_mode,
        !defer_unready_compile_time_dependencies,
        diagnostics);
  } else {
    final_round = evaluate_compile_time_round(
        sources,
        loaded,
        result.package,
        target,
        result.selections,
        synthesis_mode,
        !defer_unready_compile_time_dependencies,
        diagnostics);
    result.constants = std::move(final_round.constants);
  }
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
            discovery.resolved_integers,
            synthesis_mode,
            discovery.blocked_integer_synthesis,
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
  (void)validate_package_compile_time_expression_types(
      sources,
      loaded,
      result.selections,
      result.package,
      result.constants,
      target,
      diagnostics);
  (void)validate_target_types(result.package.types, target, diagnostics);
  result.ok = discovery.discovery_ok &&
      diagnostics.error_count() == initial_error_count &&
      (final_round.unresolved_conditionals == 0 ||
       defer_unready_compile_time_dependencies);
  return result;
}

SemanticAnalysisResult finish_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    CompileTimeSynthesisMode synthesis_mode,
    PackageDeclarationDiscovery discovery,
    DiagnosticSink &diagnostics) {
  return finish_package_semantics_impl(
      sources,
      loaded,
      target,
      synthesis_mode,
      std::move(discovery),
      false,
      diagnostics);
}

SemanticAnalysisResult finish_package_semantics_from_products(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    CompileTimeSynthesisMode synthesis_mode,
    PackageDeclarationDiscovery discovery,
    DiagnosticSink &diagnostics) {
  return finish_package_semantics_impl(
      sources,
      loaded,
      target,
      synthesis_mode,
      std::move(discovery),
      true,
      diagnostics);
}

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics) {
  PackageDeclarationDiscovery discovery = discover_package_declarations(
      sources,
      loaded,
      target,
      imports,
      synthesis_mode,
      diagnostics);
  return finish_package_semantics(
      sources,
      loaded,
      target,
      synthesis_mode,
      std::move(discovery),
      diagnostics);
}

} // namespace draft
