// Shell-free foreground child-process execution for compiler clients.
//
// This low-level host utility launches one exact executable, supplies an exact
// argument vector, optionally selects a working directory, applies explicit
// environment overrides to the inherited environment, inherits the caller's
// standard streams, and waits. It owns native process handles only for the
// synchronous call. It knows nothing about Draft packages or build artifacts;
// the driver and IDE decide what should be run.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace draft {

struct ChildProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::optional<std::filesystem::path> working_directory;
  std::vector<std::string> environment_overrides;
};

// started distinguishes a launch failure from program failure. On POSIX,
// exited is false only for signal termination and signal contains the native
// signal number. Windows reports every completed child as an exit because its
// process status has no equivalent wait-status distinction. error is reserved
// for host operation failures and is never populated for a nonzero exit code.
struct ChildProcessResult {
  bool started = false;
  bool exited = false;
  int exit_code = 0;
  int signal = 0;
  std::string error;
};

[[nodiscard]] ChildProcessResult
run_child_process(const ChildProcessOptions &options);

} // namespace draft
