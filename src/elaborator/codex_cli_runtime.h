// Shared hardened Codex CLI runtime and canonical agent-context renderer.
//
// Synthesis and judgment have different output schemas and provider semantics,
// but they must not drift in either the typed context they receive or the child
// process boundary. This module owns the executable invocation, isolated request
// directory, fixed argv, timeout/retry/cancellation policy, and complete
// AgentObligation rendering. The installed Codex distribution is ambient user
// tooling, not a Draft program input and not something this adapter vendors or
// hashes.

#pragma once

#include "base/sha256.h"
#include "elaborator/draft_reference.h"
#include "elaborator/obligation.h"
#include "source/diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

using CodexCancellationRequested = bool (*)(void *state);

struct CodexCliProviderOptions {
  // The public driver uses the ordinary `codex` PATH lookup. Embeddings and
  // deterministic tests may supply a different command without exposing an
  // executable-path flag in the Draft language workflow.
  std::filesystem::path executable = "codex";
  // Empty omits `--model`. Because the hardened invocation ignores user
  // configuration, the installed Codex CLI then selects its built-in default.
  std::string model;
  std::uint32_t timeout_milliseconds = 5U * 60U * 1000U;
  std::uint32_t maximum_attempts = 2;
  void *cancellation_state = nullptr;
  CodexCancellationRequested cancellation_requested = nullptr;
};

// One configured state is specific to one developer-instruction, prompt, and
// schema contract. Synthesis, editor expansion, and judgment therefore use
// separate state objects even when they select the same model; their
// configuration identities intentionally differ.
struct CodexCliProviderState {
  std::filesystem::path executable;
  Sha256Digest output_schema_digest;
  std::string model;
  // Exact trusted operation policy passed through Codex's developer-instruction
  // configuration channel. Authored request data never enters this string.
  std::string developer_instructions;
  // Nonempty audit spelling used when model is empty and Codex chooses its
  // built-in default. This is provenance only.
  std::string model_identity;
  std::string configuration_identity;
  std::uint32_t timeout_milliseconds = 0;
  std::uint32_t maximum_attempts = 0;
  void *cancellation_state = nullptr;
  CodexCancellationRequested cancellation_requested = nullptr;
  // Draft-code-producing operations configure this owner; judgment leaves it
  // null. Preparation materializes it once, after which concurrent invocations
  // borrow its stable directory through request-local read-only links.
  std::unique_ptr<MaterializedDraftReference> draft_reference;
};

struct CodexCliInputFile {
  // relative_path is one compiler-generated path below the private request
  // directory. Components are validated again at materialization: absolute
  // paths, dot traversal, backslashes, reserved runtime names, duplicates, and
  // file/directory collisions are rejected. Most semantic attachments use one
  // generated leaf; the editor prototype deliberately mirrors a bounded
  // workspace source tree so Codex can navigate related Draft files normally.
  std::string relative_path;
  std::string contents;
};

struct CodexAgentRequestFile {
  std::string relative_path;
  std::uint64_t size = 0;
  Sha256Digest digest;
  std::string contents;
};

// Renders every canonical semantic field in obligation, verifies all nested
// context identities, and adds nested/direct attachment bytes under generated
// private filenames. request_header and primary field distinguish synthesis
// from judgment without mixing operation policy into request data or
// duplicating the typed context protocol.
[[nodiscard]] bool prepare_codex_agent_request(
    std::string_view request_header,
    std::string_view request_format,
    const AgentObligation &obligation,
    std::string_view primary_field_name,
    std::string_view primary_field_value,
    std::span<const CodexAgentRequestFile> attachments,
    std::vector<CodexCliInputFile> &files,
    std::string &prompt,
    DiagnosticSink &diagnostics);

// Validates and hashes the model/process policy plus caller-owned developer
// instructions, request transcript, and output-schema identities. Executable
// discovery and installation bytes are deliberately excluded because they are
// ambient user configuration.
[[nodiscard]] bool configure_codex_cli_runtime(
    const CodexCliProviderOptions &options,
    std::string_view developer_instructions,
    std::string_view prompt_contract_identity,
    std::string_view output_schema,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics);

// Writes one complete request, verifies the schema against the configured
// identity, executes the fixed retry policy, returns exact schema-validated
// final-message bytes. It does not interpret the provider-specific JSON object.
[[nodiscard]] bool invoke_codex_cli_runtime(
    const CodexCliProviderState &state,
    std::string_view output_schema,
    std::string_view prompt,
    std::span<const CodexCliInputFile> files,
    std::string &response_json,
    DiagnosticSink &diagnostics);

} // namespace draft
