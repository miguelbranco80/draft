// Codex CLI adapter tests use a tiny executable fixture instead of a network
// provider. The fixture observes the real argv, stdin, JSON Schema, isolated
// current-directory input, and output-file contract. This keeps the boundary
// test deterministic while still exercising posix_spawn and exact JSON parsing.

#include "elaborator/codex_cli.h"

#include "test_directory.h"

#include "base/sha256.h"
#include "elaborator/provider.h"
#include "source/diagnostic.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

[[nodiscard]] bool atomic_cancellation_requested(void *opaque) {
  return static_cast<std::atomic_bool *>(opaque)->load();
}

struct TemporaryFixture {
  draft::test::TemporaryDirectory directory{"draft-codex-cli-test"};
  std::filesystem::path root;
  std::filesystem::path executable;

  TemporaryFixture() {
    root = directory.path();
    executable = root / "fixture-codex";

    // The script rejects any drift in the documented adapter command. It also
    // verifies that trusted operation instructions, the factual reference
    // bundle, canonical request data, and exact attachments reached Codex's
    // private request directory, then emits one schema-shaped final message.
    std::ofstream script(executable, std::ios::binary | std::ios::trunc);
    script <<
        "#!/bin/sh\n"
        "output=\n"
        "schema=\n"
        "model=\n"
        "work=\n"
        "developer=\n"
        "test \"$1\" = exec || exit 20\n"
        "shift\n"
        "while test \"$#\" -gt 0; do\n"
        "  case \"$1\" in\n"
        "    --ephemeral|--skip-git-repo-check|--ignore-user-config|--ignore-rules) shift ;;\n"
        "    --sandbox) test \"$2\" = read-only || exit 21; shift 2 ;;\n"
        "    --color) test \"$2\" = never || exit 22; shift 2 ;;\n"
        "    -c) developer=$2; shift 2 ;;\n"
        "    --model) model=$2; shift 2 ;;\n"
        "    --cd) work=$2; shift 2 ;;\n"
        "    --output-schema) schema=$2; shift 2 ;;\n"
        "    --output-last-message) output=$2; shift 2 ;;\n"
        "    -) shift ;;\n"
        "    *) exit 23 ;;\n"
        "  esac\n"
        "done\n"
        "if test -z \"$model\"; then model=fixture-model; fi\n"
        "if test \"$model\" = slow-model; then while :; do :; done; fi\n"
        "test \"$model\" = fixture-model || exit 24\n"
        "test -f \"$schema\" || exit 25\n"
        "test -f \"$work/draft-reference/language.md\" || exit 36\n"
        "test -f \"$work/draft-reference/agent-features.md\" || exit 37\n"
        "test -f \"$work/draft-reference/memory-and-ownership.md\" || exit 41\n"
        "test -f \"$work/draft-reference/interop-and-targets.md\" || exit 42\n"
        "test -f \"$work/draft-reference/core-library.md\" || exit 43\n"
        "set -- \"$work/draft-reference\"/*\n"
        "test \"$#\" -eq 5 || exit 53\n"
        "grep -q 'tui.Color_Kind' \"$work/draft-reference/core-library.md\" || exit 44\n"
        "grep -q 'does not embed the' \"$work/draft-reference/agent-features.md\" || exit 54\n"
        "! grep -q 'embeds this complete skill' \"$work/draft-reference/agent-features.md\" || exit 55\n"
        "test ! -e \"$work/draft-reference/SKILL.md\" || exit 45\n"
        "test ! -e \"$work/draft-reference/workflow-and-testing.md\" || exit 46\n"
        "test ! -e \"$work/draft-reference/core\" || exit 47\n"
        "test ! -e \"$work/draft-skill\" || exit 48\n"
        "prompt=$(cat)\n"
        "case \"$prompt\" in *DRAFT_SYNTHESIS_PROVIDER_INSTRUCTIONS_V2*|*DRAFT_EDITOR_EXPANSION_INSTRUCTIONS_V5*) exit 51 ;; esac\n"
        "case \"$developer\" in *make-answer*|*write-answer*|*repair-answer*) exit 52 ;; esac\n"
        "case \"$prompt\" in\n"
        "  *DRAFT_EDITOR_FILE_REWRITE_REQUEST_V2*REQUEST_FORMAT*draft-editor-file-rewrite-v2*ACTIVE_SOURCE_PATH*workspace/app/package.draft*SELECTED_ANNOTATION_MARKER*//?*ORIGINAL_AUTHOR_PROMPT*write-answer*ORIGINAL_PROMPT_START_BYTE*ORIGINAL_PROMPT_END_BYTE*ORIGINAL_PROMPT_START_LINE*COMPILER_FEEDBACK_PRESENT*false*COMPILER_DIAGNOSTICS*WORKSPACE_FILE_COUNT*WORKSPACE_FILE_PATH*workspace/app/helper.draft*WORKSPACE_FILE_PATH*workspace/app/package.draft*)\n"
        "    case \"$developer\" in *DRAFT_EDITOR_EXPANSION_INSTRUCTIONS_V5*) ;; *) exit 49 ;; esac\n"
        "    case \"$developer\" in *Draft*) ;; *) exit 67 ;; esac\n"
        "    case \"$developer\" in *single-file*) ;; *) exit 56 ;; esac\n"
        "    case \"$developer\" in *workspace/*) ;; *) exit 57 ;; esac\n"
        "    case \"$developer\" in *complete*active*file*) ;; *) exit 58 ;; esac\n"
        "    case \"$developer\" in *other*annotations*) ;; *) exit 59 ;; esac\n"
        "    case \"$developer\" in *Rust*) ;; *) exit 60 ;; esac\n"
        "    case \"$developer\" in *verbatim*) ;; *) exit 61 ;; esac\n"
        "    case \"$developer\" in *TODO*no-op*) ;; *) exit 68 ;; esac\n"
        "    test \"$(cat \"$work/workspace/app/package.draft\")\" = 'package app\n//! unrelated follow-up\n//? write-answer\n//! remove after implementation\nmain :: proc() -> int { return answer }' || exit 39\n"
        "    test \"$(cat \"$work/workspace/app/helper.draft\")\" = 'package app\nhelper :: 40' || exit 40\n"
        "    printf '%s' '{\"source\":\"package app\\nimport core/console\\n//! unrelated follow-up\\n//? write-answer\\nanswer :: helper + 2\\nmain :: proc() -> int { return answer }\\n\"}' > \"$output\"\n"
        "    exit 0 ;;\n"
        "  *DRAFT_EDITOR_FILE_REWRITE_REQUEST_V2*ACTIVE_SOURCE_PATH*workspace/app/package.draft*SELECTED_ANNOTATION_MARKER*//!*ORIGINAL_AUTHOR_PROMPT*repair-answer*ORIGINAL_PROMPT_START_BYTE*400*ORIGINAL_PROMPT_END_BYTE*420*ORIGINAL_PROMPT_START_LINE*9*COMPILER_FEEDBACK_PRESENT*true*COMPILER_DIAGNOSTICS*unknown*name*Missing*WORKSPACE_FILE_COUNT*)\n"
        "    case \"$developer\" in *DRAFT_EDITOR_EXPANSION_INSTRUCTIONS_V5*advisory*predate*final*model*pass*) ;; *) exit 69 ;; esac\n"
        "    test \"$(cat \"$work/workspace/app/package.draft\")\" = 'package app\nmain :: proc() -> int { return Missing }' || exit 70\n"
        "    printf '%s' '{\"source\":\"package app\\nAnswer :: 42\\nmain :: proc() -> int { return Answer }\\n\"}' > \"$output\"\n"
        "    exit 0 ;;\n"
        "esac\n"
        "test -f \"$work/attachment-00000000.bin\" || exit 26\n"
        "attachment=$(cat \"$work/attachment-00000000.bin\")\n"
        "case \"$attachment\" in attachment-bytes|parallel-first|parallel-second) ;; *) exit 27 ;; esac\n"
        "test -f \"$work/documentation-00000000-attachment-00000000.bin\" || exit 29\n"
        "test \"$(cat \"$work/documentation-00000000-attachment-00000000.bin\")\" = design-bytes || exit 30\n"
        "test -f \"$work/judgment-00000000-attachment-00000000.bin\" || exit 31\n"
        "test \"$(cat \"$work/judgment-00000000-attachment-00000000.bin\")\" = evidence-bytes || exit 32\n"
        "test -f \"$work/import-00000000-documentation-00000000-attachment-00000000.bin\" || exit 33\n"
        "test \"$(cat \"$work/import-00000000-documentation-00000000-attachment-00000000.bin\")\" = imported-design-bytes || exit 34\n"
        "case \"$prompt\" in\n"
        "  *REJECTED_SOURCE*bad-fragment*COMPILER_DIAGNOSTICS*fixture-compiler-error*) ;;\n"
        "  *REJECTED_SOURCE*) exit 35 ;;\n"
        "esac\n"
        "case \"$prompt\" in\n"
        "  *DRAFT_SYNTHESIS_REQUEST_V1*REQUEST_FORMAT*draft-synthesis-request-v21*ROOT_IDENTITY*workspace*SOURCE_RELATIVE_PATH*package.draft*ANCHOR_NAME*visible_name*EXPECTED_TYPE_TEXT*i64*TARGET_IDENTITY*draft-aarch64-macos-v6*ENCLOSING_DECLARATION_NAME*visible_name*ENCLOSING_DECLARATION_SOURCE*visible_name*ENCLOSING_SEMANTIC_SKELETON*fixture-skeleton*BRANCH_REFINEMENTS*BRANCH_KIND*loop-condition-entered*BRANCH_SUBJECT*ready*BRANCH_SUBJECT_TYPE_TEXT*bool*LOOP_RANGES*LOOP_RANGE_KIND*header-entry-value*LOOP_RANGE_BINDING*index*LOOP_RANGE_BINDING_TYPE_TEXT*i64*LOOP_RANGE_LOWER_INCLUSIVE*0*LOOP_RANGE_UPPER*limit*LOOP_RANGE_UPPER_TYPE_TEXT*i64*ACTIVE_DENIALS*DENIAL_SELECTOR*assert*PERMITTED_CONTEXT_FIELDS*CONTEXT_FIELD_NAME*allocator*CONTEXT_FIELD_TYPE_TEXT*runtime.Allocator*PARAMETRIC_PARAMETERS*PARAMETER_NAME*T*PARAMETER_CONSTRAINT*integer*PARAMETER_TYPE_TEXT*T*TYPE_CONTEXTS*TYPE_REFERENCE_SHA256*TYPE_DEFINITION*MEMBER_NAME*IMPORTED_PACKAGES*IMPORT_ALIAS*lib*IMPORT_DEFINITION*DECLARATION_NAME*make*IMPORT_DOCUMENTATION*IMPORT_DOC_ANCHOR*make*IMPORT_DOC_TEXT*imported-design*IMPORT_DOC_ATTACHMENT_PATH*IMPORTED.md*GUIDING_JUDGMENTS*JUDGMENT_ANCHOR*visible_name*JUDGMENT_CLAIM*preserve-invariant*JUDGMENT_ATTACHMENT_PATH*EVIDENCE.md*DOCUMENTATION*DOC_ANCHOR*visible_name*DOC_TEXT*design-context*DOC_ATTACHMENT_PATH*DESIGN.md*VALIDATION_CONTEXT*VALIDATION_KIND*test*VALIDATION_SOURCE_PATH*behavior_test.draft*VALIDATION_SOURCE*test_fixture*VALIDATION_TYPING_COMPLETE*true*VALIDATION_PROCEDURE_NAME*test_fixture*VALIDATION_PROCEDURE_TYPE_TEXT*proc*VALIDATION_STATE_SIZE*24*VALIDATION_REFERENCE_NAME*visible_name*VALIDATION_REFERENCE_TYPE_TEXT*u32*VALIDATION_REFERENCE_HAS_CONSTANT*true*VALIDATION_REFERENCE_CONSTANT*fixture-constant*AUTHOR_PROMPT*make-answer*BINDING_NAME*visible_name*BINDING_TYPE_TEXT*u32*BINDING_HAS_CONSTANT*true*BINDING_CONSTANT*fixture-constant*RELEVANT_DECLARATIONS*DECLARATION_SOURCE_PATH*package.draft*DECLARATION_NAME*visible_name*DECLARATION_TYPE_TEXT*u32*DECLARATION_HAS_CONSTANT*true*DECLARATION_CONSTANT*fixture-constant*DECLARATION_SOURCE*visible_name*FRAGMENT_CONTRACT*EXPECTED_TYPE_TEXT*COMPILER_REJECTIONS*) ;;\n"
        "  *) exit 28 ;;\n"
        "esac\n"
        "case \"$developer\" in *DRAFT_SYNTHESIS_PROVIDER_INSTRUCTIONS_V2*) ;; *) exit 50 ;; esac\n"
        "case \"$developer\" in *Draft*) ;; *) exit 62 ;; esac\n"
        "case \"$developer\" in *grammar*) ;; *) exit 63 ;; esac\n"
        "case \"$developer\" in *Rust*) ;; *) exit 64 ;; esac\n"
        "case \"$developer\" in *fragment*) ;; *) exit 65 ;; esac\n"
        "case \"$developer\" in *rejections*) ;; *) exit 66 ;; esac\n"
        "result='{\"source\":\"40 + 2\\n\"}'\n"
        "fixture=$(dirname \"$0\")\n"
        "case \"$attachment\" in\n"
        "  parallel-first) marker=first; peer=second; result='{\"source\":\"11\"}' ;;\n"
        "  parallel-second) marker=second; peer=first; result='{\"source\":\"31\"}' ;;\n"
        "  *) marker=; peer= ;;\n"
        "esac\n"
        "if test -n \"$marker\"; then\n"
        "  : > \"$fixture/$marker.ready\"\n"
        "  count=0\n"
        "  while ! test -f \"$fixture/$peer.ready\"; do\n"
        "    count=$((count + 1))\n"
        "    test \"$count\" -lt 500 || exit 38\n"
        "    sleep 0.01\n"
        "  done\n"
        "fi\n"
        "printf '%s' \"$result\" > \"$output\"\n";
    script.close();
    if (!script) std::exit(EXIT_FAILURE);
#if defined(__APPLE__) || defined(__unix__)
    if (::chmod(executable.c_str(), 0700) != 0) std::exit(EXIT_FAILURE);
#endif
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
  request.obligation.target.identity = "draft-aarch64-macos-v6";
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
  request.obligation.enclosing_declaration.semantic_skeleton =
      "fixture-skeleton";
  request.obligation.enclosing_declaration.semantic_skeleton_digest =
      draft::sha256(
          request.obligation.enclosing_declaration.semantic_skeleton);
  draft::AgentBranchRefinement refinement;
  refinement.kind = draft::AgentBranchRefinementKind::LoopConditionTrue;
  refinement.subject = "ready";
  refinement.subject_digest = draft::sha256(refinement.subject);
  refinement.type_digest = draft::sha256("bool-type");
  refinement.type_text = "bool";
  request.obligation.branch_refinements.push_back(std::move(refinement));
  draft::AgentLoopRange loop_range;
  loop_range.kind = draft::AgentLoopRangeKind::HeaderEntryValue;
  loop_range.binding_name = "index";
  loop_range.binding_type_digest = draft::sha256("i64-type");
  loop_range.binding_type_text = "i64";
  loop_range.upper = "limit";
  loop_range.upper_digest = draft::sha256(loop_range.upper);
  loop_range.upper_type_digest = draft::sha256("i64-type");
  loop_range.upper_type_text = "i64";
  request.obligation.loop_ranges.push_back(std::move(loop_range));
  draft::AgentActiveDenial denial;
  denial.selector = "assert";
  denial.selector_digest = draft::sha256(denial.selector);
  request.obligation.active_denials.push_back(std::move(denial));
  draft::AgentContextField context_field;
  context_field.name = "allocator";
  context_field.offset = 0;
  context_field.type_digest = draft::sha256("runtime-allocator");
  context_field.type_text = "runtime.Allocator";
  request.obligation.context_fields.push_back(std::move(context_field));
  draft::AgentParametricParameter parameter;
  parameter.name = "T";
  parameter.kind = draft::SymbolKind::TypeParameter;
  parameter.constraint = "integer";
  parameter.type_text = "T";
  parameter.type_digest = draft::sha256("type-parameter");
  request.obligation.parametric_parameters.push_back(std::move(parameter));
  draft::AgentTypeContext type;
  type.type_digest = draft::sha256("record-type");
  type.definition = "TYPE_ROW 1\n0\nMEMBER_NAME 5\ncount\n";
  type.definition_digest = draft::sha256(type.definition);
  request.obligation.type_contexts.push_back(std::move(type));
  draft::AgentImportedPackageContext imported_package;
  imported_package.alias = "lib";
  imported_package.root_identity = "workspace";
  imported_package.root_relative_path = "lib";
  imported_package.definition =
      "IMPORTED_DECLARATIONS 1\nDECLARATION_NAME 4\nmake\n";
  imported_package.definition_digest =
      draft::sha256(imported_package.definition);
  draft::AgentDocumentationContext imported_documentation;
  imported_documentation.anchor_name = "make";
  imported_documentation.text = "imported-design";
  imported_documentation.record_digest = draft::sha256("imported-doc");
  draft::AttachedFile imported_documentation_file;
  imported_documentation_file.relative_path = "IMPORTED.md";
  imported_documentation_file.size =
      std::string_view("imported-design-bytes").size();
  imported_documentation_file.digest =
      draft::sha256("imported-design-bytes");
  imported_documentation.files.push_back(
      std::move(imported_documentation_file));
  imported_documentation.file_contents.push_back("imported-design-bytes");
  imported_package.documentation.push_back(
      std::move(imported_documentation));
  request.obligation.imported_packages.push_back(
      std::move(imported_package));
  draft::AgentJudgmentContext judgment;
  judgment.anchor_name = "visible_name";
  judgment.claim = "preserve-invariant";
  judgment.record_digest = draft::sha256("judgment");
  draft::AttachedFile judgment_file;
  judgment_file.relative_path = "EVIDENCE.md";
  judgment_file.size = std::string_view("evidence-bytes").size();
  judgment_file.digest = draft::sha256("evidence-bytes");
  judgment.files.push_back(std::move(judgment_file));
  judgment.file_contents.push_back("evidence-bytes");
  request.obligation.guiding_judgments.push_back(std::move(judgment));
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
  draft::AgentValidationContext validation;
  validation.kind = "test";
  validation.source_relative_path = "behavior_test.draft";
  validation.source = "test_fixture";
  validation.source_digest = draft::sha256(validation.source);
  validation.typing_complete = true;
  draft::AgentValidationProcedureContext typed_procedure;
  typed_procedure.name = "test_fixture";
  typed_procedure.type_digest = draft::sha256("validation-procedure-type");
  typed_procedure.type_text = "proc";
  typed_procedure.type_definition = "validation-procedure-definition";
  typed_procedure.type_definition_digest =
      draft::sha256(typed_procedure.type_definition);
  typed_procedure.state_size = 24;
  typed_procedure.state_alignment = 8;
  typed_procedure.failure_offset = 8;
  typed_procedure.report_size = 16;
  draft::AgentValidationReferenceContext typed_reference;
  typed_reference.root_identity = "workspace";
  typed_reference.root_relative_path = "app";
  typed_reference.name = "visible_name";
  typed_reference.kind = draft::SymbolKind::Constant;
  typed_reference.type_digest = draft::sha256("binding-type");
  typed_reference.type_text = "u32";
  typed_reference.type_definition = "validation-reference-definition";
  typed_reference.type_definition_digest =
      draft::sha256(typed_reference.type_definition);
  typed_reference.has_constant = true;
  typed_reference.constant_definition = "fixture-constant";
  typed_reference.constant_digest =
      draft::sha256(typed_reference.constant_definition);
  typed_procedure.references.push_back(std::move(typed_reference));
  validation.procedures.push_back(std::move(typed_procedure));
  request.obligation.validation_context.push_back(std::move(validation));
  draft::AgentVisibleBinding binding;
  binding.name = "visible_name";
  binding.kind = draft::SymbolKind::Constant;
  binding.type_digest = draft::sha256("binding-type");
  binding.type_text = "u32";
  binding.has_constant = true;
  binding.constant_definition = "fixture-constant";
  binding.constant_digest = draft::sha256(binding.constant_definition);
  request.obligation.visible_bindings.push_back(std::move(binding));
  draft::AgentDeclarationContext declaration;
  declaration.source_relative_path = "package.draft";
  declaration.name = "visible_name";
  declaration.kind = draft::SymbolKind::Constant;
  declaration.type_digest = draft::sha256("binding-type");
  declaration.type_text = "u32";
  declaration.has_constant = true;
  declaration.constant_definition = "fixture-constant";
  declaration.constant_digest = draft::sha256(declaration.constant_definition);
  declaration.source = "visible_name :: 42";
  declaration.source_digest = draft::sha256(declaration.source);
  request.obligation.relevant_declarations.push_back(std::move(declaration));
  request.prompt = "make-answer";

  draft::SynthesisRequestFile attachment;
  attachment.relative_path = "PROMPT.txt";
  attachment.contents = "attachment-bytes";
  attachment.size = attachment.contents.size();
  attachment.digest = draft::sha256(attachment.contents);
  request.attachments.push_back(std::move(attachment));
  return request;
}

// The editor experiment shares process hardening and the factual language
// references with synthesis while retaining a separate single-file contract.
// This test proves that exact active/sibling snapshots and other annotations
// enter the request, the selected marker/range remains explicit, and Codex
// returns one complete active file rather than an inferred patch.
void test_editor_comment_expansion(TestState &state) {
  TemporaryFixture fixture;
  draft::CodexCliProviderOptions options;
  options.executable = fixture.executable;
  options.model = "fixture-model";
  const std::string active =
      "package app\n//! unrelated follow-up\n//? write-answer\n"
      "//! remove after implementation\n"
      "main :: proc() -> int { return answer }\n";
  const std::string helper = "package app\nhelper :: 40\n";
  const std::array<draft::CodexEditorWorkspaceFile, 2> workspace{{
      {"app/helper.draft", helper},
      {"app/package.draft", active},
  }};
  const std::size_t prompt_start = active.find("//?");
  const std::size_t prompt_end = active.find("//!", prompt_start + 3);
  const draft::CodexEditorExpansionRequest request{
      "app/package.draft",
      workspace,
      draft::CodexEditorExpansionPhase::Initial,
      "//?",
      prompt_start,
      prompt_end,
      3,
      active.size(),
      "write-answer",
      {},
  };
  draft::DiagnosticSink diagnostics;
  draft::CodexEditorExpansion expansion;
  EXPECT(state, draft::expand_editor_comment_with_codex(options, request,
                                                        expansion, diagnostics));
  EXPECT(state, expansion.source ==
      "package app\n"
      "import core/console\n"
      "//! unrelated follow-up\n"
      "//? write-answer\n"
      "answer :: helper + 2\n"
      "main :: proc() -> int { return answer }\n");
  EXPECT(state, !diagnostics.has_errors());

  // The feedback phase keeps the original annotation identity and coordinates
  // even when the first candidate has removed that marker and is much shorter.
  // It is one direct adapter invocation: retry policy remains CompilerSession's
  // responsibility rather than becoming hidden provider behavior.
  const std::string candidate =
      "package app\nmain :: proc() -> int { return Missing }\n";
  const std::array<draft::CodexEditorWorkspaceFile, 2> feedback_workspace{{
      {"app/helper.draft", helper},
      {"app/package.draft", candidate},
  }};
  const draft::CodexEditorExpansionRequest feedback_request{
      "app/package.draft",
      feedback_workspace,
      draft::CodexEditorExpansionPhase::CompilerFeedback,
      "//!",
      400,
      420,
      9,
      512,
      "repair-answer",
      "app/package.draft:2:38: error: unknown name 'Missing'\n",
  };
  draft::DiagnosticSink feedback_diagnostics;
  draft::CodexEditorExpansion repaired;
  EXPECT(state, draft::expand_editor_comment_with_codex(
                    options, feedback_request, repaired,
                    feedback_diagnostics));
  EXPECT(state, repaired.source ==
      "package app\n"
      "Answer :: 42\n"
      "main :: proc() -> int { return Answer }\n");
  EXPECT(state, !feedback_diagnostics.has_errors());
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
  EXPECT(state, provider.prepare != nullptr);
  EXPECT(state, provider.provider_identity == "openai-codex-cli-v30");
  EXPECT(state, provider.maximum_parallel_calls == 4);
  EXPECT(state, provider.model_identity == "fixture-model");
  EXPECT(state, provider.configuration_identity ==
      provider_state.configuration_identity);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, provider_state.draft_reference != nullptr);
  if (provider_state.draft_reference != nullptr) {
    EXPECT(state, provider_state.draft_reference->root().empty());
  }

