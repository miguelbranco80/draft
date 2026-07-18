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

draft::CompileWorkspaceResult compile_fixture(
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics,
    bool emit_program_entry = true,
    std::string_view package = "external-assembly") {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
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

  draft::NativeBuildOptions options;
  options.clang_path = fake_clang.string();
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

  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
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
  EXPECT(state, std::filesystem::exists(temporary / "assembly" / "package-0.s"));
  EXPECT(state, read_file(
      temporary / "assembly" / "package-0-assembly-0.s") ==
      compiled.packages.front()->assembly_sources.front().contents);

  const std::string arguments = read_file(log);
  EXPECT(state, arguments.find("\n-Wl,-r\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-no_uuid\n") == std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-reproducible\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-dynamiclib\n") != std::string::npos);
  EXPECT(state,
      arguments.find("\n-- ar --\n-static\n-D\n-o\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-S\n") != std::string::npos);

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

void test_locked_build_verifies_and_isolates_inputs(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      "draft-locked-toolchain-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(temporary, error);
  error.clear();
  const std::filesystem::path toolchain = temporary / "toolchain";
  const std::filesystem::path sdk = temporary / "sdk";
  std::filesystem::create_directories(toolchain / "bin", error);
  EXPECT(state, !error);
  std::filesystem::create_directories(sdk / "usr" / "lib", error);
  EXPECT(state, !error);
  if (error) return;

  const std::filesystem::path log = temporary / "locked-arguments.log";
  const std::filesystem::path clang = toolchain / "bin" / "clang";
  {
    std::ofstream script(clang, std::ios::binary);
    script << "#!/bin/sh\n"
              "is_version=''\n"
              "for argument in \"$@\"; do\n"
              "  if [ \"$argument\" = \"--version\" ]; then is_version=1; fi\n"
              "done\n"
              "if [ -n \"$is_version\" ]; then\n"
              "  echo 'clang version 22.1.0'\n"
              "  exit 0\n"
              "fi\n"
              "printf '%s\\n' '-- command --' \"$@\" >> '"
           << log.string()
           << "'\n"
              "printf 'ENV:%s|%s|%s|%s\\n' \"$PATH\" \"${SDKROOT-unset}\" "
              "\"${CPATH-unset}\" \"${LIBRARY_PATH-unset}\" >> '"
           << log.string()
           << "'\n"
              "previous=''\n"
              "for argument in \"$@\"; do\n"
              "  if [ \"$previous\" = \"-o\" ]; then : > \"$argument\"; fi\n"
              "  previous=\"$argument\"\n"
              "done\n"
              "exit 0\n";
  }
  const std::filesystem::path linker = toolchain / "bin" / "ld64.lld";
  {
    std::ofstream script(linker, std::ios::binary);
    script << "#!/bin/sh\nexit 0\n";
  }
  const std::filesystem::path archiver = toolchain / "bin" / "llvm-ar";
  {
    std::ofstream script(archiver, std::ios::binary);
    script << "#!/bin/sh\n"
              "printf '%s\\n' '-- archive --' \"$@\" >> '"
           << log.string()
           << "'\n"
              ": > \"$2\"\n"
              "exit 0\n";
  }
  std::ofstream(sdk / "usr" / "lib" / "libSystem.tbd", std::ios::binary)
      << "pinned SDK bytes\n";
  EXPECT(state, chmod(clang.c_str(), 0700) == 0);
  EXPECT(state, chmod(linker.c_str(), 0700) == 0);
  EXPECT(state, chmod(archiver.c_str(), 0700) == 0);

  draft::LockedNativeInputRoots roots;
  roots.toolchain_root = toolchain;
  roots.sdk_root = sdk;
  std::vector<draft::ExternalInputPin> pins;
  draft::DiagnosticSink pin_diagnostics;
  EXPECT(state,
      draft::pin_locked_native_inputs(roots, pins, pin_diagnostics));
  EXPECT(state, !pin_diagnostics.has_errors());
  EXPECT(state, pins.size() == 2);

  draft::SourceManager sources;
  draft::DiagnosticSink compile_diagnostics;
  draft::CompileWorkspaceResult compiled =
      compile_fixture(sources, compile_diagnostics);
  EXPECT(state, compiled.ok);
  compiled.resolution_manifest.emplace();
  compiled.resolution_manifest->target_identity =
      draft::make_aarch64_macos_profile().facts.identity;
  compiled.resolution_manifest->external_inputs = pins;

  draft::NativeBuildOptions options;
  options.locked = true;
  options.locked_inputs = roots;
  options.build_directory = (temporary / "build").string();
  options.output_path = (temporary / "program").string();
  draft::DiagnosticSink build_diagnostics;
  const draft::NativeBuildResult built = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      compiled,
      options,
      build_diagnostics);
  if (!built.ok) {
    std::cerr << draft::render_diagnostics(sources, build_diagnostics);
  }
  EXPECT(state, built.ok);
  EXPECT(state, std::filesystem::exists(temporary / "program"));

  const std::string arguments = read_file(log);
  EXPECT(state,
      arguments.find("\n--no-default-config\n") != std::string::npos);
  EXPECT(state,
      arguments.find("\n--no-xcselect\n") != std::string::npos);
  const std::filesystem::path canonical_sdk =
      std::filesystem::canonical(sdk, error);
  const std::filesystem::path canonical_linker =
      std::filesystem::canonical(linker, error);
  EXPECT(state, !error);
  EXPECT(state,
      arguments.find("\n-isysroot\n" + canonical_sdk.string()) !=
          std::string::npos);
  EXPECT(state,
      arguments.find("\n--ld-path=" + canonical_linker.string() + "\n") !=
          std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-no_uuid\n") == std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-reproducible\n") != std::string::npos);
  EXPECT(state,
      arguments.find("ENV:|unset|unset|unset") != std::string::npos);

  // Repeating the same build emits the exact same process arguments. Paths are
  // intentionally unchanged here: output location is an explicit build input,
  // while filesystem enumeration and the host environment are not.
  draft::DiagnosticSink repeated_diagnostics;
  const draft::NativeBuildResult repeated = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      compiled,
      options,
      repeated_diagnostics);
  EXPECT(state, repeated.ok);
  EXPECT(state, !repeated_diagnostics.has_errors());
  EXPECT(state, read_file(log) == arguments + arguments);

  // The locked archive path uses the archiver inside the already pinned
  // toolchain tree. Every archive build, locked or not, requests deterministic
  // member metadata because repeatability is an artifact invariant.
  draft::NativeBuildOptions archive_options = options;
  archive_options.artifact_kind = draft::NativeArtifactKind::StaticLibrary;
  archive_options.output_path = (temporary / "library.a").string();
  draft::DiagnosticSink archive_diagnostics;
  const draft::NativeBuildResult archive = draft::build_native_artifact(
      draft::make_aarch64_macos_profile(),
      compiled,
      archive_options,
      archive_diagnostics);
  EXPECT(state, archive.ok);
  EXPECT(state, !archive_diagnostics.has_errors());
  EXPECT(state, std::filesystem::exists(temporary / "library.a"));
  EXPECT(state, read_file(log).find("\n-- archive --\nrcsD\n") !=
      std::string::npos);

  // A changed SDK is rejected before a second compiler process starts.
  std::ofstream(sdk / "usr" / "lib" / "libSystem.tbd", std::ios::binary)
      << "mutated SDK bytes\n";
  const std::string log_before_failure = read_file(log);
  draft::NativeBuildOptions stale_options = options;
  stale_options.build_directory = (temporary / "stale-build").string();
  stale_options.output_path = (temporary / "stale-program").string();
  draft::DiagnosticSink stale_diagnostics;
  const draft::NativeBuildResult stale = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      compiled,
      stale_options,
      stale_diagnostics);
  EXPECT(state, !stale.ok);
  EXPECT(state, stale_diagnostics.has_errors());
  EXPECT(state, read_file(log) == log_before_failure);

  std::filesystem::remove_all(temporary, error);
}

} // namespace

int main() {
  TestState state;
  test_package_assembly_reaches_link(state);
  test_explicit_foreign_provider_mapping(state);
  test_all_native_artifact_kinds(state);
  test_locked_build_verifies_and_isolates_inputs(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " toolchain expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all toolchain tests passed\n";
  return EXIT_SUCCESS;
}
