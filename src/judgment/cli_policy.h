// Public CLI inputs for ordered Codex judgment policies.
//
// Argument-loop mechanics remain in the driver, while this module owns the
// nontrivial boundaries behind those flags: strict spelling, bounded exact
// artifact reads, provider-state lifetime, and canonical policy identity.

#pragma once

#include "judgment/codex_cli.h"
#include "judgment/command.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

struct JudgmentArtifactPath {
  std::string kind;
  std::filesystem::path path;
};

struct NamedCodexJudgmentValidator {
  std::string identity;
  CodexCliProviderOptions codex;
};

[[nodiscard]] bool parse_judgment_validator(
    std::string_view spelling,
    std::string &identity,
    std::string &model,
    std::string &reason);

[[nodiscard]] bool parse_judgment_artifact_path(
    std::string_view spelling,
    JudgmentArtifactPath &input,
    std::string &reason);

// Reads every explicit regular file completely, with a per-file bound, before
// any provider starts. Only the returned bytes and digest cross the provider
// boundary; physical paths are never evidence or request inputs.
[[nodiscard]] bool read_judgment_artifacts(
    const std::vector<JudgmentArtifactPath> &inputs,
    std::vector<JudgmentRequestArtifact> &artifacts,
    std::string &reason);

// Installs either an explicit ordered validator set or the legacy single Codex
// validator. State objects are sized before provider tables borrow them, so no
// later vector growth can invalidate an opaque provider pointer.
[[nodiscard]] bool configure_codex_judgment_policy(
    const std::optional<CodexCliProviderOptions> &default_codex,
    const std::vector<NamedCodexJudgmentValidator> &configured,
    const std::vector<JudgmentRequestArtifact> &artifacts,
    std::vector<CodexCliProviderState> &states,
    JudgmentCommandOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
