// Public workspace-build discovery, selection, and output-layout tests.
//
// This process-level fixture owns one disposable workspace with two executable
// packages and one library. It invokes the real driver with assembly output so
// every selected root crosses argument parsing, discovery, semantic compilation,
// LLVM lowering, and artifact publication without requiring a host linker. The
// test then introduces a broken executable to prove aggregate failure and
// explicit-root isolation. All filesystem state is removed with the fixture.

#include "target/profile.h"

#include "test_directory.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (condition) return;
    ++failures;
    std::cerr << "workspace_build_driver_test.cpp:" << line
              << ": expectation failed: " << expression << '\n';
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryWorkspace {
  draft::test::TemporaryDirectory directory{
      "draft-workspace-build-driver-test"};
  std::filesystem::path root;

  TemporaryWorkspace() {
    root = directory.path();
    std::error_code error;
    std::filesystem::create_directories(root / "app", error);
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::create_directories(root / "tools" / "admin", error);
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::create_directories(root / "lib", error);
    if (error) std::exit(EXIT_FAILURE);
    // Both executable packages intentionally have the same short package name.
    // Their mirrored root paths, rather than a globally unique basename,
    // prevent default artifact collisions.
    write_package(root / "app", "runner", true, false);
    write_package(root / "tools" / "admin", "runner", true, false);
    write_package(root / "lib", "lib", false, false);
  }

  static void write_package(
      const std::filesystem::path &directory,
      std::string_view name,
      bool executable,
      bool broken) {
    std::ofstream source(directory / "package.draft", std::ios::binary);
    source << "package " << name << "\n\n";
    if (executable) {
      source << "main :: proc() {\n";
      if (broken) source << "    value: Missing\n";
      source << "}\n";
    } else {
      source << "answer :: 42\n";
    }
    source.close();
    if (!source) std::exit(EXIT_FAILURE);
  }
};

[[nodiscard]] int run_driver(std::vector<std::string> arguments) {
#if defined(__APPLE__) || defined(__unix__)
  std::vector<char *> raw;
  raw.reserve(arguments.size() + 1);
  for (std::string &argument : arguments) raw.push_back(argument.data());
  raw.push_back(nullptr);
  const pid_t child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    ::execv(raw.front(), raw.data());
    ::_exit(127);
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
#else
  (void)arguments;
  return -1;
#endif
}

[[nodiscard]] std::filesystem::path artifact_directory(
    const TemporaryWorkspace &workspace,
    std::string_view root) {
  std::filesystem::path result = workspace.root / ".draft" / "build" /
      draft::make_aarch64_macos_profile().facts.file_tag;
  result /= "packages";
  result /= root;
  return result;
}

void test_discovery_selection_and_failures(TestState &state) {
  TemporaryWorkspace workspace;

  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--kind",
      "assembly",
  }) == 0);
  EXPECT(state,
      std::filesystem::is_directory(
          artifact_directory(workspace, "app") / "runner-assembly"));
  EXPECT(state,
      std::filesystem::is_directory(
          artifact_directory(workspace, "tools/admin") / "runner-assembly"));
  EXPECT(state,
      !std::filesystem::exists(
          artifact_directory(workspace, "lib") / "lib-assembly"));

  // A single output path cannot describe two independently named artifacts.
  const std::filesystem::path ambiguous_output =
      workspace.root / "ambiguous-output";
  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--kind",
      "assembly",
      "-o",
      ambiguous_output.string(),
  }) == 2);
  EXPECT(state, !std::filesystem::exists(ambiguous_output));

  const std::filesystem::path explicit_output =
      workspace.root / "selected-admin-assembly";
  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--root",
      "tools/admin",
      "--kind",
      "assembly",
      "-o",
      explicit_output.string(),
  }) == 0);
  EXPECT(state, std::filesystem::is_directory(explicit_output));

  // Duplicate selectors are a command mistake rather than two builds of the
  // same output namespace.
  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--root",
      "app",
      "--root",
      "app",
      "--kind",
      "assembly",
  }) == 1);

  std::error_code error;
  std::filesystem::create_directories(workspace.root / "optional-tool", error);
  EXPECT(state, !error);
  TemporaryWorkspace::write_package(
      workspace.root / "optional-tool", "optional_tool", true, true);
  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--kind",
      "assembly",
  }) == 1);

  // Selection excludes the broken sibling completely. Its existence affects
  // neither the chosen graph nor the independently namespaced output.
  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--root",
      "app",
      "--kind",
      "assembly",
  }) == 0);

  // Root `.` has a fixed `workspace` namespace which cannot collide with a
  // child package path. It is selected explicitly because bare build now means
  // executable discovery, not an implicit root package.
  TemporaryWorkspace::write_package(
      workspace.root, "root_runner", true, false);
  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--root",
      ".",
      "--kind",
      "assembly",
  }) == 0);
  EXPECT(state,
      std::filesystem::is_directory(
          workspace.root / ".draft" / "build" /
          draft::make_aarch64_macos_profile().facts.file_tag /
          "workspace" / "root_runner-assembly"));
}

} // namespace

int main() {
  TestState state;
  test_discovery_selection_and_failures(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " workspace-build driver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all workspace-build driver tests passed\n";
  return EXIT_SUCCESS;
}
