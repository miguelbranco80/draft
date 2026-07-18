// Shell-free validation child process execution.

#include "validation/runner.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)

namespace draft {

ValidationRunResult run_validation_executable(
    const ValidationRunOptions &,
    DiagnosticSink &diagnostics) {
  diagnostics.error(
      SourceRange::invalid(),
      "validation execution is not implemented on this bootstrap host");
  return {};
}

} // namespace draft

#else

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace draft {
namespace {

enum class ChildFailureStage : int {
  ChangeDirectory = 1,
  Execute = 2,
  InstallReportPipe = 3,
};

struct ChildFailure {
  int stage = 0;
  int error = 0;
};

void write_child_failure(int descriptor, ChildFailureStage stage) {
  const ChildFailure failure{static_cast<int>(stage), errno};
  const char *bytes = reinterpret_cast<const char *>(&failure);
  std::size_t written = 0;
  while (written < sizeof(failure)) {
    const ssize_t count = ::write(
        descriptor, bytes + written, sizeof(failure) - written);
    if (count > 0) {
      written += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
}

[[nodiscard]] std::string child_failure_message(
    const ChildFailure &failure) {
  const std::string operation =
      failure.stage == static_cast<int>(ChildFailureStage::ChangeDirectory)
      ? "cannot enter validation working directory"
      : failure.stage == static_cast<int>(ChildFailureStage::InstallReportPipe)
      ? "cannot install validation report pipe"
      : "cannot execute validation artifact";
  return operation + ": " + std::strerror(failure.error);
}

} // namespace

ValidationRunResult run_validation_executable(
    const ValidationRunOptions &options,
    DiagnosticSink &diagnostics) {
  ValidationRunResult result;
  if (options.executable.empty()) {
    diagnostics.error(
        SourceRange::invalid(), "validation executable path is required");
    return result;
  }

  int failure_pipe[2] = {-1, -1};
  if (::pipe(failure_pipe) != 0) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot create validation launch pipe: " +
            std::string(std::strerror(errno)));
    return result;
  }
  // The child reports setup failures through the pipe. A successful exec
  // closes the write end automatically, which gives the parent an unambiguous
  // distinction between "program returned 127" and "exec itself failed".
  const int descriptor_flags = ::fcntl(failure_pipe[1], F_GETFD);
  if (descriptor_flags < 0 ||
      ::fcntl(failure_pipe[1], F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    const int failure = errno;
    ::close(failure_pipe[0]);
    ::close(failure_pipe[1]);
    diagnostics.error(
        SourceRange::invalid(),
        "cannot configure validation launch pipe: " +
            std::string(std::strerror(failure)));
    return result;
  }

  int report_pipe[2] = {-1, -1};
  if (::pipe(report_pipe) != 0) {
    const int failure = errno;
    ::close(failure_pipe[0]);
    ::close(failure_pipe[1]);
    diagnostics.error(
        SourceRange::invalid(),
        "cannot create validation report pipe: " +
            std::string(std::strerror(failure)));
    return result;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    const int failure = errno;
    ::close(failure_pipe[0]);
    ::close(failure_pipe[1]);
    ::close(report_pipe[0]);
    ::close(report_pipe[1]);
    diagnostics.error(
        SourceRange::invalid(),
        "cannot create validation process: " +
            std::string(std::strerror(failure)));
    return result;
  }
  if (child == 0) {
    ::close(failure_pipe[0]);
    ::close(report_pipe[0]);
    constexpr int report_descriptor = 3;
    if (report_pipe[1] != report_descriptor) {
      if (::dup2(report_pipe[1], report_descriptor) < 0) {
        write_child_failure(
            failure_pipe[1], ChildFailureStage::InstallReportPipe);
        ::_exit(127);
      }
      ::close(report_pipe[1]);
    }
    if (!options.working_directory.empty() &&
        ::chdir(options.working_directory.c_str()) != 0) {
      write_child_failure(
          failure_pipe[1], ChildFailureStage::ChangeDirectory);
      ::_exit(127);
    }
    std::vector<char *> arguments;
    arguments.reserve(options.arguments.size() + 2);
    arguments.push_back(const_cast<char *>(options.executable.c_str()));
    for (const std::string &argument : options.arguments) {
      arguments.push_back(const_cast<char *>(argument.c_str()));
    }
    arguments.push_back(nullptr);
    ::execv(options.executable.c_str(), arguments.data());
    write_child_failure(failure_pipe[1], ChildFailureStage::Execute);
    ::_exit(127);
  }

  ::close(failure_pipe[1]);
  ::close(report_pipe[1]);
  ChildFailure child_failure;
  char *failure_bytes = reinterpret_cast<char *>(&child_failure);
  std::size_t received = 0;
  while (received < sizeof(child_failure)) {
    const ssize_t count = ::read(
        failure_pipe[0],
        failure_bytes + received,
        sizeof(child_failure) - received);
    if (count > 0) {
      received += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  ::close(failure_pipe[0]);

  // Read before wait so a large suite cannot fill the pipe and deadlock while
  // the parent waits for a child that is blocked reporting its final rows.
  constexpr std::size_t maximum_report_bytes = 16U * 1024U * 1024U;
  bool report_read_failed = false;
  std::uint8_t buffer[4096];
  while (true) {
    const ssize_t count = ::read(report_pipe[0], buffer, sizeof(buffer));
    if (count > 0) {
      const std::size_t byte_count = static_cast<std::size_t>(count);
      if (byte_count <= maximum_report_bytes - result.report.size()) {
        result.report.insert(
            result.report.end(), buffer, buffer + byte_count);
      } else {
        report_read_failed = true;
      }
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else if (count < 0) {
      report_read_failed = true;
      break;
    } else {
      break;
    }
  }
  ::close(report_pipe[0]);

  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot wait for validation process: " +
            std::string(std::strerror(errno)));
    return result;
  }
  if (report_read_failed) {
    diagnostics.error(
        SourceRange::invalid(),
        "validation report exceeded the 16 MiB limit or could not be read");
    return result;
  }
  if (received != 0) {
    if (received == sizeof(child_failure)) {
      diagnostics.error(
          SourceRange::invalid(), child_failure_message(child_failure));
    } else {
      diagnostics.error(
          SourceRange::invalid(),
          "validation child returned a truncated launch failure");
    }
    return result;
  }

  result.started = true;
  if (WIFEXITED(status)) {
    result.exited = true;
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.signal = WTERMSIG(status);
  }
  return result;
}

} // namespace draft

#endif
