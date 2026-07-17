// Dependency-ordered full provider-free compiler pipeline tests.

#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

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
      std::cerr << "compiler_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_multi_package_native_pipeline(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages/app",
      std::move(options),
      diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 2);
  EXPECT(state, result.packages.size() == 2);
  EXPECT(state, result.packages[0].has_value());
  EXPECT(state, result.packages[1].has_value());
  if (result.packages[0].has_value() && result.packages[1].has_value()) {
    EXPECT(state, result.packages[0]->llvm.ok);
    EXPECT(state, result.packages[1]->llvm.ok);
    EXPECT(state, result.packages[0]->llvm.text.find("define i32 @main") !=
        std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find("define i32 @main") ==
        std::string::npos);
    EXPECT(state, result.packages[0]->llvm.text.find(
        "draft.workspace.lib_2Fmath.translate") != std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find(
        "draft.workspace.lib_2Fmath.translate") != std::string::npos);
  }
}

} // namespace

int main() {
  TestState state;
  test_multi_package_native_pipeline(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " compiler pipeline expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all compiler pipeline tests passed\n";
  return EXIT_SUCCESS;
}
