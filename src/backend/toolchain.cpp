// Native artifact publication for Draft's concrete target contracts.
//
// This backend layer accepts a completely lowered CompileWorkspaceResult plus
// one explicit TargetProfile and produces an object, archive, shared library,
// executable, or assembly directory. It owns command-local build paths,
// worker-private tool inputs, subprocess lifetimes, canonical publication,
// source-correlation output, and Mach-O debug companions. It does not decide
// Draft semantics, layout, ABI, or target selection; those facts must already
// be explicit in the compiler result and target profile.
//
// Native object work may run concurrently, but workers own disjoint result
// slots and never touch diagnostics or the timing recorder. The command thread
// joins all work before selecting the lowest stable task-ID failure and before
// publishing linker inputs in canonical order. Remaining platform tools are
// launched without a shell and are operational configuration, never semantic
// identity. This file may depend on lowered compiler products, target facts,
// backend adapters, and base utilities; semantic phases must not depend on it.
//
// Relevant specification: docs/specification/04-native-interop.md sections
// 11-12 and docs/specification/06-compiler.md, "Native lowering and summaries".

#include "backend/toolchain.h"

#include "backend/llvm_object_emitter.h"
#include "backend/native_object_tasks.h"
#include "backend/source_correlation.h"
#include "base/content_tree.h"
#include "base/timing.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace draft {
namespace {

// ProcessResult owns the complete observable result of one synchronous child.
// started distinguishes a launch failure from any child exit status. exit_code
// uses ordinary exit values or 128 + signal; captured output combines stdout
// and stderr in byte order from one pipe. CPU fields are nonnegative kernel
// usage measurements and never influence compilation or artifact identity.
struct ProcessResult {
  bool started = false;
  int exit_code = -1;
  std::uint64_t user_nanoseconds = 0;
  std::uint64_t system_nanoseconds = 0;
  std::string output;
  std::string error;
};

// Converts wait4's platform timeval into a unit shared with TimingRecorder.
// Child usage reported by the kernel is nonnegative; keep the assertion close
// to the signed-to-unsigned conversion rather than letting malformed values
// turn into enormous timing rows.
#if !defined(_WIN32)
[[nodiscard]] std::uint64_t timeval_nanoseconds(const timeval &value) {
  assert(value.tv_sec >= 0 && value.tv_usec >= 0);
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
      static_cast<std::uint64_t>(value.tv_usec) * 1'000ULL;
}
#endif

// Runs a process without a shell. Each host implementation performs ordinary
// executable lookup, passes the exact argument vector, captures both output
// streams through one pipe, and returns child CPU accounting. POSIX uses
// posix_spawnp because workers may launch concurrently; Windows uses
// CreateProcessW with an explicit inherited-handle list for the same reason.
// No implementation may inherit unrelated process handles or interpolate a
// command through a shell.
[[nodiscard]] ProcessResult run_process(
    const std::vector<std::string> &arguments) {
  ProcessResult result;
  if (arguments.empty()) {
    result.error = "empty process argument vector";
    return result;
  }
#if defined(_WIN32)
  // One small owner is sufficient here because every Windows resource is a
  // HANDLE closed by CloseHandle. INVALID_HANDLE_VALUE is treated as empty so
  // the same owner can hold CreateFileW and CreatePipe results.
  struct HandleOwner {
    HANDLE value = nullptr;
    ~HandleOwner() {
      if (value != nullptr && value != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(value);
      }
    }
    HandleOwner() = default;
    HandleOwner(const HandleOwner &) = delete;
    HandleOwner &operator=(const HandleOwner &) = delete;
  };

  const auto windows_error = [](std::string_view operation) {
    return std::string(operation) + " failed with Windows error " +
        std::to_string(static_cast<unsigned long>(GetLastError()));
  };
  const auto utf8_to_wide = [](std::string_view input,
                               std::wstring &output) -> bool {
    output.clear();
    if (input.empty()) return true;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) return false;
    output.resize(static_cast<std::size_t>(required));
    return MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
               static_cast<int>(input.size()), output.data(), required) ==
        required;
  };
  const auto quote_argument = [](std::wstring_view argument) {
    const bool needs_quotes = argument.empty() ||
        argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(argument);
    std::wstring quoted;
    quoted.push_back(L'\"');
    std::size_t backslashes = 0;
    for (wchar_t character : argument) {
      if (character == L'\\') {
        ++backslashes;
        continue;
      }
      if (character == L'\"') {
        quoted.append(backslashes * 2U + 1U, L'\\');
        quoted.push_back(L'\"');
        backslashes = 0;
        continue;
      }
      quoted.append(backslashes, L'\\');
      backslashes = 0;
      quoted.push_back(character);
    }
    // Backslashes immediately before the closing quote must be doubled or the
    // child CRT interprets the final quote as escaped data.
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
  };

  std::wstring command_line;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    std::wstring wide;
    if (arguments[index].size() > static_cast<std::size_t>(INT_MAX) ||
        !utf8_to_wide(arguments[index], wide)) {
      result.error = "process argument is not valid UTF-8";
      return result;
    }
    if (index != 0) command_line.push_back(L' ');
    command_line += quote_argument(wide);
  }

  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;
  HandleOwner pipe_read;
  HandleOwner pipe_write;
  if (!CreatePipe(&pipe_read.value, &pipe_write.value, &inheritable, 0)) {
    result.error = windows_error("CreatePipe");
    return result;
  }
  if (!SetHandleInformation(pipe_read.value, HANDLE_FLAG_INHERIT, 0)) {
    result.error = windows_error("SetHandleInformation");
    return result;
  }
  HandleOwner null_input;
  null_input.value = CreateFileW(
      L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (null_input.value == INVALID_HANDLE_VALUE) {
    result.error = windows_error("CreateFileW(NUL)");
    return result;
  }

  SIZE_T attributes_size = 0;
  (void)InitializeProcThreadAttributeList(
      nullptr, 1, 0, &attributes_size);
  if (attributes_size == 0) {
    result.error = windows_error("InitializeProcThreadAttributeList(size)");
    return result;
  }
  std::vector<std::byte> attributes_storage(attributes_size);
  auto *attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
      attributes_storage.data());
  if (!InitializeProcThreadAttributeList(
          attributes, 1, 0, &attributes_size)) {
    result.error = windows_error("InitializeProcThreadAttributeList");
    return result;
  }
  struct AttributeListOwner {
    PPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~AttributeListOwner() {
      if (value != nullptr) DeleteProcThreadAttributeList(value);
    }
  } attributes_owner{attributes};
  HANDLE inherited_handles[] = {null_input.value, pipe_write.value};
  if (!UpdateProcThreadAttribute(
          attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          inherited_handles, sizeof(inherited_handles), nullptr, nullptr)) {
    result.error = windows_error("UpdateProcThreadAttribute");
    return result;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = null_input.value;
  startup.StartupInfo.hStdOutput = pipe_write.value;
  startup.StartupInfo.hStdError = pipe_write.value;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(
          nullptr, command_line.data(), nullptr, nullptr, TRUE,
          EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
          &startup.StartupInfo, &process)) {
    result.error = windows_error("CreateProcessW");
    return result;
  }
  HandleOwner process_handle;
  process_handle.value = process.hProcess;
  HandleOwner thread_handle;
  thread_handle.value = process.hThread;
  result.started = true;
  (void)CloseHandle(pipe_write.value);
  pipe_write.value = nullptr;

  char buffer[4096];
  while (true) {
    DWORD count = 0;
    if (ReadFile(pipe_read.value, buffer, sizeof(buffer), &count, nullptr)) {
      if (count == 0) break;
      result.output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (GetLastError() == ERROR_BROKEN_PIPE) break;
    result.error = windows_error("ReadFile(child output)");
    return result;
  }
  if (WaitForSingleObject(process_handle.value, INFINITE) != WAIT_OBJECT_0) {
    result.error = windows_error("WaitForSingleObject");
    return result;
  }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process_handle.value, &exit_code)) {
    result.error = windows_error("GetExitCodeProcess");
    return result;
  }
  result.exit_code = static_cast<int>(exit_code);
  FILETIME created{}, exited{}, kernel{}, user{};
  if (GetProcessTimes(
          process_handle.value, &created, &exited, &kernel, &user)) {
    const auto nanoseconds = [](const FILETIME &time) {
      ULARGE_INTEGER value{};
      value.LowPart = time.dwLowDateTime;
      value.HighPart = time.dwHighDateTime;
      return static_cast<std::uint64_t>(value.QuadPart) * 100ULL;
    };
    result.user_nanoseconds = nanoseconds(user);
    result.system_nanoseconds = nanoseconds(kernel);
  }
  return result;
