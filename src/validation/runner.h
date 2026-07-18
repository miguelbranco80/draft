// Direct process adapter for compiled validation executables.

#pragma once

#include "source/diagnostic.h"

#include <string>
#include <vector>

namespace draft {

struct ValidationRunOptions {
  std::string executable;
  std::string working_directory;
  std::vector<std::string> arguments;
};

// A nonzero exit and a terminating signal are validation outcomes, not runner
// diagnostics. Infrastructure errors (fork, chdir, or exec) are diagnosed and
// leave started false. This distinction lets the evidence layer record failed
// attempts without pretending that the compiler itself malfunctioned.
struct ValidationRunResult {
  bool started = false;
  bool exited = false;
  int exit_code = 0;
  int signal = 0;
};

[[nodiscard]] ValidationRunResult run_validation_executable(
    const ValidationRunOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
