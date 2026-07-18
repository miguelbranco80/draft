// Deterministic judgment selection, provider execution, and evidence recording.

#pragma once

#include "compile/compiler.h"
#include "elaborator/resolution.h"
#include "judgment/provider.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

inline constexpr std::string_view kDefaultJudgmentPolicyIdentity =
    "draft-judgment-policy-v1:validators=1:aggregate=all-pass:artifacts=none";

struct JudgmentCommandOptions {
  std::filesystem::path workspace_directory;
  TargetProfile target;
  JudgmentProvider provider;
  // v1 invokes exactly one validator per site and passes only when every site
  // passes. The explicit identity prevents a later count/aggregation change
  // from reusing evidence produced under this first policy.
  std::string policy_identity = std::string(kDefaultJudgmentPolicyIdentity);
  std::vector<JudgmentRequestArtifact> artifacts;
};

struct JudgmentCommandResult {
  // completed means every selected provider invocation produced a typed verdict
  // and every attempt reached durable storage. It may still have passed=false.
  bool completed = false;
  bool passed = false;
  std::size_t selected_judgments = 0;
  // Rows are safe to select in a resolution manifest only when completed and
  // passed are both true. Failed commands retain their rows solely to make
  // testing/auditing the durable attempts possible.
  std::vector<ResolutionEvidencePin> evidence;
};

[[nodiscard]] JudgmentCommandResult execute_judgment_command(
    const CompileWorkspaceResult &compiled,
    JudgmentCommandOptions options,
    DiagnosticSink &diagnostics);

} // namespace draft
