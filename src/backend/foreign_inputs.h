// Exact physical mappings for logical foreign link providers.
//
// This backend boundary parses the public `name=kind:path` spelling into owned
// process-facing records, then validates the filesystem objects immediately
// before native publication. Callers own input vectors; returned verified rows
// own canonical paths for one build. The module does not inspect symbols,
// traverse dynamic dependencies, hash artifacts, or influence Draft semantic
// identity. Workspace/driver/IDE policy may call the parser, while only the
// native toolchain consumes verified rows.

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

// ForeignProviderInput is one unverified mapping. provider is semantic source
// spelling; path may be lexical until the parser makes it absolute. Equality is
// incidental command configuration and the value is never serialized.
struct ForeignProviderInput {
  std::string provider;
  ForeignArtifactKind kind = ForeignArtifactKind::Object;
  std::filesystem::path path;
};

// VerifiedForeignProviderInput names one real non-symlink regular file whose
// kind and provider uniqueness were checked for the synchronous native build.
struct VerifiedForeignProviderInput {
  std::string provider;
  ForeignArtifactKind kind = ForeignArtifactKind::Object;
  std::filesystem::path path;
};

// Parses the public `name=kind:path` process spelling and makes path absolute
// without inspecting the filesystem. CLI and embedding clients share this
// conversion so provider syntax cannot drift across compiler front ends.
[[nodiscard]] bool parse_foreign_provider_input(std::string_view spelling,
                                                ForeignProviderInput &input,
                                                std::string &reason);

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
