// Filesystem validation for explicit foreign-provider linker inputs.
//
// This module validates the small operational boundary that the compiler can
// honestly own: every mapping has one unique logical provider, an absolute
// path, and a real regular file at command time. It deliberately does not hash
// artifacts. A shared library's bytes do not identify its transitive libraries,
// loader, SDK, or runtime environment, so putting that digest in Draft source
// identity would be both expensive and misleading. The linker remains the
// authority for consuming these paths and resolving their symbols.

#include "backend/foreign_inputs.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool inspect_input(
    const ForeignProviderInput &input,
    std::filesystem::path &canonical,
    DiagnosticSink &diagnostics) {
  if (input.provider.empty()) {
    diagnostics.error(
        SourceRange::invalid(), "foreign provider identity must not be empty");
    return false;
  }
  if (input.path.empty() || !input.path.is_absolute()) {
    diagnostics.error(
        SourceRange::invalid(),
        "foreign provider '" + input.provider +
            "' artifact path must be absolute");
    return false;
  }
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(input.path, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    diagnostics.error(
        SourceRange::invalid(),
        "foreign provider '" + input.provider +
            "' artifact must be a real regular file, not a symlink");
    return false;
  }
  canonical = std::filesystem::canonical(input.path, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize foreign provider '" + input.provider +
            "' artifact: " + error.message());
    return false;
  }
  return true;
}

} // namespace

std::string_view foreign_artifact_kind_name(ForeignArtifactKind kind) {
  switch (kind) {
  case ForeignArtifactKind::Object: return "object";
  case ForeignArtifactKind::Archive: return "archive";
  case ForeignArtifactKind::SharedLibrary: return "shared-library";
  }
  return "unknown";
}

bool inspect_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics) {
  std::vector<VerifiedForeignProviderInput> result;
  result.reserve(inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (inputs[previous].provider == inputs[index].provider) {
        diagnostics.error(
            SourceRange::invalid(),
            "foreign provider '" + inputs[index].provider +
                "' is mapped more than once");
        return false;
      }
    }
    std::filesystem::path canonical;
    if (!inspect_input(inputs[index], canonical, diagnostics)) return false;
    result.push_back(
        {inputs[index].provider, inputs[index].kind, std::move(canonical)});
  }
  verified = std::move(result);
  return true;
}

} // namespace draft
