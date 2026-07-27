// Real native execution matrix for handwritten Draft programs.
//
// Semantic and LLVM snapshot tests localize compiler failures well, but they do
// not prove that runtime Context setup, the selected platform ABI, package
// assembly, atomics, pthread bridges, and system linking agree in a launched
// process. This gate runs only when the bootstrap host can execute one of the
// implemented targets directly: Apple Silicon selects Mach-O/macOS, while
// AArch64 and x86-64 Linux select their matching ELF/GNU profile. Every package
// still passes through the public compiler and native-adapter APIs before its
// executable is launched.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include "test_directory.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct TestState {
  int failures = 0;

  void expect(
      bool condition,
      std::string_view package,
      std::string_view expression,
      int line) {
    if (!condition) {
      ++failures;
      std::cerr << "native_conformance_test.cpp:" << line << ": " << package
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, package, expression) \
  (state).expect((expression), (package), #expression, __LINE__)

struct ConformanceCase {
  std::string name;
  std::string workspace;
  std::string package;
  std::string argument;
  std::string standard_input;
};

// Reads every row classified as a runnable native program from the repository
// qualification matrix. The matrix, rather than this test binary, owns example
// coverage: adding a new executable package without classifying it makes the
// frontend inventory test fail, and classifying it as `run` includes it here.
// Malformed rows are test-infrastructure errors and fail before compilation.
[[nodiscard]] std::vector<ConformanceCase> load_conformance_cases() {
  const std::filesystem::path source_root(DRAFT_SOURCE_DIRECTORY);
  std::ifstream input(source_root / "examples" / "qualification.tsv");
  if (!input) {
    std::cerr << "cannot open examples/qualification.tsv\n";
    std::exit(EXIT_FAILURE);
  }

  std::vector<ConformanceCase> cases;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') continue;
    if (line.back() == '\r') line.pop_back();

    std::array<std::string, 5> fields;
    std::size_t begin = 0;
    for (std::size_t field = 0; field != fields.size(); ++field) {
      const std::size_t end = line.find('\t', begin);
      if ((field + 1 == fields.size()) != (end == std::string::npos)) {
        std::cerr << "malformed example qualification row: " << line << '\n';
        std::exit(EXIT_FAILURE);
      }
      fields[field] = line.substr(begin, end - begin);
      begin = end + 1;
    }
    if (fields[3] != "run") continue;

    ConformanceCase test;
    test.workspace = fields[0];
    if (test.workspace == ".") {
      test.package = fields[1];
    } else {
      test.package = fields[1] == "."
          ? test.workspace
          : test.workspace + "/" + fields[1];
    }
    if (!test.package.starts_with("examples/")) {
      std::cerr << "runnable example is outside examples/: " << test.package
                << '\n';
      std::exit(EXIT_FAILURE);
    }
    test.name = test.package.substr(std::string("examples/").size());
    for (char &byte : test.name) {
      if (byte == '/') byte = '-';
    }
    if (test.package == "examples/simple-editor") {
      // The editor is intentionally interactive. A relative document path and
      // one quit command exercise its startup, unavailable-file, command-loop,
      // and clean shutdown paths without leaving a source-tree artifact.
      test.argument = "document.txt";
      test.standard_input = "q\n";
    } else if (test.package == "examples/tetris" ||
               test.package == "examples/turbo-editor" ||
               test.package == "examples/turbo-ui-gallery") {
      // Full gameplay requires a terminal whose native mode can be changed.
      // The example's deterministic smoke path still executes simulation and
      // complete TUI cell painting under the ordinary pipe used for every
      // conformance child, without weakening interactive behavior.
      test.argument = "--smoke";
    }
    cases.push_back(std::move(test));
  }
  if (!input.eof()) {
    std::cerr << "cannot read examples/qualification.tsv\n";
    std::exit(EXIT_FAILURE);
  }
  return cases;
}

// Selects the Draft target whose executables the current CI host can launch
// directly. CMake only builds this test on supported native hosts, so
// reaching another branch would be a build-configuration error rather than a
// runtime condition that should be hidden by skipping the test.
[[nodiscard]] draft::TargetProfile native_host_target() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return draft::make_aarch64_macos_profile();
#elif defined(__linux__) && defined(__aarch64__)
  return draft::make_aarch64_linux_profile();
#elif defined(__linux__) && defined(__x86_64__)
  return draft::make_x86_64_linux_profile();
#else
#error "native conformance requires an implemented host target"
#endif
}