  // Provider preparation is lazy and command-scoped. Repeating it reuses the
  // same materialization rather than copying the references per synthesis site.
  EXPECT(state, provider.prepare(provider.state, diagnostics));
  std::filesystem::path materialized_reference;
  if (provider_state.draft_reference != nullptr) {
    materialized_reference = provider_state.draft_reference->root();
    EXPECT(state, !materialized_reference.empty());
    EXPECT(state,
           (std::filesystem::status(materialized_reference).permissions() &
            std::filesystem::perms::owner_write) ==
               std::filesystem::perms::none);
    EXPECT(state, std::filesystem::is_regular_file(materialized_reference /
                                                   "language.md"));
    EXPECT(state,
           !std::filesystem::exists(materialized_reference / "SKILL.md"));
  }
  EXPECT(state, provider.prepare(provider.state, diagnostics));
  if (provider_state.draft_reference != nullptr) {
    EXPECT(state,
        provider_state.draft_reference->root() == materialized_reference);
  }

  draft::SynthesisRequest request = make_request();
  draft::SynthesisResponse response;
  const bool synthesized = provider.synthesize(
      provider.state, request, response, diagnostics);
  EXPECT(state, synthesized);
  EXPECT(state, response.source == "40 + 2\n");
  EXPECT(state, !diagnostics.has_errors());

