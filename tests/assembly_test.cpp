// Parsed AArch64 assembly validation and MIR/LLVM handoff tests.

#include "assembly/aarch64.h"
#include "backend/llvm_ir.h"
#include "mir/lower.h"
#include "sema/body_checker.h"
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
      std::cerr << "assembly_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct CheckedAssembly {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics;
  draft::BodyCheckResult bodies;
  draft::AssemblyProgram assembly;

  explicit CheckedAssembly(std::string text) {
    loaded.short_name = "assembly";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
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
    if (bodies.ok) {
      assembly = draft::analyze_aarch64_assembly(
          sources,
          loaded,
          target,
          semantics.package,
          bodies.program,
          diagnostics);
    }
  }
};

void test_valid_fixed_register_regions(TestState &state) {
  CheckedAssembly source(R"draft(
package assembly

increment :: proc(value: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = value
        out x0
        add x0, x0, #1
    }
}

barrier :: proc() {
    asm aarch64 {
        clobber memory
        dmb ish
    }
}

main :: proc() -> i32 {
    return cast[i32](increment(40))
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, source.assembly.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.assembly.regions.size() == 2);
  if (source.assembly.regions.size() == 2) {
    EXPECT(state, source.assembly.regions[0].llvm_constraints == "={x0},0");
    EXPECT(state, source.assembly.regions[0].instruction_text ==
        "add x0, x0, #1");
    EXPECT(state, source.assembly.regions[1].llvm_constraints == "~{memory}");
  }

  const draft::MirLoweringResult mir = draft::lower_package_to_mir(
      source.semantics.package,
      source.bodies.program,
      source.assembly,
      source.diagnostics);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "assembly"};
  options.emit_program_entry = true;
  const draft::LlvmIrResult llvm = draft::emit_llvm_ir(
      source.target,
      source.sources,
      options,
      source.semantics.package,
      source.semantics.constants,
      mir.program,
      source.diagnostics);
  EXPECT(state, mir.ok);
  EXPECT(state, llvm.ok);
  EXPECT(state, llvm.text.find(
      "asm sideeffect \"add x0, x0, #1\", \"={x0},0\"") !=
      std::string::npos);
  EXPECT(state, llvm.text.find(
      "asm sideeffect \"dmb ish\", \"~{memory}\"") !=
      std::string::npos);
}

void test_invalid_effects_and_architecture(TestState &state) {
  CheckedAssembly source(R"draft(
package assembly

bad_arch :: proc(value: u64) -> u64 {
    return asm x86_64 -> u64 {
        in x0 = value
        out x0
        add x0, x0, #1
    }
}

bad_effects :: proc(value: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = value
        out x0
        add x1, x0, #1
        cmp x0, #0
        b somewhere
    }
}
)draft");

  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.assembly.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("does not match target") != std::string::npos);
  EXPECT(state, rendered.find("must be declared as an output or clobber") !=
      std::string::npos);
  EXPECT(state, rendered.find("without 'clobber flags'") != std::string::npos);
  EXPECT(state, rendered.find("unsupported instruction 'b'") != std::string::npos);
  EXPECT(state, rendered.find("output register is never written") !=
      std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_valid_fixed_register_regions(state);
  test_invalid_effects_and_architecture(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " assembly expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all assembly tests passed\n";
  return EXIT_SUCCESS;
}
