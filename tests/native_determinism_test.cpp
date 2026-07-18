// Byte-for-byte determinism gate for the real macOS native toolchain path.
//
// Most toolchain tests use a recording process so failures describe our exact
// argument contract without depending on a host installation. This test has a
// different job: on the first supported host, compile a complete Draft program
// and ask the actual Clang driver to link it twice. Comparing the entire Mach-O
// catches timestamps, random UUIDs, object-order drift, and other ambient state
// that an argument-only test cannot see.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "native_determinism_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void test_repeated_native_link_is_byte_identical(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      ("draft-native-determinism-" + std::to_string(getpid()));
  EXPECT(state, !error);
  if (error) return;

  // A process-specific directory permits parallel CTest invocations while the
  // fixed output name inside it is the explicit identity used by both links.
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, !error);
  if (error) return;

  draft::SourceManager sources;
  draft::DiagnosticSink compile_diagnostics;
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = draft::make_aarch64_macos_profile();
  compile_options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  compile_options.lower_mir = true;
  compile_options.emit_llvm = true;
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/hello",
      std::move(compile_options),
      compile_diagnostics);
  if (compile_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, compile_diagnostics);
  }
  EXPECT(state, compiled.ok);
  if (!compiled.ok) {
    std::filesystem::remove_all(temporary, error);
    return;
  }

  draft::NativeBuildOptions native_options;
  native_options.build_directory = (temporary / "build").string();
  native_options.output_path = (temporary / "hello").string();
  // Release builds use verified LLVM/SDK roots. This host integration gate is
  // deliberately about the emitted bytes and is allowed to use the installed
  // Apple toolchain; the locked-input contract has separate unit coverage.
  native_options.allow_unpinned_toolchain = true;

  draft::DiagnosticSink first_diagnostics;
  const draft::NativeBuildResult first = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      compiled,
      native_options,
      first_diagnostics);
  EXPECT(state, first.ok);
  EXPECT(state, !first_diagnostics.has_errors());
  const std::string first_bytes = read_file(native_options.output_path);
  EXPECT(state, !first_bytes.empty());

  draft::DiagnosticSink second_diagnostics;
  const draft::NativeBuildResult second = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      compiled,
      native_options,
      second_diagnostics);
  EXPECT(state, second.ok);
  EXPECT(state, !second_diagnostics.has_errors());
  const std::string second_bytes = read_file(native_options.output_path);
  EXPECT(state, !second_bytes.empty());
  EXPECT(state, first_bytes == second_bytes);

  std::filesystem::remove_all(temporary, error);
}

} // namespace

int main() {
  TestState state;
  test_repeated_native_link_is_byte_identical(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " native-determinism expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
