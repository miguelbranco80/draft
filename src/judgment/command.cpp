// Provider-neutral judgment command orchestration.

#include "judgment/command.h"

#include "judgment/evidence.h"
#include "judgment/evidence_store.h"
#include "judgment/selection.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace draft {
namespace {

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - 1 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(bytes);
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

[[nodiscard]] const AgentRecord *find_record(
    const CompiledPackage &package,
    SyntaxReference syntax) {
  for (const AgentRecord &record : package.metadata.records) {
    if (record.syntax == syntax) return &record;
  }
  return nullptr;
}

[[nodiscard]] bool provider_is_configured(
    const JudgmentProvider &provider,
    DiagnosticSink &diagnostics) {
  if (provider.judge == nullptr) {
    diagnostics.error(
        SourceRange::invalid(), "judgment provider is not configured");
    return false;
  }
  if (provider.provider_identity.empty() || provider.model_identity.empty() ||
      provider.configuration_identity.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "judgment provider identities must not be empty");
    return false;
  }
  return true;
}

[[nodiscard]] bool validators_are_configured(
    const std::vector<JudgmentValidatorConfiguration> &validators,
    DiagnosticSink &diagnostics) {
  if (validators.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "judgment policy requires at least one validator");
    return false;
  }
  for (std::size_t index = 0; index < validators.size(); ++index) {
    const JudgmentValidatorConfiguration &validator = validators[index];
    if (validator.identity.empty()) {
      diagnostics.error(
          SourceRange::invalid(),
          "judgment validator identities must not be empty");
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (validators[previous].identity == validator.identity) {
        diagnostics.error(
            SourceRange::invalid(),
            "judgment validator identities must be unique");
        return false;
      }
    }
    if (!provider_is_configured(validator.provider, diagnostics)) return false;
  }
  return true;
}

