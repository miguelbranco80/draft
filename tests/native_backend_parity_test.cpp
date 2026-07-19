// Native qualification parity between embedded LLVM and external Clang.
//
// Embedded LLVM is the ordinary object-emission implementation. The previous
// Clang IR subprocess remains deliberately reachable only through the C++
// qualification option, so it needs one real host gate that prevents it from
// decaying into an argument-shaped mock. This test compiles the same checked
// package graphs through both emitters for every artifact kind, verifies the
// expected native container, compares compiler-owned correlation identity, and
// launches both executable results.
//
// Object bytes are not compared across emitters. LLVM's library and Clang
// driver may choose different incidental encodings while satisfying the same
// target profile. The stable comparison boundary is accepted object/link
// behavior, artifact shape, source correlation, and launched Draft behavior.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
      std::string_view artifact,
      std::string_view expression,
      int line) {
    if (condition) return;
    ++failures;
    std::cerr << "native_backend_parity_test.cpp:" << line << ": "
              << artifact << ": expectation failed: " << expression << '\n';
  }
};

#define EXPECT(state, artifact, expression) \
  (state).expect((expression), (artifact), #expression, __LINE__)

struct ArtifactCase {
  std::string_view name;
  draft::NativeArtifactKind kind;
  bool executable_program = false;
};

// CMake builds this file only where one implemented target is also the host
// execution target. Selecting by compile-time host facts makes an accidental
// inclusion on another machine a configuration error, never a silent skip.
[[nodiscard]] draft::TargetProfile native_host_target() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return draft::make_aarch64_macos_profile();
#elif defined(__linux__) && defined(__aarch64__)
  return draft::make_aarch64_linux_profile();
#else
#error "native backend parity requires an implemented AArch64 host target"
#endif
}

[[nodiscard]] draft::CompileWorkspaceResult compile_fixture(
    draft::SourceManager &sources,
    const draft::TargetProfile &target,
    std::string_view workspace,
    std::string_view package,
    bool emit_program_entry,
    draft::DiagnosticSink &diagnostics) {
  const std::filesystem::path source_root(DRAFT_SOURCE_DIRECTORY);
  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory = (source_root / workspace).string();
  options.workspace.core_directory = (source_root / "core").string();
  options.workspace.core_content_identity = "draft-core-bootstrap-v3";
  options.lower_mir = true;
  options.emit_llvm = true;
  options.emit_program_entry = emit_program_entry;
  return draft::compile_workspace(
      sources,
      (source_root / package).string(),
      std::move(options),
      diagnostics);
}

[[nodiscard]] std::string read_prefix(
    const std::filesystem::path &path,
    std::size_t count) {
  std::ifstream input(path, std::ios::binary);
  std::string bytes(count, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(input.gcount()));
  return bytes;
}

[[nodiscard]] bool has_native_magic(
    const std::filesystem::path &path,
    const draft::TargetProfile &target) {
  const std::string prefix = read_prefix(path, 4);
  if (target.facts.object_format == "elf") {
    return prefix == std::string("\x7f" "ELF", 4);
  }
  return prefix == std::string("\xcf\xfa\xed\xfe", 4);
}

