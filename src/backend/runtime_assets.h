// Explicit physical mappings for runtime files outside the Draft distribution.
//
// Runtime assets are deployment inputs, not linker operands or resolved Draft
// source identity. This module validates the physical mapping supplied to one
// invocation and deliberately does not choose an installation or deployment
// layout for the caller.
//
// Both regular files and directory roots are valid assets. The root itself must
// be real rather than a symlink; contents remain the embedding build system's
// responsibility because Draft neither traverses nor copies them.

#pragma once

#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// One command-line or embedding-API mapping. name identifies the deployment
// role for this invocation; path is a physical file or directory.
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

// Parses one public `name:path` mapping and makes the path absolute without
// checking asset shape. Validation remains a native-build operation.
[[nodiscard]] bool parse_runtime_asset_input(std::string_view spelling,
                                             RuntimeAssetInput &input,
                                             std::string &reason);

// Validates and canonicalizes runtime-asset roots supplied to this invocation.
// Assets are deployment inputs, not Draft source identity: the compiler does
// not traverse or hash their contents. Names must be unique and roots must be
// absolute real files or directories rather than symlinks.
[[nodiscard]] bool inspect_runtime_asset_inputs(
    std::span<const RuntimeAssetInput> inputs,
    std::vector<VerifiedRuntimeAssetInput> &verified,
    DiagnosticSink &diagnostics);

} // namespace draft
