// Public package-path build discovery, selection, and output-layout tests.
//
// This process-level fixture owns one disposable workspace with two executable
// packages and one library. It invokes the real driver with assembly output so
// every selected root crosses argument parsing, discovery, semantic compilation,
// LLVM lowering, and artifact publication without requiring a host linker. The
// test then introduces a broken executable to prove aggregate failure and
// exact-subtree isolation. All filesystem state is removed with the fixture.

#include "target/profile.h"

#include "draft/version.h"
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

    std::ofstream marker(root / "draft.workspace", std::ios::binary);
    marker << "draft-workspace-v1\n";
    marker.close();
    if (!marker) std::exit(EXIT_FAILURE);

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

// Resolves the same CMake-owned host fact that initializes the driver under
// test. The fixture needs the resulting file tag only to locate default output;
// copying the host-to-target mapping here would let the assertion drift from
// draftc again. Published test configurations always provide a supported
// selector, so failure is a broken build configuration rather than a source
// diagnostic and terminates the fixture immediately.
[[nodiscard]] const draft::TargetProfile &native_host_profile() {
  static const draft::TargetProfile profile = [] {
    draft::TargetProfile selected;
    std::string reason;
    if (!draft::select_builtin_target_profile(
            DRAFT_NATIVE_HOST_TARGET, selected, reason)) {
      std::cerr << "cannot select test host target: " << reason << '\n';
      std::exit(EXIT_FAILURE);
    }
    return selected;
  }();
  return profile;
}

[[nodiscard]] std::filesystem::path artifact_directory(
    const TemporaryWorkspace &workspace,
    std::string_view root) {
  std::filesystem::path result = workspace.root / ".draft" / "build" /
      native_host_profile().facts.file_tag;
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
      (workspace.root / "tools" / "admin").string(),
      "--kind",
      "assembly",
      "-o",
      explicit_output.string(),
  }) == 0);
  EXPECT(state, std::filesystem::is_directory(explicit_output));

  // A package at the workspace boundary has the fixed `workspace` namespace,
  // which cannot collide with any child package path. Recursive discovery
  // includes it exactly once alongside the already existing programs.
  TemporaryWorkspace::write_package(
      workspace.root, "root_runner", true, false);
  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
      "--kind",
      "assembly",
  }) == 0);
  EXPECT(state,
      std::filesystem::is_directory(
          workspace.root / ".draft" / "build" /
          native_host_profile().facts.file_tag /
          "workspace" / "root_runner-assembly"));

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
      (workspace.root / "app").string(),
      "--kind",
      "assembly",
  }) == 0);
}

void test_aggregate_program_configuration(TestState &state) {
  TemporaryWorkspace workspace;

  // The admin root exists on every target but declares main only for a profile
  // deliberately different from this test binary's native host. Aggregate
  // discovery must therefore inspect that exact package under its named
  // program target instead of applying the workspace default to every root.
  // Distinct explicit outputs also prove that configuration is resolved before
  // any artifact is published rather than after a common build loop has
  // already selected one path and kind.
  const std::string alternate_target =
      native_host_profile().facts.file_tag == "x86_64-linux"
          ? "aarch64-macos"
          : "x86_64-linux";
  TemporaryWorkspace::write_package(
      workspace.root / "tools" / "admin", "runner", false, false);
  {
    std::ofstream source(
        workspace.root / "tools" / "admin" /
            ("entry@" + alternate_target + ".draft"),
        std::ios::binary);
    source << "package runner\n\nmain :: proc() {}\n";
    source.close();
    EXPECT(state, static_cast<bool>(source));
  }
  {
    std::ofstream marker(workspace.root / "draft.workspace",
                         std::ios::binary | std::ios::trunc);
    marker <<
        "draft-workspace-v1\n"
        "[build]\n"
        "kind = assembly\n"
        "[program app]\n"
        "root = app\n"
        "output = configured-app-assembly\n"
        "[program admin]\n"
        "root = tools/admin\n"
        "target = " << alternate_target << "\n"
        "output = configured-admin-assembly\n";
    marker.close();
    EXPECT(state, static_cast<bool>(marker));
  }

  EXPECT(state, run_driver({
      DRAFT_DRIVER_PATH,
      "build",
      workspace.root.string(),
  }) == 0);
  EXPECT(state,
         std::filesystem::is_directory(
             workspace.root / "configured-app-assembly"));
  EXPECT(state,
         std::filesystem::is_directory(
             workspace.root / "configured-admin-assembly"));
}

} // namespace

int main() {
  TestState state;
  test_discovery_selection_and_failures(state);
  test_aggregate_program_configuration(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " workspace-build driver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all workspace-build driver tests passed\n";
  return EXIT_SUCCESS;
}
