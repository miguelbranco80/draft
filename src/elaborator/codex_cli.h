// Codex CLI implementation of the provider-neutral synthesis boundary and one
// deliberately separate ephemeral editor-expansion operation.
//
// The adapter uses the documented non-interactive `codex exec` interface. It
// writes one JSON Schema and the bounded attachment set into a private temporary
// directory, supplies the canonical textual request on stdin, requests a
// schema-validated final JSON message, and returns only the caller-specific
// source string or editor slot strings.
// Codex runs with a read-only sandbox, no persisted rollout, no project/user
// rules, and no workspace as its current directory.
//
// The executable is discovered through ordinary user configuration and model
// selection is optional generation policy. Provider, model, and fixed adapter
// policy are recorded as provenance but never decide pin freshness or resolved-
// program identity. Authentication remains the Codex CLI's process-local
// concern and is not serialized. Relevant official interface: Codex CLI non-
// interactive mode and command reference; Draft semantics:
// docs/specification/03-agent-synthesis.md sections 9-10.

#pragma once

#include "elaborator/codex_cli_runtime.h"
#include "elaborator/provider.h"
#include "source/diagnostic.h"

namespace draft {

// CodexEditorWorkspaceFile is one workspace-owned Draft source in the
// deterministic snapshot visible to an editor request. relative_path is a
// normalized workspace-relative semantic/display path, never a physical host
// path. contents is the exact current editor overlay when one exists and the
// exact disk source otherwise. Compiler-owned core and dependency roots never
// enter this table; Codex receives their public contracts through the compact
// factual Draft reference instead.
struct CodexEditorWorkspaceFile {
  std::string_view relative_path;
  std::string_view contents;
};

// CodexEditorExpansionRequest is the deliberately narrow, ephemeral editor
// counterpart to a typed SynthesisRequest. It identifies one contiguous `//?`
// block in one exact active source and three compiler-calculated insertion
// slots: file imports, package declarations, and the local gap after the
// comment. The read-only workspace source snapshot lets Codex inspect related
// names and implementations without receiving the physical workspace path.
// Prompt offsets are half-open bytes in the active file and prompt_line is
// one-based display metadata.
//
// This request is an IDE experiment, not Draft language semantics. In
// particular it creates no AgentObligation, performs no semantic checking, and
// cannot write resolution pins. The caller owns all views until the synchronous
// operation returns.
struct CodexEditorExpansionRequest {
  std::string_view source_relative_path;
  std::span<const CodexEditorWorkspaceFile> workspace_files;
  std::size_t prompt_start = 0;
  std::size_t prompt_end = 0;
  std::size_t prompt_line = 0;
  std::size_t import_insertion_offset = 0;
  std::size_t declaration_insertion_offset = 0;
  std::size_t local_insertion_offset = 0;
  std::string_view prompt;
};

// CodexEditorExpansion is the complete unsaved edit proposed for one request.
// Empty fields mean that slot is unnecessary. The adapter bounds their combined
// bytes and rejects NUL, but deliberately does not parse or type-check them;
// normal editor undo is the prototype's only acceptance mechanism.
struct CodexEditorExpansion {
  std::string imports;
  std::string declarations;
  std::string local;
};

// Validates the adapter configuration, initializes state, and
// returns a synchronous provider function table. Failure leaves state cleared
// and returns a provider with a null callback.
[[nodiscard]] SynthesisProvider configure_codex_cli_provider(
    const CodexCliProviderOptions &options,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics);

// Asks Codex for zero or more exact ordinary Draft fragments in the three legal
// slots. The embedded factual Draft references and hardened read-only CLI
// runtime are reused, but the result is neither compiled nor persisted here. On
// success at least one field is nonempty and the combined NUL-free result fits
// the editor's atomic undo policy. Failure clears every field and reports a
// provider-owned diagnostic.
[[nodiscard]] bool expand_editor_comment_with_codex(
    const CodexCliProviderOptions &options,
    const CodexEditorExpansionRequest &request,
    CodexEditorExpansion &expansion,
    DiagnosticSink &diagnostics);

} // namespace draft
