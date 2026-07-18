// End-to-end public `draftc judge` coverage with a local Codex executable.
//
// The test crosses argument parsing, provider configuration, typed compilation,
// judgment execution, durable evidence history, and conditional manifest
// publication. No network service or release Codex installation is required.

#include "elaborator/resolution_store.h"
#include "judgment/evidence_store.h"
#include "source/diagnostic.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "judgment_driver_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryWorkspace {
  std::filesystem::path root;
  std::filesystem::path package;
  std::filesystem::path distribution;
  std::filesystem::path executable;

  TemporaryWorkspace(std::string_view name, bool passed) {
    std::error_code error;
    root = std::filesystem::temp_directory_path(error) /
        ("draft-judgment-driver-test-" + std::string(name));
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::remove_all(root, error);
    error.clear();
    package = root / "app";
    distribution = root / "codex-distribution";
    std::filesystem::create_directories(package, error);
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::create_directories(distribution, error);
    if (error) std::exit(EXIT_FAILURE);

    std::ofstream source(package / "package.draft", std::ios::binary);
    source << "package app\n\n"
              "judge \"The package remains coherent.\"\n\n"
              "main :: proc() {\n"
              "    value := 42\n"
              "    judge \"The typed local preserves the claim.\"\n"
              "    _ = value\n"
              "}\n";
    source.close();
    if (!source) std::exit(EXIT_FAILURE);

    executable = distribution / "fixture-codex";
    std::ofstream script(executable, std::ios::binary | std::ios::trunc);
    script <<
        "#!/bin/sh\n"
        "output=\n"
        "test \"$1\" = exec || exit 20\n"
        "shift\n"
        "while test \"$#\" -gt 0; do\n"
        "  case \"$1\" in\n"
        "    --ephemeral|--skip-git-repo-check|--ignore-user-config|--ignore-rules) shift ;;\n"
        "    --sandbox|--color|--model|--cd|--output-schema) shift 2 ;;\n"
        "    --output-last-message) output=$2; shift 2 ;;\n"
        "    -) shift ;;\n"
        "    *) exit 21 ;;\n"
        "  esac\n"
        "done\n"
        "test -n \"$output\" || exit 22\n";
    if (passed) {
      script <<
          "printf '%s' '{\"verdict\":\"pass\",\"rationale\":\"fixture pass\"}' > \"$output\"\n";
    } else {
      script <<
          "printf '%s' '{\"verdict\":\"fail\",\"rationale\":\"fixture fail\"}' > \"$output\"\n";
    }
    script.close();
    if (!script) std::exit(EXIT_FAILURE);
#if defined(__APPLE__) || defined(__unix__)
    if (::chmod(executable.c_str(), 0700) != 0) std::exit(EXIT_FAILURE);
#endif
  }

  ~TemporaryWorkspace() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

[[nodiscard]] int run_judge(const TemporaryWorkspace &workspace) {
#if defined(__APPLE__) || defined(__unix__)
  std::vector<std::string> arguments{
      DRAFT_DRIVER_PATH,
      "judge",
      workspace.package.string(),
      "--codex-distribution-root",
      workspace.distribution.string(),
      "--codex-executable",
      workspace.executable.string(),
      "--codex-model",
      "fixture-model",
  };
  std::vector<char *> raw;
  raw.reserve(arguments.size() + 1);
  for (std::string &argument : arguments) raw.push_back(argument.data());
  raw.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    ::execv(raw.front(), raw.data());
    ::_exit(127);
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
#else
  (void)workspace;
  return -1;
#endif
}

void test_passing_command_selects_evidence(TestState &state) {
  TemporaryWorkspace workspace("pass", true);
  EXPECT(state, run_judge(workspace) == 0);

  draft::DiagnosticSink diagnostics;
  draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.pins.empty());
  EXPECT(state, loaded.manifest.evidence.size() == 2);
  for (const draft::ResolutionEvidencePin &pin : loaded.manifest.evidence) {
    EXPECT(state, pin.kind == "judgment");
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root, pin.key, evidence, diagnostics));
    EXPECT(state,
        evidence.status == draft::JudgmentEvidenceStateStatus::Active);
    EXPECT(state, evidence.active_digest == pin.content_digest);
    EXPECT(state, evidence.active_evidence.has_value());
  }
  EXPECT(state, !diagnostics.has_errors());

  // A second command compiles through the now-present manifest, appends fresh
  // attempts for the same static keys, and atomically advances both rows.
  EXPECT(state, run_judge(workspace) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.evidence.size() == 2);
  for (const draft::ResolutionEvidencePin &pin : loaded.manifest.evidence) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root, pin.key, evidence, diagnostics));
    EXPECT(state, evidence.attempts.size() == 2);
    EXPECT(state, evidence.active_digest == pin.content_digest);
  }
  EXPECT(state, !diagnostics.has_errors());
}

void test_failing_command_leaves_manifest_missing(TestState &state) {
  TemporaryWorkspace workspace("fail", false);
  EXPECT(state, run_judge(workspace) == 1);
  draft::DiagnosticSink diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);
  EXPECT(state, !diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_passing_command_selects_evidence(state);
  test_failing_command_leaves_manifest_missing(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " judgment driver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all judgment driver tests passed\n";
  return EXIT_SUCCESS;
}
