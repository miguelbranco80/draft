// Exact locked-build checks for qualitative judgment evidence.

#include "judgment/verification.h"

#include "judgment/evidence_store.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] const AgentObligation *find_judgment(
    const CompileWorkspaceResult &compiled,
    std::string_view site_identity) {
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation :
         package->obligations.obligations) {
      if (obligation.kind == AgentConstructKind::Judgment &&
          obligation.site_identity == site_identity) {
        return &obligation;
      }
    }
  }
  return nullptr;
}

[[nodiscard]] std::size_t judgment_count(
    const CompileWorkspaceResult &compiled) {
  std::size_t result = 0;
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation :
         package->obligations.obligations) {
      if (obligation.kind == AgentConstructKind::Judgment) ++result;
    }
  }
  return result;
}

[[nodiscard]] bool claim_matches(
    const JudgmentClaimIdentity &claim,
    const AgentObligation &obligation) {
  return claim.site_identity == obligation.site_identity &&
      claim.root_identity == obligation.root_identity &&
      claim.root_relative_path == obligation.root_relative_path &&
      claim.source_relative_path == obligation.source_relative_path &&
      claim.anchor_name == obligation.anchor_name &&
      claim.occurrence == obligation.occurrence &&
      claim.input_digest == obligation.input_digest &&
      claim.record_digest == obligation.record_digest;
}

void verification_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(SourceRange::invalid(), std::move(message));
}

} // namespace

bool verify_active_judgment_evidence(
    const CompileWorkspaceResult &compiled,
    const std::filesystem::path &workspace_directory,
    std::vector<Sha256Digest> &active_digests,
    DiagnosticSink &diagnostics,
    std::string_view policy_identity) {
  active_digests.clear();
  if (!compiled.ok || !compiled.resolved_program_digest.has_value() ||
      workspace_directory.empty() || policy_identity.empty()) {
    verification_error(
        diagnostics,
        "judgment evidence verification requires a complete resolved program, "
        "workspace, and policy identity");
    return false;
  }

  const std::size_t required_count = judgment_count(compiled);
  if (!compiled.resolution_manifest.has_value()) {
    if (required_count == 0) return true;
    verification_error(
        diagnostics,
        "locked build requires judgment evidence but has no selecting "
        "resolution manifest");
    return false;
  }

  const ResolutionManifest &manifest = *compiled.resolution_manifest;
  std::vector<std::string> matched_sites;
  for (const ResolutionEvidencePin &pin : manifest.evidence) {
    if (pin.kind != "judgment") continue;

    JudgmentEvidenceState state;
    if (!load_judgment_evidence_state(
            workspace_directory, pin.key, state, diagnostics)) {
      return false;
    }
    if (state.status == JudgmentEvidenceStateStatus::Missing) {
      verification_error(
          diagnostics,
          "locked build requires missing judgment evidence for key " +
              pin.key.hex());
      return false;
    }
    if (state.status == JudgmentEvidenceStateStatus::Revoked) {
      verification_error(
          diagnostics,
          "locked build requires revoked judgment evidence for key " +
              pin.key.hex());
      return false;
    }
    if (!state.active_digest.has_value() ||
        !state.active_evidence.has_value() ||
        !state.active_evidence->passed) {
      verification_error(
          diagnostics,
          "locked build found incomplete active judgment evidence");
      return false;
    }
    if (*state.active_digest != pin.content_digest) {
      verification_error(
          diagnostics,
          "active judgment evidence differs from the attempt selected by the "
          "resolution manifest; rerun judgment");
      return false;
    }

    const JudgmentEvidence &evidence = *state.active_evidence;
    const AgentObligation *obligation =
        find_judgment(compiled, evidence.claim.site_identity);
    if (obligation == nullptr ||
        !claim_matches(evidence.claim, *obligation)) {
      verification_error(
          diagnostics,
          "manifest-selected judgment evidence does not match a current typed "
          "judgment obligation");
      return false;
    }
    for (const std::string &matched : matched_sites) {
      if (matched == evidence.claim.site_identity) {
        verification_error(
            diagnostics,
            "resolution manifest selects duplicate evidence for one judgment "
            "site");
        return false;
      }
    }
    matched_sites.push_back(evidence.claim.site_identity);

    if (pin.root_identity != obligation->root_identity ||
        pin.root_relative_path != obligation->root_relative_path ||
        evidence.resolved_program != *compiled.resolved_program_digest ||
        evidence.target_identity != obligation->target.identity ||
        evidence.target_identity != manifest.target_identity ||
        evidence.compiler_identity != compiled.compiler_content_identity ||
        evidence.policy_identity != policy_identity) {
      verification_error(
          diagnostics,
          "manifest-selected judgment evidence has stale program, target, "
          "compiler, package, or policy identity");
      return false;
    }
    if (!evidence.artifacts.empty() || evidence.validators.size() != 1 ||
        evidence.validators.front().validator_identity != "validator-0" ||
        !evidence.validators.front().passed) {
      verification_error(
          diagnostics,
          "manifest-selected judgment evidence does not match the active "
          "one-validator/no-artifact policy");
      return false;
    }
    active_digests.push_back(*state.active_digest);
  }

  if (matched_sites.size() != required_count) {
    verification_error(
        diagnostics,
        "locked build requires one manifest-selected active evidence object "
        "for every current judgment");
    return false;
  }
  return true;
}

} // namespace draft
