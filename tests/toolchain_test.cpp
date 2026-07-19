// Native adapter tests with a recording toolchain process.
//
// The test does not depend on an installed cross compiler. A tiny executable
// records the argument vector and creates each requested output. This keeps the
// important contract under test: captured package assembly is written from the
// compiled snapshot, forced through the non-preprocessed assembler language,
// and included in the final deterministic link inputs.

#include "backend/toolchain.h"
#include "base/content_tree.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

// The recording Clang scripts below create empty link outputs, so a real
// dsymutil cannot consume them. This tiny companion records its exact argv and
// creates a deterministic bundle-shaped tree. Real LLVM dSYM behavior is
// covered by the native integration and release qualification gates.
void write_recording_dsymutil(
    const std::filesystem::path &path,
    const std::filesystem::path &log) {
  std::ofstream script(path, std::ios::binary);
  script << "#!/bin/sh\n"
            "printf '%s\\n' '-- dsymutil --' \"$@\" >> '"
         << log.string()
         << "'\n"
            "previous=''\n"
            "input=''\n"
            "output=''\n"
            "for argument in \"$@\"; do\n"
            "  if [ \"$previous\" = \"-o\" ]; then\n"
            "    output=\"$argument\"\n"
            "    previous=\"$argument\"\n"
            "    continue\n"
            "  fi\n"
            "  if [ \"$argument\" = \"-o\" ]; then\n"
            "    previous=\"$argument\"\n"
            "    continue\n"
            "  fi\n"
            "  input=\"$argument\"\n"
            "  previous=\"$argument\"\n"
            "done\n"
            "name=$(/usr/bin/basename \"$input\")\n"
            "/bin/mkdir -p \"$output/Contents/Resources/DWARF\" "
            "\"$output/Contents/Resources/Relocations/aarch64\"\n"
            ": > \"$output/Contents/Info.plist\"\n"
            ": > \"$output/Contents/Resources/DWARF/$name\"\n"
            "printf '%s\\n' \"binary-path: '$1'\" > "
            "\"$output/Contents/Resources/Relocations/aarch64/program.yml\"\n"
            "exit 0\n";
}

draft::CompileWorkspaceResult compile_fixture(
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics,
    bool emit_program_entry = true,
    std::string_view package = "external-assembly",
    const draft::TargetProfile &target =
        draft::make_aarch64_macos_profile()) {
  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.lower_mir = true;
  options.emit_llvm = true;
  options.emit_program_entry = emit_program_entry;
  return draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/" + std::string(package),
      std::move(options),
      diagnostics);
}

void test_explicit_foreign_provider_mapping(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      "draft-provider-link-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, !error);
  if (error) return;

  draft::SourceManager sources;
  draft::DiagnosticSink compile_diagnostics;
  const draft::CompileWorkspaceResult compiled = compile_fixture(
      sources, compile_diagnostics, true, "foreign-provider");
  EXPECT(state, compiled.ok);
  if (!compiled.ok) {
    std::filesystem::remove_all(temporary, error);
    return;
  }

  const std::filesystem::path provider = temporary / "provider.o";
  std::ofstream(provider, std::ios::binary) << "provider object bytes\n";
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
              "  if [ \"$previous\" = \"-o\" ]; then : > \"$argument\"; fi\n"
              "  previous=\"$argument\"\n"
              "done\n"
              "exit 0\n";
  }
  EXPECT(state, chmod(fake_clang.c_str(), 0700) == 0);
  const std::filesystem::path fake_dsymutil = temporary / "record-dsymutil";
  write_recording_dsymutil(fake_dsymutil, log);
  EXPECT(state, chmod(fake_dsymutil.c_str(), 0700) == 0);

  draft::NativeBuildOptions options;
  options.clang_path = fake_clang.string();
  options.dsymutil_path = fake_dsymutil.string();
  options.build_directory = (temporary / "build").string();
  options.output_path = (temporary / "program").string();
  draft::DiagnosticSink missing_diagnostics;
  const draft::NativeBuildResult missing = draft::build_native_artifact(
      draft::make_aarch64_macos_profile(),
      compiled,
      options,
      missing_diagnostics);
  EXPECT(state, !missing.ok);
  EXPECT(state, missing_diagnostics.has_errors());
  EXPECT(state, !std::filesystem::exists(log));

  draft::ForeignProviderInput mapping;
  mapping.provider = "custom_math";
  mapping.kind = draft::ForeignArtifactKind::Object;
  mapping.path = provider;
  options.foreign_providers.push_back(mapping);
  draft::DiagnosticSink mapped_diagnostics;
  const draft::NativeBuildResult mapped = draft::build_native_artifact(
      draft::make_aarch64_macos_profile(),
      compiled,
      options,
      mapped_diagnostics);
  EXPECT(state, mapped.ok);
  EXPECT(state, !mapped_diagnostics.has_errors());
  EXPECT(state, read_file(log).find(provider.string()) != std::string::npos);

  std::filesystem::remove_all(temporary, error);
}

