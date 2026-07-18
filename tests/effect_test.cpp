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

Callback_Box :: @repr(C) struct {
    callback: Callback,
}

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
    audited :: c proc(box: Callback_Box)
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
    audited(Callback_Box{callback = worker})
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
  callback.flow_path = {"callback"};
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

void test_typed_and_context_flow_paths(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "path_effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package path_effects

Callback_Box :: struct {
    callback: proc(),
}

danger :: proc() {
    assert(true)
}

invoke_box :: proc(box: Callback_Box) {
    box.callback()
}

through_box :: proc() {
    box: Callback_Box
    box.callback = danger
    invoke_box(box)
}

context_callback :: proc(
    condition, message, source_file: string,
    line, column: usize,
) {
    assert(true)
}

through_context :: proc() {
    context.assertion_failure_proc = context_callback
    context.assertion_failure_proc("", "", "", 0, 0)
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
      draft::summarize_package_effects(
          semantics.package, bodies.program, &target);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);

  const std::optional<draft::SymbolId> invoke_box =
      symbol(semantics.package, "invoke_box");
  const std::optional<draft::SymbolId> through_box =
      symbol(semantics.package, "through_box");
  const std::optional<draft::SymbolId> through_context =
      symbol(semantics.package, "through_context");
  EXPECT(state, invoke_box.has_value());
  EXPECT(state, through_box.has_value());
  EXPECT(state, through_context.has_value());
  if (!invoke_box || !through_box || !through_context) return;

  const draft::ProcedureEffectSummary *invoke_summary = effects.find(*invoke_box);
  const draft::ProcedureEffectSummary *box_summary = effects.find(*through_box);
  const draft::ProcedureEffectSummary *context_summary =
      effects.find(*through_context);
  EXPECT(state, invoke_summary != nullptr);
  EXPECT(state, box_summary != nullptr);
  EXPECT(state, context_summary != nullptr);
  if (invoke_summary == nullptr || box_summary == nullptr ||
      context_summary == nullptr) {
    return;
  }

  const auto typed_slot = std::find_if(
      invoke_summary->effects.begin(),
      invoke_summary->effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::FlowCall;
      });
  EXPECT(state, typed_slot != invoke_summary->effects.end());
  if (typed_slot != invoke_summary->effects.end()) {
    EXPECT(state, typed_slot->flow_parameter == 0);
    EXPECT(state,
        typed_slot->flow_path == std::vector<std::string>{"callback"});
    EXPECT(state, !typed_slot->flow_context);
  }
  EXPECT(state, has_effect(*box_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state, !has_effect(*box_summary, draft::EffectKind::UnknownCall));

  const auto context_slot = std::find_if(
      context_summary->effects.begin(),
      context_summary->effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::FlowCall &&
            effect.flow_context;
      });
  EXPECT(state, context_slot != context_summary->effects.end());
  if (context_slot != context_summary->effects.end()) {
    EXPECT(state,
        context_slot->flow_path ==
            std::vector<std::string>{"assertion_failure_proc"});
  }
  EXPECT(state,
      has_effect(*context_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      !has_effect(*context_summary, draft::EffectKind::UnknownCall));
}

void test_returned_procedure_flow(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "return_effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package return_effects

Callback_Box :: struct {
    callback: proc(),
}

forward :: proc(callback: proc()) -> proc() {
    return identity(callback)
}

identity :: proc(callback: proc()) -> proc() {
    return callback
}

danger :: proc() {
    assert(true)
}

named_factory :: proc() -> proc() {
    return danger
}

box_factory :: proc(callback: proc()) -> Callback_Box {
    return Callback_Box{callback = callback}
}

through_return :: proc() {
    selected := forward(danger)
    selected()
    named := named_factory()
    named()
    box := box_factory(danger)
    box.callback()
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
      draft::summarize_package_effects(
          semantics.package, bodies.program, &target);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  const std::optional<draft::SymbolId> identity =
      symbol(semantics.package, "identity");
  const std::optional<draft::SymbolId> box_factory =
      symbol(semantics.package, "box_factory");
  const std::optional<draft::SymbolId> through_return =
      symbol(semantics.package, "through_return");
  EXPECT(state, identity.has_value());
  EXPECT(state, box_factory.has_value());
  EXPECT(state, through_return.has_value());
  if (!identity || !box_factory || !through_return) return;
  const draft::ProcedureEffectSummary *identity_summary =
      effects.find(*identity);
  const draft::ProcedureEffectSummary *box_factory_summary =
      effects.find(*box_factory);
  const draft::ProcedureEffectSummary *caller_summary =
      effects.find(*through_return);
  EXPECT(state, identity_summary != nullptr);
  EXPECT(state, box_factory_summary != nullptr);
  EXPECT(state, caller_summary != nullptr);
  if (identity_summary == nullptr || box_factory_summary == nullptr ||
      caller_summary == nullptr) {
    return;
  }
  EXPECT(state, identity_summary->return_values.size() == 1);
  if (identity_summary->return_values.size() == 1) {
    EXPECT(state,
        identity_summary->return_values[0].value.flow_slots.size() == 1);
    if (identity_summary->return_values[0].value.flow_slots.size() == 1) {
      EXPECT(state,
          identity_summary->return_values[0].value.flow_slots[0].parameter == 0);
    }
  }
  EXPECT(state, box_factory_summary->return_values.size() == 1);
  if (box_factory_summary->return_values.size() == 1) {
    EXPECT(state,
        box_factory_summary->return_values[0].path ==
            std::vector<std::string>{"callback"});
  }
  EXPECT(state,
      has_effect(*caller_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      !has_effect(*caller_summary, draft::EffectKind::UnknownCall));
}

} // namespace

int main() {
  TestState state;
  test_transitive_effects(state);
  test_target_and_package_assembly_summaries(state);
  test_typed_and_context_flow_paths(state);
  test_returned_procedure_flow(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " effect summary expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all effect summary tests passed\n";
  return EXIT_SUCCESS;
}
