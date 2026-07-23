// Canonical resolution pins and manifest serialization.
//
// A manifest commits stable synthesis-site identities and exact external build
// inputs to one checked resolved program. This module owns the plain data format
// and deterministic JSON encoding only. It does not invoke a model, apply an
// expansion to syntax, hash a filesystem tree, or write files; those operations
// remain separate so failed checking cannot partially update a workspace.
//
// The parser accepts the exact versioned schema while permitting insignificant
// JSON whitespace. Unknown, missing, reordered, or duplicate fields are rejected
// deliberately: compiler-owned canonical inputs should never be silently
// reinterpreted. Relevant specification: docs/specification/03-agent-synthesis.md section 10.

#pragma once

#include "base/sha256.h"
#include "sema/agent_metadata.h"
#include "source/diagnostic.h"
#include "workspace/workspace.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// Persistent origin of one generated fragment. Package/file identity is
// semantic and path-independent; byte offsets are half-open coordinates in the
// exact surface file whose content contributed to the obligation digest.
// expansion_bytes closes the map without loading the object and is reverified
// against content-addressed bytes whenever an overlay is built.
struct ResolutionSourceMap {
  std::string root_identity;
  std::string root_relative_path;
  std::string source_relative_path;
  std::uint64_t surface_begin = 0;
  std::uint64_t surface_end = 0;
  std::uint64_t expansion_bytes = 0;
};

// One immutable selection for a synthesis site. site_identity is structural;
// input_digest changes when any supplied semantic context changes; expansion
// identifies exact generated Draft bytes. The three provider strings are
// inspectable provenance only: changing the configured provider, model, or
// generation policy does not stale accepted source and does not change the
// resolved-program identity. No process-local ID or physical path is
// serialized.
struct ResolutionPin {
  std::string site_identity;
  AgentConstructKind kind = AgentConstructKind::SynthesisExpression;
  Sha256Digest input_digest;
  Sha256Digest expansion_digest;
  ResolutionSourceMap source_map;
  std::string provider_identity;
  std::string model_identity;
  std::string configuration_identity;
};

// The manifest selects one target-qualified coherent program. Pins are a
// logical map keyed by site_identity even though a vector retains deterministic
// compact ownership. resolved_program_digest is computed only after every
// selected expansion has passed the ordinary compiler pipeline. Test,
// benchmark, and judgment evidence live in their independent stores and never
// mutate source selection.
struct ResolutionManifest {
  std::string format = "draft-resolution-v7";
  std::string target_identity;
  // The selected executable root is semantic workspace identity, never a
  // physical path. It authenticates a loaded manifest and owns that program's
  // root/target-specific namespace in the persistent resolution store.
  PackageIdentity root_package{"workspace", "."};
  Sha256Digest resolved_program_digest;
  std::vector<ResolutionPin> pins;
};

// Returns canonical UTF-8 JSON ending in one newline. Pins are sorted by site
// identity in the serialized form; the caller's vector is not mutated.
[[nodiscard]] std::string serialize_resolution_manifest(
    const ResolutionManifest &manifest);

// Parses the versioned canonical schema. Digest strings must contain exactly 64
// hexadecimal digits and site identities must be unique. User-facing failures
// are reported as diagnostics with no source range because the manifest is not
// a Draft source buffer.
[[nodiscard]] bool parse_resolution_manifest(
    std::string_view json,
    ResolutionManifest &manifest,
    DiagnosticSink &diagnostics);

} // namespace draft
