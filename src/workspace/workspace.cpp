// Explicit-root package graph construction.
//
// The loader uses canonical filesystem paths only to enforce containment and to
// deduplicate physical reads. Semantic identity is always computed separately
// from the selected PackageRoot identity and its normalized relative path.
// This distinction is essential for reproducible builds: moving a checkout
// must not rename every package inside it.

#include "workspace/workspace.h"

#include "syntax/token.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace draft {
namespace {

enum class VisitState {
  Loading,
  Complete,
};

struct PackageVisit {
  PackageIdentity identity;
  PackageId package;
  VisitState state = VisitState::Loading;
};

struct ParsedImport {
  FileId file;
  NodeId syntax;
  SourceRange range;
  std::string path;
};

// GraphLoader owns only temporary traversal state. The result graph itself owns
// all lasting package rows. A single loader call is deterministic and contains
// no process-global cache, so a failed load cannot poison a later build.
class GraphLoader {
public:
  GraphLoader(
      SourceManager &sources,
      const WorkspaceLoadOptions &options,
      DiagnosticSink &diagnostics)
      : sources_(sources), options_(options), diagnostics_(diagnostics) {}

  [[nodiscard]] WorkspaceLoadResult run(const std::string &root_package_directory) {
    WorkspaceLoadResult result;
    const std::size_t initial_errors = diagnostics_.error_count();
    if (!initialize_roots()) {
      result.graph = std::move(graph_);
      return result;
    }

    const std::optional<std::string> physical = canonical_existing_directory(
        root_package_directory, SourceRange::invalid(), "root package");
    if (!physical.has_value()) {
      result.graph = std::move(graph_);
      return result;
    }

    const std::filesystem::path workspace(graph_.roots.front().physical_directory);
    const std::filesystem::path package_path(*physical);
    const std::optional<std::string> relative = contained_relative_path(
        workspace, package_path, SourceRange::invalid(), "root package");
    if (!relative.has_value()) {
      result.graph = std::move(graph_);
      return result;
    }

    graph_.root_package = load_package_recursive(0, *relative, SourceRange::invalid());
    for (const WorkspaceSourceOverride &source_override :
         options_.source_overrides) {
      bool package_loaded = false;
      for (const WorkspacePackage &package : graph_.packages) {
        if (package.identity == source_override.identity) {
          package_loaded = true;
          break;
        }
      }
      if (!package_loaded) {
        diagnostics_.error(
            SourceRange::invalid(),
            "resolved source override names an unreachable package '" +
                display_package_identity(source_override.identity) + "'");
      }
    }
    result.ok = graph_.root_package.is_valid() &&
        diagnostics_.error_count() == initial_errors;
    result.graph = std::move(graph_);
    return result;
  }

private:
  // Canonicalizes a configured root once. Roots must already exist because a
  // missing selected root is a build-configuration error, not a package error.
  [[nodiscard]] std::optional<std::string> canonical_existing_directory(
      const std::string &path,
      SourceRange range,
      std::string_view description) {
    if (path.empty()) {
      diagnostics_.error(range, std::string(description) + " directory is empty");
      return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error) {
      diagnostics_.error(
          range,
          "cannot resolve " + std::string(description) + " directory '" + path +
              "': " + error.message());
      return std::nullopt;
    }
    const bool directory = std::filesystem::is_directory(canonical, error);
    if (error || !directory) {
      const std::string detail = error ? error.message() : "not a directory";
      diagnostics_.error(
          range,
          std::string(description) + " path '" + path + "' is invalid: " + detail);
      return std::nullopt;
    }
    return canonical.generic_string();
  }

  [[nodiscard]] bool valid_import_prefix(std::string_view prefix) const {
    if (prefix.empty() || prefix.front() == '/' || prefix.back() == '/') {
      return false;
    }
    std::size_t begin = 0;
    while (begin < prefix.size()) {
      const std::size_t slash = prefix.find('/', begin);
      const std::size_t end = slash == std::string_view::npos ? prefix.size() : slash;
      const std::string_view component = prefix.substr(begin, end - begin);
      if (component.empty() || component == "." || component == "..") {
        return false;
      }
      begin = end + 1;
    }
    return true;
  }

