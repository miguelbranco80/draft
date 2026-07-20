// Deterministic folder-package selection and package-name validation tests.
//
// The test creates one isolated physical directory because the package loader's
// contract is explicitly about filesystem enumeration and filename selection.
// It verifies direct-child selection, exact target tags, test/benchmark gates,
// assembly participation, canonical sorting, parsing, and cross-file package
// name consistency.

#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include "test_directory.h"

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

} // namespace

int main() {
  TestState state;
  test_default_selection(state);
  test_validation_file_selection(state);
  test_mismatched_package_name(state);
  test_contextual_core_package_name(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " package test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all package tests passed\n";
  return EXIT_SUCCESS;
}
