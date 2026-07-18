// Codex CLI adapter tests use a tiny executable fixture instead of a network
// provider. The fixture observes the real argv, stdin, JSON Schema, isolated
// current-directory input, and output-file contract. This keeps the boundary
// test deterministic while still exercising fork/exec and exact JSON parsing.

#include "elaborator/codex_cli.h"

#include "base/sha256.h"
#include "elaborator/provider.h"
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
      std::cerr << "codex_cli_test.cpp:" << line
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
    root = std::filesystem::temp_directory_path(error) / "draft-codex-cli-test";
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error) std::exit(EXIT_FAILURE);
    executable = root / "fixture-codex";

    // The script rejects any drift in the documented adapter command. It also
    // verifies that the canonical prompt and exact attachment reached Codex's
    // private request directory, then emits one schema-shaped final message.
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
        "if test \"$model\" = slow-model; then while :; do :; done; fi\n"
        "test \"$model\" = fixture-model || exit 24\n"
        "test -f \"$schema\" || exit 25\n"
        "test -f \"$work/attachment-00000000.bin\" || exit 26\n"
        "test \"$(cat \"$work/attachment-00000000.bin\")\" = attachment-bytes || exit 27\n"
        "test -f \"$work/documentation-00000000-attachment-00000000.bin\" || exit 29\n"
        "test \"$(cat \"$work/documentation-00000000-attachment-00000000.bin\")\" = design-bytes || exit 30\n"
        "prompt=$(cat)\n"
        "case \"$prompt\" in\n"
        "  *REQUEST_FORMAT*draft-synthesis-request-v4*ROOT_IDENTITY*workspace*SOURCE_RELATIVE_PATH*package.draft*ANCHOR_NAME*visible_name*EXPECTED_TYPE_TEXT*i64*TARGET_IDENTITY*draft-aarch64-macos-v5*ENCLOSING_DECLARATION_NAME*visible_name*ENCLOSING_DECLARATION_SOURCE*visible_name*DOCUMENTATION*DOC_ANCHOR*visible_name*DOC_TEXT*design-context*DOC_ATTACHMENT_PATH*DESIGN.md*AUTHOR_PROMPT*make-answer*BINDING_NAME*visible_name*BINDING_TYPE_TEXT*u32*) ;;\n"
        "  *) exit 28 ;;\n"
        "esac\n"
        "printf '%s' '{\"source\":\"40 + 2\\n\"}' > \"$output\"\n";
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

draft::SynthesisRequest make_request() {
  draft::SynthesisRequest request;
  request.obligation.kind = draft::AgentConstructKind::SynthesisExpression;
  request.obligation.site_identity = "agent-site-fixture";
  request.obligation.root_identity = "workspace";
  request.obligation.root_relative_path = "context";
  request.obligation.source_relative_path = "package.draft";
  request.obligation.anchor_name = "visible_name";
  request.obligation.record_digest = draft::sha256("record");
  request.obligation.input_digest = draft::sha256("input");
  request.obligation.expected_type_digest = draft::sha256("i64");
  request.obligation.expected_type_text = "i64";
  request.obligation.target.identity = "draft-aarch64-macos-v5";
  request.obligation.target.arch = "aarch64";
  request.obligation.target.os = "macos";
  request.obligation.target.abi = "darwin_arm64";
  request.obligation.target.byte_order = "little";
  request.obligation.target.object_format = "macho";
  request.obligation.target.file_tag = "aarch64-macos";
  request.obligation.target.pointer_bits = 64;
  request.obligation.target.page_size = 16384;
  request.obligation.target.features = {"neon"};
  request.obligation.target.simd_shapes = {{"u32", 4}};
  request.obligation.target.assembly_architecture = "aarch64";
  request.obligation.target.assembly_dialect = "draft-aarch64-apple-v2";
  request.obligation.target.assembly_instructions = {"add"};
  request.obligation.enclosing_declaration.present = true;
  request.obligation.enclosing_declaration.name = "visible_name";
  request.obligation.enclosing_declaration.kind = draft::SymbolKind::Procedure;
  request.obligation.enclosing_declaration.source =
      "visible_name :: proc() -> i64 { return ... }";
  request.obligation.enclosing_declaration.source_digest = draft::sha256(
      request.obligation.enclosing_declaration.source);
  draft::AgentDocumentationContext documentation;
  documentation.anchor_name = "visible_name";
  documentation.text = "design-context";
  documentation.record_digest = draft::sha256("documentation");
  draft::AttachedFile documentation_file;
  documentation_file.relative_path = "DESIGN.md";
  documentation_file.size = std::string_view("design-bytes").size();
  documentation_file.digest = draft::sha256("design-bytes");
  documentation.files.push_back(std::move(documentation_file));
  documentation.file_contents.push_back("design-bytes");
  request.obligation.documentation.push_back(std::move(documentation));
  draft::AgentVisibleBinding binding;
  binding.name = "visible_name";
  binding.kind = draft::SymbolKind::Constant;
  binding.type_digest = draft::sha256("binding-type");
  binding.type_text = "u32";
  request.obligation.visible_bindings.push_back(std::move(binding));
  request.prompt = "make-answer";

  draft::SynthesisRequestFile attachment;
  attachment.relative_path = "PROMPT.txt";
  attachment.contents = "attachment-bytes";
  attachment.size = attachment.contents.size();
  attachment.digest = draft::sha256(attachment.contents);
  request.attachments.push_back(std::move(attachment));
  return request;
}

