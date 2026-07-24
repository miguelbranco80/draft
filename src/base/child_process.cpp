// Cross-platform implementation of inherited-terminal foreground execution.
//
// POSIX uses posix_spawn rather than fork so the compiler never executes
// allocator or runtime code in a copied multithreaded process. Darwin and glibc
// both provide the file-action chdir extension used for an explicit working
// directory. Windows constructs the command line according to the documented
// CRT backslash/quote rules and supplies a sorted UTF-16 environment block.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "base/child_process.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace draft {
namespace {

[[nodiscard]] bool contains_nul(std::string_view text) {
  return text.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool split_environment_override(std::string_view row,
                                              std::string &name,
                                              std::string &value) {
  const std::size_t equals = row.find('=');
  if (equals == std::string_view::npos || equals == 0 || contains_nul(row)) {
    return false;
  }
  name.assign(row.substr(0, equals));
  value.assign(row.substr(equals + 1));
  return name.find('=') == std::string::npos;
}

#if defined(_WIN32)

[[nodiscard]] std::string windows_error(std::string_view operation) {
  return std::string(operation) + " failed with Windows error " +
         std::to_string(static_cast<unsigned long>(GetLastError()));
}

[[nodiscard]] bool utf8_to_wide(std::string_view input, std::wstring &output) {
  output.clear();
  if (input.empty())
    return true;
  if (input.size() > static_cast<std::size_t>(INT_MAX))
    return false;
  const int required =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                          static_cast<int>(input.size()), nullptr, 0);
  if (required <= 0)
    return false;
  output.resize(static_cast<std::size_t>(required));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                             static_cast<int>(input.size()), output.data(),
                             required) == required;
}

