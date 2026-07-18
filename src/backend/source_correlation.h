// Canonical source correlation for native coverage and profile consumers.
//
// LLVM debug locations are sufficient for a debugger, but they are an awkward
// API for compiler-owned validation tooling. This small sidecar records the
// same operation coordinates in an implementation-owned format. A consumer can
// therefore attribute a sampled or counted native operation to both the
// resolved generated file and the authored synthesis site without parsing
// DWARF or depending on LLVM metadata numbering.

#pragma once

#include "source/source.h"
#include "workspace/workspace.h"

#include <cstdint>
#include <string>
#include <vector>

namespace draft {

// One row corresponds exactly to one LLVM debug marker emitted for a verified,
// source-addressable MIR instruction or terminator. procedure_ordinal plus
// operation ordinal is unique inside a package. The procedure spelling and
// operation kind are descriptive data because concrete specializations may
// share a source name.
struct SourceCorrelationEntry {
  PackageIdentity package;
  std::string procedure;
  std::uint64_t procedure_ordinal = 0;
  std::uint64_t ordinal = 0;
  std::string operation;
  std::string authored_file;
  LineColumn authored;
  std::string generated_file;
  LineColumn generated;
  // Empty for handwritten source. For generated source this is the persistent
  // synthesis-site identity stored in the resolution manifest.
  std::string synthesis_site;

  [[nodiscard]] bool operator==(
      const SourceCorrelationEntry &other) const {
    return package == other.package && procedure == other.procedure &&
        procedure_ordinal == other.procedure_ordinal &&
        ordinal == other.ordinal && operation == other.operation &&
        authored_file == other.authored_file &&
        authored.line == other.authored.line &&
        authored.column == other.authored.column &&
        generated_file == other.generated_file &&
        generated.line == other.generated.line &&
        generated.column == other.generated.column &&
        synthesis_site == other.synthesis_site;
  }
};

// The map binds coordinates to the exact compiler, target, and resolved
// program which produced them. It is a derived artifact and is not itself a
// resolved-program input, avoiding a circular identity dependency.
struct SourceCorrelationMap {
  std::string format = "draft-source-correlation-v1";
  std::string target_identity;
  std::string compiler_identity;
  // Usually `resolved-program-sha256:<digest>`. Direct backend embeddings that
  // intentionally bypass resolution use `llvm-modules-sha256:<digest>` for the
  // exact module set instead of inventing a resolved-program identity.
  std::string program_identity;
  std::vector<SourceCorrelationEntry> entries;
};

// Checks the compiler-owned format before it becomes a public native sidecar.
// The function is intentionally independent from diagnostics so the backend
// adapter can add the phase-specific prefix at its own boundary.
[[nodiscard]] bool validate_source_correlation_map(
    const SourceCorrelationMap &map,
    std::string &reason);

// Serialization sorts a copy of the rows by their stable operation identity.
// Callers may collect packages in any deterministic graph order without making
// that internal order part of the public sidecar format.
[[nodiscard]] std::string serialize_source_correlation_map(
    const SourceCorrelationMap &map);

} // namespace draft