// Assembly is a directory contract. Return only canonical relative filenames;
// instruction spelling is intentionally not required to match across the two
// LLVM entry paths.
[[nodiscard]] std::vector<std::string> assembly_files(
    const std::filesystem::path &directory,
    std::error_code &error) {
  std::vector<std::string> files;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end;
       iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) continue;
    files.push_back(
        std::filesystem::relative(iterator->path(), directory, error)
            .generic_string());
    if (error) break;
    if (std::filesystem::file_size(iterator->path(), error) == 0 || error) {
      error = std::make_error_code(std::errc::invalid_argument);
      break;
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// No worker threads survive build_native_artifact. The child therefore needs
// only async-signal-safe chdir/exec setup and returns a raw wait status so the
// caller can distinguish launch failure, signal termination, and Draft exit.
[[nodiscard]] bool run_executable(
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
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

[[nodiscard]] draft::NativeBuildResult build_route(
    const draft::TargetProfile &target,
    const draft::CompileWorkspaceResult &compiled,
    const std::filesystem::path &root,
    const ArtifactCase &artifact,
    draft::NativeObjectEmitter emitter,
    draft::DiagnosticSink &diagnostics) {
  draft::NativeBuildOptions options;
  options.build_directory = (root / "build").string();
  options.output_path = artifact.kind == draft::NativeArtifactKind::Assembly
      ? (root / "assembly").string()
      : (root / "artifact").string();
  options.artifact_kind = artifact.kind;
  options.object_emitter = emitter;
  options.object_worker_count = 4;
  return draft::build_native_artifact(
      target, compiled, options, diagnostics);
}

void test_backend_routes(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      ("draft-native-backend-parity-" + std::to_string(getpid()));
  EXPECT(state, "setup", !error);
  if (error) return;
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, "setup", !error);
  if (error) return;

  const draft::TargetProfile target = native_host_target();
  draft::SourceManager executable_sources;
  draft::SourceManager library_sources;
  draft::DiagnosticSink compile_diagnostics;
  const draft::CompileWorkspaceResult executable = compile_fixture(
      executable_sources,
      target,
      "examples/packages",
      "examples/packages/app",
      true,
      compile_diagnostics);
  const draft::CompileWorkspaceResult library = compile_fixture(
      library_sources,
      target,
      "examples",
      "examples/c-library",
      false,
      compile_diagnostics);
  EXPECT(state, "compile", executable.ok);
  EXPECT(state, "compile", library.ok);
  if (!executable.ok || !library.ok) {
    std::cerr << draft::render_diagnostics(
        executable_sources, compile_diagnostics);
    std::filesystem::remove_all(temporary, error);
    return;
  }

  constexpr std::array artifacts{
      ArtifactCase{"executable", draft::NativeArtifactKind::Executable, true},
      ArtifactCase{"object", draft::NativeArtifactKind::Object},
      ArtifactCase{"static-library", draft::NativeArtifactKind::StaticLibrary},
      ArtifactCase{"dynamic-library", draft::NativeArtifactKind::DynamicLibrary},
      ArtifactCase{"assembly", draft::NativeArtifactKind::Assembly},
  };
  for (const ArtifactCase &artifact : artifacts) {
    const draft::CompileWorkspaceResult &compiled =
        artifact.executable_program ? executable : library;
    draft::DiagnosticSink embedded_diagnostics;
    draft::DiagnosticSink oracle_diagnostics;
    const std::filesystem::path embedded_root =
        temporary / artifact.name / "embedded";
    const std::filesystem::path oracle_root =
        temporary / artifact.name / "oracle";
    const draft::NativeBuildResult embedded = build_route(
        target,
        compiled,
        embedded_root,
        artifact,
        draft::NativeObjectEmitter::InProcessLlvm,
        embedded_diagnostics);
    const draft::NativeBuildResult oracle = build_route(
        target,
        compiled,
        oracle_root,
        artifact,
        draft::NativeObjectEmitter::ExternalClangOracle,
        oracle_diagnostics);
    if (!embedded.ok || !oracle.ok) {
      for (const draft::Diagnostic &diagnostic :
           embedded_diagnostics.diagnostics()) {
        std::cerr << "embedded " << artifact.name << ": "
                  << diagnostic.message << '\n';
      }
      for (const draft::Diagnostic &diagnostic : oracle_diagnostics.diagnostics()) {
        std::cerr << "oracle " << artifact.name << ": "
                  << diagnostic.message << '\n';
      }
    }
    EXPECT(state, artifact.name, embedded.ok);
    EXPECT(state, artifact.name, oracle.ok);
    if (!embedded.ok || !oracle.ok) continue;
    EXPECT(state, artifact.name,
        embedded.toolchain_version == oracle.toolchain_version);
    EXPECT(state, artifact.name,
        embedded.source_correlation_digest == oracle.source_correlation_digest);

    if (artifact.kind == draft::NativeArtifactKind::Assembly) {
      error.clear();
      const std::vector<std::string> embedded_files =
          assembly_files(embedded.output_path, error);
      EXPECT(state, artifact.name, !error);
      error.clear();
      const std::vector<std::string> oracle_files =
          assembly_files(oracle.output_path, error);
      EXPECT(state, artifact.name, !error);
      EXPECT(state, artifact.name, !embedded_files.empty());
      EXPECT(state, artifact.name, embedded_files == oracle_files);
      continue;
    }

    EXPECT(state, artifact.name, has_native_magic(embedded.output_path, target) ||
        artifact.kind == draft::NativeArtifactKind::StaticLibrary);
    EXPECT(state, artifact.name, has_native_magic(oracle.output_path, target) ||
        artifact.kind == draft::NativeArtifactKind::StaticLibrary);
    if (artifact.kind == draft::NativeArtifactKind::StaticLibrary) {
      EXPECT(state, artifact.name,
          read_prefix(embedded.output_path, 8) == "!<arch>\n");
      EXPECT(state, artifact.name,
          read_prefix(oracle.output_path, 8) == "!<arch>\n");
    }
    const bool expects_debug_companion = target.facts.object_format == "macho" &&
        (artifact.kind == draft::NativeArtifactKind::Executable ||
         artifact.kind == draft::NativeArtifactKind::DynamicLibrary);
    EXPECT(state, artifact.name,
        expects_debug_companion == !embedded.debug_symbols_path.empty());
    EXPECT(state, artifact.name,
        expects_debug_companion == !oracle.debug_symbols_path.empty());

    if (artifact.executable_program) {
      int embedded_status = 0;
      int oracle_status = 0;
      EXPECT(state, artifact.name,
          run_executable(embedded.output_path, embedded_root, embedded_status));
      EXPECT(state, artifact.name,
          run_executable(oracle.output_path, oracle_root, oracle_status));
      EXPECT(state, artifact.name, WIFEXITED(embedded_status));
      EXPECT(state, artifact.name, WIFEXITED(oracle_status));
      if (WIFEXITED(embedded_status)) {
        EXPECT(state, artifact.name, WEXITSTATUS(embedded_status) == 0);
      }
      if (WIFEXITED(oracle_status)) {
        EXPECT(state, artifact.name, WEXITSTATUS(oracle_status) == 0);
      }
    }
  }

  std::filesystem::remove_all(temporary, error);
}

} // namespace

int main() {
  TestState state;
  test_backend_routes(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " native-backend parity expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
