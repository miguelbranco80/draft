// Native direct-reference extraction and artifact liveness tests.
//
// These tests keep whole-source semantic checking separate from native roots.
// They build ordinary checked HIR/effect facts, prove the reference extractor
// sees calls, procedure values, and globals, then prove an unreferenced but
// valid procedure remains outside the artifact closure.

#include "mir/native_reachability.h"

#include "sema/body_checker.h"
#include "sema/effect.h"
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
#include <vector>

namespace {

struct TestState {
  int failures = 0;
  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "native_reachability_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) \
  (state).expect((expression), #expression, __LINE__)

struct CheckedSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics;
  draft::PackageBodyWorkState bodies;
  draft::DirectEffectSummaryResult direct;

  explicit CheckedSource(std::string text) {
    loaded.short_name = "native_refs";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(
        draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    semantics = draft::analyze_package_semantics(
        sources, loaded, target.facts, diagnostics);
    bodies = draft::check_package_bodies(
        sources,
        loaded,
        semantics.selections,
        semantics.package,
        semantics.constants,
        target.facts,
        diagnostics);
    const draft::ImportedProcedureContracts imported =
        draft::imported_procedure_contracts(bodies.package);
    direct = draft::collect_direct_procedure_effects(
        bodies.package, bodies.procedures, imported, &target);
  }
};

[[nodiscard]] bool has_identity(
    const std::vector<draft::NativeSymbolIdentity> &identities,
    std::string_view name) {
  return std::any_of(
      identities.begin(), identities.end(), [&](const auto &identity) {
        return identity.name == name;
      });
}

[[nodiscard]] std::optional<std::size_t> procedure_summary(
    const std::vector<draft::NativeProcedureReferenceSummary> &summaries,
    std::string_view name) {
  for (std::size_t index = 0; index < summaries.size(); ++index) {
    if (summaries[index].procedure.name == name) return index;
  }
  return std::nullopt;
}

void test_checked_and_live_procedures_are_distinct(TestState &state) {
  CheckedSource source(R"draft(package native_refs

counter: i64

helper :: proc() {
    counter += 1
}

unused_but_checked :: proc() {
    counter += 100
}

main :: proc() {
    selected := helper
    selected()
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.bodies.procedures.size() == 3);
  EXPECT(state, source.direct.procedures.size() == 3);
  if (source.diagnostics.has_errors() ||
      source.bodies.procedures.size() != source.direct.procedures.size()) {
    return;
  }

  const draft::PackageIdentity package{"workspace", "native_refs"};
  draft::NativeReachabilityInput input;
  for (std::size_t index = 0; index < source.bodies.procedures.size(); ++index) {
    input.procedures.push_back(draft::collect_native_procedure_references(
        package,
        source.bodies.package,
        source.bodies.procedures[index],
        index,
        source.direct.procedures[index]));
  }
  input.globals = draft::collect_native_global_references(
      package,
      source.bodies.package,
      source.semantics.global_initializers);
  input.procedure_roots.push_back({package, "main"});

  const std::optional<std::size_t> main =
      procedure_summary(input.procedures, "main");
  const std::optional<std::size_t> helper =
      procedure_summary(input.procedures, "helper");
  const std::optional<std::size_t> unused =
      procedure_summary(input.procedures, "unused_but_checked");
  EXPECT(state, main.has_value());
  EXPECT(state, helper.has_value());
  EXPECT(state, unused.has_value());
  EXPECT(state, input.globals.size() == 1);
  if (!main.has_value() || !helper.has_value() || !unused.has_value() ||
      input.globals.size() != 1) {
    return;
  }
  EXPECT(
      state,
      has_identity(input.procedures[*main].procedure_values, "helper"));
  EXPECT(
      state,
      has_identity(input.procedures[*helper].referenced_globals, "counter"));

  const draft::NativeReachabilityResult reachable =
      draft::compute_native_reachability(input);
  EXPECT(state, reachable.ok);
  EXPECT(state, reachable.failure.empty());
  EXPECT(
      state,
      std::find(
          reachable.live_procedures.begin(),
          reachable.live_procedures.end(),
          *main) != reachable.live_procedures.end());
  EXPECT(
      state,
      std::find(
          reachable.live_procedures.begin(),
          reachable.live_procedures.end(),
          *helper) != reachable.live_procedures.end());
  EXPECT(
      state,
      std::find(
          reachable.live_procedures.begin(),
          reachable.live_procedures.end(),
          *unused) == reachable.live_procedures.end());
  EXPECT(state, reachable.live_globals.size() == 1);
}

// A live global can retain a callback even when no live procedure names that
// callback directly. This exercises the procedure/global alternating closure
// and protects aggregate initializer relocation discovery.
void test_global_initializer_reaches_procedure(TestState &state) {
  const draft::PackageIdentity package{"workspace", "manual"};
  draft::NativeReachabilityInput input;
  draft::NativeProcedureReferenceSummary main;
  main.procedure = {package, "main"};
  main.referenced_globals.push_back({package, "callback"});
  draft::NativeProcedureReferenceSummary callback;
  callback.procedure = {package, "run"};
  input.procedures.push_back(std::move(main));
  input.procedures.push_back(std::move(callback));
  draft::NativeGlobalReferenceSummary global;
  global.global = {package, "callback"};
  global.procedure_values.push_back({package, "run"});
  input.globals.push_back(std::move(global));
  input.procedure_roots.push_back({package, "main"});

  const draft::NativeReachabilityResult reachable =
      draft::compute_native_reachability(input);
  EXPECT(state, reachable.ok);
  EXPECT(state, reachable.live_procedures.size() == 2);
  EXPECT(state, reachable.live_globals.size() == 1);
}

void test_missing_internal_edge_is_rejected(TestState &state) {
  const draft::PackageIdentity package{"workspace", "broken"};
  draft::NativeReachabilityInput input;
  draft::NativeProcedureReferenceSummary main;
  main.procedure = {package, "main"};
  main.direct_calls.push_back({package, "missing"});
  input.procedures.push_back(std::move(main));
  input.procedure_roots.push_back({package, "main"});

  const draft::NativeReachabilityResult reachable =
      draft::compute_native_reachability(input);
  EXPECT(state, !reachable.ok);
  EXPECT(state, reachable.failure.find("missing") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_checked_and_live_procedures_are_distinct(state);
  test_global_initializer_reaches_procedure(state);
  test_missing_internal_edge_is_rejected(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " native reachability expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all native reachability tests passed\n";
  return EXIT_SUCCESS;
}