  // Materializes roots in a stable order: workspace, configured dependencies,
  // then core. Duplicate or component-overlapping dependency prefixes are
  // rejected because the source spelling would not select exactly one root.
  [[nodiscard]] bool initialize_roots() {
    const std::optional<std::string> workspace = canonical_existing_directory(
        options_.workspace_directory, SourceRange::invalid(), "workspace");
    if (!workspace.has_value()) {
      return false;
    }
    graph_.roots.push_back({PackageRootKind::Workspace, "workspace", "", *workspace});

    for (const DependencyMapping &mapping : options_.dependencies) {
      if (!valid_import_prefix(mapping.import_prefix)) {
        diagnostics_.error(
            SourceRange::invalid(),
            "invalid dependency import prefix '" + mapping.import_prefix + "'");
        continue;
      }
      if (mapping.import_prefix == "core") {
        diagnostics_.error(
            SourceRange::invalid(),
            "dependency import prefix 'core' is reserved for the compiler distribution");
        continue;
      }
      if (mapping.content_identity.empty() || mapping.content_identity == "workspace") {
        diagnostics_.error(
            SourceRange::invalid(),
            "dependency '" + mapping.import_prefix + "' requires a pinned content identity");
        continue;
      }
      bool ambiguous = false;
      for (std::size_t index = 1; index < graph_.roots.size(); ++index) {
        const std::string &existing = graph_.roots[index].import_prefix;
        const bool mapping_inside_existing = component_prefix(existing, mapping.import_prefix);
        const bool existing_inside_mapping = component_prefix(mapping.import_prefix, existing);
        if (mapping_inside_existing || existing_inside_mapping) {
          diagnostics_.error(
              SourceRange::invalid(),
              "ambiguous dependency import prefixes '" + existing + "' and '" +
                  mapping.import_prefix + "'");
          ambiguous = true;
          break;
        }
      }
      if (ambiguous) {
        continue;
      }
      const std::optional<std::string> physical = canonical_existing_directory(
          mapping.physical_directory,
          SourceRange::invalid(),
          "dependency '" + mapping.import_prefix + "'");
      if (!physical.has_value()) {
        continue;
      }
      graph_.roots.push_back({
          PackageRootKind::Dependency,
          mapping.content_identity,
          mapping.import_prefix,
          *physical,
      });
    }

    if (!options_.core_directory.empty()) {
      if (options_.core_content_identity.empty() ||
          options_.core_content_identity == "workspace") {
        diagnostics_.error(
            SourceRange::invalid(),
            "core distribution requires a pinned content identity");
      } else {
        const std::optional<std::string> physical = canonical_existing_directory(
            options_.core_directory, SourceRange::invalid(), "core distribution");
        if (physical.has_value()) {
          graph_.roots.push_back({
              PackageRootKind::Core,
              options_.core_content_identity,
              "core",
              *physical,
          });
        }
      }
    }
    return !diagnostics_.has_errors();
  }

  // Returns true when prefix selects path at a whole-component boundary.
  [[nodiscard]] static bool component_prefix(
      std::string_view prefix, std::string_view path) {
    return path == prefix ||
        (path.size() > prefix.size() && path.substr(0, prefix.size()) == prefix &&
         path[prefix.size()] == '/');
  }

  // Produces the normalized semantic path of child within root. lexically
  // relative paths are evaluated only after both operands are canonical, so a
  // symlink that leaves the configured root is rejected.
  [[nodiscard]] std::optional<std::string> contained_relative_path(
      const std::filesystem::path &root,
      const std::filesystem::path &child,
      SourceRange range,
      std::string_view description) {
    const std::filesystem::path relative = child.lexically_relative(root);
    if (relative.empty() && child != root) {
      diagnostics_.error(range, std::string(description) + " is not inside its selected root");
      return std::nullopt;
    }
    for (const std::filesystem::path &component : relative) {
      if (component == "..") {
        diagnostics_.error(
            range, std::string(description) + " escapes its selected package root");
        return std::nullopt;
      }
    }
    if (relative.empty() || relative == ".") {
      return std::string(".");
    }
    return relative.generic_string();
  }

