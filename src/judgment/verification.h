// Offline verification of manifest-selected qualitative judgment evidence.

#pragma once

#include "compile/compiler.h"
#include "judgment/command.h"
#include "judgment/evidence.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// Exact policy shape expected by an offline build. Artifact rows include their
// content digests because a kind alone cannot prove that the validator inspected
// the artifact belonging to this build. Validator order is semantic and must
// match the order used by judgment execution.
struct JudgmentVerificationPolicy {
  std::string identity = std::string(kDefaultJudgmentPolicyIdentity);
  std::vector<std::string> validator_identities = {"validator-0"};
  std::vector<JudgmentArtifactIdentity> artifacts;
};

// Checks one active, manifest-selected passing object for every judgment in the
// already compiled resolved program. It performs no provider call, process
// launch, compilation, or store write. The default argument is the exact first
// all-sites/one-validator/no-artifact profile used by `draftc judge`; embedding
// callers can provide any explicit all-pass validator/artifact policy.
[[nodiscard]] bool verify_active_judgment_evidence(
    const CompileWorkspaceResult &compiled,
    const std::filesystem::path &workspace_directory,
    std::vector<Sha256Digest> &active_digests,
    DiagnosticSink &diagnostics,
    const JudgmentVerificationPolicy &policy = JudgmentVerificationPolicy{});

} // namespace draft
