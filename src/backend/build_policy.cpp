// See build_policy.h for the shared ownership and layering contract. This file
// deliberately contains one linear conversion rather than separate CLI and IDE
// adapters: BuildDefaults is already the common, precedence-resolved input, and
// every native spelling has one backend-owned parser.

#include "backend/build_policy.h"

#include <string_view>
#include <utility>

namespace draft {
namespace {

// Appends one already-selected native-input layer. Keeping the three physical
// parsers in this single operation guarantees that unconditional and
// target-qualified rows receive identical workspace-relative path treatment.
[[nodiscard]] bool append_native_inputs(
    const std::filesystem::path &workspace_directory,
    const std::vector<std::string> &providers,
    const std::vector<std::string> &provider_summaries,
    const std::vector<std::string> &runtime_assets,
    ResolvedBuildPolicy &resolved, std::string &reason) {
  for (const std::string &spelling : providers) {
    ForeignProviderInput input;
    if (!parse_foreign_provider_input(
            resolve_manifest_mapping_path(workspace_directory, spelling), input,
            reason)) {
      return false;
    }
    resolved.foreign_providers.push_back(std::move(input));
  }
  for (const std::string &spelling : provider_summaries) {
    ForeignProviderSummaryInput input;
    if (!parse_foreign_provider_summary_input(
            resolve_manifest_mapping_path(workspace_directory, spelling), input,
            reason)) {
      return false;
    }
    resolved.provider_summaries.push_back(std::move(input));
  }
  for (const std::string &spelling : runtime_assets) {
    RuntimeAssetInput input;
    if (!parse_runtime_asset_input(
            resolve_manifest_mapping_path(workspace_directory, spelling), input,
            reason)) {
      return false;
    }
    resolved.runtime_assets.push_back(std::move(input));
  }
  return true;
}

} // namespace

bool resolve_build_policy(const std::filesystem::path &workspace_directory,
                          const BuildDefaults &defaults,
                          const TargetProfile &fallback_target,
                          ResolvedBuildPolicy &result, std::string &reason) {
  ResolvedBuildPolicy resolved;
  resolved.target = fallback_target;
  reason.clear();

  if (defaults.target.has_value()) {
    if (!select_builtin_target_profile(*defaults.target, resolved.target,
                                       reason)) {
      return false;
    }
    if (!validate_target_profile(resolved.target, reason)) {
      reason = "invalid built-in target profile: " + reason;
      return false;
    }
  }
  if (defaults.output.has_value()) {
    const std::filesystem::path path(*defaults.output);
    resolved.output_path =
        path.is_absolute() ? path : workspace_directory / path;
  }
  if (defaults.artifact_kind.has_value()) {
    const std::optional<NativeArtifactKind> parsed =
        parse_native_artifact_kind(*defaults.artifact_kind);
    if (!parsed.has_value()) {
      reason =
          "invalid manifest artifact kind '" + *defaults.artifact_kind + "'";
      return false;
    }
    resolved.artifact_kind = *parsed;
  }
  if (defaults.optimization.has_value()) {
    const std::string_view spelling = *defaults.optimization;
    if (spelling == "O0") {
      resolved.optimization = NativeOptimizationLevel::O0;
    } else if (spelling == "O2") {
      resolved.optimization = NativeOptimizationLevel::O2;
    } else {
      reason = "invalid manifest optimization '" + std::string(spelling) +
               "'; expected O0 or O2";
      return false;
    }
  }
  if (defaults.debug_symbols.has_value())
    resolved.emit_debug_symbols = *defaults.debug_symbols;
  if (defaults.assertions.has_value())
    resolved.runtime_assertions = *defaults.assertions;

  // Mapping path resolution precedes the public parsers because those parsers
  // make their path suffix absolute against the process directory. Resolving
  // here preserves the manifest's workspace-relative contract independently of
  // the directory from which draftc or an embedding was launched.
  if (!append_native_inputs(workspace_directory, defaults.providers,
                            defaults.provider_summaries,
                            defaults.runtime_assets, resolved, reason)) {
    return false;
  }
  for (const TargetBuildInputs &inputs : defaults.target_inputs) {
    TargetProfile qualified_target;
    if (!select_builtin_target_profile(inputs.target, qualified_target,
                                       reason) ||
        !validate_target_profile(qualified_target, reason)) {
      reason = "invalid target-qualified build input '" + inputs.target +
          "': " + reason;
      return false;
    }
    if (qualified_target.facts.identity != resolved.target.facts.identity)
      continue;
    if (!append_native_inputs(workspace_directory, inputs.providers,
                              inputs.provider_summaries,
                              inputs.runtime_assets, resolved, reason)) {
      return false;
    }
  }

  result = std::move(resolved);
  return true;
}

} // namespace draft
