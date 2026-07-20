// Complete-source projection tests over the committed agent acceptance graph.
//
// This crosses provider-free manifest authentication, generated overlays,
// dependency loading, final source ownership, transactional materialization,
// and source-map sidecars. No provider or native toolchain is involved.

#include "compile/compiler.h"
#include "compile/expanded_source.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include "test_directory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "expanded_source_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression)                                                \
  (state).expect((expression), #expression, __LINE__)

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>());
}

void test_materializes_complete_graph_with_maps(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-expanded-source-test-output"};
  const std::filesystem::path source_root = DRAFT_SOURCE_DIRECTORY;
  const std::filesystem::path workspace =
      source_root / "examples" / "agent-acceptance";
  // The projection API atomically creates its requested output and therefore
  // requires that leaf not to exist. The RAII owner claims only its parent,
  // preserving both that API precondition and isolated cleanup.
  const std::filesystem::path output =
      temporary_directory.path() / "expanded-source";
  std::error_code ignored;
  std::filesystem::remove_all(output.string() + ".tmp", ignored);

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = workspace.string();
  options.workspace.core_directory = (source_root / "core").string();
  options.workspace.core_content_identity = "draft-core-bootstrap-v4";
  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          (workspace / "app").string(),
          std::move(options),
          diagnostics);
  EXPECT(state, compiled.ok);
  EXPECT(state, !diagnostics.has_errors());

  const draft::ExpandedSourceProjectionResult projected =
      draft::materialize_expanded_source(
          sources, compiled, output, diagnostics);
  EXPECT(state, projected.ok);
  EXPECT(state, projected.output_directory == output);
  EXPECT(state, projected.source_files >= 2);
  EXPECT(state, projected.mapped_expansions == 4);
  EXPECT(state, !diagnostics.has_errors());

  const std::filesystem::path app_source =
      output / "root-0000-workspace" / "app" / "package.draft";
  const std::string expanded = read_file(app_source);
  EXPECT(state,
      expanded.find("generated_offset :: cast[i64](2);") !=
          std::string::npos);
  EXPECT(state, expanded.find("value: i64,") != std::string::npos);
  EXPECT(state, expanded.find("expected: i64 = 42") != std::string::npos);
  EXPECT(state,
      expanded.find("assert(actual == expected)") != std::string::npos);
  EXPECT(state, expanded.find("... ") == std::string::npos);

  const std::string map = read_file(app_source.string() + ".draft-map");
  EXPECT(state,
      map.starts_with("draft-expanded-source-map-v1\nexpansions 4\n"));
  EXPECT(state, map.find("site-393538") != std::string::npos);
  const std::string roots =
      read_file(output / "draft-expanded-source.map");
  EXPECT(state, roots.starts_with("draft-expanded-source-v1\n"));
  EXPECT(state, roots.find("workspace") != std::string::npos);
  EXPECT(state, roots.find("draft-core-bootstrap-v4") != std::string::npos);

  // Refusing an existing destination prevents stale files or maps from a
  // previous graph being mixed with a new projection.
  draft::DiagnosticSink duplicate_diagnostics;
  const draft::ExpandedSourceProjectionResult duplicate =
      draft::materialize_expanded_source(
          sources, compiled, output, duplicate_diagnostics);
  EXPECT(state, !duplicate.ok);
  EXPECT(state, duplicate_diagnostics.error_count() == 1);
  EXPECT(state, read_file(app_source) == expanded);

}

} // namespace

int main() {
  TestState state;
  test_materializes_complete_graph_with_maps(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " expanded-source expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all expanded-source tests passed\n";
  return EXIT_SUCCESS;
}
