// Byte-for-byte determinism gate for the real macOS native toolchain path.
//
// Most toolchain tests use a recording process so failures describe our exact
// argument contract without depending on a host installation. This test has a
// different job: on the first supported host, compile complete Draft programs
// and ask the actual Clang driver to produce every artifact kind twice.
// Comparing complete output trees catches timestamps, UUID drift, archive
// metadata, object-order drift, and other ambient state an argument-only test
// cannot see.

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
    draft::DiagnosticSink &diagnostics) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-bootstrap-v1";
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
    const draft::CompileWorkspaceResult &compiled,
    const std::filesystem::path &temporary,
    std::string_view name,
    draft::NativeArtifactKind kind) {
  const std::filesystem::path artifact_directory = temporary / name;
  draft::NativeBuildOptions options;
  options.build_directory = (artifact_directory / "build").string();
  options.output_path = (artifact_directory / "output").string();
  options.artifact_kind = kind;
  // Release builds use verified LLVM/SDK roots. This host integration gate is
  // deliberately about emitted bytes and may use the installed Apple tools;
  // the locked-input contract has separate exact-argument/unit coverage.
  options.allow_unpinned_toolchain = true;

  draft::DiagnosticSink first_diagnostics;
  const draft::NativeBuildResult first = draft::build_native_artifact(
      draft::make_aarch64_macos_profile(),
      compiled,
      options,
      first_diagnostics);
  if (!first.ok) {
    std::cerr << "first native build failed for artifact: " << name << '\n';
    for (const draft::Diagnostic &diagnostic : first_diagnostics.diagnostics()) {
      std::cerr << "  " << diagnostic.message << '\n';
    }
  }
  EXPECT(state, first.ok);
  EXPECT(state, !first_diagnostics.has_errors());
  if (!first.ok) return;
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
      draft::make_aarch64_macos_profile(),
      compiled,
      options,
      second_diagnostics);
  if (!second.ok) {
    std::cerr << "second native build failed for artifact: " << name << '\n';
    for (const draft::Diagnostic &diagnostic : second_diagnostics.diagnostics()) {
      std::cerr << "  " << diagnostic.message << '\n';
    }
  }
  EXPECT(state, second.ok);
  EXPECT(state, !second_diagnostics.has_errors());
  if (!second.ok) return;
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
  const draft::CompileWorkspaceResult executable = compile_fixture(
      sources, "nested-procedures", true, compile_diagnostics);
  const draft::CompileWorkspaceResult library = compile_fixture(
      sources, "c-library", false, compile_diagnostics);
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