  // One prepared state may serve independent request directories concurrently.
  // This exercises the real posix_spawn boundary and shared immutable reference
  // link rather than proving concurrency only with the in-process resolver
  // fake. Distinct attachment/response pairs detect request-directory
  // cross-talk; the child fixture releases neither response until both
  // processes are present.
  std::array<draft::SynthesisResponse, 2> parallel_responses;
  std::array<draft::DiagnosticSink, 2> parallel_diagnostics;
  std::array<bool, 2> parallel_ok{false, false};
  std::array<draft::SynthesisRequest, 2> parallel_requests{request, request};
  parallel_requests[0].attachments[0].contents = "parallel-first";
  parallel_requests[1].attachments[0].contents = "parallel-second";
  for (draft::SynthesisRequest &parallel_request : parallel_requests) {
    parallel_request.attachments[0].size =
        parallel_request.attachments[0].contents.size();
    parallel_request.attachments[0].digest =
        draft::sha256(parallel_request.attachments[0].contents);
  }
  std::thread first_call([&]() {
    parallel_ok[0] = provider.synthesize(
        provider.state,
        parallel_requests[0],
        parallel_responses[0],
        parallel_diagnostics[0]);
  });
  std::thread second_call([&]() {
    parallel_ok[1] = provider.synthesize(
        provider.state,
        parallel_requests[1],
        parallel_responses[1],
        parallel_diagnostics[1]);
  });
  first_call.join();
  second_call.join();
  for (std::size_t index = 0; index < parallel_ok.size(); ++index) {
    EXPECT(state, parallel_ok[index]);
    const std::string expected = index == 0 ? "11" : "31";
    EXPECT(state, parallel_responses[index].source == expected);
    EXPECT(state, !parallel_diagnostics[index].has_errors());
  }

