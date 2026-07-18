// Exact physical mappings for logical foreign link providers.

#pragma once

#include "elaborator/resolution.h"
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

// Appends one canonical content pin per provider. Paths are never serialized;
// each selected root must be an absolute, non-symlink regular file. Duplicate
// logical providers are rejected because one build cannot silently choose link
// order between alternative implementations of the same source identity.
[[nodiscard]] bool pin_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics);

// Validates and canonicalizes a development build's explicit paths without a
// manifest comparison. Locked callers use verify_foreign_provider_inputs.
[[nodiscard]] bool inspect_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics);

// Re-hashes every configured artifact and requires exact equality with all
// object/archive/shared-library rows in the manifest. Other manifest roles are
// left to their owning verifier.
[[nodiscard]] bool verify_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::span<const ExternalInputPin> manifest_pins,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics);

} // namespace draft
