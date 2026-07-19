// Exact physical mappings for runtime files outside the Draft distribution.
//
// Runtime assets are external program-identity inputs, but they are not linker
// operands. A resolver records a logical name and canonical content-tree hash;
// a later build may supply the same bytes at another absolute path. This module
// owns that physical-to-portable mapping and deliberately does not choose an
// installation or deployment layout for the caller.
//
// Both regular files and directory trees are valid assets. The shared content-
// tree layer rejects a symlink root, escaping links, and special files, so an
// asset cannot reach ambient host content after its manifest row is verified.
// Relevant specification: docs/specification/03-agent-synthesis.md, "Resolution and reproducible
// builds".

#pragma once

#include "elaborator/resolution.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace draft {

// One command-line or embedding-API mapping. name is the stable manifest key;
// path is a physical file or directory and never enters program identity.
struct RuntimeAssetInput {
  std::string name;
  std::filesystem::path path;
};

// A mapping whose root shape has been checked and whose path has been
// canonicalized. The path remains a read-only physical locator owned by the
// caller's filesystem; the native adapter does not copy or install the asset.
struct VerifiedRuntimeAssetInput {
  std::string name;
  std::filesystem::path path;
};

// Appends one RuntimeAsset manifest row per mapping. Names must be nonempty and
// unique. Paths must be absolute, non-symlink regular files or directories;
// the complete root is hashed with the portable content-tree algorithm.
[[nodiscard]] bool pin_runtime_asset_inputs(
    std::span<const RuntimeAssetInput> inputs,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics);

// Re-hashes every configured mapping and requires exact equality with the
// complete RuntimeAsset row set in the manifest. Other external roles are left
// to their owning verifier. On success, verified is replaced in caller order.
[[nodiscard]] bool verify_runtime_asset_inputs(
    std::span<const RuntimeAssetInput> inputs,
    std::span<const ExternalInputPin> manifest_pins,
    std::vector<VerifiedRuntimeAssetInput> &verified,
    DiagnosticSink &diagnostics);

} // namespace draft
