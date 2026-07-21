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

std::optional<std::size_t> effect_row(
    const draft::EffectSummaryResult &effects, draft::SymbolId procedure) {
  for (std::size_t index = 0; index < effects.procedures.size(); ++index) {
    if (effects.procedures[index].procedure == procedure) return index;
  }
  return std::nullopt;
}

std::optional<std::size_t> effect_component(
    const draft::EffectSummaryResult &effects, std::size_t procedure_row) {
  for (std::size_t index = 0; index < effects.components.size(); ++index) {
    const std::vector<std::size_t> &procedures =
        effects.components[index].procedure_indices;
    if (std::find(procedures.begin(), procedures.end(), procedure_row) !=
        procedures.end()) {
      return index;
    }
  }
  return std::nullopt;
}

// Legal recursive procedures are one explicit closure product. The SCC must
// publish before its acyclic caller, and an effect originating in either member
// must reach both members and the later caller.
void test_recursive_effect_component(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "recursive_effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package recursive_effects

left :: proc() {
    right()
}

right :: proc() {
    assert(true)
    left()
}

entry :: proc() {
    left()
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
          bodies.package,
          draft::project_package_body_hir(bodies.procedures),
          &target);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);

  const std::optional<draft::SymbolId> left = symbol(bodies.package, "left");
  const std::optional<draft::SymbolId> right = symbol(bodies.package, "right");
  const std::optional<draft::SymbolId> entry = symbol(bodies.package, "entry");
  EXPECT(state, left.has_value());
  EXPECT(state, right.has_value());
  EXPECT(state, entry.has_value());
  if (!left || !right || !entry) return;

  const std::optional<std::size_t> left_row = effect_row(effects, *left);
  const std::optional<std::size_t> right_row = effect_row(effects, *right);
  const std::optional<std::size_t> entry_row = effect_row(effects, *entry);
  EXPECT(state, left_row.has_value());
  EXPECT(state, right_row.has_value());
  EXPECT(state, entry_row.has_value());
  if (!left_row || !right_row || !entry_row) return;

  const std::optional<std::size_t> left_component =
      effect_component(effects, *left_row);
  const std::optional<std::size_t> right_component =
      effect_component(effects, *right_row);
  const std::optional<std::size_t> entry_component =
      effect_component(effects, *entry_row);
  EXPECT(state, left_component.has_value());
  EXPECT(state, right_component.has_value());
  EXPECT(state, entry_component.has_value());
  if (!left_component || !right_component || !entry_component) return;
  EXPECT(state, *left_component == *right_component);
  EXPECT(state, *left_component < *entry_component);
  EXPECT(state, effects.components[*left_component].procedure_indices.size() == 2);
  EXPECT(state,
      has_effect(effects.procedures[*left_row], draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      has_effect(effects.procedures[*right_row], draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      has_effect(effects.procedures[*entry_row], draft::EffectKind::RuntimeAssert));
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
      draft::summarize_package_effects(
          bodies.package,
          draft::project_package_body_hir(bodies.procedures));
  const draft::AgentMetadataResult empty_metadata;
  const draft::PackageInterface package_interface = draft::build_package_interface(
      {"workspace", "effects"},
      bodies.package,
      bodies.constants,
      empty_metadata,
      effects,
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }

  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, effects.procedures.size() == 5);
  const std::optional<draft::SymbolId> leaf = symbol(bodies.package, "leaf");
  const std::optional<draft::SymbolId> caller = symbol(bodies.package, "caller");
  const std::optional<draft::SymbolId> invoke = symbol(bodies.package, "invoke");
  const std::optional<draft::SymbolId> flow_caller =
      symbol(bodies.package, "flow_caller");
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

// raw_data is a representation escape rather than an ordinary declaration.
// Its dedicated effect must therefore begin at the intrinsic, close through a
// local caller, and survive publication in the same package interface that a
// downstream denial will consume.
void test_raw_string_data_effect(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "raw_string_effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package raw_string_effects

pub expose :: proc(text: string) -> [^]u8 {
    return raw_data(text)
}

pub forward :: proc(text: string) -> [^]u8 {
    return expose(text)
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
          bodies.package,
          draft::project_package_body_hir(bodies.procedures));
  const draft::AgentMetadataResult empty_metadata;
  const draft::PackageInterface package_interface = draft::build_package_interface(
      {"workspace", "raw_string_effects"},
      bodies.package,
      bodies.constants,
      empty_metadata,
      effects,
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }

  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  const std::optional<draft::SymbolId> expose =
      symbol(bodies.package, "expose");
  const std::optional<draft::SymbolId> forward =
      symbol(bodies.package, "forward");
  EXPECT(state, expose.has_value());
  EXPECT(state, forward.has_value());
  if (!expose.has_value() || !forward.has_value()) return;

  const draft::ProcedureEffectSummary *expose_summary = effects.find(*expose);
  const draft::ProcedureEffectSummary *forward_summary = effects.find(*forward);
  EXPECT(state, expose_summary != nullptr);
  EXPECT(state, forward_summary != nullptr);
  if (expose_summary == nullptr || forward_summary == nullptr) return;

  const bool expose_has_direct_raw_data = std::any_of(
      expose_summary->direct_effects.begin(),
      expose_summary->direct_effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::RawStringData;
      });
  EXPECT(state, expose_has_direct_raw_data);
  EXPECT(state,
      has_effect(*expose_summary, draft::EffectKind::RawStringData));
  EXPECT(state,
      !has_effect(*expose_summary, draft::EffectKind::UnknownCall));
  EXPECT(state, forward_summary->direct_calls.size() == 1);
  EXPECT(state,
      has_effect(*forward_summary, draft::EffectKind::RawStringData));
  EXPECT(state,
      !has_effect(*forward_summary, draft::EffectKind::UnknownCall));

  EXPECT(state, package_interface.declarations.size() == 2);
  for (const draft::InterfaceDeclaration &declaration :
       package_interface.declarations) {
    EXPECT(state, declaration.has_effect_summary);
    const bool exports_raw_data = std::any_of(
        declaration.effects.begin(),
        declaration.effects.end(),
        [](const draft::InterfaceDeclaration::Effect &effect) {
          return effect.kind == draft::EffectKind::RawStringData;
        });
    EXPECT(state, exports_raw_data);
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
          bodies.package,
          draft::project_package_body_hir(bodies.procedures),
          &target,
          audits);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  const std::optional<draft::SymbolId> through_system =
      symbol(bodies.package, "through_system");
  const std::optional<draft::SymbolId> through_assembly =
      symbol(bodies.package, "through_assembly");
  const std::optional<draft::SymbolId> through_unknown =
      symbol(bodies.package, "through_unknown");
  const std::optional<draft::SymbolId> through_audit =
      symbol(bodies.package, "through_audit");
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
          bodies.package,
          draft::project_package_body_hir(bodies.procedures),
          &target);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);

  const std::optional<draft::SymbolId> invoke_box =
      symbol(bodies.package, "invoke_box");
  const std::optional<draft::SymbolId> through_box =
      symbol(bodies.package, "through_box");
  const std::optional<draft::SymbolId> through_context =
      symbol(bodies.package, "through_context");
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
          bodies.package,
          draft::project_package_body_hir(bodies.procedures),
          &target);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  const std::optional<draft::SymbolId> identity =
      symbol(bodies.package, "identity");
  const std::optional<draft::SymbolId> box_factory =
      symbol(bodies.package, "box_factory");
  const std::optional<draft::SymbolId> through_return =
      symbol(bodies.package, "through_return");
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

