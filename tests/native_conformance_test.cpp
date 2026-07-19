// Real native AArch64 execution matrix for handwritten Draft programs.
//
// Semantic and LLVM snapshot tests localize compiler failures well, but they do
// not prove that runtime Context setup, the selected platform ABI, package
// assembly, atomics, pthread bridges, and system linking agree in a launched
// process. This gate runs only when the bootstrap host can execute one of the
// implemented AArch64 targets directly: Apple Silicon selects Mach-O/macOS and
// AArch64 Linux selects ELF/GNU Linux. Every package still passes through the
// public compiler and native-adapter APIs before its executable is launched.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct TestState {
  int failures = 0;

  void expect(
      bool condition,
      std::string_view package,
      std::string_view expression,
      int line) {
    if (!condition) {
      ++failures;
      std::cerr << "native_conformance_test.cpp:" << line << ": " << package
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, package, expression) \
  (state).expect((expression), (package), #expression, __LINE__)

struct ConformanceCase {
  std::string_view name;
  std::string_view workspace;
  std::string_view package;
};

struct NativeInputSelection {
  bool locked = false;
  draft::LockedNativeInputRoots roots;
  std::vector<draft::ExternalInputPin> pins;
};

// Selects the Draft target whose executables the current CI host can launch
// directly. CMake only builds this test on the two supported AArch64 hosts, so
// reaching another branch would be a build-configuration error rather than a
// runtime condition that should be hidden by skipping the test.
[[nodiscard]] draft::TargetProfile native_host_target() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return draft::make_aarch64_macos_profile();
#elif defined(__linux__) && defined(__aarch64__)
  return draft::make_aarch64_linux_profile();
#else
#error "native conformance requires an implemented AArch64 host target"
#endif
}

// Normal CTest runs keep using the installed host toolchain. Release
// qualification sets both variables and drives the identical matrix through
// the production locked-input verifier. Pin once here; every native build still
// independently re-verifies the roots immediately before invoking its tools.
[[nodiscard]] bool select_native_inputs(
    const draft::TargetProfile &target,
    NativeInputSelection &selection,
    draft::DiagnosticSink &diagnostics) {
  const char *toolchain = std::getenv("DRAFT_TEST_LOCKED_TOOLCHAIN_ROOT");
  const char *sdk = std::getenv("DRAFT_TEST_LOCKED_SDK_ROOT");
  if (toolchain == nullptr && sdk == nullptr) return true;
  if (toolchain == nullptr || sdk == nullptr || toolchain[0] == '\0' ||
      sdk[0] == '\0') {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "locked native conformance requires both toolchain and SDK roots");
    return false;
  }
  selection.locked = true;
  selection.roots.toolchain_root = toolchain;
  selection.roots.sdk_root = sdk;
  return draft::pin_locked_native_inputs(
      target, selection.roots, selection.pins, diagnostics);
}

// Runs an already linked path directly. No shell, inherited command search, or
// source-authored byte can become command syntax. The child uses an isolated
// working directory so relative process state cannot leak between fixtures.
[[nodiscard]] bool run_executable(
    const std::filesystem::path &executable,
    const std::filesystem::path &working_directory,
    std::string_view argument,
    int &status) {
  // Materialize the optional argument before fork. The child performs only the
  // small async-signal-safe setup needed for exec and never allocates through
  // a post-fork C++ library path.
  const std::string argument_storage(argument);
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    if (::chdir(working_directory.c_str()) != 0) _exit(126);
    if (argument_storage.empty()) {
      ::execl(executable.c_str(), executable.c_str(), nullptr);
    } else {
      ::execl(
          executable.c_str(),
          executable.c_str(),
          argument_storage.c_str(),
          nullptr);
    }
    _exit(127);
  }
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

