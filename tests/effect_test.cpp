// Direct and transitive procedure effect-summary tests.

#include "sema/body_checker.h"
#include "sema/effect.h"
#include "sema/interface.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "target/profile.h"
#include "workspace/package.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;
  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "effect_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

std::optional<draft::SymbolId> symbol(
    const draft::SemanticPackage &package, std::string_view name) {
  return package.symbols.lookup_direct(package.package_scope, name);
}

bool has_effect(
    const draft::ProcedureEffectSummary &summary, draft::EffectKind kind) {
  return std::any_of(
      summary.effects.begin(), summary.effects.end(), [kind](const draft::SemanticEffect &effect) {
        return effect.kind == kind;
      });
}

void test_transitive_effects(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package effects

counter: i64

pub leaf :: proc(values: [^]i64) {
    counter += values[0]
    assert(counter >= 0)
}

pub caller :: proc(values: [^]i64) {
    leaf(values)
}

pub invoke :: proc(callback: proc()) {
    callback()
}

flow_leaf :: proc() {
    assert(true)
}

flow_caller :: proc() {
    copy := flow_leaf
    invoke(copy)
}
)draft");
  file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
  loaded.files.push_back(std::move(file));
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded, target.facts, diagnostics);
  draft::BodyCheckResult bodies = draft::check_package_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::EffectSummaryResult effects =
      draft::summarize_package_effects(semantics.package, bodies.program);
  const draft::AgentMetadataResult empty_metadata;
  const draft::PackageInterface package_interface = draft::build_package_interface(
      {"workspace", "effects"},
      semantics.package,
      semantics.constants,
      empty_metadata,
      effects,
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }

  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, effects.procedures.size() == 5);
  const std::optional<draft::SymbolId> leaf = symbol(semantics.package, "leaf");
  const std::optional<draft::SymbolId> caller = symbol(semantics.package, "caller");
  const std::optional<draft::SymbolId> invoke = symbol(semantics.package, "invoke");
  const std::optional<draft::SymbolId> flow_caller =
      symbol(semantics.package, "flow_caller");
  EXPECT(state, leaf.has_value());
  EXPECT(state, caller.has_value());
  EXPECT(state, invoke.has_value());
  EXPECT(state, flow_caller.has_value());
  if (!leaf || !caller || !invoke || !flow_caller) return;
  const draft::ProcedureEffectSummary *leaf_summary = effects.find(*leaf);
  const draft::ProcedureEffectSummary *caller_summary = effects.find(*caller);
  const draft::ProcedureEffectSummary *invoke_summary = effects.find(*invoke);
  const draft::ProcedureEffectSummary *flow_caller_summary =
      effects.find(*flow_caller);
  EXPECT(state, leaf_summary != nullptr);
  EXPECT(state, caller_summary != nullptr);
  EXPECT(state, invoke_summary != nullptr);
  EXPECT(state, flow_caller_summary != nullptr);
  if (leaf_summary == nullptr || caller_summary == nullptr ||
      invoke_summary == nullptr || flow_caller_summary == nullptr) {
    return;
  }
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::PackageGlobal));
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::Unchecked));
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::ContextField));
  EXPECT(state, caller_summary->direct_calls.size() == 1);
  EXPECT(state, has_effect(*caller_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state, has_effect(*caller_summary, draft::EffectKind::Unchecked));
  EXPECT(state, has_effect(*invoke_summary, draft::EffectKind::FlowCall));
  EXPECT(state, !has_effect(*invoke_summary, draft::EffectKind::UnknownCall));
  EXPECT(state,
      has_effect(*flow_caller_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      !has_effect(*flow_caller_summary, draft::EffectKind::UnknownCall));
  EXPECT(state, package_interface.declarations.size() == 3);
  if (package_interface.declarations.size() == 3) {
    EXPECT(state, package_interface.declarations[1].name == "caller");
    EXPECT(state, package_interface.declarations[1].has_effect_summary);
    EXPECT(state, package_interface.declarations[1].effects.size() >= 4);
    EXPECT(state, package_interface.declarations[2].name == "invoke");
    EXPECT(state, package_interface.declarations[2].effects.size() == 1);
    if (package_interface.declarations[2].effects.size() == 1) {
      EXPECT(state,
          package_interface.declarations[2].effects[0].kind ==
              draft::EffectKind::FlowCall);
      EXPECT(state,
          package_interface.declarations[2].effects[0].flow_parameter == 0);
    }
  }
}

void test_target_and_package_assembly_summaries(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "native_effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package native_effects

Callback :: c proc(user: rawptr) -> rawptr

foreign darwin {
    pthread_create :: c proc(
        thread: ^rawptr,
        attributes: rawptr,
        start: Callback,
        user: rawptr,
    ) -> i32
    mystery :: c proc()
}

foreign package_assembly {
    external :: c proc()
}

foreign custom_provider {
    audited :: c proc(callback: Callback, user: rawptr)
}

counter: i32

worker :: c proc(user: rawptr) -> rawptr {
    counter += 1
    return user
}

through_system :: proc() {
    thread: rawptr
    pthread_create(&thread, nil, worker, nil)
}

through_assembly :: proc() {
    external()
}

through_unknown :: proc() {
    mystery()
}

through_audit :: proc() {
    audited(worker, nil)
}
)draft");
  file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
  loaded.files.push_back(std::move(file));
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded, target.facts, diagnostics);
  draft::BodyCheckResult bodies = draft::check_package_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  draft::ForeignProviderAudit audit;
  audit.provider = "custom_provider";
  draft::ForeignAuditSymbol audited_symbol;
  audited_symbol.linker_name = "audited";
  draft::ForeignAuditEffect callback;
  callback.kind = draft::EffectKind::FlowCall;
  callback.flow_parameter = 0;
  audited_symbol.effects.push_back(callback);
  draft::ForeignAuditEffect assembly;
  assembly.kind = draft::EffectKind::Assembly;
  assembly.detail = "audited foreign assembly";
  audited_symbol.effects.push_back(assembly);
  audit.symbols.push_back(std::move(audited_symbol));
  const std::vector<draft::ForeignProviderAudit> audits{audit};
  const draft::EffectSummaryResult effects =
      draft::summarize_package_effects(
          semantics.package, bodies.program, &target, audits);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  const std::optional<draft::SymbolId> through_system =
      symbol(semantics.package, "through_system");
  const std::optional<draft::SymbolId> through_assembly =
      symbol(semantics.package, "through_assembly");
  const std::optional<draft::SymbolId> through_unknown =
      symbol(semantics.package, "through_unknown");
  const std::optional<draft::SymbolId> through_audit =
      symbol(semantics.package, "through_audit");
  EXPECT(state, through_system.has_value());
  EXPECT(state, through_assembly.has_value());
  EXPECT(state, through_unknown.has_value());
  EXPECT(state, through_audit.has_value());
  if (!through_system || !through_assembly || !through_unknown ||
      !through_audit) {
    return;
  }
  const draft::ProcedureEffectSummary *system_summary =
      effects.find(*through_system);
  const draft::ProcedureEffectSummary *assembly_summary =
      effects.find(*through_assembly);
  const draft::ProcedureEffectSummary *unknown_summary =
      effects.find(*through_unknown);
  const draft::ProcedureEffectSummary *audit_summary =
      effects.find(*through_audit);
  EXPECT(state, system_summary != nullptr);
  EXPECT(state, assembly_summary != nullptr);
  EXPECT(state, unknown_summary != nullptr);
  EXPECT(state, audit_summary != nullptr);
  if (system_summary == nullptr || assembly_summary == nullptr ||
      unknown_summary == nullptr || audit_summary == nullptr) {
    return;
  }
  EXPECT(state,
      has_effect(*system_summary, draft::EffectKind::PackageGlobal));
  EXPECT(state,
      !has_effect(*system_summary, draft::EffectKind::UnknownCall));
  EXPECT(state,
      has_effect(*assembly_summary, draft::EffectKind::Assembly));
  EXPECT(state,
      !has_effect(*assembly_summary, draft::EffectKind::UnknownCall));
  EXPECT(state,
      has_effect(*unknown_summary, draft::EffectKind::UnknownCall));
  EXPECT(state,
      has_effect(*audit_summary, draft::EffectKind::PackageGlobal));
  EXPECT(state,
      has_effect(*audit_summary, draft::EffectKind::Assembly));
  EXPECT(state,
      !has_effect(*audit_summary, draft::EffectKind::UnknownCall));
}

} // namespace

int main() {
  TestState state;
  test_transitive_effects(state);
  test_target_and_package_assembly_summaries(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " effect summary expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all effect summary tests passed\n";
  return EXIT_SUCCESS;
}