// LLVM's target lowering chooses the architecture's conventional hard trap.
// AArch64 BRK is delivered as SIGTRAP by Darwin and Linux, whereas x86-64 UD2
// is delivered as SIGILL by Linux. Requiring that exact target-specific signal
// still distinguishes an intentional Draft trap from an incidental arithmetic
// fault or memory violation without pretending the two ISAs share an opcode.
[[nodiscard]] constexpr int native_trap_signal() {
#if defined(__linux__) && defined(__x86_64__)
  return SIGILL;
#else
  return SIGTRAP;
#endif
}

// Runs an already linked path directly. No shell, inherited command search, or
// source-authored byte can become command syntax. The child uses an isolated
// working directory so relative process state cannot leak between fixtures.
[[nodiscard]] bool run_executable(
    const std::filesystem::path &executable,
    const std::filesystem::path &working_directory,
    std::string_view argument,
    std::string_view standard_input,
    int &status) {
  // Materialize inputs and create the pipe before fork. The child performs only
  // async-signal-safe descriptor setup before exec and never allocates through
  // a post-fork C++ library path. Redirecting every fixture's stdin also keeps
  // interactive behavior independent of the terminal that launched CTest.
  const std::string argument_storage(argument);
  const std::string input_storage(standard_input);
  int input_pipe[2];
  if (::pipe(input_pipe) != 0) return false;
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    return false;
  }
  if (child == 0) {
    ::close(input_pipe[1]);
    if (::dup2(input_pipe[0], STDIN_FILENO) < 0) _exit(125);
    ::close(input_pipe[0]);
    if (::chdir(working_directory.c_str()) != 0) _exit(126);
    if (argument_storage.empty()) {
      ::execl(executable.c_str(), executable.c_str(), nullptr);
    } else {
      ::execl(
          executable.c_str(),
          executable.c_str(),
          argument_storage.c_str(),
          nullptr);
    }
    _exit(127);
  }
  ::close(input_pipe[0]);
  std::size_t written = 0;
  while (written != input_storage.size()) {
    const ssize_t count = ::write(
        input_pipe[1],
        input_storage.data() + written,
        input_storage.size() - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      ::close(input_pipe[1]);
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

void test_native_examples(TestState &state) {
  const std::vector<ConformanceCase> cases = load_conformance_cases();

  draft::test::TemporaryDirectory temporary_directory{
      "draft-native-conformance"};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::error_code error;

  const draft::TargetProfile target = native_host_target();
  const std::filesystem::path source_root(DRAFT_SOURCE_DIRECTORY);
  for (const ConformanceCase &test : cases) {
    draft::SourceManager sources;
    draft::DiagnosticSink diagnostics;
    draft::CompileWorkspaceOptions compile_options;
    compile_options.target = target;
    compile_options.workspace.workspace_directory =
        (source_root / test.workspace).string();
    compile_options.workspace.core_directory =
        (source_root / "core").string();
    compile_options.workspace.core_content_identity =
        "draft-core-bootstrap-v4";
    compile_options.lower_mir = true;
    compile_options.emit_native_output = true;
    compile_options.emit_debug_information = true;
    draft::CompileWorkspaceResult compiled = draft::compile_workspace(
        sources,
        (source_root / test.package).string(),
        std::move(compile_options),
        diagnostics);
    if (diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    EXPECT(state, test.name, compiled.ok);
    if (!compiled.ok) continue;

    const std::filesystem::path case_directory = temporary / test.name;
    draft::NativeBuildOptions native_options;
    native_options.build_directory = (case_directory / "build").string();
    native_options.output_path = (case_directory / "program").string();
    native_options.emit_debug_symbols = true;
    const draft::NativeBuildResult built = draft::build_native_executable(
        target, compiled, native_options, diagnostics);
    if (diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    EXPECT(state, test.name, built.ok);
    if (!built.ok) continue;
    if (target.facts.object_format == "macho") {
      EXPECT(state, test.name, !built.debug_symbols_path.empty());
      EXPECT(state, test.name,
          std::filesystem::exists(built.debug_symbols_path));
    } else {
      // ELF retains its DWARF in the executable. An empty result field is the
      // explicit backend contract, not a missing Linux-shaped dSYM substitute.
      EXPECT(state, test.name, built.debug_symbols_path.empty());
    }

    std::filesystem::create_directories(case_directory, error);
    EXPECT(state, test.name, !error);
    if (error) {
      error.clear();
      continue;
    }
    if (test.name == "runtime-traps") {
      // One runtime-selected package covers every mandatory trap class without
      // recompiling eight nearly identical fixtures. Each selector enters one
      // path whose invalid value depends on argv, keeping the failure at
      // runtime rather than turning it into a required-constant diagnostic.
      constexpr std::array trap_selectors{
          "d", // Integer division by zero.
          "o", // Signed minimum divided by negative one.
          "s", // Out-of-range shift count.
          "n", // Negative shift count.
          "w", // Narrow negative count for a u128 shift.
          "f", // Out-of-range float-to-integer conversion.
          "q", // NaN-to-integer conversion.
          "r", // Invalid Unicode scalar conversion.
          "e", // Invalid enum backing value.
          "i", // Array index out of bounds.
          "l", // Slice bound out of range.
      };
      for (const std::string_view selector : trap_selectors) {
        int process_status = 0;
        EXPECT(state, test.name,
            run_executable(
                built.output_path,
                case_directory,
                selector,
                {},
                process_status));
        EXPECT(state, test.name, WIFSIGNALED(process_status));
        if (WIFSIGNALED(process_status)) {
          EXPECT(
              state,
              test.name,
              WTERMSIG(process_status) == native_trap_signal());
        }
      }
    } else {
      int process_status = 0;
      EXPECT(state, test.name,
          run_executable(
              built.output_path,
              case_directory,
              test.argument,
              test.standard_input,
              process_status));
      EXPECT(state, test.name, WIFEXITED(process_status));
      if (WIFEXITED(process_status)) {
        EXPECT(state, test.name, WEXITSTATUS(process_status) == 0);
      }
    }
  }

}

// Builds the production core/os and core/console sources against a deterministic
// package-local native-write implementation. Real regular files commonly accept
// a complete short write, so they cannot prove the retry loop. This fixture
// caps each successful call at three bytes, records the exact source bytes, and
// can inject native errors at an exact call. The launched Draft program
// therefore proves literal, sliced, empty, large, partial, failed,
// zero-progress, and cross-package console paths without depending on kernel
// pipe capacity or scheduler timing.
void test_partial_text_writes(TestState &state) {
  constexpr std::string_view name = "partial-text-write";
  constexpr std::size_t large_text_size = 16U * 1024U;
  draft::test::TemporaryDirectory temporary_directory{
      "draft-partial-text-write"};
  const std::filesystem::path &root = temporary_directory.path();
  const std::filesystem::path workspace = root / "workspace";
  const std::filesystem::path core = root / "core";
  std::error_code error;
  for (const std::filesystem::path &directory : {
           workspace / "app",
           core / "c_abi",
           core / "console",
           core / "format",
           core / "io",
           core / "os",
           core / "runtime",
       }) {
    std::filesystem::create_directories(directory, error);
    EXPECT(state, name, !error);
    if (error) return;
  }

  const draft::TargetProfile target = native_host_target();
  const std::filesystem::path source_root(DRAFT_SOURCE_DIRECTORY);
  for (const std::filesystem::path &relative : {
           std::filesystem::path("c_abi/package.draft"),
           std::filesystem::path("console/package.draft"),
           std::filesystem::path("format/package.draft"),
           std::filesystem::path("io/package.draft"),
           std::filesystem::path("os/package.draft"),
           std::filesystem::path("runtime/package.draft"),
       }) {
    std::filesystem::copy_file(
        source_root / "core" / relative,
        core / relative,
        std::filesystem::copy_options::none,
        error);
    EXPECT(state, name, !error);
    if (error) return;
  }
  const std::string target_tag = target.facts.file_tag;
  std::ofstream platform(
      core / "os" / ("platform@" + target_tag + ".draft"),
      std::ios::binary);
  platform << R"draft(// This target-selected test provider replaces only core/os's native calls.
// It records bytes from synchronous writes, caps forward progress to three
// bytes per call, and can report zero progress or a native error on demand. The
// fixture owns one 16-KiB package-global capture buffer which the launched app
// resets between cases; no pointer is retained after native_write returns.
//
// Production package.draft, console, format, io, and runtime remain unchanged.
// This provider depends only on core/c_abi and intentionally supplies the
// complete private seam required by core/os.
package os

import core/c_abi

pub open_append :: cast[c_abi.int](0)
pub open_create :: cast[c_abi.int](0)
pub open_truncate :: cast[c_abi.int](0)
pub open_exclusive :: cast[c_abi.int](0)
pub default_creation_permissions :: cast[c_abi.unsigned_int](0)

Test_Capacity :: 16384

pub test_bytes: [Test_Capacity]u8
pub test_length: usize
pub test_calls: usize
test_zero_progress: bool
test_failure_call: usize

// Clears all recorded state so one launched-process case cannot inherit bytes,
// call counts, or policy from the preceding case.
pub test_reset :: proc() {
    for index: usize = 0; index < len(test_bytes); index += 1 {
        test_bytes[index] = 0
    }
    test_length = 0
    test_calls = 0
    test_zero_progress = false
    test_failure_call = 0
}

// Selects the zero-progress result used to prove that write_text_all terminates
// with an error instead of retrying the same nonempty suffix forever.
pub test_force_zero_progress :: proc() {
    test_zero_progress = true
}

// Selects one one-based native call which reports failure before consuming any
// bytes. Zero, restored by test_reset, means that every call may make progress.
pub test_fail_on_call :: proc(call: usize) {
    test_failure_call = call
}

// Satisfies the portable core/os surface; this deterministic value has no role
// in text-write behavior.
pub page_size :: proc() -> usize {
    return 4096
}

// The launched fixture never reads through this provider. Returning a native
// error keeps accidental reads deterministic and outside the write assertions.
native_open :: proc(
    path: cstring,
    flags: c_abi.int,
    mode: c_abi.unsigned_int,
) -> c_abi.int {
    return -1
}

native_read :: proc(
    descriptor: c_abi.int,
    destination: [^]u8,
    count: c_abi.size_t,
) -> c_abi.ssize_t {
    return -1
}

// Borrows exactly the reported prefix of source for this call, copies it into
// the bounded capture record, and retains no pointer. Three-byte progress makes
// every ten-byte test require four calls and therefore exercises retry order.
// An injected failure consumes no bytes, matching the native negative result
// observed by core/os.
native_write :: proc(
    descriptor: c_abi.int,
    source: [^]u8,
    count: c_abi.size_t,
) -> c_abi.ssize_t {
    test_calls += 1
    if test_failure_call != 0 && test_calls == test_failure_call {
        return -1
    }
    if test_zero_progress {
        return 0
    }
    accepted := cast[usize](count)
    if accepted > 3 {
        accepted = 3
    }
    if accepted > len(test_bytes) - test_length {
        return -1
    }
    for index: usize = 0; index < accepted; index += 1 {
        test_bytes[test_length + index] = source[index]
    }
    test_length += accepted
    return cast[c_abi.ssize_t](accepted)
}

// The remaining procedures complete core/os's private target seam. They own no
// resources in this provider because the launched app uses only standard_output.
native_close :: proc(descriptor: c_abi.int) -> c_abi.int {
    return 0
}

native_unlink :: proc(path: cstring) -> c_abi.int {
    return 0
}

native_process_id :: proc() -> c_abi.int {
    return 1
}

native_exit :: proc(status: c_abi.int) {
}
)draft";
  platform.close();

  std::ofstream app(workspace / "app" / "package.draft", std::ios::binary);
  app << R"draft(// This launched conformance app drives production core/console and core/os
// against the deterministic target provider. It owns no resource: the selected
// standard-output handle is borrowed and the provider owns its capture globals.
// Exit codes identify literal/partial, sliced/cross-package, empty, large,
// native-error, and zero-progress failures without relying on host stdout.
package app

import core/console
import core/os

// Compares the provider's exact recorded prefix with expected immutable text.
// Both values are borrowed for the call and no encoding interpretation occurs.
captured :: proc(expected: string) -> bool {
    if os.test_length != len(expected) {
        return false
    }
    for index: usize = 0; index < len(expected); index += 1 {
        if os.test_bytes[index] != expected[index] {
            return false
        }
    }
    return true
}

// Large_Text is generated by the C++ harness as exactly 16 KiB of repeating
// lowercase ASCII. Keeping it in one immutable source literal gives the text
// path an explicit stress size while avoiding a generated repository fixture
// or coupling conformance coverage to an application example's prose.
Large_Text :: `)draft";
  for (std::size_t index = 0; index < large_text_size; ++index) {
    app.put(static_cast<char>('a' + (index % 26U)));
  }
  app << R"draft(`

// Runs each independent write contract after resetting provider state. A zero
// result proves byte order, call counts, empty calls, large views, native-error
// propagation, and progress assertions.
main :: proc() -> int {
    os.test_reset()
    if os.write_text_all(os.standard_output, "abcdefghij") != .none ||
        os.test_calls != 4 || !captured("abcdefghij") {
        return 1
    }

    os.test_reset()
    decorated := "discardklmnopqrstrailer"
    if console.write_to(os.standard_output, decorated[7:17]) != .none ||
        os.test_calls != 4 || !captured("klmnopqrst") {
        return 2
    }

    os.test_reset()
    (count, error) := os.write_text(os.standard_output, "")
    if error != .none || count != 0 || os.test_calls != 0 {
        return 3
    }
    if os.write_text_all(os.standard_output, "") != .none ||
        os.test_calls != 0 {
        return 3
    }

    os.test_reset()
    if console.write_to(os.standard_output, Large_Text) != .none ||
        os.test_calls != (len(Large_Text) + 2) / 3 ||
        !captured(Large_Text) {
        return 4
    }

    os.test_reset()
    os.test_fail_on_call(1)
    (failed_count, failed_error) := os.write_text(
        os.standard_output,
        "failure",
    )
    if failed_error != .unavailable || failed_count != 0 ||
        os.test_calls != 1 || os.test_length != 0 {
        return 5
    }

    os.test_reset()
    os.test_fail_on_call(2)
    if console.write_to(os.standard_output, "abcdef") != .unavailable ||
        os.test_calls != 2 || !captured("abc") {
        return 6
    }

    os.test_reset()
    os.test_force_zero_progress()
    if os.write_text_all(os.standard_output, "x") != .unavailable ||
        os.test_calls != 1 {
        return 7
    }
    return 0
}
)draft";
  app.close();
  EXPECT(state, name, platform.good() && app.good());
  if (!platform.good() || !app.good()) return;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
  compile_options.workspace.workspace_directory = workspace.string();
  compile_options.workspace.core_directory = core.string();
  compile_options.workspace.core_content_identity =
      "draft-partial-text-write-core-v1";
  compile_options.lower_mir = true;
  compile_options.emit_native_output = true;
  compile_options.emit_debug_information = true;
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources,
      (workspace / "app").string(),
      std::move(compile_options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, name, compiled.ok);
  if (!compiled.ok) return;

  // Runtime output equality alone cannot distinguish the zero-copy path from
  // an implementation which first copies into a temporary. Pin the production
  // structure as well: console.write_to must transitively expose the raw-data
  // effect imported from core/os, only core/os may contain RawData MIR, and
  // write_text itself must contain exactly one such extraction.
  const draft::CompiledPackage *console_package = nullptr;
  const draft::CompiledPackage *os_package = nullptr;
  std::optional<std::size_t> console_package_index;
  std::optional<std::size_t> os_package_index;
  for (std::size_t package_index = 0;
       package_index < compiled.packages.size(); ++package_index) {
    const std::optional<draft::CompiledPackage> &package =
        compiled.packages[package_index];
    if (!package.has_value()) continue;
    if (package->identity.root_relative_path == "console") {
      console_package = &*package;
      console_package_index = package_index;
    } else if (package->identity.root_relative_path == "os") {
      os_package = &*package;
      os_package_index = package_index;
    }
  }
  EXPECT(state, name, console_package != nullptr);
  EXPECT(state, name, os_package != nullptr);
  if (console_package == nullptr || os_package == nullptr ||
      !console_package_index.has_value() || !os_package_index.has_value()) {
    return;
  }

  const std::optional<draft::SymbolId> write_to =
      console_package->bodies.package.symbols.lookup_direct(
          console_package->bodies.package.package_scope, "write_to");
  EXPECT(state, name, write_to.has_value());
  if (!write_to.has_value()) return;
  const draft::ProcedureEffectSummary *write_to_effects =
      console_package->effects.find(*write_to);
  EXPECT(state, name, write_to_effects != nullptr);
  if (write_to_effects == nullptr) return;
  EXPECT(state, name,
      std::any_of(
          write_to_effects->effects.begin(),
          write_to_effects->effects.end(),
          [](const draft::SemanticEffect &effect) {
            return effect.kind == draft::EffectKind::RawStringData;
          }));

  std::size_t console_raw_data = 0;
  for (draft::SemanticProductId product :
       compiled.semantic_products.packages[*console_package_index]
           .mir_procedures) {
    EXPECT(state, name,
        product.value <
            compiled.semantic_products.mir_procedure_by_product.size());
    if (product.value >=
        compiled.semantic_products.mir_procedure_by_product.size()) {
      continue;
    }
    const std::optional<draft::MirProcedure> &payload =
        compiled.semantic_products.mir_procedure_by_product[product.value];
    EXPECT(state, name, payload.has_value());
    if (!payload.has_value()) continue;
    const draft::MirProcedure &procedure = *payload;
    console_raw_data += static_cast<std::size_t>(std::count_if(
        procedure.instructions.begin(),
        procedure.instructions.end(),
        [](const draft::MirInstruction &instruction) {
          return instruction.kind == draft::MirInstructionKind::RawData;
        }));
  }
  EXPECT(state, name, console_raw_data == 0);

  const std::optional<draft::SymbolId> write_text =
      os_package->bodies.package.symbols.lookup_direct(
          os_package->bodies.package.package_scope, "write_text");
  EXPECT(state, name, write_text.has_value());
  if (!write_text.has_value()) return;
  std::size_t write_text_raw_data = 0;
  for (draft::SemanticProductId product :
       compiled.semantic_products.packages[*os_package_index].mir_procedures) {
    EXPECT(state, name,
        product.value <
            compiled.semantic_products.mir_procedure_by_product.size());
    if (product.value >=
        compiled.semantic_products.mir_procedure_by_product.size()) {
      continue;
    }
    const std::optional<draft::MirProcedure> &payload =
        compiled.semantic_products.mir_procedure_by_product[product.value];
    EXPECT(state, name, payload.has_value());
    if (!payload.has_value()) continue;
    const draft::MirProcedure &procedure = *payload;
    if (procedure.symbol != *write_text) continue;
    write_text_raw_data += static_cast<std::size_t>(std::count_if(
        procedure.instructions.begin(),
        procedure.instructions.end(),
        [](const draft::MirInstruction &instruction) {
          return instruction.kind == draft::MirInstructionKind::RawData;
        }));
  }
  EXPECT(state, name, write_text_raw_data == 1);

  const std::filesystem::path case_directory = root / "native";
  draft::NativeBuildOptions native_options;
  native_options.build_directory = (case_directory / "build").string();
  native_options.output_path = (case_directory / "program").string();
  native_options.emit_debug_symbols = true;
  const draft::NativeBuildResult built = draft::build_native_executable(
      target, compiled, native_options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, name, built.ok);
  if (!built.ok) return;

  std::filesystem::create_directories(case_directory, error);
  EXPECT(state, name, !error);
  if (error) return;
  int process_status = 0;
  EXPECT(state, name,
      run_executable(
          built.output_path, case_directory, {}, {}, process_status));
  EXPECT(state, name, WIFEXITED(process_status));
  if (WIFEXITED(process_status)) {
    EXPECT(state, name, WEXITSTATUS(process_status) == 0);
  }
}

} // namespace

int main() {
  TestState state;
  test_native_examples(state);
  test_partial_text_writes(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " native-conformance expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
