// Package-path workspace discovery and executable-root discovery.
//
// The command layer supplies an ordinary package or search directory. This
// module canonicalizes it, searches its ancestors for the nearest
// `draft.workspace` marker, and derives both the workspace boundary and the
// stable PackageIdentity used by the compiler. A directory with no marker
// above it is a standalone workspace. This makes the common command
// `draftc check path/to/package` exact with one unambiguous path coordinate.
//
// Recursive executable discovery never follows symlinks, skips every directory
// whose leaf name begins with '.', excludes marked expanded-source projections,
// mapped dependency/core roots, and nested workspaces. It
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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

inline constexpr std::string_view WorkspaceManifestName = "draft.workspace";

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

// ExecutablePackageInspectionResult is the exact-package counterpart to
// recursive discovery. contains_main reports only an explicit surface
// package-level procedure selected for the supplied target. ok is false when
// package loading or parsing diagnosed malformed input; callers must not treat
// that failure as an ordinary non-executable package.
struct ExecutablePackageInspectionResult {
  bool ok = false;
  bool contains_main = false;
};

// One command path has two meanings which must remain distinct. package is the
// exact root package selected by check/test/run-like commands.
// workspace_directory owns root-relative imports and `.draft/` state.
// manifest_path is empty for a standalone package and otherwise names the
// marker which established that boundary. All paths are canonical and therefore
// safe to retain for command lifetime I/O; none enters semantic identity except
// through package.identity.
struct CommandPackageSelection {
  std::filesystem::path workspace_directory;
  WorkspacePackageSelection package;
  std::filesystem::path manifest_path;
};

// Locates the exact package supplied to a command. The nearest ancestor marker
// wins. A present marker must be a regular non-symlink file; malformed manifest
// contents are parsed by the configuration layer after this physical boundary
// has been established.
[[nodiscard]] bool
locate_command_package(const std::filesystem::path &package_directory,
                       CommandPackageSelection &selection,
                       DiagnosticSink &diagnostics);

// Locates a recursive build scope. search_directory is canonicalized exactly
// like a package path, but it need not itself contain Draft source. The
// returned workspace is the nearest marked ancestor, or the search directory
// when no marker exists.
[[nodiscard]] bool
locate_command_scope(const std::filesystem::path &search_directory,
                     std::filesystem::path &workspace_directory,
                     std::filesystem::path &canonical_search_directory,
                     std::filesystem::path &manifest_path,
                     DiagnosticSink &diagnostics);

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
// excluded_directories prune complete subtrees. independently_inspected_packages
// skip only those exact candidate packages while retaining descendant traversal,
// allowing an embedding to inspect a named root under a different target once.
[[nodiscard]] ExecutableRootDiscoveryResult discover_executable_roots(
    SourceManager &sources,
    const std::filesystem::path &search_directory,
    const WorkspaceLoadOptions &options,
    DiagnosticSink &diagnostics,
    std::span<const std::filesystem::path> excluded_directories = {},
    std::span<const std::filesystem::path> independently_inspected_packages = {});

// Inspects one already-selected package under one target file-selection rule.
// This is used when operator configuration assigns a target to a named program:
// recursive discovery can use the workspace default for unnamed packages while
// the configured package is checked exactly once under its own target. The
// operation does not recurse, follow imports, or type-check the entry signature.
[[nodiscard]] ExecutablePackageInspectionResult inspect_executable_package(
    SourceManager &sources,
    const WorkspacePackageSelection &package,
    const WorkspaceLoadOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
