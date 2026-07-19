// Codex CLI implementation of the provider-neutral judgment boundary.
//
// Judgment reuses the synthesis adapter's complete typed-context renderer and
// hardened child-process runtime. This small adapter owns only judgment policy:
// the extra resolved-program/artifact fields, output schema, and exact verdict
// parser. It cannot select evidence or mutate a resolution manifest.

#pragma once

#include "elaborator/codex_cli_runtime.h"
#include "judgment/provider.h"
#include "source/diagnostic.h"

namespace draft {

// Validates the Codex process policy and judgment-specific prompt/schema
// contract, initializes state, and returns a synchronous judge.
[[nodiscard]] JudgmentProvider configure_codex_cli_judgment_provider(
    const CodexCliProviderOptions &options,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics);

} // namespace draft