  // A stateless correction call carries the exact rejected bytes and rendered
  // compiler diagnostics in explicit length-prefixed fields. The fixture
  // process above rejects a partial or renamed correction transcript.
  draft::SynthesisRequest correction = request;
  correction.prior_rejections.push_back({
      1,
      "bad-fragment",
      "fixture-compiler-error",
  });
  draft::DiagnosticSink correction_diagnostics;
  draft::SynthesisResponse correction_response;
  EXPECT(state,
      provider.synthesize(
          provider.state,
          correction,
          correction_response,
          correction_diagnostics));
  EXPECT(state, correction_response.source == "40 + 2\n");
  EXPECT(state, !correction_diagnostics.has_errors());

  // Readable branch facts duplicate their content identity deliberately. The
  // adapter rechecks that boundary before starting Codex, just as it does for
  // attached source and documentation bytes.
  draft::SynthesisRequest invalid_refinement = request;
  invalid_refinement.obligation.branch_refinements.front().subject_digest =
      draft::sha256("different condition");
  draft::DiagnosticSink invalid_refinement_diagnostics;
  draft::SynthesisResponse invalid_refinement_response;
  EXPECT(state,
      !provider.synthesize(
          provider.state,
          invalid_refinement,
          invalid_refinement_response,
          invalid_refinement_diagnostics));
  EXPECT(state, invalid_refinement_diagnostics.error_count() == 1);

