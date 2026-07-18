// Deterministic judgment selection, provider execution, and evidence recording.

#pragma once

#include "compile/compiler.h"
#include "elaborator/resolution.h"
#include "judgment/evidence.h"
#include "judgment/provider.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

inline constexpr std::string_view kDefaultJudgmentPolicyIdentity =
    "draft-judgment-policy-v1:validators=1:aggregate=all-pass:artifacts=none";

// One independent validator selected by a judgment policy. The identity is
// compiler-owned policy data; provider/model/configuration identities describe
// the concrete implementation used for this invocation. Keeping those fields
// separate lets evidence prove both the requested validator slot and the agent
// that actually filled it.
struct JudgmentValidatorConfiguration {
  std::string identity;
  JudgmentProvider provider;
};

struct JudgmentCommandOptions {
  std::filesystem::path workspace_directory;
  TargetProfile target;
  // Validators run in policy order for every selected site. The current
  // provider-neutral aggregation rule is deliberately small and exact: every
  // configured validator must return a typed verdict and all must pass.
  std::vector<JudgmentValidatorConfiguration> validators;
  // The explicit identity prevents a different count, ordering, aggregation,
  // or requested-artifact policy from reusing evidence. The default public CLI
  // profile installs one `validator-0` row and requests no artifacts.
  std::string policy_identity = std::string(kDefaultJudgmentPolicyIdentity);
  std::vector<JudgmentRequestArtifact> artifacts;
  std::vector<std::string> selectors;
};

struct JudgmentCommandResult {
  // completed means every selected provider invocation produced a typed verdict
  // and every attempt reached durable storage. It may still have passed=false.
  bool completed = false;
  bool passed = false;
  std::size_t selected_judgments = 0;
  std::vector<std::string> selected_site_identities;
  // Rows are safe to select in a resolution manifest only when completed and
  // passed are both true. Failed commands retain their rows solely to make
  // testing/auditing the durable attempts possible.
  std::vector<ResolutionEvidencePin> evidence;
};

// Canonical identity for the public all-pass policy shape. Validator order is
// semantic; artifact kinds are a set and are sorted internally. Concrete
// provider/model identities and artifact content digests stay in the evidence
// key rather than changing the abstract policy shape.
[[nodiscard]] std::string judgment_policy_identity(
    std::span<const std::string> validator_identities,
    std::span<const JudgmentArtifactIdentity> artifacts);

[[nodiscard]] JudgmentCommandResult execute_judgment_command(
    const CompileWorkspaceResult &compiled,
    JudgmentCommandOptions options,
    DiagnosticSink &diagnostics);

} // namespace draft
