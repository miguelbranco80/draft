// Dependency-ordered full provider-free compiler pipeline tests.

#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
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
    EXPECT(state, result.packages[0]->llvm.text.find(
        "define hidden void @__draft.assert") != std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find(
        "declare hidden void @__draft.assert") != std::string::npos);
  }
}

void test_hosted_entry_contract(TestState &state) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) / "draft-bootstrap-entry-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  // i32 is a perfectly valid ordinary result type, but the hosted entry
  // contract deliberately accepts only void or the target-native `int`.
  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n"
            "main :: proc() -> i32 {\n"
            "    return 0\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  if (rendered.find("main result must be void or int") == std::string::npos) {
    std::cerr << rendered;
  }
  EXPECT(state, rendered.find("main result must be void or int") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_compiler_distributed_core(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-runtime",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.graph.packages.size() == 5);

  // The source-visible Context and the entry shim intentionally duplicate one
  // versioned physical contract.  This fixed layout check makes drift fail in
  // the ordinary compiler suite instead of appearing as a callback crash.
  const draft::CompiledPackage *runtime = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_identity == "draft-core-test-v1" &&
        package->identity.root_relative_path == "runtime") {
      runtime = &*package;
      break;
    }
  }
  EXPECT(state, runtime != nullptr);
  if (runtime == nullptr) return;
  const std::optional<draft::SymbolId> context =
      runtime->semantics.package.symbols.lookup_direct(
          runtime->semantics.package.package_scope, "Context");
  EXPECT(state, context.has_value());
  if (!context.has_value()) return;
  const draft::Type &type = runtime->semantics.package.types.type(
      runtime->semantics.package.symbols.symbol(*context).type);
  EXPECT(state, type.layout.known);
  EXPECT(state, type.layout.size == 96);
  EXPECT(state, type.layout.alignment == 8);
  EXPECT(state, type.member_offsets == std::vector<std::uint64_t>({
      0, 16, 32, 40, 56, 72, 80, 88}));
}

} // namespace

int main() {
  TestState state;
  test_multi_package_native_pipeline(state);
  test_hosted_entry_contract(state);
  test_compiler_distributed_core(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " compiler pipeline expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all compiler pipeline tests passed\n";
  return EXIT_SUCCESS;
}