  draft::SynthesisRequest invalid_loop_range = request;
  invalid_loop_range.obligation.loop_ranges.front().upper_digest =
      draft::sha256("different upper bound");
  draft::DiagnosticSink invalid_loop_range_diagnostics;
  draft::SynthesisResponse invalid_loop_range_response;
  EXPECT(state,
      !provider.synthesize(
          provider.state,
          invalid_loop_range,
          invalid_loop_range_response,
          invalid_loop_range_diagnostics));
  EXPECT(state, invalid_loop_range_diagnostics.error_count() == 1);

  draft::SynthesisRequest invalid_declaration = request;
  invalid_declaration.obligation.relevant_declarations.front().source_digest =
      draft::sha256("different declaration");
  draft::DiagnosticSink invalid_declaration_diagnostics;
  draft::SynthesisResponse invalid_declaration_response;
  EXPECT(state,
      !provider.synthesize(
          provider.state,
          invalid_declaration,
          invalid_declaration_response,
          invalid_declaration_diagnostics));
  EXPECT(state, invalid_declaration_diagnostics.error_count() == 1);

  draft::SynthesisRequest invalid_validation = request;
  invalid_validation.obligation.validation_context.front()
      .procedures.front().type_definition_digest =
      draft::sha256("different validation type");
  draft::DiagnosticSink invalid_validation_diagnostics;
  draft::SynthesisResponse invalid_validation_response;
  EXPECT(state,
      !provider.synthesize(
          provider.state,
          invalid_validation,
          invalid_validation_response,
          invalid_validation_diagnostics));
  EXPECT(state, invalid_validation_diagnostics.error_count() == 1);

