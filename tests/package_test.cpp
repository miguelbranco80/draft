// Deterministic folder-package selection and package-name validation tests.
//
// The test creates one isolated physical directory because the package loader's
// contract is explicitly about filesystem enumeration and filename selection.
// It verifies direct-child selection, exact target tags, test/benchmark gates,
// assembly participation, canonical sorting, parsing, and cross-file package
// name consistency.

#include "base/work_graph.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include "test_directory.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "package_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

struct TemporaryPackage {
  draft::test::TemporaryDirectory directory{"draft-bootstrap-package-test"};
  std::filesystem::path path;

  TemporaryPackage() {
    path = directory.path();
    std::error_code error;
    std::filesystem::create_directories(path / "nested", error);
    if (error) {
      std::cerr << "cannot create temporary package: " << error.message() << '\n';
      std::exit(EXIT_FAILURE);
    }
  }

};

void populate_package(const TemporaryPackage &package) {
  write_file(package.path / "package.draft", "package demo\nRoot :: 1\n");
  write_file(package.path / "extra.draft", "package demo\nExtra :: 2\n");
  write_file(
      package.path / "platform@aarch64-macos.draft",
      "package demo\nPlatform :: 3\n");
  write_file(
      package.path / "platform@x86_64-linux.draft",
      "package demo\nWrong_Target :: 4\n");
  write_file(package.path / "feature_test.draft", "package demo\nTest :: 5\n");
  write_file(package.path / "feature_bench.draft", "package demo\nBench :: 6\n");
  write_file(package.path / "native.s", ".text\n");
  write_file(package.path / "README.md", "ignored\n");
  write_file(package.path / "nested" / "hidden.draft", "package wrong\n");
}

void test_default_selection(TestState &state) {
  TemporaryPackage temporary;
  populate_package(temporary);

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::PackageLoadOptions options;
  options.file_tag = "aarch64-macos";
  const draft::PackageLoadResult result =
      draft::load_package(sources, temporary.path.string(), options, diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.package.short_name == "demo");
  EXPECT(state, result.package.files.size() == 4);
  EXPECT(state, result.package.files[0].relative_name == "extra.draft");
  EXPECT(state, result.package.files[1].relative_name == "native.s");
  EXPECT(state, result.package.files[2].relative_name == "package.draft");
  EXPECT(state, result.package.files[3].relative_name == "platform@aarch64-macos.draft");
  EXPECT(state, result.package.files[1].kind == draft::PackageFileKind::AssemblySource);
  EXPECT(state, !result.package.files[1].syntax.has_value());
}

void test_validation_file_selection(TestState &state) {
  TemporaryPackage temporary;
  populate_package(temporary);

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::PackageLoadOptions options;
  options.file_tag = "aarch64-macos";
  options.include_tests = true;
  options.include_benchmarks = true;
  const draft::PackageLoadResult result =
      draft::load_package(sources, temporary.path.string(), options, diagnostics);

  EXPECT(state, result.ok);
  EXPECT(state, result.package.files.size() == 6);
}

void test_mismatched_package_name(TestState &state) {
  TemporaryPackage temporary;
  populate_package(temporary);
  write_file(temporary.path / "z_mismatch.draft", "package other\nValue :: 7\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::PackageLoadOptions options;
  options.file_tag = "aarch64-macos";
  const draft::PackageLoadResult result =
      draft::load_package(sources, temporary.path.string(), options, diagnostics);

  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("does not match package name") != std::string::npos);
}

