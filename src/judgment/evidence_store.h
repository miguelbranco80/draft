// Persistent judgment evidence history and exact-key revocation state.

#pragma once

#include "judgment/evidence.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace draft {

enum class JudgmentEvidenceStateStatus {
  Missing,
  Active,
  Revoked,
};

struct JudgmentEvidenceState {
  JudgmentEvidenceStateStatus status = JudgmentEvidenceStateStatus::Missing;
  Sha256Digest key;
  std::vector<Sha256Digest> attempts;
  std::optional<Sha256Digest> active_digest;
  std::optional<JudgmentEvidence> active_evidence;
};

struct JudgmentEvidenceCommitResult {
  bool ok = false;
  Sha256Digest key;
  Sha256Digest evidence_digest;
  std::uint64_t attempt = 0;
  bool active = false;
  std::filesystem::path evidence_path;
};

// Loads and type-checks the complete immutable history. Missing state is a
// successful read; active state includes the latest passing typed object.
[[nodiscard]] bool load_judgment_evidence_state(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key,
    JudgmentEvidenceState &state,
    DiagnosticSink &diagnostics);

// Appends one actual validator aggregate. Pass activates itself; fail revokes
// the same static judgment key while preserving every prior attempt.
[[nodiscard]] JudgmentEvidenceCommitResult commit_judgment_evidence(
    const std::filesystem::path &workspace_directory,
    JudgmentEvidence evidence,
    DiagnosticSink &diagnostics);

} // namespace draft
