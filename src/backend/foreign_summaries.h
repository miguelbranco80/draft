// Explicit denial summaries for logical foreign providers.
//
// Summary files are semantic inputs to effect/denial closure for one command.
// This backend-facing module owns their public mapping parser and deterministic
// load/validation operation; it returns ordinary sema ForeignProviderAudit
// records and retains no file storage. The source analyzer consumes only those
// records and never performs I/O. Summary paths are operational configuration,
// not resolution-manifest or native-artifact content hashes.

#pragma once

#include "backend/foreign_inputs.h"
#include "sema/foreign_summary.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace draft {

// ForeignProviderSummaryInput pairs one logical source provider with one
// physical summary file. The path becomes absolute during public spelling
// parsing and is canonicalized when loaded.
struct ForeignProviderSummaryInput {
  std::string provider;
  std::filesystem::path path;
};

// Parses one public `provider:path` mapping and makes the path absolute without
// reading it. Actual summary parsing remains the separate operation below.
[[nodiscard]] bool
parse_foreign_provider_summary_input(std::string_view spelling,
                                     ForeignProviderSummaryInput &input,
                                     std::string &reason);

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
