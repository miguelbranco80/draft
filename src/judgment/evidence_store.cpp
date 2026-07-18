// Typed judgment adapter for the shared evidence attempt store.

#include "judgment/evidence_store.h"

#include "validation/evidence_store_core.h"

#include <string>
#include <string_view>
#include <utility>

namespace draft {
namespace {

struct JudgmentCodecState {
  JudgmentEvidence *pending = nullptr;
  JudgmentEvidence decoded;
};

[[nodiscard]] bool encode_judgment_attempt(
    void *opaque,
    const Sha256Digest &key,
    std::uint64_t attempt,
    std::string &bytes,
    bool &passed,
    DiagnosticSink &diagnostics) {
  auto *state = static_cast<JudgmentCodecState *>(opaque);
  if (state == nullptr || state->pending == nullptr) {
    diagnostics.error(
        SourceRange::invalid(), "judgment evidence encoder has no pending attempt");
    return false;
  }
  state->pending->key = key;
  state->pending->attempt = attempt;
  bytes = serialize_judgment_evidence(*state->pending);
  passed = state->pending->passed;
  return true;
}

[[nodiscard]] bool decode_judgment_attempt(
    void *opaque,
    std::string_view bytes,
    const Sha256Digest &key,
    std::uint64_t attempt,
    bool &passed,
    DiagnosticSink &diagnostics) {
  auto *state = static_cast<JudgmentCodecState *>(opaque);
  if (state == nullptr) {
    diagnostics.error(
        SourceRange::invalid(), "judgment evidence decoder has no state");
    return false;
  }
  JudgmentEvidence parsed;
  if (!parse_judgment_evidence(bytes, parsed, diagnostics)) return false;
  if (serialize_judgment_evidence(parsed) != bytes) {
    diagnostics.error(
        SourceRange::invalid(), "judgment evidence object is noncanonical");
    return false;
  }
  if (parsed.key != key || parsed.attempt != attempt) {
    diagnostics.error(
        SourceRange::invalid(),
        "judgment evidence history key or ordinal is inconsistent");
    return false;
  }
  passed = parsed.passed;
  state->decoded = std::move(parsed);
  return true;
}

[[nodiscard]] EvidenceAttemptCodec codec(JudgmentCodecState &state) {
  return {&state, encode_judgment_attempt, decode_judgment_attempt};
}

[[nodiscard]] JudgmentEvidenceStateStatus typed_status(
    EvidenceAttemptStateStatus status) {
  switch (status) {
  case EvidenceAttemptStateStatus::Missing:
    return JudgmentEvidenceStateStatus::Missing;
  case EvidenceAttemptStateStatus::Active:
    return JudgmentEvidenceStateStatus::Active;
  case EvidenceAttemptStateStatus::Revoked:
    return JudgmentEvidenceStateStatus::Revoked;
  }
  return JudgmentEvidenceStateStatus::Missing;
}

} // namespace

bool load_judgment_evidence_state(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key,
    JudgmentEvidenceState &state,
    DiagnosticSink &diagnostics) {
  JudgmentCodecState codec_state;
  EvidenceAttemptState raw;
  if (!load_evidence_attempt_state(
          workspace_directory,
          key,
          codec(codec_state),
          raw,
          diagnostics)) {
    return false;
  }
  JudgmentEvidenceState result;
  result.status = typed_status(raw.status);
  result.key = raw.key;
  result.attempts = std::move(raw.attempts);
  result.active_digest = raw.active_digest;
  if (raw.status == EvidenceAttemptStateStatus::Active) {
    result.active_evidence = std::move(codec_state.decoded);
  }
  state = std::move(result);
  return true;
}

JudgmentEvidenceCommitResult commit_judgment_evidence(
    const std::filesystem::path &workspace_directory,
    JudgmentEvidence evidence,
    DiagnosticSink &diagnostics) {
  evidence.key = hash_judgment_evidence_key(evidence);
  JudgmentCodecState codec_state;
  codec_state.pending = &evidence;
  const EvidenceAttemptCommitResult raw = commit_evidence_attempt(
      workspace_directory,
      evidence.key,
      codec(codec_state),
      diagnostics);
  JudgmentEvidenceCommitResult result;
  result.ok = raw.ok;
  result.key = raw.key;
  result.evidence_digest = raw.evidence_digest;
  result.attempt = raw.attempt;
  result.active = raw.active;
  result.evidence_path = raw.evidence_path;
  return result;
}

} // namespace draft
