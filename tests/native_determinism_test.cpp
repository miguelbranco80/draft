// Byte-for-byte determinism gate across native target worker schedules.
//
// Most toolchain tests use a recording process so failures describe our exact
// argument contract without depending on a host installation. This test asks
// the embedded LLVM and actual host linker path to produce every artifact kind
// first with one object worker and then with four. Comparing complete output
// trees catches scheduling-dependent publication, timestamps, Mach-O UUID or
// ELF build-ID drift, archive metadata, object-order drift, and other ambient
// state an argument-only test cannot see.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/workspace.h"

#include "test_directory.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

[[nodiscard]] std::size_t native_task_count(
    const draft::CompileWorkspaceResult &compiled) {
  std::size_t count = 0;
  for (const std::optional<draft::CompiledPackage> &package :
       compiled.packages) {
    if (!package.has_value()) continue;
    count += package->artifact_layout.inputs.size();
  }
  return count;
}

// Returns the one implemented target that this process can execute natively.
// CMake excludes this source on other host/architecture pairs; the preprocessor
// guard keeps an accidental future inclusion from silently testing the wrong
// artifact format or ABI.
[[nodiscard]] draft::TargetProfile native_host_target() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return draft::make_aarch64_macos_profile();
#elif defined(__linux__) && defined(__aarch64__)
  return draft::make_aarch64_linux_profile();
#elif defined(__linux__) && defined(__x86_64__)
  return draft::make_x86_64_linux_profile();
#else
#error "native determinism requires an implemented host target"
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
  options.workspace.workspace_directory = std::string(DRAFT_SOURCE_DIRECTORY) +
      (package == "packages/app" ? "/examples/packages" : "/examples");
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-bootstrap-v4";
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
  options.object_worker_count = 1;

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
  EXPECT(state, first.object_workers_used == 1);
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

  // The same canonical task graph now runs with a wider ready set. The
  // multi-package executable below proves real overlap; one-package library
  // artifacts still prove that selecting a wider bound preserves behavior.
  const std::size_t task_count = native_task_count(compiled);
  options.object_worker_count = 4;
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
  EXPECT(state,
      second.object_workers_used == std::min<std::size_t>(4, task_count));
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
  draft::test::TemporaryDirectory temporary_directory{
      "draft-native-determinism"};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::error_code error;

  const draft::TargetProfile target = native_host_target();
  // The owned process-unique directory permits parallel CTest invocations
  // while the fixed output name inside it identifies both compared links.

  draft::SourceManager sources;
  draft::DiagnosticSink compile_diagnostics;
  // The executable fixture contains repeated short nested names and two outer
  // generic specializations. Rebuilding it proves that lexical linkage names
  // depend only on source/package identity and canonical type arguments, never
  // process addresses or filesystem paths.
  draft::CompileWorkspaceResult executable = compile_fixture(
      sources, "packages/app", true, target, compile_diagnostics);
  draft::CompileWorkspaceResult library = compile_fixture(
      sources, "c-library", false, target, compile_diagnostics);
  if (compile_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, compile_diagnostics);
  }
  EXPECT(state, executable.ok);
  EXPECT(state, library.ok);
  EXPECT(state, native_task_count(executable) > 1);
  if (!executable.ok || !library.ok) return;
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

  // Corrupt two independent package modules after planning facts have
  // been produced. Workers may fail in either order, but only the lowest task
  // ID's reason is diagnosed and no canonical object or complete-looking
  // correlation map may be published from the failed ready set.
  if (executable.packages.size() >= 2 &&
      executable.packages[0].has_value() &&
      executable.packages[1].has_value()) {
    draft::CompileWorkspaceResult broken = executable;
    broken.packages[0]->llvm_module.text =
        "not an LLVM module for task zero\n";
    broken.packages[1]->llvm_module.text =
        "not an LLVM module for a later task\n";
    const std::string first_identity =
        draft::display_package_identity(broken.packages[0]->identity) +
        " LLVM module";
    const std::filesystem::path failed_directory = temporary / "failed-ready-set";
    draft::NativeBuildOptions failed_options;
    failed_options.build_directory = (failed_directory / "build").string();
    failed_options.output_path = (failed_directory / "program").string();
    failed_options.object_worker_count = 4;
    draft::DiagnosticSink failed_diagnostics;
    const draft::NativeBuildResult failed = draft::build_native_artifact(
        target, broken, failed_options, failed_diagnostics);
    EXPECT(state, !failed.ok);
    EXPECT(state, failed_diagnostics.diagnostics().size() == 1);
    if (!failed_diagnostics.diagnostics().empty()) {
      EXPECT(state, failed_diagnostics.diagnostics().front().message.find(
          "native object task '" + first_identity + "' failed") !=
          std::string::npos);
    }
    EXPECT(state, !std::filesystem::exists(
        failed_directory / "build" / "package-0-module.o"));
    EXPECT(state, !std::filesystem::exists(
        failed_directory / "build" / "draft-source-correlation.json"));
  } else {
    EXPECT(state, false);
  }

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
