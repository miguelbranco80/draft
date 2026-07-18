// Lexical, declaration-contract, and transitive denial enforcement tests.

#include "sema/body_checker.h"
#include "sema/denial.h"
#include "sema/effect.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "target/profile.h"
#include "workspace/package.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;
  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "denial_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct DenialSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticAnalysisResult semantics;
  draft::BodyCheckResult bodies;
  draft::EffectSummaryResult effects;
  bool denials_ok = false;

  explicit DenialSource(std::string text) {
    loaded.short_name = "denials";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    const draft::TargetProfile target = draft::make_aarch64_macos_profile();
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
    effects = draft::summarize_package_effects(semantics.package, bodies.program);
    denials_ok = draft::check_package_denials(
        sources,
        loaded,
        semantics.package,
        bodies.program,
        effects,
        diagnostics);
  }
};

void test_denial_violations(TestState &state) {
  DenialSource source(R"draft(package denials

danger :: proc() {
    assert(true)
}

deny assert {
    bad :: proc() {
        danger()
    }
}

bad_index :: proc(values: [^]i64) {
    deny unchecked {
        value := values[0]
    }
}

deny context.user_index {
    bad_context :: proc() -> int {
        return context.user_index
    }
}
)draft");
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.denials_ok);
  EXPECT(state, source.diagnostics.error_count() == 3);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("denied assert") != std::string::npos);
  EXPECT(state, rendered.find("denied unchecked") != std::string::npos);
  EXPECT(state, rendered.find("denied context field") != std::string::npos);
  EXPECT(state, rendered.find("denial is established here") != std::string::npos);
}

void test_unrelated_denial(TestState &state) {
  DenialSource source(R"draft(package denials

deny asm {
    safe :: proc() {
        assert(true)
    }
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, source.denials_ok);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_flow_slot_substitution(TestState &state) {
  DenialSource source(R"draft(package denials

danger :: proc() {
    assert(true)
}

invoke :: proc(callback: proc()) {
    copy := callback
    copy()
}

deny assert {
    bad :: proc() {
        invoke(danger)
    }
}
)draft");
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.denials_ok);
  EXPECT(state, source.diagnostics.error_count() == 1);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("denied assert") != std::string::npos);
  EXPECT(state, rendered.find("unknown call") == std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_denial_violations(state);
  test_unrelated_denial(state);
  test_flow_slot_substitution(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " denial expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all denial tests passed\n";
  return EXIT_SUCCESS;
}