void test_pointer_field_write_flow(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "write_effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package write_effects

Callback_Box :: struct {
    callback: proc(),
}

// This deliberately precedes install. Effect discovery must replay procedure
// bodies until the write-through contract is independent of source order.
forward_install :: proc(destination: ^Callback_Box, callback: proc()) {
    install(destination, callback)
}

// A two-level contract must retain both dereferences. Taking the address of the
// explicit local pointer copy below removes one level, leaving a normal
// one-level write contract for callers of deep_forward.
deep_forward :: proc(destination: ^Callback_Box, callback: proc()) {
    local_destination := destination
    deep_install(&local_destination, callback)
}

install :: proc(destination: ^Callback_Box, callback: proc()) {
    destination^.callback = callback
}

deep_install :: proc(destination: ^^Callback_Box, callback: proc()) {
    destination^^.callback = callback
}

// A mutable local copied from a value parameter does not change the value held
// by the caller and therefore must not publish a write-back contract.
local_only :: proc(box: Callback_Box, callback: proc()) {
    local_box := box
    local_box.callback = callback
}

// Taking the address of an explicit local copy still only exposes this
// procedure's storage. The address and install contract's dereference cancel.
install_local_copy :: proc(box: Callback_Box, callback: proc()) {
    local_box := box
    install(&local_box, callback)
}

// Pointer locals retain a conservative may-origin set. A later call through
// the local therefore publishes the write contract for both possible formal
// parameters instead of trusting one source-order assignment.
choose_install :: proc(
    left, right: ^Callback_Box,
    callback: proc(),
    choose_left: bool,
) {
    destination := left
    if !choose_left {
        destination = right
    }
    install(destination, callback)
}

danger :: proc() {
    assert(true)
}

through_install :: proc() {
    box: Callback_Box
    forward_install(&box, danger)
    box.callback()
    deep_box: Callback_Box
    deep_forward(&deep_box, danger)
    deep_box.callback()
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
          bodies.package,
          draft::project_package_body_hir(bodies.procedures),
          &target);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);

  const std::optional<draft::SymbolId> install =
      symbol(bodies.package, "install");
  const std::optional<draft::SymbolId> forward_install =
      symbol(bodies.package, "forward_install");
  const std::optional<draft::SymbolId> deep_install =
      symbol(bodies.package, "deep_install");
  const std::optional<draft::SymbolId> deep_forward =
      symbol(bodies.package, "deep_forward");
  const std::optional<draft::SymbolId> local_only =
      symbol(bodies.package, "local_only");
  const std::optional<draft::SymbolId> install_local_copy =
      symbol(bodies.package, "install_local_copy");
  const std::optional<draft::SymbolId> choose_install =
      symbol(bodies.package, "choose_install");
  const std::optional<draft::SymbolId> through_install =
      symbol(bodies.package, "through_install");
  EXPECT(state, install.has_value());
  EXPECT(state, forward_install.has_value());
  EXPECT(state, deep_install.has_value());
  EXPECT(state, deep_forward.has_value());
  EXPECT(state, local_only.has_value());
  EXPECT(state, install_local_copy.has_value());
  EXPECT(state, choose_install.has_value());
  EXPECT(state, through_install.has_value());
  if (!install || !forward_install || !deep_install || !deep_forward ||
      !local_only || !install_local_copy || !choose_install ||
      !through_install) {
    return;
  }

  const draft::ProcedureEffectSummary *install_summary = effects.find(*install);
  const draft::ProcedureEffectSummary *forward_summary =
      effects.find(*forward_install);
  const draft::ProcedureEffectSummary *deep_summary =
      effects.find(*deep_install);
  const draft::ProcedureEffectSummary *deep_forward_summary =
      effects.find(*deep_forward);
  const draft::ProcedureEffectSummary *local_summary = effects.find(*local_only);
  const draft::ProcedureEffectSummary *local_call_summary =
      effects.find(*install_local_copy);
  const draft::ProcedureEffectSummary *choice_summary =
      effects.find(*choose_install);
  const draft::ProcedureEffectSummary *caller_summary =
      effects.find(*through_install);
  EXPECT(state, install_summary != nullptr);
  EXPECT(state, forward_summary != nullptr);
  EXPECT(state, deep_summary != nullptr);
  EXPECT(state, deep_forward_summary != nullptr);
  EXPECT(state, local_summary != nullptr);
  EXPECT(state, local_call_summary != nullptr);
  EXPECT(state, choice_summary != nullptr);
  EXPECT(state, caller_summary != nullptr);
  if (install_summary == nullptr || forward_summary == nullptr ||
      deep_summary == nullptr || deep_forward_summary == nullptr ||
      local_summary == nullptr || local_call_summary == nullptr ||
      choice_summary == nullptr || caller_summary == nullptr) {
    return;
  }

  EXPECT(state, install_summary->field_writes.size() == 1);
  if (install_summary->field_writes.size() == 1) {
    const draft::ProcedureFieldWriteSummary &write =
        install_summary->field_writes.front();
    EXPECT(state, write.parameter == 0);
    EXPECT(state, write.indirection == 1);
    EXPECT(state,
        write.path == std::vector<std::string>{"callback"});
    EXPECT(state, write.value.flow_slots.size() == 1);
    if (write.value.flow_slots.size() == 1) {
      EXPECT(state, write.value.flow_slots.front().parameter == 1);
      EXPECT(state, write.value.flow_slots.front().path.empty());
    }
  }
  EXPECT(state, forward_summary->field_writes.size() == 1);
  EXPECT(state, deep_summary->field_writes.size() == 1);
  if (deep_summary->field_writes.size() == 1) {
    EXPECT(state, deep_summary->field_writes.front().indirection == 2);
  }
  EXPECT(state, deep_forward_summary->field_writes.size() == 1);
  if (deep_forward_summary->field_writes.size() == 1) {
    EXPECT(state,
        deep_forward_summary->field_writes.front().indirection == 1);
  }
  EXPECT(state, local_summary->field_writes.empty());
  EXPECT(state, local_call_summary->field_writes.empty());
  EXPECT(state, choice_summary->field_writes.size() == 2);
  bool writes_left = false;
  bool writes_right = false;
  for (const draft::ProcedureFieldWriteSummary &write :
       choice_summary->field_writes) {
    writes_left = writes_left ||
        (write.parameter == 0 && write.indirection == 1 &&
         write.path == std::vector<std::string>{"callback"});
    writes_right = writes_right ||
        (write.parameter == 1 && write.indirection == 1 &&
         write.path == std::vector<std::string>{"callback"});
  }
  EXPECT(state, writes_left);
  EXPECT(state, writes_right);
  EXPECT(state,
      has_effect(*caller_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      !has_effect(*caller_summary, draft::EffectKind::UnknownCall));
}