  // Selects exactly one configured root and strips only its import prefix. The
  // workspace is the fallback for paths not claimed by dependencies or core;
  // it is not probed alongside an explicit mapping.
  [[nodiscard]] std::optional<std::pair<std::uint32_t, std::string>> resolve_import_root(
      std::string_view import_path,
      SourceRange range) {
    if (component_prefix("core", import_path)) {
      bool has_core_root = false;
      for (const PackageRoot &root : graph_.roots) {
        if (root.kind == PackageRootKind::Core) {
          has_core_root = true;
          break;
        }
      }
      if (!has_core_root) {
        diagnostics_.error(
            range,
            "import path '" + std::string(import_path) +
                "' requires a configured core distribution");
        return std::nullopt;
      }
    }

    std::optional<std::uint32_t> selected;
    for (std::uint32_t index = 1;
         index < static_cast<std::uint32_t>(graph_.roots.size());
         ++index) {
      const PackageRoot &root = graph_.roots[index];
      if (!component_prefix(root.import_prefix, import_path)) {
        continue;
      }
      if (selected.has_value()) {
        diagnostics_.error(
            range, "import path '" + std::string(import_path) + "' has ambiguous root mappings");
        return std::nullopt;
      }
      selected = index;
    }

    const std::uint32_t root_index = selected.value_or(0);
    const PackageRoot &root = graph_.roots[root_index];
    if (root.kind == PackageRootKind::Core && import_path == "core") {
      diagnostics_.error(range, "import path 'core' must name a package below the core root");
      return std::nullopt;
    }
    std::string relative(import_path);
    if (root_index != 0) {
      if (import_path.size() == root.import_prefix.size()) {
        relative = ".";
      } else {
        relative = std::string(import_path.substr(root.import_prefix.size() + 1));
      }
    }
    return std::pair<std::uint32_t, std::string>{root_index, std::move(relative)};
  }

  // Extracts import paths directly from parsed syntax. The semantic collector
  // separately creates aliases because aliases are file-local symbols, whereas
  // graph construction needs only the package path and source occurrence.
  [[nodiscard]] std::vector<ParsedImport> imports_in(const LoadedPackage &package) const {
    std::vector<ParsedImport> imports;
    for (const LoadedPackageFile &file : package.files) {
      if (!file.syntax.has_value() || !file.syntax->root().is_valid()) {
        continue;
      }
      const SyntaxTree &tree = *file.syntax;
      const SyntaxNode &root = tree.node(tree.root());
      for (NodeId child_id : root.children) {
        const SyntaxNode &node = tree.node(child_id);
        if (node.kind != NodeKind::ImportClause || node.children.empty()) {
          continue;
        }
        const SyntaxNode &path_node = tree.node(node.children.front());
        std::string path;
        for (std::uint32_t token_index = path_node.token_begin;
             token_index < path_node.token_end;
             ++token_index) {
          const Token &token = tree.token(token_index);
          if (token.kind == TokenKind::Slash) {
            path.push_back('/');
          } else {
            path += sources_.text(token.range);
          }
        }
        if (!path.empty()) {
          imports.push_back({file.source, child_id, node.range, std::move(path)});
        }
      }
    }
    return imports;
  }

  [[nodiscard]] std::optional<PackageId> find_visit(
      const PackageIdentity &identity,
      VisitState *state) const {
    for (const PackageVisit &visit : visits_) {
      if (visit.identity == identity) {
        if (state != nullptr) {
          *state = visit.state;
        }
        return visit.package;
      }
    }
    return std::nullopt;
  }

