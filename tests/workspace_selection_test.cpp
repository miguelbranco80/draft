// Workspace command-root selection and executable discovery tests.
//
// These fixtures exercise the filesystem policy before compiler graph loading:
// explicit selectors are canonical workspace identities, discovery walks
// ordinary directories in stable order, hidden, symlinked, dependency, and
// expanded-projection trees are absent, and only target-selected surface
// procedure declarations named `main` create executable roots.

#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/selection.h"

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
    if (condition) return;
    ++failures;
    std::cerr << "workspace_selection_test.cpp:" << line
              << ": expectation failed: " << expression << '\n';
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
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    std::cerr << "cannot write test file: " << path << '\n';
    std::exit(EXIT_FAILURE);
  }
}

struct TemporaryTree {
  draft::test::TemporaryDirectory directory{
      "draft-workspace-selection-test"};
  std::filesystem::path root;
  std::filesystem::path workspace;
  std::filesystem::path core;

  TemporaryTree() {
    root = directory.path();
    std::error_code error;
    workspace = root / "workspace";
    core = root / "distribution" / "core";
    std::filesystem::create_directories(workspace, error);
    if (error) {
      std::cerr << "cannot create temporary workspace: " << error.message() << '\n';
      std::exit(EXIT_FAILURE);
    }
    std::filesystem::create_directories(core, error);
    if (error) {
      std::cerr << "cannot create temporary core root: " << error.message() << '\n';
      std::exit(EXIT_FAILURE);
    }
  }

};

draft::WorkspaceLoadOptions discovery_options(const TemporaryTree &tree) {
  draft::WorkspaceLoadOptions options;
  options.workspace_directory = tree.workspace.string();
  options.core_directory = tree.core.string();
  options.core_content_identity = "selection-test-core";
  options.package_options.file_tag = "aarch64-macos";
  return options;
}

void populate_discovery_tree(TemporaryTree &tree) {
  write_file(
      tree.workspace / "package.draft",
      "package root\nmain :: proc() {}\n");
  write_file(
      tree.workspace / "app" / "package.draft",
      "package app\nmain :: proc() {}\n");
  write_file(
      tree.workspace / "lib" / "math" / "package.draft",
      "package math\npub Answer :: 42\n");
  write_file(
      tree.workspace / "tools" / "admin" / "package.draft",
      "package admin\n"
      "when true {\n"
      "    main :: proc() {}\n"
      "}\n");
  write_file(
      tree.workspace / "linux-only" / "package@aarch64-linux.draft",
      "package linux_only\nmain :: proc() {}\n");
  write_file(
      tree.workspace / "validation-only" / "entry_test.draft",
      "package validation_only\nmain :: proc() {}\n");
  write_file(
      tree.workspace / ".hidden" / "package.draft",
      "package hidden\nmain :: proc() {}\n");
  write_file(
      tree.workspace / "vendor" / "tool" / "package.draft",
      "package vendor_tool\nmain :: proc() {}\n");
  write_file(
      tree.workspace / "expanded-view" / "draft-expanded-source.map",
      "draft-expanded-source-v1\n");
  write_file(
      tree.workspace / "expanded-view" / "copied-app" / "package.draft",
      "package copied_app\nmain :: proc() {}\n");

  std::error_code error;
  std::filesystem::create_directory_symlink(
      tree.workspace / "app", tree.workspace / "linked-app", error);
  if (error) {
    std::cerr << "cannot create test symlink: " << error.message() << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void test_discovery(TestState &state) {
  TemporaryTree tree;
  populate_discovery_tree(tree);
  draft::WorkspaceLoadOptions options = discovery_options(tree);
  options.dependencies.push_back({
      "vendor",
      (tree.workspace / "vendor").string(),
      "selection-test-vendor",
  });

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ExecutableRootDiscoveryResult discovered =
      draft::discover_executable_roots(sources, options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, discovered.ok);
  EXPECT(state, discovered.roots.size() == 3);
  if (discovered.roots.size() != 3) return;
  EXPECT(state, discovered.roots[0].identity.root_identity == "workspace");
  EXPECT(state, discovered.roots[0].identity.root_relative_path == ".");
  EXPECT(state, discovered.roots[1].identity.root_relative_path == "app");
  EXPECT(
      state,
      discovered.roots[2].identity.root_relative_path == "tools/admin");
  EXPECT(
      state,
      discovered.roots[2].physical_directory ==
          std::filesystem::canonical(tree.workspace / "tools" / "admin"));
}

void test_explicit_selection(TestState &state) {
  TemporaryTree tree;
  populate_discovery_tree(tree);

  {
    draft::DiagnosticSink diagnostics;
    draft::WorkspacePackageSelection selected;
    EXPECT(
        state,
        draft::select_workspace_package(
            tree.workspace, "tools/admin", selected, diagnostics));
    EXPECT(state, !diagnostics.has_errors());
    EXPECT(state, selected.identity.root_identity == "workspace");
    EXPECT(state, selected.identity.root_relative_path == "tools/admin");
  }
  {
    draft::DiagnosticSink diagnostics;
    draft::WorkspacePackageSelection selected;
    EXPECT(
        state,
        draft::select_workspace_package(
            tree.workspace, ".", selected, diagnostics));
    EXPECT(state, selected.identity.root_relative_path == ".");
    EXPECT(state, selected.physical_directory == std::filesystem::canonical(tree.workspace));
  }
  for (const std::string_view invalid : {
           "",
           "/absolute",
           "tools/../app",
           "tools//admin",
           "linked-app",
       }) {
    draft::DiagnosticSink diagnostics;
    draft::WorkspacePackageSelection selected;
    EXPECT(
        state,
        !draft::select_workspace_package(
            tree.workspace, invalid, selected, diagnostics));
    EXPECT(state, diagnostics.has_errors());
  }

  // Hidden packages are skipped only by discovery. An exact explicit package
  // identity remains selectable for focused tooling.
  draft::DiagnosticSink diagnostics;
  draft::WorkspacePackageSelection hidden;
  EXPECT(
      state,
      draft::select_workspace_package(
          tree.workspace, ".hidden", hidden, diagnostics));
  EXPECT(state, hidden.identity.root_relative_path == ".hidden");
}

void test_malformed_candidate_fails_discovery(TestState &state) {
  TemporaryTree tree;
  write_file(
      tree.workspace / "unfinished" / "package.draft",
      "package unfinished\nmain :: proc(\n");
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ExecutableRootDiscoveryResult discovered =
      draft::discover_executable_roots(
          sources, discovery_options(tree), diagnostics);
  EXPECT(state, !discovered.ok);
  EXPECT(state, diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_discovery(state);
  test_explicit_selection(state);
  test_malformed_candidate_fails_discovery(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
