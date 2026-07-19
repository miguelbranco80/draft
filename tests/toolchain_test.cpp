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
// covered by the native integration and pinned-release qualification gates.
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

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
void append_u32(std::uint32_t value, std::vector<unsigned char> &bytes) {
  bytes.push_back(static_cast<unsigned char>(value & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

// Produces just enough of a valid arm64 MH_DYLIB for the recording-toolchain
// test. The production closure parser validates this file exactly as it does
// the real Clang runtime; the recording linker only needs a stable byte input.
[[nodiscard]] bool write_synthetic_address_runtime(
    const std::filesystem::path &path) {
  constexpr std::string_view install_name =
      "@rpath/libclang_rt.asan_osx_dynamic.dylib";
  constexpr std::uint32_t string_offset = 24;
  const std::uint32_t command_size = static_cast<std::uint32_t>(
      (string_offset + install_name.size() + 1U + 7U) & ~7U);

  std::vector<unsigned char> command;
  append_u32(0x0000000dU, command); // LC_ID_DYLIB.
  append_u32(command_size, command);
  append_u32(string_offset, command);
  append_u32(0, command); // Timestamp.
  append_u32(0, command); // Current version.
  append_u32(0, command); // Compatibility version.
  command.insert(command.end(), install_name.begin(), install_name.end());
  command.push_back(0);
  command.resize(command_size, 0);

  std::vector<unsigned char> bytes;
  append_u32(0xfeedfacfU, bytes); // Thin little-endian Mach-O 64.
  append_u32(0x0100000cU, bytes); // CPU_TYPE_ARM64.
  append_u32(0, bytes);          // CPU subtype.
  append_u32(6, bytes);          // MH_DYLIB.
  append_u32(1, bytes);          // One load command.
  append_u32(command_size, bytes);
  append_u32(0, bytes); // Flags.
  append_u32(0, bytes); // Reserved.
  bytes.insert(bytes.end(), command.begin(), command.end());

  std::ofstream output(path, std::ios::binary);
  output.write(
      reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  output.close();
  return output.good();
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
  const std::filesystem::path sanitizer_directory =
      toolchain / "lib" / "clang" / "22" / "lib" / "darwin";
  std::filesystem::create_directories(sanitizer_directory, error);
  EXPECT(state, !error);
  std::filesystem::create_directories(sdk / "usr" / "lib", error);
  EXPECT(state, !error);
  if (error) return;

  const std::filesystem::path log = temporary / "locked-arguments.log";
  const std::filesystem::path clang = toolchain / "bin" / "clang";
  const std::filesystem::path linker = toolchain / "bin" / "ld";
  const std::filesystem::path classic_linker =
      toolchain / "bin" / "ld-classic";
  const std::filesystem::path archiver = toolchain / "bin" / "llvm-ar";
  const std::filesystem::path dsymutil = toolchain / "bin" / "dsymutil";
  const std::filesystem::path symbolizer =
      toolchain / "bin" / "llvm-symbolizer";
  const std::filesystem::path address_runtime =
      sanitizer_directory / "libclang_rt.asan_osx_dynamic.dylib";
  // All five names use one CMake-built Mach-O recorder. Unlike shell scripts,
  // its complete dynamic dependency closure can be checked by the same locked
  // input policy used for a release LLVM distribution.
  for (const std::filesystem::path &tool :
       {clang, linker, classic_linker, archiver, dsymutil, symbolizer}) {
    error.clear();
    std::filesystem::copy_file(
        DRAFT_RECORDING_NATIVE_TOOL,
        tool,
        std::filesystem::copy_options::overwrite_existing,
        error);
    EXPECT(state, !error);
  }
  EXPECT(state, write_synthetic_address_runtime(address_runtime));
  std::ofstream(sdk / "usr" / "lib" / "libSystem.tbd", std::ios::binary)
      << "pinned SDK bytes\n";
  const std::filesystem::path runtime_asset = temporary / "unicode-tables.bin";
  std::ofstream(runtime_asset, std::ios::binary) << "runtime table bytes\n";
  EXPECT(state, chmod(clang.c_str(), 0700) == 0);
  EXPECT(state, chmod(linker.c_str(), 0700) == 0);
  EXPECT(state, chmod(classic_linker.c_str(), 0700) == 0);
  EXPECT(state, chmod(archiver.c_str(), 0700) == 0);
  EXPECT(state, chmod(dsymutil.c_str(), 0700) == 0);
  EXPECT(state, chmod(symbolizer.c_str(), 0700) == 0);

  draft::LockedNativeInputRoots roots;
  roots.toolchain_root = toolchain;
  roots.sdk_root = sdk;
  std::vector<draft::ExternalInputPin> pins;
  draft::DiagnosticSink pin_diagnostics;
  EXPECT(state,
      draft::pin_locked_native_inputs(
          draft::make_aarch64_macos_profile(),
          roots,
          pins,
          pin_diagnostics));
  draft::RuntimeAssetInput asset_mapping;
  asset_mapping.name = "unicode-tables";
  asset_mapping.path = runtime_asset;
  EXPECT(state,
      draft::pin_runtime_asset_inputs(
          std::span<const draft::RuntimeAssetInput>(&asset_mapping, 1),
          pins,
          pin_diagnostics));
  EXPECT(state, !pin_diagnostics.has_errors());
  EXPECT(state, pins.size() == 3);

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
  options.runtime_assets.push_back(asset_mapping);
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
  EXPECT(state,
      built.debug_symbols_path == (temporary / "program.dSYM").string());
  EXPECT(state, std::filesystem::exists(built.debug_symbols_path));
  EXPECT(state, !std::filesystem::exists(
      temporary / "program.dSYM" / "Contents" / "Resources" /
          "Relocations"));
  const std::string locked_source_correlation =
      read_file(temporary / "build" / "draft-source-correlation.json");
  EXPECT(state, !locked_source_correlation.empty());
  EXPECT(state, built.source_correlation_digest ==
      draft::sha256(locked_source_correlation));
  EXPECT(state, built.runtime_assets.size() == 1);
  if (built.runtime_assets.size() == 1) {
    EXPECT(state, built.runtime_assets.front().name == "unicode-tables");
    EXPECT(state,
        built.runtime_assets.front().path ==
            std::filesystem::canonical(runtime_asset));
  }

  const std::string arguments = read_file(log);
  EXPECT(state,
      arguments.find("\n--no-default-config\n") != std::string::npos);
  EXPECT(state,
      arguments.find("\n--no-xcselect\n") == std::string::npos);
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
  EXPECT(state, repeated.source_correlation_digest ==
      built.source_correlation_digest);
  EXPECT(state, repeated.debug_symbols_digest == built.debug_symbols_digest);
  EXPECT(
      state,
      read_file(temporary / "build" / "draft-source-correlation.json") ==
          locked_source_correlation);
  EXPECT(state, read_file(log) == arguments + arguments);

  // The address profile is a fixed backend recipe, not arbitrary driver
  // passthrough. It instruments only LLVM IR compilation, links the verified
  // runtime snapshot, publishes that dylib beside the executable, and records
  // the exact runpath needed by its relocatable install name.
  draft::NativeBuildOptions instrumented_options = options;
  instrumented_options.instrumentation =
      draft::NativeInstrumentationProfile::AddressSanitizer;
  instrumented_options.build_directory =
      (temporary / "instrumented-build").string();
  instrumented_options.output_path =
      (temporary / "instrumented-program").string();
  draft::DiagnosticSink instrumented_diagnostics;
  const draft::NativeBuildResult instrumented =
      draft::build_native_executable(
          draft::make_aarch64_macos_profile(),
          compiled,
          instrumented_options,
          instrumented_diagnostics);
  EXPECT(state, instrumented.ok);
  EXPECT(state, !instrumented_diagnostics.has_errors());
  EXPECT(state,
      instrumented.instrumentation_runtime_path ==
          (temporary / "libclang_rt.asan_osx_dynamic.dylib").string());
  EXPECT(state,
      instrumented.instrumentation_symbolizer_path ==
          std::filesystem::canonical(symbolizer).string());
  EXPECT(state,
      read_file(instrumented.instrumentation_runtime_path) ==
          read_file(address_runtime));
  const std::string instrumented_arguments = read_file(log);
  EXPECT(state, instrumented_arguments.find("\n-fsanitize=address\n") !=
      std::string::npos);
  EXPECT(state, instrumented_arguments.find("\n-fno-omit-frame-pointer\n") !=
      std::string::npos);
  EXPECT(state, instrumented_arguments.find(
      "\n-Wl,-rpath,@executable_path\n") != std::string::npos);
  EXPECT(state, instrumented_arguments.find(
      (temporary / "instrumented-build" /
       "libclang_rt.asan_osx_dynamic.dylib").string()) !=
      std::string::npos);
  EXPECT(state, read_file(
      temporary / "instrumented-build" / "package-0.ll").find(
          "sanitize_address") != std::string::npos);

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

  // Runtime assets are verified before any compiler process. They are returned
  // to the embedding build system for deployment, never appended to the Clang
  // or linker command merely because a manifest names them.
  std::ofstream(runtime_asset, std::ios::binary | std::ios::trunc)
      << "mutated runtime table bytes\n";
  const std::string log_before_asset_failure = read_file(log);
  draft::NativeBuildOptions stale_asset_options = options;
  stale_asset_options.build_directory =
      (temporary / "stale-asset-build").string();
  stale_asset_options.output_path = (temporary / "stale-asset-program").string();
  draft::DiagnosticSink stale_asset_diagnostics;
  const draft::NativeBuildResult stale_asset = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      compiled,
      stale_asset_options,
      stale_asset_diagnostics);
  EXPECT(state, !stale_asset.ok);
  EXPECT(state, stale_asset_diagnostics.has_errors());
  EXPECT(state, read_file(log) == log_before_asset_failure);
  std::ofstream(runtime_asset, std::ios::binary | std::ios::trunc)
      << "runtime table bytes\n";

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

  // Apple ld delegates relocatable output to this sibling. Omitting it would
  // leave final links apparently usable while object output failed at runtime,
  // so the helper is a mandatory verified entry rather than an implicit host
  // dependency.
  error.clear();
  EXPECT(state, std::filesystem::remove(classic_linker, error));
  EXPECT(state, !error);
  draft::DiagnosticSink missing_helper_diagnostics;
  std::vector<draft::ExternalInputPin> missing_helper_pins;
  EXPECT(state,
      !draft::pin_locked_native_inputs(
          draft::make_aarch64_macos_profile(),
          roots, missing_helper_pins, missing_helper_diagnostics));
  EXPECT(state, missing_helper_diagnostics.has_errors());

  std::filesystem::remove_all(temporary, error);
}

// Linux uses the same content-tree and clean-process guarantees but selects a
// different closed set of entry paths and manifest names. This recording test
// keeps that boundary independent of the slower real Ubuntu execution gate.
void test_locked_aarch64_linux_inputs(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      "draft-locked-aarch64-linux-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(temporary, error);
  error.clear();
  const std::filesystem::path toolchain = temporary / "toolchain";
  const std::filesystem::path sysroot = temporary / "sysroot";
  std::filesystem::create_directories(toolchain / "bin", error);
  EXPECT(state, !error);
  std::filesystem::create_directories(sysroot / "usr" / "lib", error);
  EXPECT(state, !error);
  if (error) return;

  for (const std::string_view name : {"clang", "ld.lld", "llvm-ar"}) {
    const std::filesystem::path tool = toolchain / "bin" / name;
    std::filesystem::copy_file(
        DRAFT_RECORDING_NATIVE_TOOL,
        tool,
        std::filesystem::copy_options::overwrite_existing,
        error);
    EXPECT(state, !error);
    error.clear();
    EXPECT(state, chmod(tool.c_str(), 0700) == 0);
  }
  std::ofstream(sysroot / "usr" / "lib" / "libc.so", std::ios::binary)
      << "fixed glibc sysroot fixture\n";

  const draft::TargetProfile target = draft::make_aarch64_linux_profile();
  draft::LockedNativeInputRoots roots;
  roots.toolchain_root = toolchain;
  roots.sdk_root = sysroot;
  std::vector<draft::ExternalInputPin> pins;
  draft::DiagnosticSink pin_diagnostics;
  EXPECT(state, draft::pin_locked_native_inputs(
      target, roots, pins, pin_diagnostics));
  EXPECT(state, !pin_diagnostics.has_errors());
  EXPECT(state, pins.size() == 2);
  if (pins.size() == 2) {
    const bool has_toolchain = std::any_of(
        pins.begin(), pins.end(), [](const draft::ExternalInputPin &pin) {
          return pin.name == "llvm-aarch64-linux";
        });
    const bool has_sysroot = std::any_of(
        pins.begin(), pins.end(), [](const draft::ExternalInputPin &pin) {
          return pin.name == "aarch64-linux-sysroot";
        });
    EXPECT(state, has_toolchain);
    EXPECT(state, has_sysroot);
  }

  draft::SourceManager sources;
  draft::DiagnosticSink compile_diagnostics;
  draft::CompileWorkspaceResult compiled = compile_fixture(
      sources, compile_diagnostics, true, "hello", target);
  EXPECT(state, compiled.ok);
  compiled.resolution_manifest.emplace();
  compiled.resolution_manifest->target_identity = target.facts.identity;
  compiled.resolution_manifest->external_inputs = pins;

  draft::NativeBuildOptions options;
  options.locked = true;
  options.locked_inputs = roots;
  options.build_directory = (temporary / "build").string();
  options.output_path = (temporary / "program").string();
  draft::DiagnosticSink build_diagnostics;
  const draft::NativeBuildResult built = draft::build_native_executable(
      target, compiled, options, build_diagnostics);
  if (!built.ok) {
    std::cerr << draft::render_diagnostics(sources, build_diagnostics);
  }
  EXPECT(state, built.ok);
  EXPECT(state, built.debug_symbols_path.empty());

  const std::string arguments = read_file(
      temporary / "locked-arguments.log");
  const std::filesystem::path canonical_sysroot =
      std::filesystem::canonical(sysroot, error);
  EXPECT(state, !error);
  error.clear();
  const std::filesystem::path canonical_toolchain =
      std::filesystem::canonical(toolchain, error);
  EXPECT(state, !error);
  EXPECT(state, arguments.find(
      "\n--sysroot=" + canonical_sysroot.string() + "\n") !=
      std::string::npos);
  EXPECT(state, arguments.find(
      "\n--ld-path=" +
          (canonical_toolchain / "bin" / "ld.lld").string() + "\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\naarch64-unknown-linux-gnu\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,--build-id=sha1\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("-mmacosx-version-min") == std::string::npos);
  EXPECT(state, arguments.find("-- dsymutil --") == std::string::npos);

  std::filesystem::remove_all(temporary, error);
}
#endif

} // namespace

int main() {
  TestState state;
  test_package_assembly_reaches_link(state);
  test_explicit_foreign_provider_mapping(state);
  test_all_native_artifact_kinds(state);
  test_aarch64_linux_toolchain_arguments(state);
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  test_locked_build_verifies_and_isolates_inputs(state);
  test_locked_aarch64_linux_inputs(state);
#endif
  if (state.failures != 0) {
    std::cerr << state.failures << " toolchain expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all toolchain tests passed\n";
  return EXIT_SUCCESS;
}