#else
  int pipe_descriptors[2] = {-1, -1};
  if (pipe(pipe_descriptors) != 0) {
    result.error = std::string("pipe failed: ") + std::strerror(errno);
    return result;
  }
  posix_spawn_file_actions_t actions;
  const int actions_error = posix_spawn_file_actions_init(&actions);
  if (actions_error != 0) {
    result.error = std::string("posix_spawn file actions failed: ") +
        std::strerror(actions_error);
    (void)close(pipe_descriptors[0]);
    (void)close(pipe_descriptors[1]);
    return result;
  }

  // File-action construction is ordinary parent-thread code, so every error
  // can be handled before a child exists. The spawned tool sees one write end
  // as both standard streams and does not inherit the unused read end.
  int file_action_error =
      posix_spawn_file_actions_addclose(&actions, pipe_descriptors[0]);
  if (file_action_error == 0) {
    file_action_error = posix_spawn_file_actions_adddup2(
        &actions, pipe_descriptors[1], STDOUT_FILENO);
  }
  if (file_action_error == 0) {
    file_action_error = posix_spawn_file_actions_adddup2(
        &actions, pipe_descriptors[1], STDERR_FILENO);
  }
  if (file_action_error == 0) {
    file_action_error =
        posix_spawn_file_actions_addclose(&actions, pipe_descriptors[1]);
  }
  if (file_action_error != 0) {
    result.error = std::string("posix_spawn file action failed: ") +
        std::strerror(file_action_error);
    (void)posix_spawn_file_actions_destroy(&actions);
    (void)close(pipe_descriptors[0]);
    (void)close(pipe_descriptors[1]);
    return result;
  }

  std::vector<char *> raw_arguments;
  raw_arguments.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments) {
    raw_arguments.push_back(const_cast<char *>(argument.c_str()));
  }
  raw_arguments.push_back(nullptr);

  pid_t child = -1;
  const int spawn_error = posix_spawnp(
      &child,
      raw_arguments.front(),
      &actions,
      nullptr,
      raw_arguments.data(),
      environ);
  (void)posix_spawn_file_actions_destroy(&actions);
  (void)close(pipe_descriptors[1]);
  if (spawn_error != 0) {
    result.error = std::string("posix_spawnp failed: ") +
        std::strerror(spawn_error);
    (void)close(pipe_descriptors[0]);
    return result;
  }

  result.started = true;
  char buffer[4096];
  while (true) {
    const ssize_t count = read(pipe_descriptors[0], buffer, sizeof(buffer));
    if (count > 0) {
      result.output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  (void)close(pipe_descriptors[0]);
  int status = 0;
  rusage usage{};
  while (wait4(child, &status, 0, &usage) < 0) {
    if (errno != EINTR) {
      result.error = std::string("wait4 failed: ") + std::strerror(errno);
      return result;
    }
  }
  result.user_nanoseconds = timeval_nanoseconds(usage.ru_utime);
  result.system_nanoseconds = timeval_nanoseconds(usage.ru_stime);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
#endif
}

// Runs and accounts for one external tool beneath a named timing event. The
// process count is recorded only after spawn succeeds. Keeping this wrapper at
// the process boundary ensures every Clang, linker, archiver, and dsymutil path
// reports child CPU consistently, including nonzero exits.
[[nodiscard]] ProcessResult run_timed_process(
    const std::vector<std::string> &arguments,
    TimingRecorder *timings,
    std::string_view phase,
    TimingVisibility visibility = TimingVisibility::Summary) {
  TimingScope timing = timings != nullptr
      ? timings->scope(phase, visibility)
      : TimingScope{};
  ProcessResult result = run_process(arguments);
  if (timings != nullptr && result.started) {
    timings->record_child_process(
        result.user_nanoseconds, result.system_nanoseconds);
  }
  return result;
}

// Darwin's deployment floor is a Clang driver option. The Linux profile's
// version string instead identifies its kernel/libc contract and must never be
// passed as a made-up compiler flag. Keeping this branch beside every target
// invocation prevents object and assembly paths from drifting apart.
void append_target_arguments(
    const TargetProfile &target,
    std::vector<std::string> &arguments) {
  arguments.push_back("-target");
  arguments.push_back(target.llvm_triple);
  if (target.facts.object_format == "macho") {
    arguments.push_back("-mmacosx-version-min=" + target.minimum_os_version);
  }
}

[[nodiscard]] std::string_view object_file_extension(
    const TargetProfile &target) {
  return target.facts.object_format == "coff" ? ".obj" : ".o";
}

[[nodiscard]] bool read_file_bytes(
    const std::filesystem::path &path,
    std::string &contents,
    std::string &reason);

// Reads and hashes one regular companion file. PDB and import-library digests
// use bytes rather than the directory-tree hash used by dSYM bundles.
[[nodiscard]] bool hash_regular_file(
    const std::filesystem::path &path,
    Sha256Digest &digest,
    DiagnosticSink &diagnostics,
    std::string_view role) {
  std::string bytes;
  std::string reason;
  if (!read_file_bytes(path, bytes, reason)) {
    diagnostics.error(
        SourceRange::invalid(), "cannot read " + std::string(role) + ": " +
            reason);
    return false;
  }
  digest = sha256(bytes);
  return true;
}

// Writes one complete compiler-owned file through a sibling temporary and
// renames it into place. The caller owns the parent directory. On failure the
// final path is not replaced, reason is diagnostic-ready, and a failed rename
// removes the temporary; the function never changes semantic identity.
[[nodiscard]] bool write_atomic(
    const std::filesystem::path &path,
    std::string_view contents,
    std::string &reason) {
  reason.clear();
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      reason = "cannot open temporary file '" + temporary.string() + "'";
      return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.flush();
    if (!stream) {
      reason = "cannot write temporary file '" + temporary.string() + "'";
      return false;
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    reason = "cannot commit file '" + path.string() + "': " +
        rename_error.message();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return true;
}

// Reads a worker-private native product back into its task slot. Object bytes
// may contain zeroes, so this operation is binary and uses the exact file size
// rather than a text iterator. The private path is never a published artifact.
[[nodiscard]] bool read_file_bytes(
    const std::filesystem::path &path,
    std::string &contents,
    std::string &reason) {
  contents.clear();
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    reason = "cannot open native task output '" + path.string() + "'";
    return false;
  }
  const std::streampos end = stream.tellg();
  if (end < 0) {
    reason = "cannot measure native task output '" + path.string() + "'";
    return false;
  }
  contents.resize(static_cast<std::size_t>(end));
  stream.seekg(0, std::ios::beg);
  if (!contents.empty()) {
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  if (!stream) {
    reason = "cannot read native task output '" + path.string() + "'";
    contents.clear();
    return false;
  }
  reason.clear();
  return true;
}

[[nodiscard]] std::string phase_failure(
    std::string_view phase, const ProcessResult &process) {
  std::string message(phase);
  message += " failed with exit code ";
  message += std::to_string(process.exit_code);
  if (!process.output.empty()) {
    message += ":\n";
    message += process.output;
  }
  if (!process.error.empty()) {
    message += ": ";
    message += process.error;
  }
  return message;
}

// Draft hands already-formed LLVM IR to either the in-process ASan pass or the
// external qualification driver. Add the standard function attribute that opts
// every definition into load/store checks and retain every frame pointer needed
// by the profile's diagnostic contract. The canonical compiler IR remains
// unchanged; this textual snapshot belongs only to native validation.
[[nodiscard]] bool add_address_sanitizer_function_attributes(
    std::string_view module,
    std::string &instrumented,
    std::string &reason) {
  instrumented.clear();
  instrumented.reserve(module.size() + module.size() / 100U);
  std::size_t cursor = 0;
  while (cursor < module.size()) {
    const std::size_t newline = module.find('\n', cursor);
    const std::size_t end = newline == std::string_view::npos
        ? module.size()
        : newline;
    const std::string_view line = module.substr(cursor, end - cursor);
    if (line.starts_with("define ")) {
      // Draft's emitter keeps each definition header on one line. Parameter
      // types may themselves contain `{`, so the body opener is the last one.
      const std::size_t body = line.rfind('{');
      if (body == std::string_view::npos) {
        reason = "LLVM function definition has no body opener";
        return false;
      }
      // Debug attachments follow function attributes in LLVM grammar. Insert
      // before the first attachment when present, otherwise before the body.
      const std::size_t metadata = line.find(" !", 0);
      const std::size_t insertion =
          metadata != std::string_view::npos && metadata < body
          ? metadata + 1
          : body;
      instrumented.append(line.substr(0, insertion));
      instrumented += "sanitize_address \"frame-pointer\"=\"all\" ";
      instrumented.append(line.substr(insertion));
    } else {
      instrumented.append(line);
    }
    if (newline != std::string_view::npos) instrumented.push_back('\n');
    cursor = newline == std::string_view::npos ? module.size() : newline + 1;
  }
  reason.clear();
  return true;
}

// dsymutil owns a conventional directory layout, but a successful process exit
// is not by itself proof that a usable companion was published. Check the two
// files consumed by macOS symbol tooling before hashing or returning the tree.
[[nodiscard]] bool require_regular_file(
    const std::filesystem::path &path,
    std::string_view role,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    diagnostics.error(
        SourceRange::invalid(),
        "Mach-O dSYM does not contain a regular " + std::string(role) +
            " at '" + path.string() + "'");
    return false;
  }
  return true;
}

void add_provider(std::vector<std::string> &providers, std::string_view provider) {
  if (std::find(providers.begin(), providers.end(), provider) == providers.end()) {
    providers.emplace_back(provider);
  }
}

[[nodiscard]] bool has_package_assembly(
    const CompileWorkspaceResult &compiled) {
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (package.has_value() && !package->assembly_sources.empty()) return true;
  }
  return false;
}

