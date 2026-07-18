// Direct validation process launch and outcome classification tests.

#include "source/diagnostic.h"
#include "validation/runner.h"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "validation_runner_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_outcomes(TestState &state, const std::string &executable) {
  draft::DiagnosticSink diagnostics;
  draft::ValidationRunOptions options;
  options.executable = executable;
  options.arguments = {"--child-report"};
  draft::ValidationRunResult result =
      draft::run_validation_executable(options, diagnostics);
  EXPECT(state, result.started);
  EXPECT(state, result.exited);
  EXPECT(state, result.exit_code == 0);
  EXPECT(state, result.report.size() == 3);
  if (result.report.size() == 3) {
    EXPECT(state, result.report[0] == static_cast<std::uint8_t>('o'));
    EXPECT(state, result.report[1] == static_cast<std::uint8_t>('k'));
    EXPECT(state, result.report[2] == static_cast<std::uint8_t>('!'));
  }
  EXPECT(state, !diagnostics.has_errors());

  options.arguments = {"--child-fail"};
  result = draft::run_validation_executable(options, diagnostics);
  EXPECT(state, result.started);
  EXPECT(state, result.exited);
  EXPECT(state, result.exit_code == 7);
  EXPECT(state, !diagnostics.has_errors());

  options.arguments = {"--child-signal"};
  result = draft::run_validation_executable(options, diagnostics);
  EXPECT(state, result.started);
  EXPECT(state, !result.exited);
  EXPECT(state, result.signal == SIGTERM);
  EXPECT(state, !diagnostics.has_errors());
}

void test_launch_failures(TestState &state, const std::string &executable) {
  draft::DiagnosticSink diagnostics;
  draft::ValidationRunOptions options;
  options.executable = executable + ".missing";
  draft::ValidationRunResult result =
      draft::run_validation_executable(options, diagnostics);
  EXPECT(state, !result.started);
  EXPECT(state, diagnostics.has_errors());

  draft::DiagnosticSink directory_diagnostics;
  options.executable = executable;
  options.working_directory = executable + ".missing-directory";
  result = draft::run_validation_executable(options, directory_diagnostics);
  EXPECT(state, !result.started);
  EXPECT(state, directory_diagnostics.has_errors());
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--child-pass") return 0;
#if !defined(_WIN32)
  if (argc == 2 && std::string_view(argv[1]) == "--child-report") {
    const char report[] = "ok!";
    return ::write(3, report, 3) == 3 ? 0 : 1;
  }
#endif
  if (argc == 2 && std::string_view(argv[1]) == "--child-fail") return 7;
  if (argc == 2 && std::string_view(argv[1]) == "--child-signal") {
    std::raise(SIGTERM);
    return 1;
  }

  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(argv[0], error);
  if (error) {
    std::cerr << "cannot make test executable path absolute: "
              << error.message() << '\n';
    return EXIT_FAILURE;
  }
  TestState state;
  test_outcomes(state, absolute.lexically_normal().string());
  test_launch_failures(state, absolute.lexically_normal().string());
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
