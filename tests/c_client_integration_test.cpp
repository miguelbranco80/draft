// End-to-end native C consumer test for a Draft shared library.
//
// Header snapshots and LLVM ABI checks are useful because they report a small,
// precise failure. They still cannot prove that the independent C compiler,
// platform linker, dynamic loader, and Draft-generated code all agree. On each
// implemented AArch64 host this test builds the checked-in c-library package in
// the native `.dylib` or `.so` format, emits its real target-selected header,
// compiles the checked-in C client, and launches that client.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "interop/c_header.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "c_client_integration_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

// Selects the Draft ABI and object format that the current process can consume.
// The enclosing CMake condition admits only supported native AArch64 hosts, so
// another platform is a configuration error rather than a reason to skip the
// independent C ABI oracle.
[[nodiscard]] draft::TargetProfile native_host_target() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return draft::make_aarch64_macos_profile();
#elif defined(__linux__) && defined(__aarch64__)
  return draft::make_aarch64_linux_profile();
#else
#error "C client integration requires an implemented AArch64 host target"
#endif
}

// Waits through signal interruptions and returns the raw wait status. The
// caller distinguishes a launch failure from a launched program's exit code.
[[nodiscard]] bool wait_for_child(pid_t child, int &status) {
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

// Compiles the C client without a shell. The source and output paths are data,
// never command text. The full shared-library path selects exactly the artifact
// built by this test, while the matching rpath satisfies its Mach-O install name
// or ELF SONAME when the child is launched from the private test directory.
[[nodiscard]] bool compile_c_client(
    const std::filesystem::path &source,
    const std::filesystem::path &include_directory,
    const std::filesystem::path &shared_library,
    const std::filesystem::path &output,
    int &status) {
  const std::string include = "-I" + include_directory.string();
  const std::string rpath = "-Wl,-rpath," + include_directory.string();
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    ::execlp(
        "clang",
        "clang",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        include.c_str(),
        source.c_str(),
        shared_library.c_str(),
        rpath.c_str(),
        "-o",
        output.c_str(),
        nullptr);
    _exit(127);
  }
  return wait_for_child(child, status);
}

// Runs the linked client in its private directory. Keeping the launch direct
// also makes an unexpected loader or client failure visible as an exact status.
[[nodiscard]] bool run_c_client(
    const std::filesystem::path &executable,
    const std::filesystem::path &working_directory,
    int &status) {
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    if (::chdir(working_directory.c_str()) != 0) _exit(126);
    ::execl(executable.c_str(), executable.c_str(), nullptr);
    _exit(127);
  }
  return wait_for_child(child, status);
}

[[nodiscard]] bool write_file(
    const std::filesystem::path &path,
    std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  return static_cast<bool>(output);
}

void test_c_client_consumes_draft_shared_library(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      ("draft-c-client-integration-" + std::to_string(getpid()));
  EXPECT(state, !error);
  if (error) return;
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, !error);
  if (error) return;

  const std::filesystem::path source_root(DRAFT_SOURCE_DIRECTORY);
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = native_host_target();
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
  compile_options.workspace.workspace_directory =
      (source_root / "examples").string();
  compile_options.workspace.core_directory = (source_root / "core").string();
  compile_options.workspace.core_content_identity = "draft-core-bootstrap-v3";
  compile_options.lower_mir = true;
  compile_options.emit_llvm = true;
  compile_options.emit_program_entry = false;
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources,
      (source_root / "examples/c-library").string(),
      std::move(compile_options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  if (!compiled.ok) {
    std::filesystem::remove_all(temporary, error);
    return;
  }

  const std::size_t root =
      static_cast<std::size_t>(compiled.graph.root_package.value);
  EXPECT(state, root < compiled.packages.size());
  if (root >= compiled.packages.size()) {
    std::filesystem::remove_all(temporary, error);
    return;
  }
  EXPECT(state, compiled.packages[root].has_value());
  if (!compiled.packages[root].has_value()) {
    std::filesystem::remove_all(temporary, error);
    return;
  }

  const draft::CHeaderResult header = draft::emit_c_header(
      compiled.packages[root]->semantics.package,
      target,
      {},
      diagnostics);
  EXPECT(state, header.ok);
  const std::filesystem::path header_path = temporary / "draft-c-library.h";
  if (header.ok) {
    EXPECT(state, write_file(header_path, header.text));
  }

  const std::string library_extension =
      target.facts.object_format == "elf" ? ".so" : ".dylib";
  const std::filesystem::path shared_library =
      temporary / ("libdraft_c_library" + library_extension);
  draft::NativeBuildOptions native_options;
  native_options.build_directory = (temporary / "build").string();
  native_options.output_path = shared_library.string();
  native_options.artifact_kind = draft::NativeArtifactKind::DynamicLibrary;
  native_options.allow_unpinned_toolchain = true;
  const draft::NativeBuildResult built = draft::build_native_artifact(
      target, compiled, native_options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, built.ok);

  const std::filesystem::path client = temporary / "c-client";
  int compiler_status = 0;
  if (header.ok && built.ok) {
    EXPECT(state,
        compile_c_client(
            source_root / "examples/c-library/client.c",
            temporary,
            shared_library,
            client,
            compiler_status));
    EXPECT(state, WIFEXITED(compiler_status));
    if (WIFEXITED(compiler_status)) {
      EXPECT(state, WEXITSTATUS(compiler_status) == 0);
    }
  }

  int client_status = 0;
  if (std::filesystem::exists(client)) {
    EXPECT(state, run_c_client(client, temporary, client_status));
    EXPECT(state, WIFEXITED(client_status));
    if (WIFEXITED(client_status)) {
      EXPECT(state, WEXITSTATUS(client_status) == 0);
    }
  }

  std::filesystem::remove_all(temporary, error);
}

} // namespace

int main() {
  TestState state;
  test_c_client_consumes_draft_shared_library(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " C client integration expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