  // Model selection is inspectable generation provenance. It changes the
  // provider's audit identity but not the resolver's content-freshness rule.
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

  // An omitted model delegates to Codex user configuration and omits the
  // `--model` pair entirely. The fixture treats that absence as its default,
  // proving this path through the real child-process argument parser.
  draft::CodexCliProviderOptions default_options;
  default_options.executable = fixture.executable;
  draft::DiagnosticSink default_diagnostics;
  draft::CodexCliProviderState default_state;
  const draft::SynthesisProvider default_provider =
      draft::configure_codex_cli_provider(
          default_options, default_state, default_diagnostics);
  EXPECT(state, default_provider.synthesize != nullptr);
  EXPECT(state, default_provider.prepare != nullptr);
  EXPECT(state,
      default_provider.model_identity == "codex-built-in-default");
  draft::SynthesisResponse default_response;
  EXPECT(state,
      default_provider.prepare(default_provider.state, default_diagnostics));
  EXPECT(state, default_provider.synthesize(
      default_provider.state,
      request,
      default_response,
      default_diagnostics));
  EXPECT(state, default_response.source == "40 + 2\n");
  EXPECT(state, !default_diagnostics.has_errors());

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
  EXPECT(state, slow.prepare(slow.state, slow_diagnostics));
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