void test_native_examples(TestState &state) {
  constexpr std::array cases{
      ConformanceCase{"hello", "examples", "examples/hello"},
      ConformanceCase{
          "language-tour", "examples", "examples/language-tour"},
      ConformanceCase{"console", "examples", "examples/console"},
      ConformanceCase{"file-io", "examples", "examples/file-io"},
      ConformanceCase{"denials", "examples", "examples/denials"},
      ConformanceCase{
          "runtime-checks", "examples", "examples/runtime-checks"},
      ConformanceCase{
          "runtime-traps", "examples", "examples/runtime-traps"},
      ConformanceCase{
          "core-runtime", "examples", "examples/core-runtime"},
      ConformanceCase{"core-memory", "examples", "examples/core-memory"},
      ConformanceCase{"core-array", "examples", "examples/core-array"},
      ConformanceCase{"core-map", "examples", "examples/core-map"},
      ConformanceCase{"core-os", "examples", "examples/core-os"},
      ConformanceCase{"core-thread", "examples", "examples/core-thread"},
      ConformanceCase{"core-atomic", "examples", "examples/core-atomic"},
      ConformanceCase{
          "core-atomic-thread", "examples", "examples/core-atomic-thread"},
      ConformanceCase{
          "nested-procedures", "examples", "examples/nested-procedures"},
      ConformanceCase{"assembly", "examples", "examples/assembly"},
      ConformanceCase{
          "external-assembly", "examples", "examples/external-assembly"},
      ConformanceCase{"c-interop", "examples", "examples/c-interop"},
      ConformanceCase{
          "packages", "examples/packages", "examples/packages/app"},
      ConformanceCase{
          "packages-generic",
          "examples/packages-generic",
          "examples/packages-generic/app"},
  };

  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      ("draft-native-conformance-" + std::to_string(getpid()));
  EXPECT(state, "setup", !error);
  if (error) return;
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, "setup", !error);
  if (error) return;

  const draft::TargetProfile target = native_host_target();
  NativeInputSelection native_inputs;
  draft::DiagnosticSink input_diagnostics;
  const bool inputs_ok =
      select_native_inputs(target, native_inputs, input_diagnostics);
  if (input_diagnostics.has_errors()) {
    for (const draft::Diagnostic &diagnostic :
         input_diagnostics.diagnostics()) {
      std::cerr << diagnostic.message << '\n';
    }
  }
  EXPECT(state, "setup", inputs_ok);
  if (!inputs_ok) return;

  const std::filesystem::path source_root(DRAFT_SOURCE_DIRECTORY);
  for (const ConformanceCase &test : cases) {
    draft::SourceManager sources;
    draft::DiagnosticSink diagnostics;
    draft::CompileWorkspaceOptions compile_options;
    compile_options.target = target;
    compile_options.workspace.workspace_directory =
        (source_root / test.workspace).string();
    compile_options.workspace.core_directory =
        (source_root / "core").string();
    compile_options.workspace.core_content_identity =
        "draft-core-bootstrap-v3";
    compile_options.lower_mir = true;
    compile_options.emit_llvm = true;
    draft::CompileWorkspaceResult compiled = draft::compile_workspace(
        sources,
        (source_root / test.package).string(),
        std::move(compile_options),
        diagnostics);
    if (diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    EXPECT(state, test.name, compiled.ok);
    if (!compiled.ok) continue;
    if (native_inputs.locked) {
      compiled.resolution_manifest.emplace();
      compiled.resolution_manifest->target_identity =
          target.facts.identity;
      compiled.resolution_manifest->external_inputs = native_inputs.pins;
    }

    const std::filesystem::path case_directory = temporary / test.name;
    draft::NativeBuildOptions native_options;
    native_options.build_directory = (case_directory / "build").string();
    native_options.output_path = (case_directory / "program").string();
    if (native_inputs.locked) {
      native_options.locked = true;
      native_options.locked_inputs = native_inputs.roots;
    } else {
      native_options.allow_unpinned_toolchain = true;
    }
    const draft::NativeBuildResult built = draft::build_native_executable(
        target, compiled, native_options, diagnostics);
    if (diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    EXPECT(state, test.name, built.ok);
    if (!built.ok) continue;
    if (target.facts.object_format == "macho") {
      EXPECT(state, test.name, !built.debug_symbols_path.empty());
      EXPECT(state, test.name,
          std::filesystem::exists(built.debug_symbols_path));
    } else {
      // ELF retains its DWARF in the executable. An empty result field is the
      // explicit backend contract, not a missing Linux-shaped dSYM substitute.
      EXPECT(state, test.name, built.debug_symbols_path.empty());
    }

    std::filesystem::create_directories(case_directory, error);
    EXPECT(state, test.name, !error);
    if (error) {
      error.clear();
      continue;
    }
    if (test.name == "runtime-traps") {
      // One runtime-selected package covers every mandatory trap class without
      // recompiling eight nearly identical fixtures. Each selector enters one
      // path whose invalid value depends on argv, keeping the failure at
      // runtime rather than turning it into a required-constant diagnostic.
      constexpr std::array trap_selectors{
          "d", // Integer division by zero.
          "o", // Signed minimum divided by negative one.
          "s", // Out-of-range shift count.
          "n", // Negative shift count.
          "w", // Narrow negative count for a u128 shift.
          "f", // Out-of-range float-to-integer conversion.
          "q", // NaN-to-integer conversion.
          "r", // Invalid Unicode scalar conversion.
          "e", // Invalid enum backing value.
          "i", // Array index out of bounds.
          "l", // Slice bound out of range.
      };
      for (const std::string_view selector : trap_selectors) {
        int process_status = 0;
        EXPECT(state, test.name,
            run_executable(
                built.output_path, case_directory, selector, process_status));
        EXPECT(state, test.name, WIFSIGNALED(process_status));
        if (WIFSIGNALED(process_status)) {
          // LLVM lowers llvm.trap to AArch64 BRK for both selected targets, and
          // each selected kernel reports that deliberate breakpoint as
          // SIGTRAP. Requiring the exact signal distinguishes the compiler's
          // trap edge from an accidental arithmetic or memory fault.
          EXPECT(state, test.name, WTERMSIG(process_status) == SIGTRAP);
        }
      }
    } else {
      int process_status = 0;
      EXPECT(state, test.name,
          run_executable(built.output_path, case_directory, {}, process_status));
      EXPECT(state, test.name, WIFEXITED(process_status));
      if (WIFEXITED(process_status)) {
        EXPECT(state, test.name, WEXITSTATUS(process_status) == 0);
      }
    }
  }

  std::filesystem::remove_all(temporary, error);
}

} // namespace

int main() {
  TestState state;
  test_native_examples(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " native-conformance expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