[[nodiscard]] bool valid_record(
    const AgentObligation &obligation,
    const AgentRecord &record,
    DiagnosticSink &diagnostics) {
  if (record.kind != AgentConstructKind::Judgment ||
      record.record_digest != obligation.record_digest) {
    diagnostics.error(
        SourceRange::invalid(),
        "judgment obligation and provider metadata record are inconsistent");
    return false;
  }
  if (record.files.size() != record.file_contents.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "judgment attachment identities and bytes are inconsistent");
    return false;
  }
  for (std::size_t index = 0; index < record.files.size(); ++index) {
    const AttachedFile &file = record.files[index];
    const std::string &contents = record.file_contents[index];
    if (file.size != static_cast<std::uint64_t>(contents.size()) ||
        file.digest != sha256(contents)) {
      diagnostics.error(
          SourceRange::invalid(),
          "judgment attachment bytes do not match their checked identity");
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_artifacts(
    std::vector<JudgmentRequestArtifact> &artifacts,
    DiagnosticSink &diagnostics) {
  std::sort(
      artifacts.begin(), artifacts.end(),
      [](const JudgmentRequestArtifact &left,
         const JudgmentRequestArtifact &right) {
        return left.kind < right.kind;
      });
  for (std::size_t index = 0; index < artifacts.size(); ++index) {
    const JudgmentRequestArtifact &artifact = artifacts[index];
    if (artifact.kind.empty() || artifact.digest != sha256(artifact.contents) ||
        (index != 0 && artifacts[index - 1].kind == artifact.kind)) {
      diagnostics.error(
          SourceRange::invalid(),
          "judgment artifacts require unique nonempty kinds and exact content identities");
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool build_request(
    const CompileWorkspaceResult &compiled,
    const AgentObligation &obligation,
    const AgentRecord &record,
    const JudgmentCommandOptions &options,
    JudgmentRequest &request,
    DiagnosticSink &diagnostics) {
  if (!valid_record(obligation, record, diagnostics)) return false;
  request.obligation = obligation;
  // Process-local arena routes never cross a provider boundary.
  request.obligation.syntax = {};
  request.resolved_program = *compiled.resolved_program_digest;
  request.compiler_identity = compiled.compiler_content_identity;
  request.policy_identity = options.policy_identity;
  request.claim = record.text;
  request.artifacts = options.artifacts;
  for (std::size_t index = 0; index < record.files.size(); ++index) {
    const AttachedFile &file = record.files[index];
    request.attachments.push_back({
        file.relative_path,
        file.size,
        file.digest,
        record.file_contents[index],
    });
  }
  return true;
}

[[nodiscard]] JudgmentEvidence make_evidence(
    const CompileWorkspaceResult &compiled,
    const JudgmentRequest &request) {
  JudgmentEvidence evidence;
  evidence.resolved_program = request.resolved_program;
  evidence.target_identity = request.obligation.target.identity;
  evidence.compiler_identity = compiled.compiler_content_identity;
  evidence.policy_identity = request.policy_identity;
  evidence.claim.site_identity = request.obligation.site_identity;
  evidence.claim.root_identity = request.obligation.root_identity;
  evidence.claim.root_relative_path = request.obligation.root_relative_path;
  evidence.claim.source_relative_path = request.obligation.source_relative_path;
  evidence.claim.anchor_name = request.obligation.anchor_name;
  evidence.claim.occurrence = request.obligation.occurrence;
  evidence.claim.input_digest = request.obligation.input_digest;
  evidence.claim.record_digest = request.obligation.record_digest;
  for (const JudgmentRequestArtifact &artifact : request.artifacts) {
    evidence.artifacts.push_back({artifact.kind, artifact.digest});
  }
  return evidence;
}

void append_validator_result(
    const JudgmentRequest &request,
    const JudgmentProvider &provider,
    JudgmentResponse response,
    JudgmentEvidence &evidence) {
  JudgmentValidatorResult validator;
  validator.validator_identity = request.validator_identity;
  validator.provider_identity = provider.provider_identity;
  validator.model_identity = provider.model_identity;
  validator.configuration_identity = provider.configuration_identity;
  validator.passed = response.passed;
  validator.rationale = std::move(response.rationale);
  evidence.validators.push_back(std::move(validator));
}

} // namespace

std::string judgment_policy_identity(
    std::span<const std::string> validator_identities,
    std::span<const JudgmentArtifactIdentity> artifacts) {
  if (validator_identities.size() == 1 &&
      validator_identities.front() == "validator-0" && artifacts.empty()) {
    return std::string(kDefaultJudgmentPolicyIdentity);
  }

  std::vector<JudgmentArtifactIdentity> ordered_artifacts(
      artifacts.begin(), artifacts.end());
  std::sort(
      ordered_artifacts.begin(), ordered_artifacts.end(),
      [](const JudgmentArtifactIdentity &left,
         const JudgmentArtifactIdentity &right) {
        return left.kind < right.kind;
      });
  Sha256 hash;
  hash_field(hash, "draft-judgment-all-pass-policy-v2");
  hash_u64(hash, static_cast<std::uint64_t>(validator_identities.size()));
  for (const std::string &identity : validator_identities) {
    hash_field(hash, identity);
  }
  hash_u64(hash, static_cast<std::uint64_t>(ordered_artifacts.size()));
  for (const JudgmentArtifactIdentity &artifact : ordered_artifacts) {
    hash_field(hash, artifact.kind);
  }
  return "draft-judgment-policy-v2:" + hash.finalize().hex();
}

JudgmentCommandResult execute_judgment_command(
    const CompileWorkspaceResult &compiled,
    JudgmentCommandOptions options,
    DiagnosticSink &diagnostics) {
  JudgmentCommandResult result;
  if (!compiled.ok || !compiled.resolved_program_digest.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "judgment execution requires a complete resolved program");
    return result;
  }
  if (options.workspace_directory.empty() || options.policy_identity.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "judgment workspace and policy identities must not be empty");
    return result;
  }
  if (!valid_artifacts(options.artifacts, diagnostics)) return result;
  JudgmentSelection selection;
  if (!select_judgment_sites(
          compiled, options.selectors, selection, diagnostics)) {
    return result;
  }
  result.selected_judgments = selection.sites.size();
  result.selected_site_identities.reserve(selection.sites.size());
  for (const JudgmentSiteDescription &site : selection.sites) {
    result.selected_site_identities.push_back(site.site_identity);
  }
  if (result.selected_judgments == 0) {
    result.completed = true;
    result.passed = true;
    return result;
  }
  if (!validators_are_configured(options.validators, diagnostics)) {
    return result;
  }

  bool aggregate_passed = true;
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation : package->obligations.obligations) {
      if (obligation.kind != AgentConstructKind::Judgment ||
          !judgment_selection_contains(
              selection, obligation.site_identity)) {
        continue;
      }
      if (obligation.target.identity != options.target.facts.identity) {
        diagnostics.error(
            SourceRange::invalid(),
            "judgment obligation target does not match command target");
        return result;
      }
      const AgentRecord *record = find_record(*package, obligation.syntax);
      if (record == nullptr) {
        diagnostics.error(
            SourceRange::invalid(),
            "judgment obligation has no provider metadata record");
        return result;
      }
      JudgmentRequest request;
      if (!build_request(
              compiled,
              obligation,
              *record,
              options,
              request,
              diagnostics)) {
        return result;
      }
      JudgmentEvidence evidence = make_evidence(compiled, request);
      bool site_passed = true;
      for (const JudgmentValidatorConfiguration &validator :
           options.validators) {
        request.validator_identity = validator.identity;
        JudgmentResponse response;
        const std::size_t before_provider = diagnostics.error_count();
        if (!validator.provider.judge(
                validator.provider.state,
                request,
                response,
                diagnostics)) {
          if (diagnostics.error_count() == before_provider) {
            diagnostics.error(
                SourceRange::invalid(),
                "judgment provider failed without a diagnostic");
          }
          return result;
        }
        if (response.rationale.empty()) {
          diagnostics.error(
              SourceRange::invalid(),
              "judgment provider returned an empty rationale");
          return result;
        }
        site_passed = site_passed && response.passed;
        append_validator_result(
            request,
            validator.provider,
            std::move(response),
            evidence);
      }

      evidence.passed = site_passed;
      evidence.key = hash_judgment_evidence_key(evidence);
      const JudgmentEvidenceCommitResult committed = commit_judgment_evidence(
          options.workspace_directory, evidence, diagnostics);
      if (!committed.ok) return result;
      result.evidence.push_back({
          committed.key,
          committed.evidence_digest,
      });
      aggregate_passed = aggregate_passed && committed.active;
    }
  }
  result.completed = true;
  result.passed = aggregate_passed;
  return result;
}

} // namespace draft
