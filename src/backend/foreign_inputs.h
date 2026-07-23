// Exact physical mappings for logical foreign link providers.

#pragma once

#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

enum class ForeignArtifactKind {
  Object,
  Archive,
  SharedLibrary,
};

struct ForeignProviderInput {
  std::string provider;
  ForeignArtifactKind kind = ForeignArtifactKind::Object;
  std::filesystem::path path;
};

struct VerifiedForeignProviderInput {
  std::string provider;
  ForeignArtifactKind kind = ForeignArtifactKind::Object;
  std::filesystem::path path;
};

[[nodiscard]] std::string_view foreign_artifact_kind_name(
    ForeignArtifactKind kind);

// Validates and canonicalizes explicit linker inputs. The compiler checks
// shape, absolute location, and duplicate provider identities, then passes the
// path to the linker. It does not hash the file or pretend that one artifact's
// bytes describe its transitive dynamic-library environment.
[[nodiscard]] bool inspect_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics);

} // namespace draft