void test_adapter_contract_and_identity(TestState &state) {
  TemporaryFixture fixture;
  draft::DiagnosticSink diagnostics;
  draft::CodexCliProviderState provider_state;
  draft::CodexCliProviderOptions options;
  options.executable = fixture.executable;
  options.model = "fixture-model";
  const draft::SynthesisProvider provider =
      draft::configure_codex_cli_provider(options, provider_state, diagnostics);
  EXPECT(state, provider.synthesize != nullptr);
  EXPECT(state, provider.provider_identity == "openai-codex-cli-v5");
  EXPECT(state, provider.model_identity == "fixture-model");
  EXPECT(state, provider.configuration_identity ==
      provider_state.configuration_identity);
  EXPECT(state, !diagnostics.has_errors());

  draft::SynthesisRequest request = make_request();
  draft::SynthesisResponse response;
  const bool synthesized = provider.synthesize(
      provider.state, request, response, diagnostics);
  EXPECT(state, synthesized);
  EXPECT(state, response.source == "40 + 2\n");
  EXPECT(state, !diagnostics.has_errors());

  // Model identity participates in pin freshness even when the executable is
  // unchanged. Configuration is computed before any provider process starts.
  draft::DiagnosticSink changed_diagnostics;
  draft::CodexCliProviderState changed_state;
  options.model = "another-model";
  const draft::SynthesisProvider changed =
      draft::configure_codex_cli_provider(
          options, changed_state, changed_diagnostics);
  EXPECT(state, changed.synthesize != nullptr);
  EXPECT(state, changed.configuration_identity !=
      provider.configuration_identity);
  EXPECT(state, !changed_diagnostics.has_errors());

  // A provider process cannot hold resolution indefinitely. Each failed
  // attempt is killed and reaped, then the fixed retry budget ends with one
  // compiler-owned diagnostic rather than leaking partial adapter errors.
  draft::CodexCliProviderOptions slow_options;
  slow_options.executable = fixture.executable;
  slow_options.model = "slow-model";
  slow_options.timeout_milliseconds = 25;
  slow_options.maximum_attempts = 2;
  draft::DiagnosticSink slow_diagnostics;
  draft::CodexCliProviderState slow_state;
  const draft::SynthesisProvider slow =
      draft::configure_codex_cli_provider(
          slow_options, slow_state, slow_diagnostics);
  EXPECT(state, slow.synthesize != nullptr);
  EXPECT(state, !slow_diagnostics.has_errors());
  draft::SynthesisResponse slow_response;
  EXPECT(state,
      !slow.synthesize(
          slow.state,
          request,
          slow_response,
          slow_diagnostics));
  EXPECT(state, slow_diagnostics.error_count() == 1);
  if (!slow_diagnostics.diagnostics().empty()) {
    EXPECT(state,
        slow_diagnostics.diagnostics().front().message.find(
            "failed after 2 attempt(s)") != std::string::npos);
    EXPECT(state,
        slow_diagnostics.diagnostics().front().message.find("timed out") !=
            std::string::npos);
  }
}

} // namespace

int main() {
  TestState state;
  test_adapter_contract_and_identity(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " Codex CLI expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all Codex CLI tests passed\n";
  return EXIT_SUCCESS;
}
