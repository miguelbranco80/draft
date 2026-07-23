// Explicit denial summaries for logical foreign providers.

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

// Parses semantic denial summaries for the current invocation. Each summary
// must name a configured provider, but neither file is content-hashed or added
// to the resolution manifest. A provider without a summary keeps the
// conservative unknown edge; a summary without a provider mapping is rejected.
[[nodiscard]] bool load_foreign_provider_summaries(
    std::span<const ForeignProviderSummaryInput> inputs,
    std::span<const ForeignProviderInput> artifacts,
    std::vector<ForeignProviderAudit> &audits,
    DiagnosticSink &diagnostics);

} // namespace draft
