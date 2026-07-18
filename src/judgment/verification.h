// Offline verification of manifest-selected qualitative judgment evidence.

#pragma once

#include "compile/compiler.h"
#include "judgment/command.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace draft {

// Checks one active, manifest-selected passing object for every judgment in the
// already compiled resolved program. It performs no provider call, process
// launch, compilation, or store write. The default policy is the exact first
// all-sites/one-validator/no-artifact profile used by `draftc judge`.
[[nodiscard]] bool verify_active_judgment_evidence(
    const CompileWorkspaceResult &compiled,
    const std::filesystem::path &workspace_directory,
    std::vector<Sha256Digest> &active_digests,
    DiagnosticSink &diagnostics,
    std::string_view policy_identity = kDefaultJudgmentPolicyIdentity);

} // namespace draft
