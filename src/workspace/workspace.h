// Canonical workspace roots, package identities, and import-graph loading.
//
// A physical directory is needed to read source, but it is not a Draft package
// identity. This module is the boundary that turns explicitly configured roots
// and source import paths into stable PackageIdentity values. It recursively
// loads every reachable package, records each file-local import edge, rejects
// cycles, and prevents a resolved path (including a symlink) from escaping the
// root selected for that import.
//
// WorkspaceGraph owns LoadedPackage values but not source bytes. FileId and
// SyntaxTree source ranges point into the caller-owned SourceManager, which must
// outlive the graph and every semantic result built from it. PackageId values
// are stable vector indices for the graph's lifetime. Discovery and edge order
// are deterministic: root source files use package filename order and imports
// use syntax order within each file.
//
// This layer intentionally knows no symbol or type rules. It depends only on
// source, syntax, and the one-package loader. Semantic analysis later consumes
// the closed graph and never performs ambient filesystem lookup.
//
// Relevant specification: docs/specification/01-core-language.md section 3, "Folder packages".

#pragma once

#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace draft {

// PackageId is a stable index into WorkspaceGraph::packages. The invalid value
// is used while an import cannot be resolved; failed graph loads are never
// passed to semantic analysis as if those edges were complete.
struct PackageId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const PackageId &) const = default;
};

enum class PackageRootKind {
  Workspace,
  Dependency,
  Core,
};

// PackageRoot is one explicitly selected namespace-to-directory mapping.
// identity participates in semantic package identity and content hashes;
// physical_directory is only the canonical I/O location. import_prefix is
// empty for the workspace root, configured for dependencies, and exactly
// "core" for the selected compiler distribution.
struct PackageRoot {
  PackageRootKind kind = PackageRootKind::Workspace;
  std::string identity;
  std::string import_prefix;
  std::string physical_directory;
};

// DependencyMapping is user/build-manifest input. A component-aligned prefix
// claims imports beginning with that prefix. The dependency content identity,
// rather than its checkout path, makes builds in different directories refer
// to the same semantic root.
struct DependencyMapping {
  std::string import_prefix;
  std::string physical_directory;
  std::string content_identity;
};

// PackageIdentity is the semantic pair required by the specification.
// root_identity is "workspace" or a pinned dependency/distribution identity.
// root_relative_path uses slash separators, has no leading/trailing slash, and
// is "." only when the package is the mapped root directory itself.
struct PackageIdentity {
  std::string root_identity;
  std::string root_relative_path;

  bool operator==(const PackageIdentity &) const = default;
};

// WorkspaceSourceOverride associates complete resolved file bytes with a
// semantic package identity. Physical checkout paths are deliberately absent:
// moving the workspace must not change which pinned source is selected.
struct WorkspaceSourceOverride {
  PackageIdentity identity;
  PackageSourceOverride source;
};

// WorkspaceLoadOptions contains every permitted source root. core_directory
// may be empty only when the reachable graph imports no core package. Likewise,
// no dependency directory or environment search path is inferred.
struct WorkspaceLoadOptions {
  std::string workspace_directory;
  std::string core_directory;
  std::string core_content_identity;
  std::vector<DependencyMapping> dependencies;
  std::vector<WorkspaceSourceOverride> source_overrides;
  PackageLoadOptions package_options;
};

// WorkspacePackage owns one loaded folder package and its canonical identity.
// root indexes WorkspaceGraph::roots. physical paths remain inside the nested
// LoadedPackage solely for subsequent source/assembly I/O.
struct WorkspacePackage {
  PackageIdentity identity;
  std::uint32_t root = 0;
  LoadedPackage loaded;
};

// PackageImport is one source import occurrence, not merely one dependency
// pair. Multiple source files may import the same package under different local
// aliases, so every occurrence retains its file and syntax node. path is the
// exact normalized package path written in source; alias binding remains a
// declaration-collection responsibility.
struct PackageImport {
  PackageId importing_package;
  PackageId imported_package;
  FileId file;
  NodeId syntax;
  std::string path;
};

// WorkspaceGraph is a closed, acyclic package graph when WorkspaceLoadResult::ok
// is true. packages use deterministic first-discovery order and imports use
// deterministic source order. root_package identifies the command's requested
// package, which need not be the mapped workspace directory itself.
struct WorkspaceGraph {
  std::vector<PackageRoot> roots;
  std::vector<WorkspacePackage> packages;
  std::vector<PackageImport> imports;
  PackageId root_package;

  [[nodiscard]] const WorkspacePackage &package(PackageId id) const;
  [[nodiscard]] const PackageRoot &root(std::uint32_t index) const;
};

struct WorkspaceLoadResult {
  bool ok = false;
  WorkspaceGraph graph;
};

// Loads the package at root_package_directory and every transitive import. All
// paths are interpreted only through options. Configuration, filesystem,
// parsing, and graph errors become diagnostics; the function does not throw.
// The returned graph may contain partial rows for tooling, but only an ok graph
// satisfies the closed and acyclic invariants documented above.
[[nodiscard]] WorkspaceLoadResult load_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    const WorkspaceLoadOptions &options,
    DiagnosticSink &diagnostics);

[[nodiscard]] std::string display_package_identity(const PackageIdentity &identity);

} // namespace draft
