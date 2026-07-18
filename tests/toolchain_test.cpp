// Native adapter tests with a recording toolchain process.
//
// The test does not depend on an installed cross compiler. A tiny executable
// records the argument vector and creates each requested output. This keeps the
// important contract under test: captured package assembly is written from the
// compiled snapshot, forced through the non-preprocessed assembler language,
// and included in the final deterministic link inputs.

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

#include <sys/stat.h>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "toolchain_test.cpp:" << line
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

void test_package_assembly_reaches_link(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      "draft-bootstrap-toolchain-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, !error);
  if (error) return;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
  compile_options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  compile_options.lower_mir = true;
  compile_options.emit_llvm = true;
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/external-assembly",
      std::move(compile_options),
      diagnostics);
  EXPECT(state, compiled.ok);
  EXPECT(state, compiled.packages.size() == 1);
  if (!compiled.ok || compiled.packages.size() != 1 ||
      !compiled.packages.front().has_value()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    std::filesystem::remove_all(temporary, error);
    return;
  }
  const draft::CompiledPackage &package = *compiled.packages.front();
  EXPECT(state, package.assembly_sources.size() == 1);
  if (package.assembly_sources.size() == 1) {
    EXPECT(state, package.assembly_sources.front().relative_name == "native.s");
    EXPECT(state, package.assembly_sources.front().contents.find(
        "_draft_external_add:") != std::string::npos);
  }

  const std::filesystem::path log = temporary / "arguments.log";
  const std::filesystem::path fake_clang = temporary / "record-clang";
  {
    std::ofstream script(fake_clang, std::ios::binary);
    script << "#!/bin/sh\n"
              "if [ \"$1\" = \"--version\" ]; then\n"
              "  echo 'clang version 22.1.0'\n"
              "  exit 0\n"
              "fi\n"
              "printf '%s\\n' '-- command --' \"$@\" >> '"
           << log.string()
           << "'\n"
              "previous=''\n"
              "for argument in \"$@\"; do\n"
              "  if [ \"$previous\" = \"-o\" ]; then\n"
              "    : > \"$argument\"\n"
              "  fi\n"
              "  previous=\"$argument\"\n"
              "done\n"
              "exit 0\n";
  }
  EXPECT(state, chmod(fake_clang.c_str(), 0700) == 0);

  draft::NativeBuildOptions native_options;
  native_options.clang_path = fake_clang.string();
  native_options.build_directory = (temporary / "build").string();
  native_options.output_path = (temporary / "program").string();
  const draft::NativeBuildResult built = draft::build_native_executable(
      target, compiled, native_options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, built.ok);
  EXPECT(state, std::filesystem::exists(temporary / "program"));

  const std::string arguments = read_file(log);
  EXPECT(state, arguments.find(
      "\n-x\nassembler\n-c\n") != std::string::npos);
  EXPECT(state, arguments.find(
      "package-0-assembly-0.s") != std::string::npos);
  EXPECT(state, arguments.find(
      "package-0-assembly-0.o") != std::string::npos);
  EXPECT(state, read_file(temporary / "build" / "package-0-assembly-0.s") ==
      package.assembly_sources.front().contents);

  std::filesystem::remove_all(temporary, error);
}

} // namespace

int main() {
  TestState state;
  test_package_assembly_reaches_link(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " toolchain expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all toolchain tests passed\n";
  return EXIT_SUCCESS;
}
