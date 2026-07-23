// Filesystem validation for explicit runtime-asset roots.
//
// The driver and embedding API provide physical deployment roots. This backend
// phase checks their shape, canonicalizes paths, and rejects duplicate logical
// names. It does not traverse or hash asset contents: assets are not Draft
// source, and the compiler neither copies nor controls the environment in which
// the finished program will load them.
//
// The implementation depends only on filesystem and diagnostic primitives. It
// does not depend on manifests, semantic packages, LLVM, or the native linker
// because runtime assets have no implicit source-language or link meaning.

#include "backend/runtime_assets.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

// Checks the physical root invariant owned by this module. canonical is
// assigned only after the root itself is accepted.
[[nodiscard]] bool inspect_input(
    const RuntimeAssetInput &input,
    std::filesystem::path &canonical,
    DiagnosticSink &diagnostics) {
  if (input.name.empty()) {
    diagnostics.error(
        SourceRange::invalid(), "runtime asset name must not be empty");
    return false;
  }
  if (input.path.empty() || !input.path.is_absolute()) {
    diagnostics.error(
        SourceRange::invalid(),
        "runtime asset '" + input.name + "' path must be absolute");
    return false;
  }

  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(input.path, error);
  if (error || std::filesystem::is_symlink(status) ||
      (!std::filesystem::is_regular_file(status) &&
       !std::filesystem::is_directory(status))) {
    diagnostics.error(
        SourceRange::invalid(),
        "runtime asset '" + input.name +
            "' must be a real regular file or directory, not a symlink");
    return false;
  }

  canonical = std::filesystem::canonical(input.path, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize runtime asset '" + input.name + "': " +
            error.message());
    return false;
  }
  return true;
}

} // namespace

bool inspect_runtime_asset_inputs(
    std::span<const RuntimeAssetInput> inputs,
    std::vector<VerifiedRuntimeAssetInput> &verified,
    DiagnosticSink &diagnostics) {
  std::vector<VerifiedRuntimeAssetInput> result;
  result.reserve(inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    // Duplicate logical names would make deployment policy ambiguous even
    // though assets are no longer part of the resolved-source identity.
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (inputs[previous].name == inputs[index].name) {
        diagnostics.error(
            SourceRange::invalid(),
            "runtime asset '" + inputs[index].name +
                "' is mapped more than once");
        return false;
      }
    }

    std::filesystem::path canonical;
    if (!inspect_input(inputs[index], canonical, diagnostics)) return false;
    result.push_back({inputs[index].name, std::move(canonical)});
  }
  verified = std::move(result);
  return true;
}

} // namespace draft
