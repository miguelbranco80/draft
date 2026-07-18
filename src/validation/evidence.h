// Canonical validation evidence identities, attempts, and native observations.

#pragma once

#include "base/sha256.h"
#include "source/diagnostic.h"
#include "validation/discovery.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

struct ValidationObservation {
  PackageIdentity package;
  std::string procedure;
  std::uint64_t checks = 0;
  std::uint64_t library_samples = 0;
  std::uint64_t failures = 0;
  std::int64_t maximum_time_ns = 0;
  std::vector<std::int64_t> durations_ns;

  bool operator==(const ValidationObservation &) const = default;
};

// One immutable evidence object describes one actual execution attempt. Static
// selection and environment fields form key; attempt and every outcome field
// are deliberately excluded from that key so a later failure revokes the exact
// same claim rather than creating a different claim that can coexist with it.
struct ValidationEvidence {
  std::string format = "draft-validation-evidence-v1";
  Sha256Digest key;
  std::uint64_t attempt = 0;
  Sha256Digest resolved_program;
  ValidationKind kind = ValidationKind::None;
  std::string target_identity;
  std::string compiler_identity;
  std::string toolchain_identity;
  std::string environment_identity;
  std::string runner_identity;
  std::string policy_identity;
  std::string artifact_identity;
  std::uint64_t warmup_runs = 0;
  std::uint64_t sample_runs = 0;
  std::vector<ValidationEntry> entries;
  std::vector<ValidationObservation> observations;
  bool observations_complete = false;
  bool passed = false;
  int exit_code = 0;
  int signal = 0;
};

[[nodiscard]] Sha256Digest hash_validation_evidence_key(
    const ValidationEvidence &evidence);

[[nodiscard]] std::string serialize_validation_evidence(
    const ValidationEvidence &evidence);

[[nodiscard]] bool parse_validation_evidence(
    std::string_view json,
    ValidationEvidence &evidence,
    DiagnosticSink &diagnostics);

// Decodes one complete fd-3 report produced by one harness execution. The
// fixed AArch64 macOS target is little-endian; report widths come from checked
// core nominal layouts rather than from host C++ structure layout.
[[nodiscard]] bool decode_validation_report(
    std::span<const std::uint8_t> report,
    const std::vector<ValidationEntry> &entries,
    std::vector<ValidationObservation> &observations,
    DiagnosticSink &diagnostics);

} // namespace draft
