// Codex CLI implementation of the provider-neutral synthesis boundary and one
// deliberately separate ephemeral editor-expansion operation.
//
// The adapter uses the documented non-interactive `codex exec` interface. It
// writes one JSON Schema and the bounded attachment set into a private temporary
// directory, supplies the canonical textual request on stdin, requests a
// schema-validated final JSON message, and returns only the caller-specific
// source string.
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

// CodexEditorExpansionPhase distinguishes the author's first request from the
// one optional reconsideration owned by DraftIDE. The adapter performs exactly
// one provider call for either value; CompilerSession alone decides whether a
// private compiler check warrants the feedback phase. Keeping this distinction
// explicit prevents an easily-misread boolean from growing retry policy inside
// the provider transport.
enum class CodexEditorExpansionPhase {
  Initial,
  CompilerFeedback,
};

// CodexEditorExpansionRequest is the deliberately narrow, ephemeral editor
// counterpart to a typed SynthesisRequest. It identifies one selected
// contiguous `//?` or `//!` block in the author's original active source. The
// read-only workspace source snapshot lets Codex inspect related names,
// implementations, and other annotations without receiving the physical
// workspace path. During CompilerFeedback the active snapshot contains the
// first proposed replacement, so the original prompt offsets need not address
// that candidate and selected_marker carries the original operation identity.
// Prompt offsets are half-open bytes and prompt_line is one-based display
// metadata in the original source.
//
// This request is an IDE experiment, not Draft language semantics. In
// particular it creates no AgentObligation and cannot write resolution pins.
// compiler_diagnostics is present only for one advisory reconsideration; the
// adapter does not compile, enforce, or retry the result. The caller owns all
// views until the synchronous operation returns.
struct CodexEditorExpansionRequest {
  std::string_view source_relative_path;
  std::span<const CodexEditorWorkspaceFile> workspace_files;
  CodexEditorExpansionPhase phase = CodexEditorExpansionPhase::Initial;
  std::string_view selected_marker;
  std::size_t prompt_start = 0;
  std::size_t prompt_end = 0;
  std::size_t prompt_line = 0;
  // The editor stores the author's pre-expansion file and the final returned
  // file in one undo transaction. This size remains the original byte count on
  // the feedback call even though workspace_files contains the first candidate.
  std::size_t undo_original_source_bytes = 0;
  std::string_view prompt;
  std::string_view compiler_diagnostics;
};

// CodexEditorExpansion is one complete proposed replacement for the active
// source. The adapter bounds the original-plus-final bytes to the Draft
// editor's atomic history policy and rejects empty or NUL-containing output.
// It deliberately does not parse, type-check, diff, or interpret marker
// changes.
// Normal editor undo is the prototype's only acceptance mechanism.
struct CodexEditorExpansion {
  std::string source;
};

// Validates the adapter configuration, initializes state, and
// returns a synchronous provider function table. Failure leaves state cleared
// and returns a provider with a null callback.
[[nodiscard]] SynthesisProvider configure_codex_cli_provider(
    const CodexCliProviderOptions &options,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics);

// Asks Codex for one complete replacement of the active Draft file. The
// embedded factual Draft references and hardened read-only CLI runtime are
// reused, but the result is neither compiled nor persisted here. The request
// may be an initial rewrite or one compiler-feedback reconsideration; this
// adapter never initiates the latter itself. On success the NUL-free
// original-plus-final source fits the editor's atomic undo policy. Failure
// clears the source and reports a provider-owned diagnostic.
[[nodiscard]] bool expand_editor_comment_with_codex(
    const CodexCliProviderOptions &options,
    const CodexEditorExpansionRequest &request,
    CodexEditorExpansion &expansion,
    DiagnosticSink &diagnostics);

} // namespace draft
