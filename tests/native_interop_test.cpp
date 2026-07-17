// C ABI validation and exact linker-symbol emission tests.

#include "backend/llvm_ir.h"
#include "interop/native.h"
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
      std::cerr << "native_interop_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct CheckedSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics;
  draft::BodyCheckResult bodies;

  explicit CheckedSource(std::string text) {
    loaded.short_name = "native";
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
        diagnostics);
  }
};

void test_valid_import_and_export(TestState &state) {
  CheckedSource source(R"draft(
package native

foreign libc {
    puts :: c "puts" proc(text: cstring) -> i32
}

export increment :: c "draft_increment" proc(value: i32) -> i32 {
    return value + 1
}
)draft");

  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package, source.bodies.program, source.diagnostics);
  const draft::MirLoweringResult mir = draft::lower_package_to_mir(
      source.semantics.package, source.bodies.program, source.diagnostics);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "native"};
  const draft::LlvmIrResult llvm = draft::emit_llvm_ir(
      source.target,
      options,
      source.semantics.package,
      source.semantics.constants,
      mir.program,
      source.diagnostics);
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, native.ok);
  EXPECT(state, native.providers.size() == 1);
  EXPECT(state, native.providers[0] == "libc");
  EXPECT(state, mir.ok);
  EXPECT(state, llvm.ok);
  EXPECT(state, llvm.text.find("declare i32 @\"puts\"(ptr)") !=
      std::string::npos);
  EXPECT(state, llvm.text.find("define i32 @\"draft_increment\"(i32 %arg0)") !=
      std::string::npos);
}

void test_invalid_c_boundaries(TestState &state) {
  CheckedSource source(R"draft(
package native

export ordinary :: proc() {
}

foreign provider {
    bad_slice :: c proc(value: []u8)
}
)draft");
  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package, source.bodies.program, source.diagnostics);
  EXPECT(state, !native.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("C-ABI-legal 'c proc'") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_valid_import_and_export(state);
  test_invalid_c_boundaries(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " native interop expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all native interop tests passed\n";
  return EXIT_SUCCESS;
}
