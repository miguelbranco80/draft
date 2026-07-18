// Validation source selection, signature proof, ordering, and harness tests.

#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
      std::cerr << "validation_discovery_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

[[nodiscard]] bool write_file(
    const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  return output.good();
}

[[nodiscard]] draft::CompileWorkspaceResult compile_validation(
    const std::filesystem::path &root,
    draft::ValidationKind kind,
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-validation-test-v1";
  options.validation_kind = kind;
  options.lower_mir = true;
  options.emit_llvm = true;
  return draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
}

void test_checked_test_harness(TestState &state) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) /
      "draft-validation-discovery-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  EXPECT(state, write_file(
      root / "app" / "package.draft",
      "package app\n"
      "main :: proc() {\n"
      "    assert(false)\n"
      "}\n"));
  // The target-qualified suffix is part of validation selection. Procedure
  // order in this file is intentionally opposite the canonical name order.
  EXPECT(state, write_file(
      root / "app" / "suite_test@aarch64-macos.draft",
      "package app\n"
      "import core/testing\n"
      "test_zeta :: proc(test: ^testing.Test) {\n"
      "    testing.expect(test, true)\n"
      "}\n"
      "test_alpha :: proc(test: ^testing.Test) {\n"
      "    testing.expect(test, false)\n"
      "}\n"));

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult result = compile_validation(
      root, draft::ValidationKind::Test, sources, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.validation_entries.size() == 2);
  if (result.validation_entries.size() == 2) {
    EXPECT(state, result.validation_entries[0].procedure == "test_alpha");
    EXPECT(state, result.validation_entries[1].procedure == "test_zeta");
    EXPECT(state, result.validation_entries[0].state_size == 24);
    EXPECT(state, result.validation_entries[0].state_alignment == 8);
    EXPECT(state, result.validation_entries[0].failure_offset == 8);
  }
  if (result.graph.root_package.is_valid()) {
    const std::optional<draft::CompiledPackage> &root_package =
        result.packages[result.graph.root_package.value];
    EXPECT(state, root_package.has_value());
    if (root_package.has_value()) {
      const std::string &llvm = root_package->llvm.text;
      EXPECT(state, llvm.find("define i32 @main") != std::string::npos);
      EXPECT(state, llvm.find(
          "call void @\"draft.workspace.app.test_5Falpha\"") !=
          std::string::npos);
      EXPECT(state, llvm.find(
          "call void @\"draft.workspace.app.main\"") ==
          std::string::npos);
      EXPECT(state, llvm.find("alloca [24 x i8], align 8") !=
          std::string::npos);
      EXPECT(state, llvm.find(
          "getelementptr i8, ptr %validation.state.0, i64 8") !=
          std::string::npos);
      EXPECT(state, llvm.find(
          "@\"__draft.runtime.reset_temporary_allocator\"()") !=
          std::string::npos);
    }
  }

  std::filesystem::remove_all(root, error);
}

void test_invalid_signature_is_rejected(TestState &state) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) /
      "draft-validation-signature-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  EXPECT(state, write_file(root / "app" / "package.draft", "package app\n"));
  EXPECT(state, write_file(
      root / "app" / "bad_test.draft",
      "package app\n"
      "import core/testing\n"
      "test_bad :: proc(test: ^testing.Test) -> int {\n"
      "    return 0\n"
      "}\n"));

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult result = compile_validation(
      root, draft::ValidationKind::Test, sources, diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("must return void") != std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_spoofed_core_nominal_is_rejected(TestState &state) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) /
      "draft-validation-spoof-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "testing", error);
  EXPECT(state, !error);
  if (error) return;

  EXPECT(state, write_file(
      root / "testing" / "package.draft",
      "package testing\n"
      "pub Test :: struct {\n"
      "    checks: usize,\n"
      "    failures: usize,\n"
      "    user: rawptr,\n"
      "}\n"));
  EXPECT(state, write_file(root / "app" / "package.draft", "package app\n"));
  EXPECT(state, write_file(
      root / "app" / "spoof_test.draft",
      "package app\n"
      "import testing\n"
      "test_spoof :: proc(test: ^testing.Test) {}\n"));

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult result = compile_validation(
      root, draft::ValidationKind::Test, sources, diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  if (rendered.find("must accept ^testing.Test") == std::string::npos) {
    std::cerr << rendered;
  }
  EXPECT(state, rendered.find("must accept ^testing.Test") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  TestState state;
  test_checked_test_harness(state);
  test_invalid_signature_is_rejected(state);
  test_spoofed_core_nominal_is_rejected(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
