// Codex CLI implementation of the provider-neutral synthesis boundary.
//
// The adapter uses the documented non-interactive `codex exec` interface. It
// writes one JSON Schema and the bounded attachment set into a private temporary
// directory, supplies the canonical textual request on stdin, requests a
// schema-validated final JSON message, and returns only its `source` string.
// Codex runs with a read-only sandbox, no persisted rollout, no project/user
// rules, and no workspace as its current directory.
//
// Executable and model selection are explicit resolution inputs. Configuration
// identity hashes the exact executable bytes plus every fixed adapter flag and
// prompt/schema version; a different installation or adapter policy stales the
// pin. Authentication remains the Codex CLI's process-local concern and is not
// serialized. Relevant official interface: Codex CLI non-interactive mode and
// command reference; Draft semantics: 03-agent-synthesis.md sections 9-10.

#pragma once

#include "elaborator/provider.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace draft {

struct CodexCliProviderOptions {
  // executable must resolve to an existing regular file. Symlinks are
  // canonicalized before hashing and execution so their target, not spelling,
  // defines provider configuration identity.
  std::filesystem::path executable;
  std::string model;
  // Resolution must never wait forever on an external provider. The defaults
  // are conservative for real model work; tests and embedding tools may select
  // a smaller explicit policy. Both values are configuration identity inputs.
  std::uint32_t timeout_milliseconds = 5U * 60U * 1000U;
  std::uint32_t maximum_attempts = 2;
};

// State owns every string referenced by the callback and must outlive the
// SynthesisProvider returned from configure_codex_cli_provider. Callers normally
// keep both values in one driver stack frame for the entire resolver call.
struct CodexCliProviderState {
  std::filesystem::path executable;
  std::string model;
  std::string configuration_identity;
  std::uint32_t timeout_milliseconds = 0;
  std::uint32_t maximum_attempts = 0;
};

// Validates and hashes the exact adapter configuration, initializes state, and
// returns a synchronous provider function table. Failure leaves state cleared
// and returns a provider with a null callback.
[[nodiscard]] SynthesisProvider configure_codex_cli_provider(
    const CodexCliProviderOptions &options,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics);

} // namespace draft