void test_higher_order_flow(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "higher_effects";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft",
      R"draft(package higher_effects

Higher_Box :: struct {
    invoke: proc(callback: proc()),
}

Callback_Box :: struct {
    callback: proc(),
}

Node :: struct {
    invoke: proc(node: ^Node),
}

invoke_one :: proc(callback: proc()) {
    callback()
}

apply :: proc(
    higher: proc(callback: proc()),
    callback: proc(),
) {
    higher(callback)
}

apply_twice :: proc(
    top: proc(higher: proc(callback: proc()), callback: proc()),
    higher: proc(callback: proc()),
    callback: proc(),
) {
    top(higher, callback)
}

apply_box :: proc(box: Higher_Box, callback: proc()) {
    box.invoke(callback)
}

danger :: proc() {
    assert(true)
}

safe :: proc() {}

select_first :: proc(first: proc(), ignored: i32) -> proc() {
    return first
}

install_and_zero :: proc(
    destination: ^Callback_Box,
    callback: proc(),
) -> i32 {
    destination^.callback = callback
    return 0
}

bounce :: proc(node: ^Node) {
    node^.invoke(node)
}

caller :: proc() {
    apply(invoke_one, danger)
    apply_twice(apply, invoke_one, danger)
    box: Higher_Box
    box.invoke = invoke_one
    apply_box(box, danger)
}

ordered_caller :: proc() {
    box: Callback_Box
    box.callback = safe
    selected := select_first(
        box.callback,
        install_and_zero(&box, danger),
    )
    selected()
}

recursive_caller :: proc() {
    node: Node
    node.invoke = bounce
    bounce(&node)
}

tuple_assignment_caller :: proc() {
    first: proc() = safe
    second: proc() = safe
    (first, second) = (danger, safe)
    (_, second) = (danger, safe)
    first()
    second()
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
          bodies.package,
          draft::project_package_body_hir(bodies.procedures),
          &target);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);

  const std::optional<draft::SymbolId> apply =
      symbol(bodies.package, "apply");
  const std::optional<draft::SymbolId> apply_box =
      symbol(bodies.package, "apply_box");
  const std::optional<draft::SymbolId> apply_twice =
      symbol(bodies.package, "apply_twice");
  const std::optional<draft::SymbolId> caller =
      symbol(bodies.package, "caller");
  const std::optional<draft::SymbolId> ordered_caller =
      symbol(bodies.package, "ordered_caller");
  const std::optional<draft::SymbolId> recursive_caller =
      symbol(bodies.package, "recursive_caller");
  const std::optional<draft::SymbolId> tuple_assignment_caller =
      symbol(bodies.package, "tuple_assignment_caller");
  EXPECT(state, apply.has_value());
  EXPECT(state, apply_box.has_value());
  EXPECT(state, apply_twice.has_value());
  EXPECT(state, caller.has_value());
  EXPECT(state, ordered_caller.has_value());
  EXPECT(state, recursive_caller.has_value());
  EXPECT(state, tuple_assignment_caller.has_value());
  if (!apply || !apply_box || !apply_twice || !caller || !ordered_caller ||
      !recursive_caller || !tuple_assignment_caller) {
    return;
  }

  const draft::ProcedureEffectSummary *apply_summary = effects.find(*apply);
  const draft::ProcedureEffectSummary *box_summary = effects.find(*apply_box);
  const draft::ProcedureEffectSummary *twice_summary =
      effects.find(*apply_twice);
  const draft::ProcedureEffectSummary *caller_summary = effects.find(*caller);
  const draft::ProcedureEffectSummary *ordered_summary =
      effects.find(*ordered_caller);
  const draft::ProcedureEffectSummary *recursive_summary =
      effects.find(*recursive_caller);
  const draft::ProcedureEffectSummary *tuple_assignment_summary =
      effects.find(*tuple_assignment_caller);
  EXPECT(state, apply_summary != nullptr);
  EXPECT(state, box_summary != nullptr);
  EXPECT(state, twice_summary != nullptr);
  EXPECT(state, caller_summary != nullptr);
  EXPECT(state, ordered_summary != nullptr);
  EXPECT(state, recursive_summary != nullptr);
  EXPECT(state, tuple_assignment_summary != nullptr);
  if (apply_summary == nullptr || box_summary == nullptr ||
      twice_summary == nullptr || caller_summary == nullptr ||
      ordered_summary == nullptr || recursive_summary == nullptr ||
      tuple_assignment_summary == nullptr) {
    return;
  }

  const auto flow = std::find_if(
      apply_summary->effects.begin(),
      apply_summary->effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::FlowCall;
      });
  EXPECT(state, flow != apply_summary->effects.end());
  if (flow != apply_summary->effects.end()) {
    EXPECT(state, flow->flow_parameter == 0);
    EXPECT(state, flow->flow_arguments.size() == 1);
    if (flow->flow_arguments.size() == 1) {
      EXPECT(state, flow->flow_arguments.front().fields.size() == 1);
      if (flow->flow_arguments.front().fields.size() == 1) {
        const draft::ProcedureValueSummary &nested =
            flow->flow_arguments.front().fields.front().value;
        EXPECT(state, nested.flow_slots.size() == 1);
        if (nested.flow_slots.size() == 1) {
          EXPECT(state, nested.flow_slots.front().parameter == 1);
        }
      }
    }
  }
  const auto box_flow = std::find_if(
      box_summary->effects.begin(),
      box_summary->effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::FlowCall;
      });
  EXPECT(state, box_flow != box_summary->effects.end());
  if (box_flow != box_summary->effects.end()) {
    EXPECT(state,
        box_flow->flow_path == std::vector<std::string>{"invoke"});
    EXPECT(state, box_flow->flow_arguments.size() == 1);
  }
  const auto twice_flow = std::find_if(
      twice_summary->effects.begin(),
      twice_summary->effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::FlowCall;
      });
  EXPECT(state, twice_flow != twice_summary->effects.end());
  if (twice_flow != twice_summary->effects.end()) {
    EXPECT(state, twice_flow->flow_parameter == 0);
    EXPECT(state, twice_flow->flow_arguments.size() == 2);
  }
  EXPECT(state,
      has_effect(*caller_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      !has_effect(*caller_summary, draft::EffectKind::UnknownCall));
  EXPECT(state,
      !has_effect(*ordered_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      !has_effect(*ordered_summary, draft::EffectKind::UnknownCall));
  EXPECT(state,
      !has_effect(*recursive_summary, draft::EffectKind::UnknownCall));
  EXPECT(state,
      has_effect(*tuple_assignment_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state,
      !has_effect(*tuple_assignment_summary, draft::EffectKind::UnknownCall));
}

} // namespace

int main() {
  TestState state;
  test_recursive_effect_component(state);
  test_transitive_effects(state);
  test_raw_string_data_effect(state);
  test_target_and_package_assembly_summaries(state);
  test_typed_and_context_flow_paths(state);
  test_returned_procedure_flow(state);
  test_pointer_field_write_flow(state);
  test_higher_order_flow(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " effect summary expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all effect summary tests passed\n";
  return EXIT_SUCCESS;
}