[[nodiscard]] bool built_in_provider(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    std::string_view provider) {
  if (provider == "draft_runtime") return true;
  if (provider == "package_assembly") return has_package_assembly(compiled);
  return std::binary_search(
      target.system_link_providers.begin(),
      target.system_link_providers.end(),
      provider);
}

[[nodiscard]] bool verify_provider_set(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics) {
  std::vector<std::string> required;
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const std::string &provider : package->native_interop.providers) {
      add_provider(required, provider);
    }
  }

  if (!inspect_foreign_provider_inputs(
          options.foreign_providers, verified, diagnostics)) {
    return false;
  }

  for (const VerifiedForeignProviderInput &input : verified) {
    if (std::find(required.begin(), required.end(), input.provider) ==
        required.end()) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider mapping '" + input.provider +
              "' is not required by the compiled program");
      return false;
    }
    if (built_in_provider(target, compiled, input.provider)) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider '" + input.provider +
              "' is owned by the compiler or target profile and cannot be remapped");
      return false;
    }
  }
  for (const std::string &provider : required) {
    if (built_in_provider(target, compiled, provider)) continue;
    const bool configured = std::any_of(
        verified.begin(),
        verified.end(),
        [&provider](const VerifiedForeignProviderInput &input) {
          return input.provider == provider;
        });
    if (!configured) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider '" + provider +
              "' has no exact object, archive, or shared-library mapping");
      return false;
    }
  }
  return true;
}

// Runtime assets are deployment roots returned to embedding callers. Validate
// only the mappings explicitly supplied to this invocation; they are not
// source identity and do not require a resolution manifest.
[[nodiscard]] bool inspect_runtime_asset_set(
    const NativeBuildOptions &options,
    std::vector<VerifiedRuntimeAssetInput> &verified,
    DiagnosticSink &diagnostics) {
  return inspect_runtime_asset_inputs(
      options.runtime_assets, verified, diagnostics);
}

