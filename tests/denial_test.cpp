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
#include <vector>

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

  explicit DenialSource(
      std::string text,
      std::vector<draft::ForeignProviderAudit> provider_audits = {}) {
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
    effects = draft::summarize_package_effects(
        semantics.package, bodies.program, &target, provider_audits);
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

void test_statement_selector_ignores_later_shadow(TestState &state) {
  DenialSource source(R"draft(package denials

forbidden :: proc() {
    assert(true)
}

bad :: proc() {
    deny forbidden {
        // This call resolves before the later local declaration and therefore
        // reaches the outer procedure selected by the denial.
        forbidden()
        forbidden := 0
    }
}
)draft");
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.denials_ok);
  EXPECT(state, source.diagnostics.error_count() == 1);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("denied declaration") != std::string::npos);
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

void test_audited_foreign_effect(TestState &state) {
  draft::ForeignProviderAudit audit;
  audit.provider = "custom_provider";
  draft::ForeignAuditSymbol symbol;
  symbol.linker_name = "native_step";
  draft::ForeignAuditEffect assembly;
  assembly.kind = draft::EffectKind::Assembly;
  assembly.detail = "audited foreign assembly";
  symbol.effects.push_back(assembly);
  audit.symbols.push_back(std::move(symbol));
  std::vector<draft::ForeignProviderAudit> audits;
  audits.push_back(std::move(audit));

  DenialSource source(R"draft(package denials

foreign custom_provider {
    native_step :: c proc()
}

deny asm {
    bad :: proc() {
        native_step()
    }
}
)draft", std::move(audits));
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.denials_ok);
  EXPECT(state, source.diagnostics.error_count() == 1);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("denied assembly") != std::string::npos);
  EXPECT(state, rendered.find("unknown call") == std::string::npos);
}

void test_typed_field_flow_substitution(TestState &state) {
  DenialSource source(R"draft(package denials

Callback_Box :: struct {
    callback: proc(),
}

danger :: proc() {
    assert(true)
}

invoke_box :: proc(box: Callback_Box) {
    box.callback()
}

deny assert {
    bad :: proc() {
        box: Callback_Box
        box.callback = danger
        invoke_box(box)
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

void test_transitive_declaration_denial(TestState &state) {
  DenialSource source(R"draft(package denials

forbidden :: proc() {}

helper :: proc() {
    forbidden()
}

deny forbidden {
    bad :: proc() {
        helper()
    }
}
)draft");
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.denials_ok);
  EXPECT(state, source.diagnostics.error_count() == 1);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("denied declaration") != std::string::npos);
  EXPECT(state, rendered.find("unknown call") == std::string::npos);
}

void test_returned_procedure_substitution(TestState &state) {
  DenialSource source(R"draft(package denials

identity :: proc(callback: proc()) -> proc() {
    return callback
}

danger :: proc() {
    assert(true)
}

deny assert {
    bad :: proc() {
        selected := identity(danger)
        selected()
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

void test_pointer_field_write_substitution(TestState &state) {
  DenialSource source(R"draft(package denials

Callback_Box :: struct {
    callback: proc(),
}

install :: proc(destination: ^Callback_Box, callback: proc()) {
    destination^.callback = callback
}

danger :: proc() {
    assert(true)
}

deny assert {
    bad :: proc() {
        box: Callback_Box
        install(&box, danger)
        box.callback()
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

void test_higher_order_substitution(TestState &state) {
  DenialSource source(R"draft(package denials

invoke_one :: proc(callback: proc()) {
    callback()
}

apply :: proc(
    higher: proc(callback: proc()),
    callback: proc(),
) {
    higher(callback)
}

danger :: proc() {
    assert(true)
}

deny assert {
    bad :: proc() {
        apply(invoke_one, danger)
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

void test_nested_procedure_inherits_statement_denial(TestState &state) {
  DenialSource source(R"draft(package denials

outer :: proc() {
    deny assert {
        nested :: proc() {
            assert(true)
        }
        nested()
    }
}
)draft");
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.denials_ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("denied assert") != std::string::npos);
  EXPECT(state, rendered.find("denial is established here") != std::string::npos);
  bool exact_source_range = false;
  for (const draft::Diagnostic &diagnostic : source.diagnostics.diagnostics()) {
    if (diagnostic.message.find("denied assert") == std::string::npos) continue;
    exact_source_range = exact_source_range ||
        source.sources.text(diagnostic.range) == "assert(true)" ||
        source.sources.text(diagnostic.range) == "nested()";
  }
  EXPECT(state, exact_source_range);
}

} // namespace

int main() {
  TestState state;
  test_denial_violations(state);
  test_unrelated_denial(state);
  test_statement_selector_ignores_later_shadow(state);
  test_flow_slot_substitution(state);
  test_audited_foreign_effect(state);
  test_typed_field_flow_substitution(state);
  test_transitive_declaration_denial(state);
  test_returned_procedure_substitution(state);
  test_pointer_field_write_substitution(state);
  test_higher_order_substitution(state);
  test_nested_procedure_inherits_statement_denial(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " denial expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all denial tests passed\n";
  return EXIT_SUCCESS;
}
