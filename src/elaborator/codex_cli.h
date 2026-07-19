// Codex CLI implementation of the provider-neutral synthesis boundary.
//
// The adapter uses the documented non-interactive `codex exec` interface. It
// writes one JSON Schema and the bounded attachment set into a private temporary
// directory, supplies the canonical textual request on stdin, requests a
// schema-validated final JSON message, and returns only its `source` string.
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

// Validates the adapter configuration, initializes state, and
// returns a synchronous provider function table. Failure leaves state cleared
// and returns a provider with a null callback.
[[nodiscard]] SynthesisProvider configure_codex_cli_provider(
    const CodexCliProviderOptions &options,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics);

} // namespace draft
