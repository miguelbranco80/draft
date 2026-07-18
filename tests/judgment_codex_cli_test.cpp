// Judgment Codex adapter tests use a local executable fixture. The fixture
// checks the real isolated request, judgment-only semantic fields, artifact
// bytes, and schema before returning controlled verdict JSON.

#include "judgment/codex_cli.h"

#include "base/sha256.h"
#include "judgment/provider.h"
#include "source/diagnostic.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/stat.h>
#endif

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "judgment_codex_cli_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryFixture {
  std::filesystem::path root;
  std::filesystem::path executable;

  TemporaryFixture() {
    std::error_code error;
    root = std::filesystem::temp_directory_path(error) /
        "draft-judgment-codex-cli-test";
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error) std::exit(EXIT_FAILURE);
    executable = root / "fixture-codex";

    std::ofstream script(executable, std::ios::binary | std::ios::trunc);
    script <<
        "#!/bin/sh\n"
        "output=\n"
        "schema=\n"
        "model=\n"
        "work=\n"
        "test \"$1\" = exec || exit 20\n"
        "shift\n"
        "while test \"$#\" -gt 0; do\n"
        "  case \"$1\" in\n"
        "    --ephemeral|--skip-git-repo-check|--ignore-user-config|--ignore-rules) shift ;;\n"
        "    --sandbox) test \"$2\" = read-only || exit 21; shift 2 ;;\n"
        "    --color) test \"$2\" = never || exit 22; shift 2 ;;\n"
        "    --model) model=$2; shift 2 ;;\n"
        "    --cd) work=$2; shift 2 ;;\n"
        "    --output-schema) schema=$2; shift 2 ;;\n"
        "    --output-last-message) output=$2; shift 2 ;;\n"
        "    -) shift ;;\n"
        "    *) exit 23 ;;\n"
        "  esac\n"
        "done\n"
        "test -f \"$schema\" || exit 24\n"
        "test -f \"$work/attachment-00000000.bin\" || exit 25\n"
        "test \"$(cat \"$work/attachment-00000000.bin\")\" = claim-evidence || exit 26\n"
        "test -f \"$work/requested-artifact-00000000.bin\" || exit 27\n"
        "test \"$(cat \"$work/requested-artifact-00000000.bin\")\" = object-bytes || exit 28\n"
        "prompt=$(cat)\n"
        "case \"$prompt\" in\n"
        "  *REQUEST_FORMAT*draft-judgment-request-v3*SITE*judgment-site*TARGET_IDENTITY*draft-aarch64-macos-v5*BRANCH_KIND*if-condition-entered-false*BRANCH_SUBJECT*validated*JUDGMENT_CLAIM*preserve-the-abi*ATTACHMENT_PATH*EVIDENCE.md*RESOLVED_PROGRAM_SHA256*COMPILER_IDENTITY*draft-bootstrap-cpp-v115*POLICY_IDENTITY*draft-judgment-policy-v1*VALIDATOR_IDENTITY*validator-0*REQUESTED_ARTIFACTS*ARTIFACT_KIND*object*ARTIFACT_FILE*requested-artifact-00000000.bin*ARTIFACT_SHA256*) ;;\n"
        "  *) exit 29 ;;\n"
        "esac\n"
        "case \"$model\" in\n"
        "  fixture-model) printf '%s' '{\"rationale\":\"meets \\u0063laim\",\"verdict\":\"pass\"}' > \"$output\" ;;\n"
        "  fail-model) printf '%s' '{\"verdict\":\"fail\",\"rationale\":\"ABI mismatch\"}' > \"$output\" ;;\n"
        "  malformed-model) printf '%s' '{\"verdict\":\"maybe\",\"rationale\":\"unclear\"}' > \"$output\" ;;\n"
        "  *) exit 30 ;;\n"
        "esac\n";
    script.close();
    if (!script) std::exit(EXIT_FAILURE);
#if defined(__APPLE__) || defined(__unix__)
    if (::chmod(executable.c_str(), 0700) != 0) std::exit(EXIT_FAILURE);
#endif
  }

  ~TemporaryFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