// One worker writes only its matching result slot. Native bytes stay in memory
// until every task has joined; source_bytes is present only when the former
// public contract also exposes an LLVM oracle input or package assembly source.
// Child CPU belongs to the one optional assembler/qualification process for
// this task and is replayed into TimingRecorder later by the owning thread.
struct NativeObjectTaskProduct {
  std::string native_bytes;
  std::string source_bytes;
  std::string timing_name;
  std::uint64_t elapsed_nanoseconds = 0;
  std::uint64_t child_user_nanoseconds = 0;
  std::uint64_t child_system_nanoseconds = 0;
  bool publish_source = false;
  bool child_started = false;
};

// The context borrows immutable build inputs for one synchronous WorkGraph
// run. products has exactly the plan's task-ID domain; workers may mutate only
// products[task]. No DiagnosticSink or TimingRecorder crosses the thread
// boundary because both deliberately preserve deterministic insertion order.
struct NativeObjectExecutionContext {
  const TargetProfile *target = nullptr;
  const NativeObjectPlan *plan = nullptr;
  NativeObjectEmitter emitter = NativeObjectEmitter::InProcessLlvm;
  NativeOptimizationLevel optimization = NativeOptimizationLevel::O0;
  NativeInstrumentationProfile instrumentation =
      NativeInstrumentationProfile::None;
  bool assembly_output = false;
  std::string clang_path;
  std::filesystem::path build_directory;
  std::vector<NativeObjectTaskProduct> *products = nullptr;
};

[[nodiscard]] std::filesystem::path native_task_private_path(
    const NativeObjectExecutionContext &context,
    const NativeObjectTask &native_task,
    WorkTaskId task,
    std::string_view role,
    std::string_view extension) {
  return context.build_directory /
      (native_task.output_stem + std::string(extension) + ".task-" +
       std::to_string(task) + "-" + std::string(role));
}

// Removes successful worker-private files after their bytes have been captured.
// Failure artifacts are deliberately retained in the isolated build directory
// for diagnosis; neither case publishes them to the canonical linker list.
void remove_successful_task_file(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

void capture_child_usage(
    const ProcessResult &process,
    NativeObjectTaskProduct &product) {
  product.child_started = process.started;
  product.child_user_nanoseconds = process.user_nanoseconds;
  product.child_system_nanoseconds = process.system_nanoseconds;
}

// Executes the semantic work for one closed native-object task. All filesystem
// paths used here are worker-private. Canonical names, object ordering, and
// diagnostics are selected only after the scheduler joins every started task.
[[nodiscard]] bool execute_native_object_task_inner(
    NativeObjectExecutionContext &context,
    WorkTaskId task_id,
    std::string &failure) {
  assert(context.target != nullptr);
  assert(context.plan != nullptr);
  assert(context.products != nullptr);
  assert(static_cast<std::size_t>(task_id) < context.plan->tasks.size());
  const NativeObjectTask &task = context.plan->tasks[task_id];
  NativeObjectTaskProduct &product = (*context.products)[task_id];

  if (task.kind != NativeObjectTaskKind::PackageAssembly) {
    std::string native_module(task.input_bytes);
    if (context.instrumentation ==
        NativeInstrumentationProfile::AddressSanitizer) {
      std::string instrumented;
      std::string attribute_error;
      if (!add_address_sanitizer_function_attributes(
              native_module, instrumented, attribute_error)) {
        failure = "cannot prepare address-instrumented LLVM module: " +
            attribute_error;
        return false;
      }
      native_module = std::move(instrumented);
    }

    if (context.emitter == NativeObjectEmitter::InProcessLlvm) {
      product.timing_name =
          "LLVM native unit emission: " + task.display_name;
      LlvmObjectEmissionOptions emission_options;
      emission_options.output_kind = context.assembly_output
          ? LlvmNativeOutputKind::Assembly
          : LlvmNativeOutputKind::Object;
      emission_options.optimization = context.optimization;
      emission_options.instrumentation = context.instrumentation ==
              NativeInstrumentationProfile::AddressSanitizer
          ? LlvmNativeInstrumentation::AddressSanitizer
          : LlvmNativeInstrumentation::None;
      LlvmObjectEmissionResult emitted = emit_llvm_object_in_process(
          *context.target,
          task.display_name,
          native_module,
          emission_options);
      if (!emitted.ok) {
        failure = std::move(emitted.failure);
        return false;
      }
      product.native_bytes = std::move(emitted.bytes);
      return true;
    }

    // The external oracle consumes a private copy and returns bytes through the
    // same result slot as the in-process adapter. Its source snapshot is later
    // published only after the complete ready set succeeds.
    product.timing_name =
        "Clang qualification compile: " + task.display_name;
    product.publish_source = true;
    product.source_bytes = native_module;
    const std::filesystem::path private_source = native_task_private_path(
        context, task, task_id, "input", task.source_extension);
    const std::filesystem::path private_output = native_task_private_path(
        context,
        task,
        task_id,
        "output",
        context.assembly_output
            ? ".s"
            : object_file_extension(*context.target));
    std::string write_error;
    if (!write_atomic(private_source, native_module, write_error)) {
      failure = std::move(write_error);
      return false;
    }
    std::vector<std::string> arguments{context.clang_path};
    append_target_arguments(*context.target, arguments);
    arguments.push_back("-x");
    arguments.push_back("ir");
    arguments.push_back(
        "-" + std::string(native_optimization_level_name(
            context.optimization)));
    if (context.instrumentation ==
        NativeInstrumentationProfile::AddressSanitizer) {
      arguments.push_back("-fsanitize=address");
      arguments.push_back("-fno-omit-frame-pointer");
    }
    arguments.push_back(context.assembly_output ? "-S" : "-c");
    arguments.push_back(private_source.string());
    arguments.push_back("-o");
    arguments.push_back(private_output.string());
    const ProcessResult process = run_process(arguments);
    capture_child_usage(process, product);
    if (!process.started || process.exit_code != 0) {
      failure = phase_failure("Clang qualification emission", process);
      return false;
    }
    if (!read_file_bytes(private_output, product.native_bytes, failure)) {
      return false;
    }
    remove_successful_task_file(private_source);
    remove_successful_task_file(private_output);
    return true;
  }

  // Package assembly bytes were already captured and selected by the target
  // profile. Assembly output is therefore a pure copy task. Native output uses
  // the matching LLVM Clang only as an assembler, never as a Draft IR compiler.
  product.publish_source = true;
  product.source_bytes = std::string(task.input_bytes);
  if (context.assembly_output) {
    product.timing_name =
        "package assembly publication: " + task.display_name;
    product.native_bytes = product.source_bytes;
    return true;
  }

  product.timing_name = "Clang package assembly: " + task.display_name;
  const std::filesystem::path private_source = native_task_private_path(
      context, task, task_id, "input", task.source_extension);
  const std::filesystem::path private_output =
      native_task_private_path(
          context, task, task_id, "output",
          object_file_extension(*context.target));
  std::string write_error;
  if (!write_atomic(private_source, task.input_bytes, write_error)) {
    failure = std::move(write_error);
    return false;
  }
  std::vector<std::string> arguments{context.clang_path};
  append_target_arguments(*context.target, arguments);
  arguments.insert(
      arguments.end(),
      {
          "-x",
          "assembler",
          "-c",
          private_source.string(),
          "-o",
          private_output.string(),
      });
  const ProcessResult process = run_process(arguments);
  capture_child_usage(process, product);
  if (!process.started || process.exit_code != 0) {
    failure = phase_failure(
        "assembly object emission for '" + task.display_name + "'", process);
    return false;
  }
  if (!read_file_bytes(private_output, product.native_bytes, failure)) {
    return false;
  }
  remove_successful_task_file(private_source);
  remove_successful_task_file(private_output);
  return true;
}

// WorkGraph's C-shaped operation boundary keeps the scheduler independent of
// backend types. This wrapper measures exactly the worker operation, including
// a child assembler wait, and stores the measurement in the same stable slot.
[[nodiscard]] bool execute_native_object_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context = *static_cast<NativeObjectExecutionContext *>(opaque_context);
  const auto started = std::chrono::steady_clock::now();
  const bool succeeded =
      execute_native_object_task_inner(context, task, failure);
  const auto finished = std::chrono::steady_clock::now();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)
          .count();
  assert(elapsed >= 0 && "steady-clock task duration must be nonnegative");
  (*context.products)[task].elapsed_nanoseconds =
      static_cast<std::uint64_t>(elapsed);
  return succeeded;
}

} // namespace

