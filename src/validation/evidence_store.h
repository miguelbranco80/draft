// Persistent immutable validation attempts and exact-key revocation state.

#pragma once

#include "source/diagnostic.h"
#include "validation/evidence.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace draft {

enum class ValidationEvidenceStateStatus {
  Missing,
  Active,
  Revoked,
};

struct ValidationEvidenceState {
  ValidationEvidenceStateStatus status =
      ValidationEvidenceStateStatus::Missing;
  Sha256Digest key;
  std::vector<Sha256Digest> attempts;
  std::optional<Sha256Digest> active_digest;
  std::optional<ValidationEvidence> active_evidence;
};

struct ValidationEvidenceCommitResult {
  bool ok = false;
  Sha256Digest evidence_digest;
  std::uint64_t attempt = 0;
  bool active = false;
  std::filesystem::path evidence_path;
};

// Loads and verifies every referenced immutable attempt. Missing state is a
// successful read with status Missing; corruption, a stale key, a noncanonical
// object, or an inconsistent active verdict is a diagnostic failure.
[[nodiscard]] bool load_validation_evidence_state(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key,
    ValidationEvidenceState &state,
    DiagnosticSink &diagnostics);

// Appends one attempt under an interprocess store lock. The immutable evidence
// object is published before the small state file; a crash can therefore leave
// only an unreferenced valid object, never state that names absent bytes. A pass
// activates itself and every failure clears active evidence for this exact key.
[[nodiscard]] ValidationEvidenceCommitResult commit_validation_evidence(
    const std::filesystem::path &workspace_directory,
    ValidationEvidence evidence,
    DiagnosticSink &diagnostics);

} // namespace draft
