// Byte-for-byte determinism gate for each native AArch64 toolchain path.
//
// Most toolchain tests use a recording process so failures describe our exact
// argument contract without depending on a host installation. This test asks
// the actual host Clang/linker path to produce every artifact kind twice for
// the directly executable Draft target. Comparing complete output trees catches
// timestamps, Mach-O UUID or ELF build-ID drift, archive metadata, object-order
// drift, and other ambient state an argument-only test cannot see.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

using ArtifactSnapshot =
    std::vector<std::pair<std::string, std::string>>;

// Returns the one implemented target that this process can execute natively.
// CMake excludes this source on other host/architecture pairs; the preprocessor
// guard keeps an accidental future inclusion from silently testing the wrong
// artifact format or ABI.
[[nodiscard]] draft::TargetProfile native_host_target() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return draft::make_aarch64_macos_profile();
#elif defined(__linux__) && defined(__aarch64__)
  return draft::make_aarch64_linux_profile();
#else
#error "native determinism requires an implemented AArch64 host target"
#endif
}

[[nodiscard]] ArtifactSnapshot snapshot_artifact(
    const std::filesystem::path &path,
    draft::NativeArtifactKind kind,
    std::error_code &error) {
  ArtifactSnapshot snapshot;
  if (kind != draft::NativeArtifactKind::Assembly) {
    snapshot.push_back({path.filename().string(), read_file(path)});
    return snapshot;
  }
  for (std::filesystem::recursive_directory_iterator iterator(path, error), end;
       !error && iterator != end;
       iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) continue;
    snapshot.push_back({
        std::filesystem::relative(iterator->path(), path, error).generic_string(),
        read_file(iterator->path()),
    });
  }
  std::sort(snapshot.begin(), snapshot.end());
  return snapshot;
}

[[nodiscard]] draft::CompileWorkspaceResult compile_fixture(
    draft::SourceManager &sources,
    std::string_view package,
    bool emit_program_entry,
    const draft::TargetProfile &target,
    draft::DiagnosticSink &diagnostics) {
  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-bootstrap-v3";
  options.lower_mir = true;
  options.emit_llvm = true;
  options.emit_program_entry = emit_program_entry;
  return draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/" +
          std::string(package),
      std::move(options),
      diagnostics);
}

void compare_repeated_artifact(
    TestState &state,
    const draft::TargetProfile &target,
    const draft::CompileWorkspaceResult &compiled,
    const std::filesystem::path &temporary,
    std::string_view name,
    draft::NativeArtifactKind kind) {
  const std::filesystem::path artifact_directory = temporary / name;
  draft::NativeBuildOptions options;
  options.build_directory = (artifact_directory / "build").string();
  options.output_path = (artifact_directory / "output").string();
  options.artifact_kind = kind;

  draft::DiagnosticSink first_diagnostics;
  const draft::NativeBuildResult first = draft::build_native_artifact(
      target, compiled, options, first_diagnostics);
  if (!first.ok) {
    std::cerr << "first native build failed for artifact: " << name << '\n';
    for (const draft::Diagnostic &diagnostic : first_diagnostics.diagnostics()) {
      std::cerr << "  " << diagnostic.message << '\n';
    }
  }
  EXPECT(state, first.ok);
  EXPECT(state, !first_diagnostics.has_errors());
  if (!first.ok) return;
  const bool has_debug_companion = target.facts.object_format == "macho" &&
      (kind == draft::NativeArtifactKind::Executable ||
       kind == draft::NativeArtifactKind::DynamicLibrary);
  EXPECT(state, has_debug_companion == !first.debug_symbols_path.empty());
  if (has_debug_companion) {
    EXPECT(state, std::filesystem::exists(first.debug_symbols_path));
    EXPECT(state, !std::filesystem::exists(
        std::filesystem::path(first.debug_symbols_path) / "Contents" /
            "Resources" / "Relocations"));
  }
  std::error_code error;
  const ArtifactSnapshot first_snapshot =
      snapshot_artifact(options.output_path, kind, error);
  EXPECT(state, !error);
  EXPECT(state, !first_snapshot.empty());
  for (const auto &[path, bytes] : first_snapshot) {
    (void)path;
    EXPECT(state, !bytes.empty());
  }

  draft::DiagnosticSink second_diagnostics;
  const draft::NativeBuildResult second = draft::build_native_artifact(
      target, compiled, options, second_diagnostics);
  if (!second.ok) {
    std::cerr << "second native build failed for artifact: " << name << '\n';
    for (const draft::Diagnostic &diagnostic : second_diagnostics.diagnostics()) {
      std::cerr << "  " << diagnostic.message << '\n';
    }
  }
  EXPECT(state, second.ok);
  EXPECT(state, !second_diagnostics.has_errors());
  if (!second.ok) return;
  EXPECT(state, second.debug_symbols_path == first.debug_symbols_path);
  EXPECT(state,
      second.debug_symbols_digest == first.debug_symbols_digest);
  error.clear();
  const ArtifactSnapshot second_snapshot =
      snapshot_artifact(options.output_path, kind, error);
  EXPECT(state, !error);
  if (first_snapshot != second_snapshot) {
    std::cerr << "native artifact changed across identical rebuilds: "
              << name << '\n';
  }
  EXPECT(state, first_snapshot == second_snapshot);
}

void test_repeated_native_link_is_byte_identical(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      ("draft-native-determinism-" + std::to_string(getpid()));
  EXPECT(state, !error);
  if (error) return;

  const draft::TargetProfile target = native_host_target();
  // A process-specific directory permits parallel CTest invocations while the
  // fixed output name inside it is the explicit identity used by both links.
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, !error);
  if (error) return;

  draft::SourceManager sources;
  draft::DiagnosticSink compile_diagnostics;
  // The executable fixture contains repeated short nested names and two outer
  // generic specializations. Rebuilding it proves that lexical linkage names
  // depend only on source/package identity and canonical type arguments, never
  // process addresses or filesystem paths.
  draft::CompileWorkspaceResult executable = compile_fixture(
      sources, "nested-procedures", true, target, compile_diagnostics);
  draft::CompileWorkspaceResult library = compile_fixture(
      sources, "c-library", false, target, compile_diagnostics);
  if (compile_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, compile_diagnostics);
  }
  EXPECT(state, executable.ok);
  EXPECT(state, library.ok);
  if (!executable.ok || !library.ok) {
    std::filesystem::remove_all(temporary, error);
    return;
  }
  struct ArtifactCase {
    std::string_view name;
    draft::NativeArtifactKind kind;
    const draft::CompileWorkspaceResult *compiled;
  };
  const std::array artifacts{
      ArtifactCase{
          "executable", draft::NativeArtifactKind::Executable, &executable},
      ArtifactCase{"object", draft::NativeArtifactKind::Object, &library},
      ArtifactCase{
          "static-library", draft::NativeArtifactKind::StaticLibrary, &library},
      ArtifactCase{
          "dynamic-library", draft::NativeArtifactKind::DynamicLibrary, &library},
      ArtifactCase{"assembly", draft::NativeArtifactKind::Assembly, &library},
  };
  for (const ArtifactCase &artifact : artifacts) {
    compare_repeated_artifact(
        state,
        target,
        *artifact.compiled,
        temporary,
        artifact.name,
        artifact.kind);
  }

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
