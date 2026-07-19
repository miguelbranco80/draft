// Canonical evidence for one agent-evaluated judgment.
//
// Judgment evidence is deliberately separate from native test and benchmark
// evidence. Tests report fixed binary counters produced by a target process;
// judgments report qualitative verdicts and rationales produced by explicitly
// identified validators. Forcing both through one record would make either the
// native report contract or the judgment contract vague.
//
// One object covers exactly one surface `judge` site. The evidence key contains
// every static input and validator identity, but excludes the attempt ordinal,
// verdicts, and rationales. A later failing invocation therefore addresses and
// revokes the same claim instead of manufacturing a second compatible key.

#pragma once

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// Stable compiler-owned identity and typed context for one judgment. The two
// digests have distinct purposes: record_digest covers the authored claim and
// attachments, while input_digest covers the complete canonical semantic
// context supplied at the program point.
struct JudgmentClaimIdentity {
  std::string site_identity;
  std::string root_identity;
  std::string root_relative_path;
  std::string source_relative_path;
  std::string anchor_name;
  std::uint64_t occurrence = 0;
  Sha256Digest input_digest;
  Sha256Digest record_digest;

  bool operator==(const JudgmentClaimIdentity &) const = default;
};

// Requested target artifacts are content identities, never physical paths.
// `kind` is a versioned policy spelling such as `llvm-ir`, `object`, or
// `disassembly`. The first provider can request none; the format is already
// complete when later validation profiles request several.
struct JudgmentArtifactIdentity {
  std::string kind;
  Sha256Digest content_digest;

  bool operator==(const JudgmentArtifactIdentity &) const = default;
};

// validator_identity distinguishes independent invocations selected by the
// active policy. Provider, model, and configuration remain separate so tooling
// can audit judge/synthesizer independence without parsing an opaque string.
// Rationale is required for both pass and fail: a qualitative verdict without
// inspectable reasoning is not useful evidence.
struct JudgmentValidatorResult {
  std::string validator_identity;
  std::string provider_identity;
  std::string model_identity;
  std::string configuration_identity;
  bool passed = false;
  std::string rationale;

  bool operator==(const JudgmentValidatorResult &) const = default;
};

struct JudgmentEvidence {
  std::string format = "draft-judgment-evidence-v1";
  Sha256Digest key;
  std::uint64_t attempt = 0;
  Sha256Digest resolved_program;
  std::string target_identity;
  std::string compiler_identity;
  std::string policy_identity;
  JudgmentClaimIdentity claim;
  std::vector<JudgmentArtifactIdentity> artifacts;
  std::vector<JudgmentValidatorResult> validators;
  bool passed = false;
};

// One command result points to the immutable evidence object just committed.
// The active/revoked state and full claim identity remain in the judgment
// evidence store; source resolution manifests deliberately do not select or
// duplicate this row.
struct JudgmentEvidenceReference {
  Sha256Digest key;
  Sha256Digest content_digest;
};

// Hashes only the immutable claim, environment, artifact, policy, and validator
// identities. Validator order is policy order and therefore semantic. Artifact
// rows are treated as a map keyed by kind and are sorted internally.
[[nodiscard]] Sha256Digest hash_judgment_evidence_key(
    const JudgmentEvidence &evidence);

// Returns canonical UTF-8 JSON ending in one newline. Artifact rows are sorted
// by kind without mutating the caller. Validator rows retain policy order.
[[nodiscard]] std::string serialize_judgment_evidence(
    const JudgmentEvidence &evidence);

// Parses the exact v1 schema and rejects missing/unknown/reordered fields,
// malformed UTF-8, duplicate identities, path traversal, inconsistent aggregate
// verdicts, and a key that does not match the static inputs.
[[nodiscard]] bool parse_judgment_evidence(
    std::string_view json,
    JudgmentEvidence &evidence,
    DiagnosticSink &diagnostics);

} // namespace draft