  // Loads one identity exactly once. A Loading hit is a back edge and therefore
  // a package-cycle error. The row is installed before following imports so IDs
  // remain stable and the back edge can name both endpoints.
  [[nodiscard]] PackageId load_package_recursive(
      std::uint32_t root_index,
      const std::string &relative_path,
      SourceRange import_range) {
    assert(root_index < graph_.roots.size());
    const PackageRoot &root = graph_.roots[root_index];
    const PackageIdentity identity{root.identity, relative_path};
    VisitState state = VisitState::Loading;
    if (const std::optional<PackageId> existing = find_visit(identity, &state)) {
      if (state == VisitState::Loading) {
        diagnostics_.error(
            import_range,
            "package import cycle reaches '" + display_package_identity(identity) + "'");
        return PackageId{};
      }
      return *existing;
    }

    const std::filesystem::path candidate = relative_path == "."
        ? std::filesystem::path(root.physical_directory)
        : std::filesystem::path(root.physical_directory) / relative_path;
    const std::optional<std::string> canonical = canonical_existing_directory(
        candidate.string(), import_range, "package");
    if (!canonical.has_value()) {
      return PackageId{};
    }
    const std::optional<std::string> checked_relative = contained_relative_path(
        root.physical_directory, *canonical, import_range, "package path");
    if (!checked_relative.has_value()) {
      return PackageId{};
    }

    const PackageIdentity checked_identity{root.identity, *checked_relative};
    if (!(checked_identity == identity)) {
      // Canonical symlinks may normalize a spelling to a different in-root
      // directory. Identity follows the canonical target so aliases cannot load
      // the same physical package under two semantic names.
      if (const std::optional<PackageId> existing = find_visit(checked_identity, &state)) {
        if (state == VisitState::Loading) {
          diagnostics_.error(
              import_range,
              "package import cycle reaches '" +
                  display_package_identity(checked_identity) + "'");
          return PackageId{};
        }
        return *existing;
      }
    }

    PackageLoadOptions package_options = options_.package_options;
    package_options.source_overrides.clear();
    for (const WorkspaceSourceOverride &source_override :
         options_.source_overrides) {
      if (source_override.identity == checked_identity) {
        package_options.source_overrides.push_back(source_override.source);
      }
    }
    PackageLoadResult loaded = draft::load_package(
        sources_, *canonical, package_options, diagnostics_);
    if (!loaded.ok) {
      return PackageId{};
    }

    if (graph_.packages.size() >=
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      diagnostics_.error(import_range, "workspace contains too many packages");
      return PackageId{};
    }
    const PackageId id{static_cast<std::uint32_t>(graph_.packages.size())};
    graph_.packages.push_back({checked_identity, root_index, std::move(loaded.package)});
    visits_.push_back({checked_identity, id, VisitState::Loading});

    // Do not retain references into graph_.packages across recursion: appending
    // a dependency may reallocate the vector. The current package's syntax owns
    // its nodes independently, and ParsedImport copies the source facts needed.
    const std::vector<ParsedImport> imports = imports_in(graph_.packages[id.value].loaded);
    for (const ParsedImport &import : imports) {
      const std::optional<std::pair<std::uint32_t, std::string>> resolved =
          resolve_import_root(import.path, import.range);
      if (!resolved.has_value()) {
        continue;
      }
      const PackageId dependency = load_package_recursive(
          resolved->first, resolved->second, import.range);
      if (dependency.is_valid()) {
        graph_.imports.push_back({id, dependency, import.file, import.syntax, import.path});
      }
    }

    for (PackageVisit &visit : visits_) {
      if (visit.package == id) {
        visit.state = VisitState::Complete;
        break;
      }
    }
    return id;
  }

  SourceManager &sources_;
  const WorkspaceLoadOptions &options_;
  DiagnosticSink &diagnostics_;
  WorkspaceGraph graph_;
  std::vector<PackageVisit> visits_;
};

} // namespace

bool PackageId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

const WorkspacePackage &WorkspaceGraph::package(PackageId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < packages.size());
  return packages[id.value];
}

const PackageRoot &WorkspaceGraph::root(std::uint32_t index) const {
  assert(static_cast<std::size_t>(index) < roots.size());
  return roots[index];
}

WorkspaceLoadResult load_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    const WorkspaceLoadOptions &options,
    DiagnosticSink &diagnostics) {
  GraphLoader loader(sources, options, diagnostics);
  return loader.run(root_package_directory);
}

std::string display_package_identity(const PackageIdentity &identity) {
  return identity.root_identity + ":" + identity.root_relative_path;
}

} // namespace draft
