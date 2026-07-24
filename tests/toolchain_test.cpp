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

#include "test_directory.h"

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

// Builds a self-contained Windows package below the test's private directory.
// It deliberately needs no core package: this toolchain test isolates COFF
// object naming, package assembly, PE linker arguments, and companion
// publication before the hosted Windows core/runtime integration layer runs.
draft::CompileWorkspaceResult compile_windows_fixture(
    const std::filesystem::path &temporary,
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics,
    bool include_assembly = true) {
  const std::filesystem::path package = temporary /
      (include_assembly ? "windows-package-with-assembly"
                        : "windows-package-object");
  std::error_code error;
  std::filesystem::create_directories(package, error);
  if (error) return {};
  std::ofstream source(package / "package.draft", std::ios::binary);
  source << "package windows_toolchain\n\n";
  if (include_assembly) {
    source << "foreign package_assembly {\n"
              "    add as \"draft_windows_add\" :: c proc(left, right: i32) -> i32\n"
              "}\n\n";
  }
  source << "export identity as \"draft_windows_identity\" :: c proc(value: i32) -> i32 {\n"
            "    return value\n"
            "}\n\n"
            "main :: proc() -> int {\n";
  source << (include_assembly
      ? "    return cast[int](add(20, 22) - 42)\n"
      : "    return 0\n");
  source << "}\n";
  if (include_assembly) {
    std::ofstream(package / "native@x86_64-windows.s", std::ios::binary)
        << ".text\n"
           ".globl draft_windows_add\n"
           ".def draft_windows_add; .scl 2; .type 32; .endef\n"
           "draft_windows_add:\n"
           "    leal (%rcx,%rdx), %eax\n"
           "    retq\n";
  }
  source.close();

  draft::CompileWorkspaceOptions options;
  options.target = draft::make_x86_64_windows_profile();
  options.workspace.workspace_directory = temporary.string();
  options.lower_mir = true;
  options.emit_llvm = true;
  options.emit_program_entry = true;
  return draft::compile_workspace(
      sources, package, std::move(options), diagnostics);
}

void test_explicit_foreign_provider_mapping(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-provider-link-test"};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::error_code error;
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
  options.object_emitter =
      draft::NativeObjectEmitter::ExternalClangOracle;
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

  // ELF shared providers use a relocatable sibling-library convention. This
  // argument-level test fixes the exact linker spelling without needing to run
  // a Linux loader on the macOS test host.
  draft::SourceManager linux_sources;
  draft::DiagnosticSink linux_compile_diagnostics;
  const draft::CompileWorkspaceResult linux_compiled = compile_fixture(
      linux_sources, linux_compile_diagnostics, true, "foreign-provider",
      draft::make_x86_64_linux_profile());
  EXPECT(state, linux_compiled.ok);
  const std::filesystem::path shared_provider = temporary / "provider.so";
  std::ofstream(shared_provider, std::ios::binary) << "shared provider bytes\n";
  std::ofstream(log, std::ios::binary | std::ios::trunc).close();
  draft::NativeBuildOptions linux_options;
  linux_options.clang_path = fake_clang.string();
  linux_options.build_directory = (temporary / "linux-build").string();
  linux_options.output_path = (temporary / "linux-program").string();
  linux_options.object_emitter =
      draft::NativeObjectEmitter::ExternalClangOracle;
  draft::ForeignProviderInput shared_mapping;
  shared_mapping.provider = "custom_math";
  shared_mapping.kind = draft::ForeignArtifactKind::SharedLibrary;
  shared_mapping.path = shared_provider;
  linux_options.foreign_providers.push_back(std::move(shared_mapping));
  draft::DiagnosticSink linux_diagnostics;
  const draft::NativeBuildResult linux_built = draft::build_native_artifact(
      draft::make_x86_64_linux_profile(), linux_compiled, linux_options,
      linux_diagnostics);
  EXPECT(state, linux_built.ok);
  EXPECT(state, !linux_diagnostics.has_errors());
  EXPECT(state, read_file(log).find("-Wl,-rpath,$ORIGIN") != std::string::npos);

  std::filesystem::remove_all(temporary, error);
}