void test_contextual_core_package_name(TestState &state) {
  TemporaryPackage temporary;
  std::error_code error;
  std::filesystem::remove_all(temporary.path, error);
  error.clear();
  std::filesystem::create_directories(temporary.path, error);
  EXPECT(state, !error);
  if (error) return;
  write_file(
      temporary.path / "package.draft",
      "package memory\nValue :: 1\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::PackageLoadOptions options;
  options.file_tag = "aarch64-macos";
  const draft::PackageLoadResult result =
      draft::load_package(sources, temporary.path.string(), options, diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.package.short_name == "memory");
}

// The package loader's observable state must not expose worker completion
// order. This runs the same malformed multi-file package through the inline
// oracle and a four-worker executor, then compares source IDs, exact bytes,
// complete trees, and rendered diagnostics.
void test_complete_file_parallelism_preserves_publication(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "00_package.draft",
      "package demo\nRoot :: 1\n");
  write_file(
      temporary.path / "10_wrong_name.draft",
      "package other\nOther :: 2\n");
  write_file(
      temporary.path / "20_bad_syntax.draft",
      "package demo\nBroken ::\n");
  write_file(
      temporary.path / "30_extra.draft",
      "package demo\nExtra :: 3\n");

  draft::SourceManager sequential_sources;
  draft::DiagnosticSink sequential_diagnostics;
  draft::WorkExecutor sequential_executor;
  draft::PackageLoadOptions sequential_options;
  sequential_options.file_tag = "aarch64-macos";
  sequential_options.work_executor = &sequential_executor;
  sequential_options.file_worker_count = 1;
  const draft::PackageLoadResult sequential = draft::load_package(
      sequential_sources,
      temporary.path.string(),
      sequential_options,
      sequential_diagnostics);

  draft::SourceManager parallel_sources;
  draft::DiagnosticSink parallel_diagnostics;
  draft::WorkExecutor parallel_executor;
  draft::PackageLoadOptions parallel_options;
  parallel_options.file_tag = "aarch64-macos";
  parallel_options.work_executor = &parallel_executor;
  parallel_options.file_worker_count = 4;
  const draft::PackageLoadResult parallel = draft::load_package(
      parallel_sources,
      temporary.path.string(),
      parallel_options,
      parallel_diagnostics);

  EXPECT(state, !sequential.ok);
  EXPECT(state, !parallel.ok);
  EXPECT(state, sequential.package.short_name == parallel.package.short_name);
  EXPECT(state, sequential.package.files.size() == parallel.package.files.size());
  EXPECT(state, sequential_sources.file_count() == parallel_sources.file_count());
  const std::size_t shared_file_count = std::min(
      sequential.package.files.size(), parallel.package.files.size());
  for (std::size_t index = 0; index < shared_file_count; ++index) {
    const draft::LoadedPackageFile &sequential_file =
        sequential.package.files[index];
    const draft::LoadedPackageFile &parallel_file =
        parallel.package.files[index];
    EXPECT(state, sequential_file.relative_name == parallel_file.relative_name);
    EXPECT(state, sequential_file.kind == parallel_file.kind);
    EXPECT(state, sequential_file.source.value == index);
    EXPECT(state, parallel_file.source.value == index);
    EXPECT(
        state,
        sequential_sources.text(sequential_file.source) ==
            parallel_sources.text(parallel_file.source));
    EXPECT(
        state,
        sequential_file.syntax.has_value() == parallel_file.syntax.has_value());
    if (sequential_file.syntax.has_value() &&
        parallel_file.syntax.has_value()) {
      EXPECT(state, sequential_file.syntax->file() == sequential_file.source);
      EXPECT(state, parallel_file.syntax->file() == parallel_file.source);
      EXPECT(
          state,
          draft::dump_syntax_tree(*sequential_file.syntax) ==
              draft::dump_syntax_tree(*parallel_file.syntax));
    }
  }

  // Both the package-name error and later parser error must remain in canonical
  // filename order even though four workers may finish in any order.
  const std::string sequential_rendered =
      draft::render_diagnostics(sequential_sources, sequential_diagnostics);
  const std::string parallel_rendered =
      draft::render_diagnostics(parallel_sources, parallel_diagnostics);
  EXPECT(state, sequential_rendered == parallel_rendered);
  EXPECT(
      state,
      sequential_rendered.find("10_wrong_name.draft") <
          sequential_rendered.find("20_bad_syntax.draft"));
}

} // namespace

int main() {
  TestState state;
  test_default_selection(state);
  test_validation_file_selection(state);
  test_mismatched_package_name(state);
  test_contextual_core_package_name(state);
  test_complete_file_parallelism_preserves_publication(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " package test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all package tests passed\n";
  return EXIT_SUCCESS;
}
