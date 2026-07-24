// Immutable compiler-distributed Draft core source bundle.
//
// The build generates one row for every target-selectable source file under
// `core/` and links the exact bytes into every compiler client. Workspace
// loading consumes these rows directly; it never materializes them or searches
// beside the executable. The content identity is computed at build time from
// framed relative paths and bytes, so two compiler binaries with different
// core source cannot share semantic resolution state accidentally.

#pragma once

#include "workspace/package.h"

#include <span>
#include <string_view>

namespace draft {

[[nodiscard]] std::span<const EmbeddedPackageFile> embedded_core_files();
[[nodiscard]] std::string_view embedded_core_content_identity();

} // namespace draft