  // Cancellation is distinct from a short timeout: the external signal can
  // become true while the child is running, and the adapter must kill/reap that
  // child without spending the remaining retry budget.
  std::atomic_bool cancelled = false;
  draft::CodexCliProviderOptions cancelled_options;
  cancelled_options.executable = fixture.executable;
  cancelled_options.model = "slow-model";
  cancelled_options.timeout_milliseconds = 5000;
  cancelled_options.maximum_attempts = 2;
  cancelled_options.cancellation_state = &cancelled;
  cancelled_options.cancellation_requested = atomic_cancellation_requested;
  draft::DiagnosticSink cancelled_diagnostics;
  draft::CodexCliProviderState cancelled_state;
  const draft::SynthesisProvider cancellable =
      draft::configure_codex_cli_provider(
          cancelled_options, cancelled_state, cancelled_diagnostics);
  EXPECT(state, cancellable.synthesize != nullptr);
  EXPECT(state,
      cancellable.prepare(cancellable.state, cancelled_diagnostics));
  std::thread canceller([&cancelled]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancelled.store(true);
  });
  const auto cancellation_started = std::chrono::steady_clock::now();
  draft::SynthesisResponse cancelled_response;
  EXPECT(state,
      !cancellable.synthesize(
          cancellable.state,
          request,
          cancelled_response,
          cancelled_diagnostics));
  const auto cancellation_elapsed =
      std::chrono::steady_clock::now() - cancellation_started;
  canceller.join();
  EXPECT(state, cancellation_elapsed < std::chrono::seconds(1));
  EXPECT(state, cancelled_diagnostics.error_count() == 1);
  if (!cancelled_diagnostics.diagnostics().empty()) {
    EXPECT(state,
        cancelled_diagnostics.diagnostics().front().message.find("cancelled") !=
            std::string::npos);
  }
}