[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument) {
  const bool needs_quotes =
      argument.empty() ||
      argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes)
    return std::wstring(argument);
  std::wstring quoted(1, L'"');
  std::size_t backslashes = 0;
  for (wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes * 2U + 1U, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2U, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

struct CaseInsensitiveWideLess {
  bool operator()(const std::wstring &left, const std::wstring &right) const {
    return _wcsicmp(left.c_str(), right.c_str()) < 0;
  }
};

[[nodiscard]] bool
windows_environment_block(const std::vector<std::string> &overrides,
                          std::vector<wchar_t> &block, std::string &error) {
  if (overrides.empty())
    return true;
  std::map<std::wstring, std::wstring, CaseInsensitiveWideLess> values;
  LPWCH environment = GetEnvironmentStringsW();
  if (environment == nullptr) {
    error = windows_error("GetEnvironmentStringsW");
    return false;
  }
  for (const wchar_t *row = environment; *row != L'\0';) {
    const std::wstring entry(row);
    row += entry.size() + 1;
    const std::size_t equals =
        entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
    if (equals != std::wstring::npos) {
      values[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
  }
  (void)FreeEnvironmentStringsW(environment);
  for (const std::string &override_row : overrides) {
    std::string name;
    std::string value;
    std::wstring wide_name;
    std::wstring wide_value;
    if (!split_environment_override(override_row, name, value) ||
        !utf8_to_wide(name, wide_name) || !utf8_to_wide(value, wide_value)) {
      error = "environment override must be valid UTF-8 NAME=value";
      return false;
    }
    values[std::move(wide_name)] = std::move(wide_value);
  }
  for (const auto &[name, value] : values) {
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return true;
}

#else

[[nodiscard]] bool posix_environment(const std::vector<std::string> &overrides,
                                     std::vector<std::string> &storage,
                                     std::vector<char *> &pointers,
                                     std::string &error) {
  std::map<std::string, std::string> values;
  for (char **row = environ; row != nullptr && *row != nullptr; ++row) {
    const std::string_view entry(*row);
    const std::size_t equals = entry.find('=');
    if (equals != std::string_view::npos && equals != 0) {
      values[std::string(entry.substr(0, equals))] =
          std::string(entry.substr(equals + 1));
    }
  }
  for (const std::string &override_row : overrides) {
    std::string name;
    std::string value;
    if (!split_environment_override(override_row, name, value)) {
      error = "environment override must be NAME=value";
      return false;
    }
    values[std::move(name)] = std::move(value);
  }
  storage.reserve(values.size());
  for (const auto &[name, value] : values) {
    storage.push_back(name + "=" + value);
  }
  pointers.reserve(storage.size() + 1);
  for (std::string &entry : storage)
    pointers.push_back(entry.data());
  pointers.push_back(nullptr);
  return true;
}

#endif

} // namespace

ChildProcessResult run_child_process(const ChildProcessOptions &options) {
  ChildProcessResult result;
  const std::string executable = options.executable.string();
  if (executable.empty() || contains_nul(executable)) {
    result.error = "child executable path is empty or contains NUL";
    return result;
  }
  for (const std::string &argument : options.arguments) {
    if (contains_nul(argument)) {
      result.error = "child argument contains NUL";
      return result;
    }
  }
  if (options.working_directory.has_value() &&
      contains_nul(options.working_directory->string())) {
    result.error = "child working directory contains NUL";
    return result;
  }

#if defined(_WIN32)
  std::wstring wide_executable;
  if (!utf8_to_wide(executable, wide_executable)) {
    result.error = "child executable path is not valid UTF-8";
    return result;
  }
  std::wstring command_line = quote_windows_argument(wide_executable);
  for (const std::string &argument : options.arguments) {
    std::wstring wide_argument;
    if (!utf8_to_wide(argument, wide_argument)) {
      result.error = "child argument is not valid UTF-8";
      return result;
    }
    command_line.push_back(L' ');
    command_line += quote_windows_argument(wide_argument);
  }
  std::wstring wide_directory;
  if (options.working_directory.has_value() &&
      !utf8_to_wide(options.working_directory->string(), wide_directory)) {
    result.error = "child working directory is not valid UTF-8";
    return result;
  }
  std::vector<wchar_t> environment;
  if (!windows_environment_block(options.environment_overrides, environment,
                                 result.error)) {
    return result;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const DWORD flags = environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT;
  if (!CreateProcessW(
          wide_executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
          flags, environment.empty() ? nullptr : environment.data(),
          options.working_directory.has_value() ? wide_directory.c_str()
                                                : nullptr,
          &startup, &process)) {
    result.error = windows_error("CreateProcessW");
    return result;
  }
  result.started = true;
  (void)CloseHandle(process.hThread);
  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
    result.error = windows_error("WaitForSingleObject");
    (void)CloseHandle(process.hProcess);
    return result;
  }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
    result.error = windows_error("GetExitCodeProcess");
    (void)CloseHandle(process.hProcess);
    return result;
  }
  (void)CloseHandle(process.hProcess);
  result.exited = true;
  result.exit_code = static_cast<int>(exit_code);
  return result;
#else
  std::vector<std::string> argument_storage;
  argument_storage.reserve(options.arguments.size() + 1);
  argument_storage.push_back(executable);
  argument_storage.insert(argument_storage.end(), options.arguments.begin(),
                          options.arguments.end());
  std::vector<char *> arguments;
  arguments.reserve(argument_storage.size() + 1);
  for (std::string &argument : argument_storage) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);

  std::vector<std::string> environment_storage;
  std::vector<char *> environment;
  if (!posix_environment(options.environment_overrides, environment_storage,
                         environment, result.error)) {
    return result;
  }

  posix_spawn_file_actions_t actions;
  int action_error = posix_spawn_file_actions_init(&actions);
  if (action_error != 0) {
    result.error = "posix_spawn file actions failed: " +
                   std::string(std::strerror(action_error));
    return result;
  }
  if (options.working_directory.has_value()) {
    const std::string directory = options.working_directory->string();
#if defined(__APPLE__)
    action_error =
        posix_spawn_file_actions_addchdir(&actions, directory.c_str());
#else
    action_error =
        posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
#endif
  }
  if (action_error != 0) {
    result.error = "posix_spawn working directory failed: " +
                   std::string(std::strerror(action_error));
    (void)posix_spawn_file_actions_destroy(&actions);
    return result;
  }

  pid_t child = -1;
  const int spawn_error =
      posix_spawn(&child, executable.c_str(), &actions, nullptr,
                  arguments.data(), environment.data());
  (void)posix_spawn_file_actions_destroy(&actions);
  if (spawn_error != 0) {
    result.error =
        "posix_spawn failed: " + std::string(std::strerror(spawn_error));
    return result;
  }
  result.started = true;
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno == EINTR)
      continue;
    result.error = "waitpid failed: " + std::string(std::strerror(errno));
    return result;
  }
  if (WIFEXITED(status)) {
    result.exited = true;
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.signal = WTERMSIG(status);
  }
  return result;
#endif
}

} // namespace draft