void test_all_native_artifact_kinds(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      "draft-native-artifact-kinds-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, !error);
  if (error) return;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult compiled =
      compile_fixture(sources, diagnostics, false);
  EXPECT(state, compiled.ok);
  EXPECT(state, compiled.packages.size() == 1);
  if (!compiled.ok || compiled.packages.empty() ||
      !compiled.packages.front().has_value()) {
    std::filesystem::remove_all(temporary, error);
    return;
  }
  const std::string &llvm = compiled.packages.front()->llvm.text;
  EXPECT(state, llvm.find("define i32 @main(") == std::string::npos);
  EXPECT(state, llvm.find(
      "define hidden void @\"__draft.runtime.default_context\"") !=
      std::string::npos);

  const std::filesystem::path log = temporary / "arguments.log";
  const std::filesystem::path fake_clang = temporary / "record-clang";
  {
    std::ofstream script(fake_clang, std::ios::binary);
    script << "#!/bin/sh\n"
              "if [ \"$1\" = \"--version\" ]; then\n"
              "  echo 'clang version 22.1.0'\n"
              "  exit 0\n"
              "fi\n"
              "printf '%s\\n' '-- clang --' \"$@\" >> '"
           << log.string()
           << "'\n"
              "previous=''\n"
              "for argument in \"$@\"; do\n"
              "  if [ \"$previous\" = \"-o\" ]; then : > \"$argument\"; fi\n"
              "  previous=\"$argument\"\n"
              "done\n"
              "exit 0\n";
  }
  const std::filesystem::path fake_archiver = temporary / "record-ar";
  {
    std::ofstream script(fake_archiver, std::ios::binary);
    script << "#!/bin/sh\n"
              "printf '%s\\n' '-- ar --' \"$@\" >> '"
           << log.string()
           << "'\n"
              "previous=''\n"
              "for argument in \"$@\"; do\n"
              "  if [ \"$previous\" = \"-o\" ]; then : > \"$argument\"; fi\n"
              "  previous=\"$argument\"\n"
              "done\n"
              "exit 0\n";
  }
  EXPECT(state, chmod(fake_clang.c_str(), 0700) == 0);
  EXPECT(state, chmod(fake_archiver.c_str(), 0700) == 0);
  const std::filesystem::path fake_dsymutil = temporary / "record-dsymutil";
  write_recording_dsymutil(fake_dsymutil, log);
  EXPECT(state, chmod(fake_dsymutil.c_str(), 0700) == 0);

  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  const auto build = [&](draft::NativeArtifactKind kind,
                         const std::filesystem::path &output) {
    draft::NativeBuildOptions options;
    options.clang_path = fake_clang.string();
    options.archiver_path = fake_archiver.string();
    options.dsymutil_path = fake_dsymutil.string();
    options.build_directory = (temporary / "build").string();
    options.output_path = output.string();
    options.artifact_kind = kind;
    return draft::build_native_artifact(
        target, compiled, options, diagnostics);
  };

  EXPECT(state, build(
      draft::NativeArtifactKind::Object,
      temporary / "library.o").ok);
  EXPECT(state, build(
      draft::NativeArtifactKind::StaticLibrary,
      temporary / "library.a").ok);
  EXPECT(state, build(
      draft::NativeArtifactKind::DynamicLibrary,
      temporary / "library.dylib").ok);
  EXPECT(state, build(
      draft::NativeArtifactKind::Assembly,
      temporary / "assembly").ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, std::filesystem::exists(temporary / "library.o"));
  EXPECT(state, std::filesystem::exists(temporary / "library.a"));
  EXPECT(state, std::filesystem::exists(temporary / "library.dylib"));
  EXPECT(state, std::filesystem::exists(temporary / "library.dylib.dSYM"));
  EXPECT(state, std::filesystem::exists(temporary / "assembly" / "package-0.s"));
  EXPECT(state, read_file(
      temporary / "assembly" / "package-0-assembly-0.s") ==
      compiled.packages.front()->assembly_sources.front().contents);

  const std::string arguments = read_file(log);
  EXPECT(state, arguments.find("\n-Wl,-r\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-no_uuid\n") == std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-reproducible\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-dynamiclib\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-- dsymutil --\n") != std::string::npos);
  EXPECT(state,
      arguments.find("\n-- ar --\n-static\n-D\n-o\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-S\n") != std::string::npos);

  std::filesystem::remove_all(temporary, error);
}

// The recording process makes the ELF driver contract observable without
// requiring a Linux sysroot in every unit-test environment. Real LLVM object
// acceptance and hosted execution are separate qualification gates.
void test_aarch64_linux_toolchain_arguments(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      "draft-aarch64-linux-toolchain-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary, error);
  EXPECT(state, !error);
  if (error) return;

  const draft::TargetProfile target = draft::make_aarch64_linux_profile();
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult compiled = compile_fixture(
      sources, diagnostics, true, "hello", target);
  EXPECT(state, compiled.ok);
  if (!compiled.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    std::filesystem::remove_all(temporary, error);
    return;
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
              "printf '%s\\n' '-- clang --' \"$@\" >> '"
           << log.string()
           << "'\n"
              "previous=''\n"
              "for argument in \"$@\"; do\n"
              "  if [ \"$previous\" = \"-o\" ]; then : > \"$argument\"; fi\n"
              "  previous=\"$argument\"\n"
              "done\n"
              "exit 0\n";
  }
  const std::filesystem::path fake_archiver = temporary / "record-ar";
  {
    std::ofstream script(fake_archiver, std::ios::binary);
    script << "#!/bin/sh\n"
              "printf '%s\\n' '-- ar --' \"$@\" >> '"
           << log.string()
           << "'\n"
              ": > \"$2\"\n"
              "exit 0\n";
  }
  EXPECT(state, chmod(fake_clang.c_str(), 0700) == 0);
  EXPECT(state, chmod(fake_archiver.c_str(), 0700) == 0);

  const auto build = [&](draft::NativeArtifactKind kind,
                         const std::filesystem::path &output) {
    draft::NativeBuildOptions options;
    options.clang_path = fake_clang.string();
    options.archiver_path = fake_archiver.string();
    options.build_directory = (temporary / "build").string();
    options.output_path = output.string();
    options.artifact_kind = kind;
    return draft::build_native_artifact(
        target, compiled, options, diagnostics);
  };

  EXPECT(state, build(
      draft::NativeArtifactKind::Executable,
      temporary / "program").ok);
  EXPECT(state, build(
      draft::NativeArtifactKind::Object,
      temporary / "library.o").ok);
  EXPECT(state, build(
      draft::NativeArtifactKind::StaticLibrary,
      temporary / "library.a").ok);
  const draft::NativeBuildResult dynamic = build(
      draft::NativeArtifactKind::DynamicLibrary,
      temporary / "library.so");
  EXPECT(state, dynamic.ok);
  EXPECT(state, dynamic.debug_symbols_path.empty());
  EXPECT(state, build(
      draft::NativeArtifactKind::Assembly,
      temporary / "assembly").ok);
  EXPECT(state, !diagnostics.has_errors());

  const std::string arguments = read_file(log);
  EXPECT(state, arguments.find("\naarch64-unknown-linux-gnu\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("-mmacosx-version-min") == std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,--build-id=sha1\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\n-fuse-ld=lld\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-no-pie\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-shared\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-soname,library.so\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\n-lc\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-dynamiclib\n") == std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-reproducible\n") ==
      std::string::npos);
  EXPECT(state, arguments.find("-- dsymutil --") == std::string::npos);
  EXPECT(state, arguments.find("\n-- ar --\nrcsD\n") !=
      std::string::npos);

  std::filesystem::remove_all(temporary, error);
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
  const draft::CompileWorkspaceResult compiled =
      compile_fixture(sources, diagnostics);
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
    EXPECT(state, package.assembly_sources.front().relative_name ==
        "native@aarch64-macos.s");
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
  const std::filesystem::path fake_dsymutil = temporary / "record-dsymutil";
  write_recording_dsymutil(fake_dsymutil, log);
  EXPECT(state, chmod(fake_dsymutil.c_str(), 0700) == 0);

  draft::NativeBuildOptions native_options;
  native_options.clang_path = fake_clang.string();
  native_options.dsymutil_path = fake_dsymutil.string();
  native_options.build_directory = (temporary / "build").string();
  native_options.output_path = (temporary / "program").string();
  const draft::NativeBuildResult built = draft::build_native_executable(
      target, compiled, native_options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, built.ok);
  EXPECT(state, std::filesystem::exists(temporary / "program"));
  EXPECT(state, built.debug_symbols_path ==
      (temporary / "program.dSYM").string());
  draft::Sha256Digest debug_symbols_digest;
  draft::DiagnosticSink debug_digest_diagnostics;
  EXPECT(state, draft::hash_content_tree(
      temporary / "program.dSYM",
      debug_symbols_digest,
      debug_digest_diagnostics));
  EXPECT(state, built.debug_symbols_digest == debug_symbols_digest);
  EXPECT(state, !std::filesystem::exists(
      temporary / "program.dSYM" / "Contents" / "Resources" /
          "Relocations"));
  EXPECT(
      state,
      built.source_correlation_path ==
          (temporary / "build" / "draft-source-correlation.json").string());
  const std::string source_correlation =
      read_file(temporary / "build" / "draft-source-correlation.json");
  EXPECT(state, !source_correlation.empty());
  EXPECT(state, built.source_correlation_digest ==
      draft::sha256(source_correlation));
  EXPECT(state, source_correlation.find("llvm-modules-sha256:") !=
      std::string::npos);

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
  test_explicit_foreign_provider_mapping(state);
  test_all_native_artifact_kinds(state);
  test_aarch64_linux_toolchain_arguments(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " toolchain expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all toolchain tests passed\n";
  return EXIT_SUCCESS;
}