void test_all_native_artifact_kinds(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-native-artifact-kinds-test"};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::error_code error;
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
  const std::string &llvm = compiled.packages.front()->llvm_module.text;
  EXPECT(state, llvm.find("define i32 @main(") == std::string::npos);
  // A library module contains only helpers reached by its authored code. The
  // invariant hosted runtime is a separate artifact input, not an unconditional
  // block of definitions inside every root LLVM module.
  EXPECT(state, llvm.find("define hidden void @__draft.runtime.default_context") ==
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
    options.emit_debug_symbols = true;
    // Exercise the retained qualification oracle for every artifact shape. The
    // default in-process path has focused byte-format tests and the native host
    // suite; this fake driver makes the independent Clang argument contract
    // observable without conflating it with ordinary build behavior.
    options.object_emitter = draft::NativeObjectEmitter::ExternalClangOracle;
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
  EXPECT(state, std::filesystem::exists(
      temporary / "assembly" / "package-0-unit-0.s"));
  EXPECT(state, read_file(
      temporary / "assembly" / "package-0-assembly-0.s") ==
      compiled.packages.front()->assembly_sources.front().contents);
  const std::filesystem::path assembly_marker =
      temporary / "assembly" / ".draft-assembly-bundle-v1";
  EXPECT(state, std::filesystem::is_regular_file(assembly_marker));

  // The directory is one replaceable artifact, not an append-only collection.
  // Simulate an output owned by an earlier plan and prove the next publication
  // removes that plan's now-obsolete file while retaining no authority over
  // unmarked directories elsewhere.
  std::ofstream(temporary / "assembly" / "obsolete.s", std::ios::binary)
      << "obsolete assembly\n";
  std::ofstream(assembly_marker, std::ios::binary | std::ios::app)
      << "obsolete.s\n";
  EXPECT(state, build(
      draft::NativeArtifactKind::Assembly,
      temporary / "assembly").ok);
  EXPECT(state,
      !std::filesystem::exists(temporary / "assembly" / "obsolete.s"));
  EXPECT(state, std::filesystem::is_regular_file(assembly_marker));

  const std::string arguments = read_file(log);
  EXPECT(state, arguments.find("\n-Wl,-r\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-no_uuid\n") == std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,-reproducible\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-dynamiclib\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-- dsymutil --\n") != std::string::npos);
  EXPECT(state,
      arguments.find("\n-- ar --\n-static\n-D\n-o\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-S\n") != std::string::npos);

  // Rebuilding the same output through the ordinary fast path must neither
  // launch dsymutil nor leave the earlier debug bundle beside the new dylib.
  // A stale companion is worse than no companion because a debugger can accept
  // it and report source locations for different machine code.
  std::ofstream(log, std::ios::binary | std::ios::trunc).close();
  draft::NativeBuildOptions fast_options;
  fast_options.clang_path = fake_clang.string();
  fast_options.archiver_path = fake_archiver.string();
  fast_options.dsymutil_path = fake_dsymutil.string();
  fast_options.build_directory = (temporary / "fast-build").string();
  fast_options.output_path = (temporary / "library.dylib").string();
  fast_options.artifact_kind = draft::NativeArtifactKind::DynamicLibrary;
  fast_options.object_emitter =
      draft::NativeObjectEmitter::ExternalClangOracle;
  const draft::NativeBuildResult fast = draft::build_native_artifact(
      target, compiled, fast_options, diagnostics);
  EXPECT(state, fast.ok);
  EXPECT(state, fast.debug_symbols_path.empty());
  EXPECT(state, !std::filesystem::exists(temporary / "library.dylib.dSYM"));
  EXPECT(state, read_file(log).find("-- dsymutil --") == std::string::npos);

  std::filesystem::remove_all(temporary, error);
}

// The recording process makes the ELF driver contract observable without
// requiring a Linux sysroot in every unit-test environment. Real LLVM object
// acceptance and hosted execution are separate qualification gates.
void test_linux_toolchain_arguments(TestState &state,
                                    const draft::TargetProfile &target,
                                    std::string_view scratch_name) {
  draft::test::TemporaryDirectory temporary_directory{
      std::string(scratch_name)};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::error_code error;
  if (error) return;
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
    options.emit_debug_symbols = true;
    options.object_emitter =
        draft::NativeObjectEmitter::ExternalClangOracle;
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
  EXPECT(state, arguments.find("\n" + target.llvm_triple + "\n") !=
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

// PE/COFF argument construction is target data and is observable on any host.
// The recording tools publish the exact side files lld-link/llvm-lib promise;
// native Windows CI separately verifies that the real tools accept and execute
// the result.
void test_windows_toolchain_arguments(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-x86-64-windows-toolchain-test"};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::error_code error;
  if (error) return;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult compiled =
      compile_windows_fixture(temporary, sources, diagnostics);
  EXPECT(state, compiled.ok);
  if (!compiled.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return;
  }

  const std::filesystem::path log = temporary / "arguments.log";
  const std::filesystem::path fake_clang = temporary / "record-clang";
  {
    std::ofstream script(fake_clang, std::ios::binary);
    script << "#!/bin/sh\n"
              "printf '%s\\n' '-- clang --' \"$@\" >> '"
           << log.string()
           << "'\n"
              "previous=''\n"
              "for argument in \"$@\"; do\n"
              "  if [ \"$previous\" = \"-o\" ]; then : > \"$argument\"; fi\n"
              "  case \"$argument\" in\n"
              "    /pdb:*) : > \"${argument#/pdb:}\" ;;\n"
              "    /implib:*) : > \"${argument#/implib:}\" ;;\n"
              "  esac\n"
              "  previous=\"$argument\"\n"
              "done\n"
              "exit 0\n";
  }
  const std::filesystem::path fake_archiver = temporary / "record-lib";
  {
    std::ofstream script(fake_archiver, std::ios::binary);
    script << "#!/bin/sh\n"
              "printf '%s\\n' '-- lib --' \"$@\" >> '"
           << log.string()
           << "'\n"
              "for argument in \"$@\"; do\n"
              "  case \"$argument\" in\n"
              "    /OUT:*) : > \"${argument#/OUT:}\" ;;\n"
              "  esac\n"
              "done\n"
              "exit 0\n";
  }
  EXPECT(state, chmod(fake_clang.c_str(), 0700) == 0);
  EXPECT(state, chmod(fake_archiver.c_str(), 0700) == 0);

  const draft::TargetProfile target = draft::make_x86_64_windows_profile();
  const auto build = [&](draft::NativeArtifactKind kind,
                         const std::filesystem::path &output) {
    draft::NativeBuildOptions options;
    options.clang_path = fake_clang.string();
    options.archiver_path = fake_archiver.string();
    options.build_directory = (temporary / "build").string();
    options.output_path = output.string();
    options.artifact_kind = kind;
    options.emit_debug_symbols = true;
    options.object_emitter =
        draft::NativeObjectEmitter::ExternalClangOracle;
    return draft::build_native_artifact(
        target, compiled, options, diagnostics);
  };

  const draft::NativeBuildResult executable = build(
      draft::NativeArtifactKind::Executable, temporary / "program.exe");
  EXPECT(state, executable.ok);
  EXPECT(state, executable.debug_symbols_path ==
      (temporary / "program.pdb").string());
  EXPECT(state, executable.import_library_path.empty());
  EXPECT(state, build(
      draft::NativeArtifactKind::StaticLibrary,
      temporary / "windows_toolchain.lib").ok);
  const draft::NativeBuildResult dynamic = build(
      draft::NativeArtifactKind::DynamicLibrary,
      temporary / "windows_toolchain.dll");
  EXPECT(state, dynamic.ok);
  EXPECT(state, dynamic.debug_symbols_path ==
      (temporary / "windows_toolchain.pdb").string());
  EXPECT(state, dynamic.import_library_path ==
      (temporary / "windows_toolchain.lib").string());
  EXPECT(state, build(
      draft::NativeArtifactKind::Assembly,
      temporary / "windows-assembly").ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, std::filesystem::exists(
      temporary / "build" / "package-0-unit-0.obj"));
  EXPECT(state, std::filesystem::exists(
      temporary / "build" / "package-0-assembly-0.obj"));

  const std::string arguments = read_file(log);
  EXPECT(state, arguments.find("\nx86_64-pc-windows-msvc\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\n-fuse-ld=lld\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,/Brepro\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-Wl,/debug:full\n") != std::string::npos);
  EXPECT(state, arguments.find("\n/pdbaltpath:%_PDB%\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\n/entry:wmainCRTStartup\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\n/implib:" +
      (temporary / "windows_toolchain.lib").string() + "\n") !=
      std::string::npos);
  EXPECT(state, arguments.find("\n-lkernel32\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-shared\n") != std::string::npos);
  EXPECT(state, arguments.find("\n-- lib --\n/nologo\n/OUT:" +
      (temporary / "windows_toolchain.lib").string() + "\n") !=
      std::string::npos);

  // COFF has no multi-object partial link. A one-module package can still
  // publish its exact relocatable object; the assembly-bearing fixture must
  // fail closed and direct callers to a static library.
  draft::SourceManager object_sources;
  draft::DiagnosticSink object_compile_diagnostics;
  const draft::CompileWorkspaceResult object_compiled = compile_windows_fixture(
      temporary, object_sources, object_compile_diagnostics, false);
  EXPECT(state, object_compiled.ok);
  draft::NativeBuildOptions object_options;
  object_options.clang_path = fake_clang.string();
  object_options.build_directory = (temporary / "object-build").string();
  object_options.output_path = (temporary / "windows_toolchain.obj").string();
  object_options.artifact_kind = draft::NativeArtifactKind::Object;
  object_options.object_emitter =
      draft::NativeObjectEmitter::ExternalClangOracle;
  draft::DiagnosticSink object_diagnostics;
  const draft::NativeBuildResult object = draft::build_native_artifact(
      target, object_compiled, object_options, object_diagnostics);
  EXPECT(state, object.ok);
  EXPECT(state, !object_diagnostics.has_errors());
  EXPECT(state, std::filesystem::exists(temporary / "windows_toolchain.obj"));

  object_options.build_directory = (temporary / "multi-object-build").string();
  draft::DiagnosticSink rejected_diagnostics;
  const draft::NativeBuildResult rejected = draft::build_native_artifact(
      target, compiled, object_options, rejected_diagnostics);
  EXPECT(state, !rejected.ok);
  EXPECT(state, rejected_diagnostics.has_errors());
  EXPECT(state, draft::render_diagnostics(sources, rejected_diagnostics).find(
      "COFF object output requires exactly one native input") !=
      std::string::npos);

  std::filesystem::remove_all(temporary, error);
}

void test_package_assembly_reaches_link(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-toolchain-test"};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::error_code error;
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
  native_options.emit_debug_symbols = true;
  native_options.object_emitter =
      draft::NativeObjectEmitter::ExternalClangOracle;
  // Both external-oracle compilation and package assembly append to one tiny
  // recording script. Keep this argument-contract fixture sequential so shell
  // writes cannot interleave; parallel artifact determinism has a real-tool gate.
  native_options.object_worker_count = 1;
  const draft::NativeBuildResult built = draft::build_native_executable(
      target, compiled, native_options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, built.ok);
  EXPECT(state, std::filesystem::exists(temporary / "program"));
  EXPECT(state, built.debug_symbols_path ==
      (temporary / "program.dSYM").string());
  EXPECT(state, !std::filesystem::exists(
      temporary / "program.dSYM" / "Contents" / "Resources" /
          "Relocations"));

  const std::string arguments = read_file(log);
  if (arguments.find("\n-x\nassembler\n-c\n") == std::string::npos) {
    std::cerr << "recorded package-assembly commands:\n" << arguments;
  }
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
  test_linux_toolchain_arguments(state, draft::make_aarch64_linux_profile(),
                                 "draft-aarch64-linux-toolchain-test");
  test_linux_toolchain_arguments(state, draft::make_x86_64_linux_profile(),
                                 "draft-x86-64-linux-toolchain-test");
  test_windows_toolchain_arguments(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " toolchain expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all toolchain tests passed\n";
  return EXIT_SUCCESS;
}