draft::JudgmentRequest make_request() {
  draft::JudgmentRequest request;
  request.obligation.kind = draft::AgentConstructKind::Judgment;
  request.obligation.site_identity = "judgment-site";
  request.obligation.root_identity = "workspace";
  request.obligation.root_relative_path = "app";
  request.obligation.source_relative_path = "package.draft";
  request.obligation.anchor_name = "exported_call";
  request.obligation.record_digest = draft::sha256("judgment record");
  request.obligation.input_digest = draft::sha256("typed judgment input");
  request.obligation.expected_type_digest = draft::sha256("unit");
  request.obligation.expected_type_text = "unit";
  request.obligation.target.identity = "draft-aarch64-macos-v5";
  request.obligation.target.arch = "aarch64";
  request.obligation.target.os = "macos";
  request.obligation.target.abi = "darwin_arm64";
  request.obligation.target.byte_order = "little";
  request.obligation.target.object_format = "macho";
  request.obligation.target.file_tag = "aarch64-macos";
  request.obligation.target.pointer_bits = 64;
  request.obligation.target.page_size = 16384;
  request.obligation.target.assembly_architecture = "aarch64";
  request.obligation.target.assembly_dialect = "draft-aarch64-apple-v2";
  draft::AgentBranchRefinement refinement;
  refinement.kind = draft::AgentBranchRefinementKind::ConditionFalse;
  refinement.subject = "validated";
  refinement.subject_digest = draft::sha256(refinement.subject);
  refinement.type_digest = draft::sha256("bool-type");
  refinement.type_text = "bool";
  request.obligation.branch_refinements.push_back(std::move(refinement));
  request.resolved_program = draft::sha256("resolved program");
  request.compiler_identity = "draft-bootstrap-cpp-v115";
  request.policy_identity = "draft-judgment-policy-v1";
  request.validator_identity = "validator-0";
  request.claim = "preserve-the-abi";

  draft::JudgmentRequestFile attachment;
  attachment.relative_path = "EVIDENCE.md";
  attachment.contents = "claim-evidence";
  attachment.size = attachment.contents.size();
  attachment.digest = draft::sha256(attachment.contents);
  request.attachments.push_back(std::move(attachment));

  draft::JudgmentRequestArtifact artifact;
  artifact.kind = "object";
  artifact.contents = "object-bytes";
  artifact.digest = draft::sha256(artifact.contents);
  request.artifacts.push_back(std::move(artifact));
  return request;
}

draft::JudgmentProvider configure(
    const TemporaryFixture &fixture,
    std::string model,
    draft::CodexCliProviderState &provider_state,
    draft::DiagnosticSink &diagnostics) {
  draft::CodexCliProviderOptions options;
  options.distribution_root = fixture.root;
  options.executable = fixture.executable;
  options.model = std::move(model);
  return draft::configure_codex_cli_judgment_provider(
      options, provider_state, diagnostics);
}

void test_adapter_contract(TestState &state) {
  TemporaryFixture fixture;
  draft::CodexCliProviderState provider_state;
  draft::DiagnosticSink diagnostics;
  const draft::JudgmentProvider provider = configure(
      fixture, "fixture-model", provider_state, diagnostics);
  EXPECT(state, provider.judge != nullptr);
  EXPECT(state, provider.provider_identity == "openai-codex-cli-v21");
  EXPECT(state, provider.model_identity == "fixture-model");
  EXPECT(state,
      provider.configuration_identity == provider_state.configuration_identity);
  EXPECT(state, !diagnostics.has_errors());

  draft::JudgmentRequest request = make_request();
  draft::JudgmentResponse response;
  EXPECT(state,
      provider.judge(
          provider.state, request, response, diagnostics));
  EXPECT(state, response.passed);
  EXPECT(state, response.rationale == "meets claim");
  EXPECT(state, !diagnostics.has_errors());

  // Artifact bytes are checked again at the adapter boundary. A caller cannot
  // make Codex inspect bytes under an incorrect compiler-owned identity.
  request.artifacts.front().contents = "changed-object";
  draft::DiagnosticSink artifact_diagnostics;
  draft::JudgmentResponse unused;
  EXPECT(state,
      !provider.judge(
          provider.state, request, unused, artifact_diagnostics));
  EXPECT(state, artifact_diagnostics.error_count() == 1);
}

void test_fail_and_malformed_verdicts(TestState &state) {
  TemporaryFixture fixture;
  draft::JudgmentRequest request = make_request();

  draft::CodexCliProviderState fail_state;
  draft::DiagnosticSink fail_diagnostics;
  const draft::JudgmentProvider failing = configure(
      fixture, "fail-model", fail_state, fail_diagnostics);
  draft::JudgmentResponse failed_response;
  EXPECT(state,
      failing.judge(
          failing.state, request, failed_response, fail_diagnostics));
  EXPECT(state, !failed_response.passed);
  EXPECT(state, failed_response.rationale == "ABI mismatch");
  EXPECT(state, !fail_diagnostics.has_errors());

  draft::CodexCliProviderState malformed_state;
  draft::DiagnosticSink malformed_diagnostics;
  const draft::JudgmentProvider malformed = configure(
      fixture, "malformed-model", malformed_state, malformed_diagnostics);
  draft::JudgmentResponse malformed_response;
  EXPECT(state,
      !malformed.judge(
          malformed.state,
          request,
          malformed_response,
          malformed_diagnostics));
  EXPECT(state, malformed_diagnostics.error_count() == 1);
}

} // namespace

int main() {
  TestState state;
  test_adapter_contract(state);
  test_fail_and_malformed_verdicts(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " judgment Codex CLI expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all judgment Codex CLI tests passed\n";
  return EXIT_SUCCESS;
}