std::string_view native_artifact_kind_name(NativeArtifactKind kind) {
  switch (kind) {
  case NativeArtifactKind::Executable: return "executable";
  case NativeArtifactKind::Object: return "object";
  case NativeArtifactKind::StaticLibrary: return "static-library";
  case NativeArtifactKind::DynamicLibrary: return "dynamic-library";
  case NativeArtifactKind::Assembly: return "assembly";
  }
  return "unknown";
}

NativeBuildResult build_native_artifact(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    DiagnosticSink &diagnostics) {
  TimingScope artifact_timing = options.timings != nullptr
      ? options.timings->scope("native artifact")
      : TimingScope{};
  NativeBuildResult result;
  if (!compiled.ok) {
    diagnostics.error(
        SourceRange::invalid(), "native build requires a successful compiled workspace");
    return result;
  }
  if (options.build_directory.empty() || options.output_path.empty()) {
    diagnostics.error(
        SourceRange::invalid(), "native build directory and output path are required");
    return result;
  }

  if (options.instrumentation != NativeInstrumentationProfile::None &&
      options.artifact_kind != NativeArtifactKind::Executable) {
    diagnostics.error(
        SourceRange::invalid(),
        "native instrumentation is currently supported only for validation "
        "executables");
    return result;
  }
  if (options.instrumentation != NativeInstrumentationProfile::None &&
      target.facts.object_format != "macho") {
    diagnostics.error(
        SourceRange::invalid(),
        "native instrumentation has no qualified runtime contract for target '" +
            target.facts.identity + "'");
    return result;
  }

  std::vector<VerifiedRuntimeAssetInput> runtime_assets;
  if (!inspect_runtime_asset_set(options, runtime_assets, diagnostics)) {
    return result;
  }

  std::vector<VerifiedForeignProviderInput> foreign_providers;
  if (!verify_provider_set(
          target, compiled, options, foreign_providers, diagnostics)) {
    return result;
  }

  std::string clang_path = options.clang_path.empty()
      ? linked_llvm_tool_path("clang")
      : options.clang_path;
  std::string archiver_path = options.archiver_path;
  std::string dsymutil_path = options.dsymutil_path.empty()
      ? linked_llvm_tool_path("dsymutil")
      : options.dsymutil_path;
  if (target.facts.object_format == "elf" && archiver_path == "libtool") {
    // The public option retains the macOS-compatible default. ELF archives
    // use LLVM ar's deterministic mode from the same installation as the
    // linked library; a caller may still supply another compatible executable
    // explicitly.
    archiver_path = linked_llvm_tool_path("llvm-ar");
  } else if (target.facts.object_format == "coff" &&
             archiver_path == "libtool") {
    // COFF archives use Microsoft's library format. LLVM's compatible
    // llvm-lib lives beside the LLVM library linked into draftc and emits
    // deterministic archives without routing through the host's Unix ar.
    archiver_path = linked_llvm_tool_path("llvm-lib");
  }

  // LLVM is selected and linked while building draftc. Recording the compiled
  // version is exact and costs no child process; it is evidence about the
  // implementation binary, not a semantic input or an ambient availability
  // check. Remaining tools diagnose their own launch failure when first used.
  result.toolchain_version = std::string(linked_llvm_version());

  std::error_code directory_error;
  std::filesystem::create_directories(options.build_directory, directory_error);
  if (directory_error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot create native build directory: " + directory_error.message());
    return result;
  }
  const std::filesystem::path build_directory(options.build_directory);
  const std::filesystem::path output_path(options.output_path);
  if (options.artifact_kind == NativeArtifactKind::Assembly) {
    std::filesystem::create_directories(output_path, directory_error);
    if (directory_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create assembly output directory: " +
              directory_error.message());
      return result;
    }
  }
  // Freeze every artifact-layout package module and assembly input into
  // stable work slots before invoking a tool. This validation boundary ensures
  // later execution can change scheduling without changing task identity,
  // output names, diagnostic order, or linker order.
  NativeObjectPlan object_plan;
  std::string plan_error;
  if (!prepare_native_object_plan(target, compiled, object_plan, plan_error)) {
    diagnostics.error(SourceRange::invalid(), plan_error);
    return result;
  }

  std::vector<std::string> objects;
  SourceCorrelationMap source_correlation;
  source_correlation.target_identity = target.facts.identity;
  source_correlation.compiler_identity = compiled.compiler_content_identity;
  if (compiled.resolved_program_digest.has_value()) {
    source_correlation.program_identity = "resolved-program-sha256:" +
        compiled.resolved_program_digest->hex();
  }
  Sha256 direct_module_identity;
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    // prepare_native_object_plan proved every row is present and lowered. This
    // separate canonical traversal publishes package-level correlation and
    // identity independent of how object tasks are eventually scheduled.
    source_correlation.entries.insert(
        source_correlation.entries.end(),
        package->llvm_module.source_correlations.begin(),
        package->llvm_module.source_correlations.end());
    direct_module_identity.update(
        sha256(package->llvm_module.text).bytes);
  }

  TimingScope object_emission_timing = options.timings != nullptr
      ? options.timings->scope("LLVM and assembly object emission")
      : TimingScope{};
  const bool assembly_output =
      options.artifact_kind == NativeArtifactKind::Assembly;
  std::vector<NativeObjectTaskProduct> products(object_plan.tasks.size());
  NativeObjectExecutionContext execution;
  execution.target = &target;
  execution.plan = &object_plan;
  execution.emitter = options.object_emitter;
  execution.optimization = options.optimization;
  execution.instrumentation = options.instrumentation;
  execution.assembly_output = assembly_output;
  execution.clang_path = clang_path;
  execution.build_directory = build_directory;
  execution.products = &products;
  WorkGraphRunOptions run_options;
  run_options.worker_count = options.object_worker_count;
  const WorkGraphRunResult scheduled = run_work_graph(
      object_plan.graph,
      run_options,
      execute_native_object_task,
      &execution);
  result.object_workers_used = scheduled.workers_used;

  // Workers never touch the timing recorder. Replaying their task-local
  // measurements in ascending task ID gives detailed reports the same order as
  // diagnostics, file publication, archive members, and linker arguments.
  if (options.timings != nullptr) {
    options.timings->add_counter(
        "native object tasks", object_plan.tasks.size());
    options.timings->add_counter(
        "native object workers", scheduled.workers_used);
    for (std::size_t task_index = 0;
         task_index < object_plan.tasks.size();
         ++task_index) {
      const NativeObjectTaskProduct &product = products[task_index];
      const std::string timing_name = product.timing_name.empty()
          ? "native object task: " + object_plan.tasks[task_index].display_name
          : product.timing_name;
      if (product.child_started) {
        options.timings->record_completed_process_event(
            timing_name,
            product.elapsed_nanoseconds,
            product.child_user_nanoseconds,
            product.child_system_nanoseconds,
            TimingVisibility::Detail);
      } else {
        options.timings->record_completed_event(
            timing_name,
            product.elapsed_nanoseconds,
            TimingVisibility::Detail);
      }
    }
  }

  if (!scheduled.ok) {
    // WorkGraph waits for all already-started independent work, then stores
    // terminal states by task ID. Select the first failure in that stable domain
    // instead of whichever worker happened to report first.
    for (std::size_t task_index = 0;
         task_index < scheduled.tasks.size();
         ++task_index) {
      if (scheduled.tasks[task_index].state == WorkTaskState::Succeeded) {
        continue;
      }
      diagnostics.error(
          SourceRange::invalid(),
          "native object task '" + object_plan.tasks[task_index].display_name +
              "' failed: " + scheduled.tasks[task_index].failure);
      return result;
    }
    diagnostics.error(
        SourceRange::invalid(),
        "native object work graph failed without a task diagnostic");
    return result;
  }

  // Publication is deliberately sequential and uses each package layout's
  // canonical module/assembly order. Worker completion order therefore
  // cannot affect directory contents, archive member order, or final linking.
  for (std::size_t task_index = 0;
       task_index < object_plan.tasks.size();
       ++task_index) {
    const NativeObjectTask &task = object_plan.tasks[task_index];
    const NativeObjectTaskProduct &product = products[task_index];
    std::string write_error;
    if (task.kind != NativeObjectTaskKind::PackageAssembly) {
      if (product.publish_source) {
        const std::filesystem::path source =
            build_directory / (task.output_stem + task.source_extension);
        if (!write_atomic(source, product.source_bytes, write_error)) {
          diagnostics.error(SourceRange::invalid(), write_error);
          return result;
        }
      }
      const std::filesystem::path native_output = assembly_output
          ? output_path / (task.output_stem + ".s")
          : build_directory /
              (task.output_stem + std::string(object_file_extension(target)));
      if (!write_atomic(native_output, product.native_bytes, write_error)) {
        diagnostics.error(SourceRange::invalid(), write_error);
        return result;
      }
      if (!assembly_output) objects.push_back(native_output.string());
      continue;
    }

    const std::filesystem::path source =
        (assembly_output ? output_path : build_directory) /
        (task.output_stem + task.source_extension);
    if (!write_atomic(source, product.source_bytes, write_error)) {
      diagnostics.error(SourceRange::invalid(), write_error);
      return result;
    }
    if (assembly_output) continue;
    const std::filesystem::path object =
        build_directory /
        (task.output_stem + std::string(object_file_extension(target)));
    if (!write_atomic(object, product.native_bytes, write_error)) {
      diagnostics.error(SourceRange::invalid(), write_error);
      return result;
    }
    objects.push_back(object.string());
  }
  object_emission_timing.finish();

  if (source_correlation.program_identity.empty()) {
    source_correlation.program_identity = "llvm-modules-sha256:" +
        direct_module_identity.finalize().hex();
  }

  // Write the map only after every native object task succeeds and canonical
  // object publication completes. A failed partial build may leave worker-
  // private diagnostic files in the isolated directory, but it must not
  // publish a complete-looking source correlation artifact for an incomplete
  // graph.
  std::string correlation_error;
  if (!validate_source_correlation_map(
          source_correlation,
          correlation_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish native source correlation: " + correlation_error);
    return result;
  }
  const std::string source_correlation_bytes =
      serialize_source_correlation_map(source_correlation);
  const std::filesystem::path source_correlation_path =
      build_directory / "draft-source-correlation.json";
  if (!write_atomic(
          source_correlation_path,
          source_correlation_bytes,
          correlation_error)) {
    diagnostics.error(SourceRange::invalid(), correlation_error);
    return result;
  }
  result.source_correlation_path = source_correlation_path.string();
  result.source_correlation_digest = sha256(source_correlation_bytes);

  if (options.artifact_kind == NativeArtifactKind::Assembly) {
    result.ok = true;
    result.output_path = output_path.string();
    result.runtime_assets = runtime_assets;
    return result;
  }

  for (const VerifiedForeignProviderInput &provider : foreign_providers) {
    if (provider.kind == ForeignArtifactKind::Object) {
      objects.push_back(provider.path.string());
    }
  }

  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path(), directory_error);
    if (directory_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create executable output directory: " + directory_error.message());
      return result;
    }
  }

  if (options.artifact_kind == NativeArtifactKind::StaticLibrary) {
    if (!foreign_providers.empty()) {
      diagnostics.error(
          SourceRange::invalid(),
          "static library output cannot embed a mapped foreign provider; "
          "link that provider when the archive is consumed");
      return result;
    }
    // Archive metadata is part of the emitted artifact, so deterministic mode
    // is required for every build. Apple's host ar has no deterministic
    // switch; its libtool replacement does. ELF uses LLVM ar's `D` mode.
    std::vector<std::string> archive_arguments{archiver_path};
    if (target.facts.object_format == "elf") {
      archive_arguments.push_back("rcsD");
      archive_arguments.push_back(output_path.string());
    } else if (target.facts.object_format == "coff") {
      archive_arguments.push_back("/nologo");
      archive_arguments.push_back("/OUT:" + output_path.string());
    } else {
      archive_arguments.push_back("-static");
      archive_arguments.push_back("-D");
      archive_arguments.push_back("-o");
      archive_arguments.push_back(output_path.string());
    }
    archive_arguments.insert(
        archive_arguments.end(), objects.begin(), objects.end());
    const ProcessResult archive = run_timed_process(
        archive_arguments, options.timings, "native archive emission");
    if (!archive.started || archive.exit_code != 0) {
      diagnostics.error(
          SourceRange::invalid(),
          phase_failure("deterministic static archive emission", archive));
      return result;
    }
    result.ok = true;
    result.output_path = output_path.string();
    result.runtime_assets = runtime_assets;
    return result;
  }

  if (options.artifact_kind == NativeArtifactKind::Object &&
      target.facts.object_format == "coff") {
    // COFF has no counterpart to the ELF/Mach-O relocatable partial link. A
    // graph which already consists of one object can publish those exact bytes
    // under the requested path. A provider-free graph with several package or
    // assembly objects must use a deterministic `.lib`; pretending that
    // archive bytes are one `.obj` would violate the public artifact contract
    // and confuse every downstream linker and object inspector. Mapped
    // providers remain separate inputs by contract, so they instead require a
    // final linked artifact or must be supplied by the eventual consumer.
    if (!foreign_providers.empty()) {
      diagnostics.error(
          SourceRange::invalid(),
          "COFF object output cannot combine a mapped foreign provider; "
          "build an executable or dynamic library, or link the provider "
          "when the object is consumed");
      return result;
    }
    if (objects.size() != 1) {
      diagnostics.error(
          SourceRange::invalid(),
          "COFF object output requires exactly one native input; use "
          "--kind static-library for a multi-package or assembly object set");
      return result;
    }
    std::string object_bytes;
    std::string read_error;
    if (!read_file_bytes(objects.front(), object_bytes, read_error)) {
      diagnostics.error(SourceRange::invalid(), read_error);
      return result;
    }
    std::string write_error;
    if (!write_atomic(output_path, object_bytes, write_error)) {
      diagnostics.error(SourceRange::invalid(), write_error);
      return result;
    }
    result.ok = true;
    result.output_path = output_path.string();
    result.runtime_assets = runtime_assets;
    return result;
  }

  std::vector<std::string> link_arguments = {
      clang_path,
  };
  append_target_arguments(target, link_arguments);
  if (target.facts.object_format == "elf" ||
      target.facts.object_format == "coff") {
    // Name the linker family explicitly. The selected Clang installation can
    // find its matching `ld.lld` beside its own tools; it must not silently
    // choose a different host linker implementation.
    link_arguments.push_back("-fuse-ld=lld");
  }
  if (options.artifact_kind == NativeArtifactKind::Object) {
    for (const VerifiedForeignProviderInput &provider : foreign_providers) {
      if (provider.kind == ForeignArtifactKind::SharedLibrary) {
        diagnostics.error(
            SourceRange::invalid(),
            "relocatable object output cannot bind shared-library provider '" +
                provider.provider + "'");
        return result;
      }
    }
    link_arguments.push_back("-nostdlib");
    if (target.facts.object_format == "elf") {
      // Clang defaults this GNU target to PIE, which is incompatible with
      // lld's relocatable `-r` mode. A merged object has no load address.
      link_arguments.push_back("-no-pie");
    }
    link_arguments.push_back("-Wl,-r");
  } else if (options.artifact_kind == NativeArtifactKind::DynamicLibrary) {
    if (target.facts.object_format == "elf") {
      link_arguments.push_back("-shared");
      link_arguments.push_back(
          "-Wl,-soname," + output_path.filename().string());
    } else if (target.facts.object_format == "coff") {
      link_arguments.push_back("-shared");
    } else {
      link_arguments.push_back("-nostdlib");
      link_arguments.push_back("-dynamiclib");
      link_arguments.push_back(
          "-Wl,-install_name,@rpath/" + output_path.filename().string());
    }
  } else if (options.artifact_kind == NativeArtifactKind::Executable) {
    if (target.facts.object_format == "macho" &&
        options.instrumentation == NativeInstrumentationProfile::None) {
      // Ordinary Draft executables name their one system library explicitly.
      // AddressSanitizer instead uses the selected LLVM Clang driver's complete
      // link recipe below so that driver can add its matching runtime.
      link_arguments.push_back("-nostdlib");
    }
  }
  if ((options.artifact_kind == NativeArtifactKind::Executable ||
       options.artifact_kind == NativeArtifactKind::DynamicLibrary) &&
      target.facts.object_format == "elf" &&
      std::any_of(foreign_providers.begin(), foreign_providers.end(),
                  [](const VerifiedForeignProviderInput &provider) {
                    return provider.kind == ForeignArtifactKind::SharedLibrary;
                  })) {
    // Draft's vendored-dependency model normally places a consumed shared
    // provider beside the final program. A loader-relative search path makes
    // that bundle relocatable and avoids requiring LD_LIBRARY_PATH merely to
    // run an artifact linked from an exact provider path. `$ORIGIN` reaches the
    // linker as one argv value, so no shell expands it here.
    link_arguments.push_back("-Wl,-rpath,$ORIGIN");
  }
  std::optional<std::filesystem::path> pdb_path;
  std::optional<std::filesystem::path> import_library_path;
  if (target.facts.object_format == "macho") {
    // Apple's linker derives its UUID from the link result. Reproducible mode
    // also removes input timestamps from debug-map bookkeeping, including a
    // relocatable object link.
    link_arguments.push_back("-Wl,-reproducible");
  } else if (target.facts.object_format == "elf" &&
             options.artifact_kind != NativeArtifactKind::Object) {
    // lld hashes the final ELF contents for this note, giving debuggers a
    // useful identifier without introducing a time or random input.
    link_arguments.push_back("-Wl,--build-id=sha1");
  } else if (target.facts.object_format == "coff") {
    // lld-link's reproducible mode derives timestamps and the PDB GUID from
    // content. Linked PE artifacts always carry CodeView debug information in
    // a sibling PDB; object/archive outputs keep their records in the members.
    link_arguments.push_back("-Wl,/Brepro");
    if (options.artifact_kind == NativeArtifactKind::Executable ||
        options.artifact_kind == NativeArtifactKind::DynamicLibrary) {
      pdb_path = output_path;
      pdb_path->replace_extension(".pdb");
      std::error_code remove_error;
      std::filesystem::remove(*pdb_path, remove_error);
      if (remove_error) {
        diagnostics.error(
            SourceRange::invalid(),
            "cannot replace PE debug-symbol file: " +
                remove_error.message());
        return result;
      }
      link_arguments.push_back("-Wl,/debug:full");
      link_arguments.push_back("-Xlinker");
      link_arguments.push_back("/pdb:" + pdb_path->string());
      link_arguments.push_back("-Xlinker");
      link_arguments.push_back("/pdbaltpath:%_PDB%");
    }
    if (options.artifact_kind == NativeArtifactKind::Executable) {
      // Draft's Windows entry is wmain so UTF-16 process vectors can be
      // converted to the language's UTF-8 byte-string views without depending
      // on the machine's active ANSI code page.
      link_arguments.push_back("-Xlinker");
      link_arguments.push_back("/entry:wmainCRTStartup");
    }
    if (options.artifact_kind == NativeArtifactKind::DynamicLibrary) {
      import_library_path = output_path;
      import_library_path->replace_extension(".lib");
      std::error_code remove_error;
      std::filesystem::remove(*import_library_path, remove_error);
      if (remove_error) {
        diagnostics.error(
            SourceRange::invalid(),
            "cannot replace PE import library: " + remove_error.message());
        return result;
      }
      link_arguments.push_back("-Xlinker");
      link_arguments.push_back("/implib:" + import_library_path->string());
    }
  }
  link_arguments.insert(link_arguments.end(), objects.begin(), objects.end());
  if (options.artifact_kind == NativeArtifactKind::Object) {
    for (const VerifiedForeignProviderInput &provider : foreign_providers) {
      if (provider.kind == ForeignArtifactKind::Archive) {
        link_arguments.push_back(provider.path.string());
      }
    }
  }
  if (options.artifact_kind == NativeArtifactKind::Executable ||
      options.artifact_kind == NativeArtifactKind::DynamicLibrary) {
    for (const VerifiedForeignProviderInput &provider : foreign_providers) {
      if (provider.kind != ForeignArtifactKind::Object) {
        link_arguments.push_back(provider.path.string());
      }
    }
    if (options.instrumentation ==
        NativeInstrumentationProfile::AddressSanitizer) {
      // Let the selected LLVM Clang driver locate and link the sanitizer runtime
      // that belongs to the linked LLVM installation. Validation evidence
      // records the linked toolchain version; the runtime is not a Draft
      // program input.
      link_arguments.push_back("-fsanitize=address");
    } else {
      link_arguments.push_back("-l" + target.system_link_library);
    }
  }
  link_arguments.push_back("-o");
  link_arguments.push_back(output_path.string());
  const ProcessResult link = run_timed_process(
      link_arguments, options.timings, "native link");
  if (!link.started || link.exit_code != 0) {
    diagnostics.error(
        SourceRange::invalid(),
        phase_failure(
            std::string(native_artifact_kind_name(options.artifact_kind)) +
                (target.facts.object_format == "elf"
                     ? " ELF link"
                 : target.facts.object_format == "coff" ? " PE/COFF link"
                                                         : " Mach-O link"),
            link));
    return result;
  }

  // A Mach-O link keeps only a debug map in the executable or dylib. Package
  // objects still contain complete DWARF, but ordinary debuggers and
  // source-aware disassemblers expect dsymutil to relocate that data into the
  // conventional sibling bundle. Run it before reporting build success so a
  // "built" native program always has the source information promised by the
  // backend contract.
  if ((options.artifact_kind == NativeArtifactKind::Executable ||
       options.artifact_kind == NativeArtifactKind::DynamicLibrary) &&
      target.facts.object_format == "macho") {
    const std::filesystem::path debug_symbols =
        output_path.string() + ".dSYM";
    std::error_code remove_error;
    std::filesystem::remove_all(debug_symbols, remove_error);
    if (remove_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot replace native debug-symbol bundle: " +
              remove_error.message());
      return result;
    }
    const std::vector<std::string> dsymutil_arguments{
        dsymutil_path,
        "--no-object-timestamp",
        "--no-swiftmodule-timestamp",
        "--num-threads=1",
        "--verify-dwarf=output",
        output_path.string(),
        "-o",
        debug_symbols.string(),
    };
    const ProcessResult debug_link = run_timed_process(
        dsymutil_arguments, options.timings, "Mach-O debug-symbol emission");
    if (!debug_link.started || debug_link.exit_code != 0) {
      diagnostics.error(
          SourceRange::invalid(),
          phase_failure("Mach-O dSYM emission", debug_link));
      return result;
    }

    const std::filesystem::path bundle_plist =
        debug_symbols / "Contents" / "Info.plist";
    const std::filesystem::path linked_dwarf =
        debug_symbols / "Contents" / "Resources" / "DWARF" /
        output_path.filename();
    if (!require_regular_file(
            bundle_plist, "Info.plist", diagnostics) ||
        !require_regular_file(
            linked_dwarf, "linked DWARF payload", diagnostics)) {
      return result;
    }

    // LLVM dsymutil emits a relocation YAML cache for later dSYM rewriting.
    // It is not needed to symbolize this exact binary, and its `binary-path`
    // row names the physical checkout. Remove only that known derived cache;
    // the standard Info.plist and linked DWARF remain intact.
    const std::filesystem::path relocation_cache =
        debug_symbols / "Contents" / "Resources" / "Relocations";
    remove_error.clear();
    std::filesystem::remove_all(relocation_cache, remove_error);
    if (remove_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot remove path-bearing dSYM relocation cache: " +
              remove_error.message());
      return result;
    }
    if (!hash_content_tree(
            debug_symbols, result.debug_symbols_digest, diagnostics)) {
      return result;
    }
    result.debug_symbols_path = debug_symbols.string();
  }
  if (pdb_path.has_value()) {
    if (!hash_regular_file(
            *pdb_path, result.debug_symbols_digest, diagnostics,
            "PE debug-symbol file")) {
      return result;
    }
    result.debug_symbols_path = pdb_path->string();
  }
  if (import_library_path.has_value()) {
    if (!hash_regular_file(
            *import_library_path, result.import_library_digest, diagnostics,
            "PE import library")) {
      return result;
    }
    result.import_library_path = import_library_path->string();
  }
  result.ok = true;
  result.output_path = output_path.string();
  result.runtime_assets = runtime_assets;
  return result;
}

NativeBuildResult build_native_executable(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    DiagnosticSink &diagnostics) {
  NativeBuildOptions executable_options = options;
  executable_options.artifact_kind = NativeArtifactKind::Executable;
  return build_native_artifact(
      target, compiled, executable_options, diagnostics);
}

} // namespace draft
