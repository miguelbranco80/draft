// Immutable compiler-distributed core bundle tests.
//
// These checks inspect the generated table directly and load one package
// without any core checkout path. They pin canonical row ordering, required
// content, build-time identity publication, and the in-memory package-loader
// seam used by every installed compiler invocation.

#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/embedded_core.h"
#include "workspace/package.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (condition)
      return;
    ++failures;
    std::cerr << "embedded_core_test.cpp:" << line
              << ": expectation failed: " << expression << '\n';
  }
};

#define EXPECT(state, expression)                                              \
  (state).expect((expression), #expression, __LINE__)

} // namespace

int main() {
  TestState state;
  const std::span<const draft::EmbeddedPackageFile> files =
      draft::embedded_core_files();
  EXPECT(state, !files.empty());
  EXPECT(state, std::is_sorted(files.begin(), files.end(),
                               [](const draft::EmbeddedPackageFile &left,
                                  const draft::EmbeddedPackageFile &right) {
                                 return left.relative_path <
                                        right.relative_path;
                               }));
  EXPECT(state, std::any_of(files.begin(), files.end(),
                            [](const draft::EmbeddedPackageFile &file) {
                              return file.relative_path ==
                                         "console/package.draft" &&
                                     file.contents.find("package console") !=
                                         std::string_view::npos;
                            }));
  EXPECT(state,
         draft::embedded_core_content_identity().starts_with("draft-core:"));

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::PackageLoadOptions options;
  options.file_tag = "aarch64-macos";
  options.embedded_files = files;
  options.embedded_package_path = "console";
  const draft::PackageLoadResult loaded =
      draft::load_package(sources, "core/console", options, diagnostics);
  EXPECT(state, loaded.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, loaded.package.embedded);
  EXPECT(state, loaded.package.short_name == "console");
  EXPECT(state, loaded.package.physical_directory == "core/console");
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
