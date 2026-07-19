// Canonical filesystem-content identity for resolved program inputs.
//
// A physical installation path is intentionally not part of the digest. The
// digest describes the selected file or directory as a sorted sequence of
// relative path records containing kind, permission bits, exact regular-file
// bytes, and exact symlink spelling. This lets a toolchain or SDK be relocated
// without changing program identity while still detecting every byte that can
// affect a resolved program.
//
// Directory traversal never follows symlinks. Relative symlinks whose lexical
// target remains inside the selected root are recorded; absolute or escaping
// links are rejected because they would smuggle unpinned host content into the
// build. Sockets, devices, FIFOs, and other special files are rejected for the
// same reason.

#pragma once

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <filesystem>

namespace draft {

// Hashes one regular file or directory tree under the versioned
// `draft.content-tree.v1` domain. The root itself must not be a symlink. On
// failure, returns false, reports one path-independent compiler diagnostic,
// and leaves digest unchanged.
[[nodiscard]] bool hash_content_tree(
    const std::filesystem::path &root,
    Sha256Digest &digest,
    DiagnosticSink &diagnostics);

} // namespace draft
