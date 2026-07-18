// Exact, artifact-bound denial summaries for logical foreign providers.

#pragma once

#include "backend/foreign_inputs.h"
#include "sema/foreign_summary.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace draft {

struct ForeignProviderSummaryInput {
  std::string provider;
  std::filesystem::path path;
};

// Parses and verifies every summary, including the artifact digest declared in
// its bytes, and appends one ProviderSummary content pin per input. The summary
// set may be smaller than the artifact set: a provider without a summary keeps
// the conservative unknown edge. A summary without an artifact is rejected.
[[nodiscard]] bool pin_foreign_provider_summary_inputs(
    std::span<const ForeignProviderSummaryInput> inputs,
    std::span<const ForeignProviderInput> artifacts,
    std::vector<ExternalInputPin> &pins,
    std::vector<ForeignProviderAudit> &audits,
    DiagnosticSink &diagnostics);

// Requires the configured files to equal the complete ProviderSummary row set
// in a loaded manifest. Artifact files are re-hashed as part of the same call,
// so a matching summary can never bless different provider code.
[[nodiscard]] bool verify_foreign_provider_summary_inputs(
    std::span<const ForeignProviderSummaryInput> inputs,
    std::span<const ForeignProviderInput> artifacts,
    std::span<const ExternalInputPin> manifest_pins,
    std::vector<ForeignProviderAudit> &audits,
    DiagnosticSink &diagnostics);

} // namespace draft