void test_runtime_instruction_identity(TestState &state) {
  draft::CodexCliProviderOptions options;
  options.executable = "fixture-codex";
  constexpr std::string_view schema =
      R"({"type":"object","properties":{},"additionalProperties":false})";

  draft::CodexCliProviderState first;
  draft::DiagnosticSink first_diagnostics;
  EXPECT(state, draft::configure_codex_cli_runtime(
      options,
      "first focused operation",
      "same-request-contract",
      schema,
      first,
      first_diagnostics));
  EXPECT(state, first.developer_instructions == "first focused operation");
  EXPECT(state, !first_diagnostics.has_errors());

  draft::CodexCliProviderState second;
  draft::DiagnosticSink second_diagnostics;
  EXPECT(state, draft::configure_codex_cli_runtime(
      options,
      "second focused operation",
      "same-request-contract",
      schema,
      second,
      second_diagnostics));
  EXPECT(state, second.configuration_identity != first.configuration_identity);
  EXPECT(state, !second_diagnostics.has_errors());

  draft::CodexCliProviderState missing;
  draft::DiagnosticSink missing_diagnostics;
  EXPECT(state, !draft::configure_codex_cli_runtime(
      options,
      {},
      "same-request-contract",
      schema,
      missing,
      missing_diagnostics));
  EXPECT(state, missing_diagnostics.error_count() == 1);
}

} // namespace

int main() {
  TestState state;
  test_adapter_contract_and_identity(state);
  test_editor_comment_expansion(state);
  test_runtime_instruction_identity(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " Codex CLI expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all Codex CLI tests passed\n";
  return EXIT_SUCCESS;
}
