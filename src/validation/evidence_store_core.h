// Shared crash-safe storage for typed evidence objects.
//
// This module knows nothing about tests, benchmarks, or judgments. A tiny codec
// validates the typed canonical bytes; the store owns only immutable object
// publication, append-only attempt history, and active/revoked state. Keeping
// those responsibilities separate lets every evidence kind share exactly one
// locking and durability implementation without weakening its schema.

#pragma once

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

enum class EvidenceAttemptStateStatus {
  Missing,
  Active,
  Revoked,
};

struct EvidenceAttemptState {
  EvidenceAttemptStateStatus status = EvidenceAttemptStateStatus::Missing;
  Sha256Digest key;
  std::vector<Sha256Digest> attempts;
  std::optional<Sha256Digest> active_digest;
};

struct EvidenceAttemptCommitResult {
  bool ok = false;
  Sha256Digest key;
  Sha256Digest evidence_digest;
  std::uint64_t attempt = 0;
  bool active = false;
  std::filesystem::path evidence_path;
};

// Encode receives the store-assigned one-based ordinal and exact evidence key.
// Decode must parse, validate, and canonical-round-trip the bytes, then prove
// that their embedded key and ordinal match the arguments. Both callbacks set
// passed from the typed object. State is borrowed for the duration of one
// synchronous load or commit.
using EncodeEvidenceAttempt = bool (*)(
    void *state,
    const Sha256Digest &key,
    std::uint64_t attempt,
    std::string &bytes,
    bool &passed,
    DiagnosticSink &diagnostics);

using DecodeEvidenceAttempt = bool (*)(
    void *state,
    std::string_view bytes,
    const Sha256Digest &key,
    std::uint64_t attempt,
    bool &passed,
    DiagnosticSink &diagnostics);

struct EvidenceAttemptCodec {
  void *state = nullptr;
  EncodeEvidenceAttempt encode = nullptr;
  DecodeEvidenceAttempt decode = nullptr;
};

// Missing state is a successful load. Every referenced immutable object is
// content-verified and decoded in attempt order before Active or Revoked is
// returned. The codec state therefore retains its own typed form of the last
// decoded object when the wrapper needs it.
[[nodiscard]] bool load_evidence_attempt_state(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key,
    EvidenceAttemptCodec codec,
    EvidenceAttemptState &state,
    DiagnosticSink &diagnostics);

// Appends exactly one codec-produced attempt under an interprocess lock. The
// immutable object is synchronized before the state file names it. A pass makes
// itself active; a failure clears active state for only this exact key.
[[nodiscard]] EvidenceAttemptCommitResult commit_evidence_attempt(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key,
    EvidenceAttemptCodec codec,
    DiagnosticSink &diagnostics);

} // namespace draft
