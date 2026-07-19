// Canonical package identity and recursive import-graph tests.
//
// These tests build isolated workspace, dependency, and core roots. They verify
// that source import spelling selects only an explicit root, repeated imports
// reuse a PackageId, physical paths do not enter semantic identity, and import
// cycles fail at the source edge that closes the cycle.

#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/workspace.h"

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
      std::cerr << "workspace_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    std::cerr << "cannot create test directory: " << error.message() << '\n';
    std::exit(EXIT_FAILURE);
  }
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

struct TemporaryWorkspace {
  std::filesystem::path path;

  TemporaryWorkspace() {
    std::error_code error;
    path = std::filesystem::temp_directory_path(error) / "draft-bootstrap-workspace-test";
    if (error) {
      std::cerr << "cannot find temporary directory: " << error.message() << '\n';
      std::exit(EXIT_FAILURE);
    }
    std::filesystem::remove_all(path, error);
    error.clear();
    std::filesystem::create_directories(path, error);
    if (error) {
      std::cerr << "cannot create temporary workspace: " << error.message() << '\n';
      std::exit(EXIT_FAILURE);
    }
  }

  ~TemporaryWorkspace() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

draft::WorkspaceLoadOptions options_for(const TemporaryWorkspace &temporary) {
  draft::WorkspaceLoadOptions options;
  options.workspace_directory = (temporary.path / "workspace").string();
  options.core_directory = (temporary.path / "distribution" / "core").string();
  options.core_content_identity = "draft-core-test-content";
  options.dependencies.push_back({
      "vendor",
      (temporary.path / "dependencies" / "vendor").string(),
      "vendor-test-content",
  });
  options.package_options.file_tag = "aarch64-macos";
  return options;
}

void populate_graph(const TemporaryWorkspace &temporary) {
  write_file(
      temporary.path / "workspace" / "app" / "package.draft",
      "package app\n"
      "import lib/math as math\n"
      "import vendor/text as text\n"
      "import core/io\n"
      "main :: proc() {}\n");
  write_file(
      temporary.path / "workspace" / "app" / "second.draft",
      "package app\nimport lib/math as numbers\n");
  write_file(
      temporary.path / "workspace" / "lib" / "math" / "package.draft",
      "package math\npub Answer :: 42\n");
  write_file(
      temporary.path / "dependencies" / "vendor" / "text" / "package.draft",
      "package text\npub Size :: 1\n");
  write_file(
      temporary.path / "distribution" / "core" / "io" / "package.draft",
      "package io\npub Handle :: distinct u64\n");
}

void test_recursive_graph(TestState &state) {
  TemporaryWorkspace temporary;
  populate_graph(temporary);

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::WorkspaceLoadResult result = draft::load_workspace(
      sources,
      (temporary.path / "workspace" / "app").string(),
      options_for(temporary),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }

  EXPECT(state, result.ok);
  EXPECT(state, result.graph.root_package.is_valid());
  EXPECT(state, result.graph.packages.size() == 4);
  EXPECT(state, result.graph.imports.size() == 4);
  EXPECT(state, result.graph.roots.size() == 3);
  if (!result.graph.root_package.is_valid() || result.graph.packages.size() < 4) {
    return;
  }
  const draft::WorkspacePackage &app = result.graph.package(result.graph.root_package);
  EXPECT(state, app.identity.root_identity == "workspace");
  EXPECT(state, app.identity.root_relative_path == "app");
  EXPECT(state, result.graph.packages[1].identity.root_relative_path == "lib/math");
  EXPECT(state, result.graph.packages[2].identity.root_identity == "vendor-test-content");
  EXPECT(state, result.graph.packages[2].identity.root_relative_path == "text");
  EXPECT(state, result.graph.packages[3].identity.root_identity == "draft-core-test-content");
  EXPECT(state, result.graph.packages[3].identity.root_relative_path == "io");
  EXPECT(state, result.graph.imports[0].imported_package == result.graph.imports[3].imported_package);
  EXPECT(state, draft::display_package_identity(app.identity) == "workspace:app");
}

void test_cycle_diagnostic(TestState &state) {
  TemporaryWorkspace temporary;
  write_file(
      temporary.path / "workspace" / "a" / "package.draft",
      "package a\nimport b\n");
  write_file(
      temporary.path / "workspace" / "b" / "package.draft",
      "package b\nimport a\n");
  // Configured roots are required to exist even when this particular graph has
  // no imports through them.
  write_file(
      temporary.path / "dependencies" / "vendor" / "placeholder" / "package.draft",
      "package placeholder\n");
  write_file(
      temporary.path / "distribution" / "core" / "placeholder" / "package.draft",
      "package placeholder\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::WorkspaceLoadResult result = draft::load_workspace(
      sources,
      (temporary.path / "workspace" / "a").string(),
      options_for(temporary),
      diagnostics);

  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("package import cycle") != std::string::npos);
  EXPECT(state, rendered.find("workspace:a") != std::string::npos);
}

void test_ambiguous_dependency_prefixes(TestState &state) {
  TemporaryWorkspace temporary;
  write_file(
      temporary.path / "workspace" / "app" / "package.draft",
      "package app\n");
  write_file(
      temporary.path / "dependencies" / "vendor" / "placeholder" / "package.draft",
      "package placeholder\n");
  write_file(
      temporary.path / "distribution" / "core" / "placeholder" / "package.draft",
      "package placeholder\n");

  draft::WorkspaceLoadOptions options = options_for(temporary);
  options.dependencies.push_back({
      "vendor/nested",
      (temporary.path / "dependencies" / "vendor").string(),
      "nested-test-content",
  });
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::WorkspaceLoadResult result = draft::load_workspace(
      sources,
      (temporary.path / "workspace" / "app").string(),
      options,
      diagnostics);

  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("ambiguous dependency import prefixes") != std::string::npos);
}

void test_import_depth_is_bounded(TestState &state) {
  TemporaryWorkspace temporary;
  constexpr std::size_t package_count = 320;
  for (std::size_t index = 0; index < package_count; ++index) {
    std::string source = "package package_" + std::to_string(index) + "\n";
    if (index + 1 < package_count) {
      source += "import package_" + std::to_string(index + 1) + "\n";
    }
    write_file(
        temporary.path / "workspace" /
            ("package_" + std::to_string(index)) / "package.draft",
        source);
  }
  // options_for selects these roots even though the deep graph never imports
  // them. Their required existence must not obscure the recursion diagnostic.
  write_file(
      temporary.path / "dependencies" / "vendor" / "placeholder" /
          "package.draft",
      "package placeholder\n");
  write_file(
      temporary.path / "distribution" / "core" / "placeholder" /
          "package.draft",
      "package placeholder\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::WorkspaceLoadResult result = draft::load_workspace(
      sources,
      (temporary.path / "workspace" / "package_0").string(),
      options_for(temporary),
      diagnostics);

  EXPECT(state, !result.ok);
  const std::string rendered =
      draft::render_diagnostics(sources, diagnostics);
  EXPECT(state,
      rendered.find(
          "package import depth exceeds the implementation limit of 256") !=
          std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_recursive_graph(state);
  test_cycle_diagnostic(state);
  test_ambiguous_dependency_prefixes(state);
  test_import_depth_is_bounded(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " workspace test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all workspace tests passed\n";
  return EXIT_SUCCESS;
}
