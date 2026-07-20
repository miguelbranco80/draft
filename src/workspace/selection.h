// Explicit workspace-package selection and executable-root discovery.
//
// The command layer supplies one canonical workspace directory. This module
// converts an explicit root-relative selector into the PackageIdentity used by
// the compiler, or walks the ordinary workspace tree to find surface packages
// that declare a package-level `main`. It owns no compiler graph and performs
// no semantic analysis: selected roots are subsequently compiled through the
// normal closed import-graph pipeline.
//
// Discovery never follows symlinks, skips every directory whose leaf name
// begins with '.', excludes marked expanded-source projections, and excludes
// explicitly mapped dependency/core roots. It
// processes directory entries and returned roots in bytewise root-relative
// path order. Package parsing uses the same target-qualified file-selection
// rules as ordinary compilation. These invariants prevent host enumeration,
// hidden compiler state, or a physical alias from changing the selected target
// set. Relevant specification: section 3, folder packages and canonical
// workspace identities.

#pragma once

#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/workspace.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// One executable-root selection pairs the persistent package identity with its
// canonical physical directory. identity is always in the `workspace` root;
// dependency and compiler-distributed packages cannot become command roots.
// physical_directory is an I/O fact and never enters source or manifest
// identity.
struct WorkspacePackageSelection {
  PackageIdentity identity;
  std::filesystem::path physical_directory;
};

struct ExecutableRootDiscoveryResult {
  bool ok = false;
  std::vector<WorkspacePackageSelection> roots;
};

// Converts one existing physical package directory to its canonical workspace
// identity. The package must be contained by workspace_directory. This is the
// shared boundary used by compiler/resolver persistence code after an embedding
// supplies physical paths directly rather than a CLI selector.
[[nodiscard]] bool identify_workspace_package(
    const std::filesystem::path &workspace_directory,
    const std::filesystem::path &package_directory,
    WorkspacePackageSelection &selection,
    DiagnosticSink &diagnostics);

// Resolves one normalized slash-separated path relative to workspace_directory.
// `.` selects the workspace directory itself. Absolute paths, `..`, empty path
// components, non-directories, paths outside the workspace, and symlink aliases
// whose canonical identity differs from the spelling are diagnosed. Hidden
// packages may be selected explicitly; only automatic discovery skips them.
[[nodiscard]] bool select_workspace_package(
    const std::filesystem::path &workspace_directory,
    std::string_view root_relative_path,
    WorkspacePackageSelection &selection,
    DiagnosticSink &diagnostics);

// Recursively discovers every selected surface package that contains an
// ordinary package-level procedure declaration named `main`. Discovery parses
// candidate packages so malformed package files cannot produce a guessed root
// set. It does not type-check, follow imports, or require a valid entry
// signature; those errors belong to normal compilation of the selected root.
[[nodiscard]] ExecutableRootDiscoveryResult discover_executable_roots(
    SourceManager &sources,
    const WorkspaceLoadOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
