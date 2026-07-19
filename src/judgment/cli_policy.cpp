// CLI judgment-policy input handling. See cli_policy.h for the boundary.

#include "judgment/cli_policy.h"

#include "base/sha256.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <utility>

namespace draft {
namespace {

constexpr std::uintmax_t kMaximumJudgmentArtifactBytes =
    64U * 1024U * 1024U;

} // namespace

bool parse_judgment_validator(
    std::string_view spelling,
    std::string &identity,
    std::string &model,
    std::string &reason) {
  const std::size_t colon = spelling.find(':');
  if (colon == 0 || colon == std::string_view::npos ||
      colon + 1 >= spelling.size()) {
    reason = "judgment validator must be identity:model";
    return false;
  }
  identity = std::string(spelling.substr(0, colon));
  model = std::string(spelling.substr(colon + 1));
  return true;
}

bool parse_judgment_artifact_path(
    std::string_view spelling,
    JudgmentArtifactPath &input,
    std::string &reason) {
  const std::size_t colon = spelling.find(':');
  if (colon == 0 || colon == std::string_view::npos ||
      colon + 1 >= spelling.size()) {
    reason = "judgment artifact must be kind:path";
    return false;
  }
  input.kind = std::string(spelling.substr(0, colon));
  std::error_code error;
  input.path = std::filesystem::absolute(
      std::filesystem::path(spelling.substr(colon + 1)), error).lexically_normal();
  if (error) {
    reason = "cannot make judgment artifact path absolute: " + error.message();
    return false;
  }
  return true;
}

bool read_judgment_artifacts(
    const std::vector<JudgmentArtifactPath> &inputs,
    std::vector<JudgmentRequestArtifact> &artifacts,
    std::string &reason) {
  artifacts.clear();
  artifacts.reserve(inputs.size());
  for (const JudgmentArtifactPath &input : inputs) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::status(input.path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
      reason = "judgment artifact is not a readable regular file: " +
          input.path.string();
      return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(input.path, error);
    if (error || size > kMaximumJudgmentArtifactBytes) {
      reason = "judgment artifact is unreadable or exceeds the 64 MiB limit: " +
          input.path.string();
      return false;
    }
    std::ifstream stream(input.path, std::ios::binary);
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size != 0) {
      stream.read(contents.data(), static_cast<std::streamsize>(size));
    }
    if (!stream || stream.peek() != std::ifstream::traits_type::eof()) {
      reason = "cannot read exact judgment artifact bytes: " +
          input.path.string();
      return false;
    }
    JudgmentRequestArtifact artifact;
    artifact.kind = input.kind;
    artifact.digest = sha256(contents);
    artifact.contents = std::move(contents);
    artifacts.push_back(std::move(artifact));
  }
  return true;
}

bool configure_codex_judgment_policy(
    const std::optional<CodexCliProviderOptions> &default_codex,
    const std::vector<NamedCodexJudgmentValidator> &configured,
    const std::vector<JudgmentRequestArtifact> &artifacts,
    std::vector<CodexCliProviderState> &states,
    JudgmentCommandOptions &options,
    DiagnosticSink &diagnostics) {
  const std::size_t validator_count =
      configured.empty() ? (default_codex.has_value() ? 1U : 0U)
                         : configured.size();
  states.clear();
  states.resize(validator_count);
  options.validators.clear();
  options.artifacts = artifacts;
  for (std::size_t index = 0; index < validator_count; ++index) {
    const std::string identity = configured.empty()
        ? "validator-0"
        : configured[index].identity;
    const CodexCliProviderOptions &codex = configured.empty()
        ? *default_codex
        : configured[index].codex;
    JudgmentProvider provider = configure_codex_cli_judgment_provider(
        codex, states[index], diagnostics);
    if (provider.judge == nullptr) return false;
    options.validators.push_back({identity, std::move(provider)});
  }

  if (validator_count != 0) {
    std::vector<std::string> identities;
    identities.reserve(options.validators.size());
    for (const JudgmentValidatorConfiguration &validator :
         options.validators) {
      identities.push_back(validator.identity);
    }
    std::vector<JudgmentArtifactIdentity> artifact_identities;
    artifact_identities.reserve(options.artifacts.size());
    for (const JudgmentRequestArtifact &artifact : options.artifacts) {
      artifact_identities.push_back({artifact.kind, artifact.digest});
    }
    options.policy_identity = judgment_policy_identity(
        identities, artifact_identities);
  }
  return true;
}

} // namespace draft
