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

invoke :: proc(callback: proc()) {
    callback()
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
  EXPECT(state, effects.procedures.size() == 3);
  const std::optional<draft::SymbolId> leaf = symbol(semantics.package, "leaf");
  const std::optional<draft::SymbolId> caller = symbol(semantics.package, "caller");
  const std::optional<draft::SymbolId> invoke = symbol(semantics.package, "invoke");
  EXPECT(state, leaf.has_value());
  EXPECT(state, caller.has_value());
  EXPECT(state, invoke.has_value());
  if (!leaf || !caller || !invoke) return;
  const draft::ProcedureEffectSummary *leaf_summary = effects.find(*leaf);
  const draft::ProcedureEffectSummary *caller_summary = effects.find(*caller);
  const draft::ProcedureEffectSummary *invoke_summary = effects.find(*invoke);
  EXPECT(state, leaf_summary != nullptr);
  EXPECT(state, caller_summary != nullptr);
  EXPECT(state, invoke_summary != nullptr);
  if (leaf_summary == nullptr || caller_summary == nullptr || invoke_summary == nullptr) return;
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::PackageGlobal));
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::Unchecked));
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state, has_effect(*leaf_summary, draft::EffectKind::ContextField));
  EXPECT(state, caller_summary->direct_calls.size() == 1);
  EXPECT(state, has_effect(*caller_summary, draft::EffectKind::RuntimeAssert));
  EXPECT(state, has_effect(*caller_summary, draft::EffectKind::Unchecked));
  EXPECT(state, has_effect(*invoke_summary, draft::EffectKind::UnknownCall));
  EXPECT(state, package_interface.declarations.size() == 2);
  if (package_interface.declarations.size() == 2) {
    EXPECT(state, package_interface.declarations[1].name == "caller");
    EXPECT(state, package_interface.declarations[1].has_effect_summary);
    EXPECT(state, package_interface.declarations[1].effects.size() >= 4);
  }
}

} // namespace

int main() {
  TestState state;
  test_transitive_effects(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " effect summary expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all effect summary tests passed\n";
  return EXIT_SUCCESS;
}
