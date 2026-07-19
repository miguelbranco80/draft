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
// opaque, nonempty, versioned policy identities and collectively distinguish
// the implementation, model, and full resolver configuration used to produce
// the bytes. No process-local ID or physical path is serialized.
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

// ExternalInputKind names the semantic role an exact external filesystem tree
// plays in a locked build. The content digest alone cannot distinguish using
// identical bytes as a toolchain versus an SDK or link artifact; the role and
// logical name are therefore part of program identity and the unique key.
enum class ExternalInputKind {
  Toolchain,
  Sdk,
  ForeignArtifact,
  Object,
  Archive,
  SharedLibrary,
  RuntimeAsset,
  ProviderSummary,
};

// One row contains no physical host path. content_digest is the canonical
// `draft.content-tree.v1` digest of the selected file or directory. entry_point
// is a normalized relative path inside that tree when one executable or file is
// invoked directly; it is empty when the root itself is the semantic input.
// name is a manifest-local, user-facing identity such as `llvm-22.1` or
// `macos-15.5-sdk` and must be unique together with kind.
struct ExternalInputPin {
  ExternalInputKind kind = ExternalInputKind::Toolchain;
  std::string name;
  Sha256Digest content_digest;
  std::string entry_point;
};

// One passing validation object selected by this resolved program. The key
// names the exact static claim/environment policy while content_digest names
// the immutable execution attempt. Package identity locates the package-owned
// evidence store without embedding a physical workspace path. Judgment uses
// the same row once its provider is enabled.
struct ResolutionEvidencePin {
  std::string kind;
  std::string root_identity;
  std::string root_relative_path;
  Sha256Digest key;
  Sha256Digest content_digest;
};

// The manifest selects one target-qualified coherent program. Pins are a
// logical map keyed by site_identity even though a vector retains deterministic
// compact ownership. resolved_program_digest is computed only after every
// selected expansion has passed the ordinary compiler pipeline.
struct ResolutionManifest {
  std::string format = "draft-resolution-v4";
  std::string target_identity;
  Sha256Digest resolved_program_digest;
  std::vector<ExternalInputPin> external_inputs;
  std::vector<ResolutionEvidencePin> evidence;
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

[[nodiscard]] std::string_view external_input_kind_name(
    ExternalInputKind kind);

} // namespace draft
