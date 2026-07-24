// Deterministic implementation of workspace root selection and discovery.
//
// Directory traversal and package parsing are intentionally separate stages.
// Traversal first collects canonical package candidates in bytewise path order;
// parsing then applies ordinary package-file rules and inspects only
// declaration syntax which contributes to the package scope. This keeps local
// procedure declarations from becoming executable roots and makes a malformed
// candidate fail discovery before any native output is attempted.

#include "workspace/selection.h"

#include "workspace/package.h"

#include <algorithm>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

void selection_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(SourceRange::invalid(), std::move(message));
}

[[nodiscard]] bool is_hidden_leaf(const std::filesystem::path &path) {
  const std::string leaf = path.filename().string();
  return !leaf.empty() && leaf.front() == '.';
}

// A workspace marker is a boundary, not merely configuration. Rejecting a
// symlinked marker prevents a repository layout from changing according to an
// external file while the containing directory still appears canonical.
[[nodiscard]] bool
inspect_workspace_marker(const std::filesystem::path &directory, bool &present,
                         DiagnosticSink &diagnostics) {
  const std::filesystem::path marker = directory / WorkspaceManifestName;
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(marker, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(status))) {
    present = false;
    return true;
  }
  if (error) {
    selection_error(diagnostics, "cannot inspect workspace marker '" +
                                     marker.string() + "': " + error.message());
    return false;
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    selection_error(diagnostics,
                    "workspace marker must be a regular non-symlink file: '" +
                        marker.string() + "'");
    return false;
  }
  present = true;
  return true;
}

// Expanded-source projections are explicit inspection artifacts with one
// fixed root marker. They may be placed below a workspace for review, but must
// not become a second recursively discovered executable tree. Other directory
// names such as `generated` remain ordinary because Draft assigns no semantic
// meaning to names alone.
[[nodiscard]] bool is_expanded_source_projection(
    const std::filesystem::path &directory,
    DiagnosticSink &diagnostics) {
  const std::filesystem::path marker =
      directory / "draft-expanded-source.map";
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(marker, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(status))) {
    return false;
  }
  if (error) {
    selection_error(
        diagnostics,
        "cannot inspect expanded-source marker '" + marker.string() +
            "': " + error.message());
    return false;
  }
  return !std::filesystem::is_symlink(status) &&
      std::filesystem::is_regular_file(status);
}

// Returns a canonical directory or diagnoses the exact filesystem operation
// that failed. Discovery accepts only real directories; symlink rejection is
// performed by the caller before canonicalization where aliases would otherwise
// lose their physical spelling.
[[nodiscard]] std::optional<std::filesystem::path> canonical_directory(
    const std::filesystem::path &path,
    std::string_view role,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::path canonical = std::filesystem::canonical(path, error);
  if (error) {
    selection_error(
        diagnostics,
        "cannot canonicalize " + std::string(role) + " directory '" +
            path.string() + "': " + error.message());
    return std::nullopt;
  }
  if (!std::filesystem::is_directory(canonical, error)) {
    selection_error(
        diagnostics,
        error
            ? "cannot inspect " + std::string(role) + " directory '" +
                canonical.string() + "': " + error.message()
            : std::string(role) + " path is not a directory: '" +
                canonical.string() + "'");
    return std::nullopt;
  }
  return canonical;
}

// component_path is accepted only when it is already one semantic package
// identity. Normalizing user input here would make two spellings select the
// same persistent manifest namespace and would reintroduce path-dependent
// ambiguity at the command boundary.
[[nodiscard]] bool valid_root_selector(
    std::string_view spelling,
    std::filesystem::path &component_path,
    std::string &reason) {
  if (spelling == ".") {
    component_path = ".";
    return true;
  }
  if (spelling.empty()) {
    reason = "root package selector must not be empty";
    return false;
  }
  if (spelling.front() == '/' || spelling.back() == '/' ||
      spelling.find('\\') != std::string_view::npos ||
      spelling.find("//") != std::string_view::npos) {
    reason = "root package selector must be a normalized relative package path";
    return false;
  }
  std::size_t begin = 0;
  while (begin < spelling.size()) {
    const std::size_t slash = spelling.find('/', begin);
    const std::size_t end = slash == std::string_view::npos
        ? spelling.size()
        : slash;
    const std::string_view component = spelling.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") {
      reason = "root package selector must not contain '.', '..', or empty "
               "components";
      return false;
    }
    begin = end + 1;
  }
  component_path = std::filesystem::path(std::string(spelling));
  if (component_path.is_absolute() || component_path.has_root_name() ||
      component_path.has_root_directory()) {
    reason = "root package selector must be relative to the workspace";
    return false;
  }
  return true;
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path &root,
    const std::filesystem::path &candidate,
    std::filesystem::path &relative) {
  relative = candidate.lexically_relative(root);
  if (relative.empty()) return candidate == root;
  for (const std::filesystem::path &component : relative) {
    if (component == "..") return false;
  }
  return !relative.is_absolute();
}

// Captures one directory snapshot in bytewise filename order. Using the
// error-code iterator operations prevents a disappearing or unreadable entry
// from becoming an exception, and sorting before status inspection makes both
// the selected error and recursion order independent of host enumeration.
[[nodiscard]] bool sorted_directory_entries(
    const std::filesystem::path &directory,
    std::vector<std::filesystem::path> &entries,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    selection_error(
        diagnostics,
        "cannot enumerate workspace directory '" + directory.string() +
            "': " + error.message());
    return false;
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    entries.push_back(iterator->path());
    iterator.increment(error);
    if (error) {
      selection_error(
          diagnostics,
          "cannot enumerate workspace directory '" + directory.string() +
              "': " + error.message());
      return false;
    }
  }
  std::sort(
      entries.begin(),
      entries.end(),
      [](const std::filesystem::path &left,
         const std::filesystem::path &right) {
        return left.filename().generic_string() <
            right.filename().generic_string();
      });
  return true;
}

[[nodiscard]] bool declaration_names_main_procedure(
    const SourceManager &sources,
    const SyntaxTree &tree,
    const SyntaxNode &declaration) {
  if (declaration.kind != NodeKind::Declaration ||
      declaration.children.size() < 2) {
    return false;
  }
  const SyntaxNode &pattern = tree.node(declaration.children.front());
  if (pattern.kind != NodeKind::BindingPattern || pattern.children.empty()) {
    return false;
  }
  const SyntaxNode &names = tree.node(pattern.children.front());
  if (names.kind != NodeKind::NameList) return false;

  bool names_main = false;
  for (std::uint32_t index = names.token_begin; index < names.token_end; ++index) {
    if (sources.text(tree.token(index).range) == "main") {
      names_main = true;
      break;
    }
  }
  if (!names_main) return false;
  return tree.node(declaration.children.back()).kind == NodeKind::Procedure;
}

// Only declaration-region nodes are traversed. In particular, an ordinary
// declaration is inspected and then treated as a leaf, so a nested `main`
// procedure inside another procedure body cannot make its package a root.
[[nodiscard]] bool declaration_region_contains_main(
    const SourceManager &sources,
    const SyntaxTree &tree,
    NodeId node_id) {
  const SyntaxNode &node = tree.node(node_id);
  if (node.kind == NodeKind::Declaration) {
    return declaration_names_main_procedure(sources, tree, node);
  }
  if (node.kind != NodeKind::DeclarationList &&
      node.kind != NodeKind::WhenDeclaration &&
      node.kind != NodeKind::DenyDeclaration &&
      node.kind != NodeKind::ExportDeclaration) {
    return false;
  }
  for (NodeId child : node.children) {
    if (declaration_region_contains_main(sources, tree, child)) return true;
  }
  return false;
}

[[nodiscard]] bool package_contains_main(
    const SourceManager &sources,
    const LoadedPackage &package) {
  for (const LoadedPackageFile &file : package.files) {
    if (!file.syntax.has_value() || !file.syntax->root().is_valid()) continue;
    const SyntaxNode &root = file.syntax->node(file.syntax->root());
    for (NodeId child : root.children) {
      if (declaration_region_contains_main(sources, *file.syntax, child)) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool is_excluded_root(
    const std::filesystem::path &candidate,
    const std::vector<std::filesystem::path> &excluded) {
  return std::find(excluded.begin(), excluded.end(), candidate) != excluded.end();
}

// Collects candidate directories in canonical semantic order. Directory entry
// rows are sorted before recursion because recursive_directory_iterator exposes
// host enumeration order and cannot satisfy Draft's determinism contract.
[[nodiscard]] bool collect_candidate_directories(
    const std::filesystem::path &directory,
    const std::filesystem::path &search_root,
    const PackageLoadOptions &package_options,
    const std::vector<std::filesystem::path> &excluded,
    std::vector<std::filesystem::path> &candidates,
    DiagnosticSink &diagnostics) {
  if (is_expanded_source_projection(directory, diagnostics)) return true;
  if (diagnostics.has_errors()) return false;
  if (directory != search_root) {
    bool nested_workspace = false;
    if (!inspect_workspace_marker(directory, nested_workspace, diagnostics)) {
      return false;
    }
    if (nested_workspace)
      return true;
  }
  std::vector<std::filesystem::path> entries;
  if (!sorted_directory_entries(directory, entries, diagnostics)) return false;

  std::vector<std::filesystem::path> children;
  bool contains_draft = false;
  for (const std::filesystem::path &entry : entries) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(entry, error);
    if (error) {
      selection_error(
          diagnostics,
          "cannot inspect workspace entry '" + entry.string() +
              "': " + error.message());
      return false;
    }
    if (std::filesystem::is_symlink(status)) continue;
    if (std::filesystem::is_regular_file(status)) {
      const std::optional<PackageFileKind> kind = selected_package_file_kind(
          entry.filename().string(), package_options);
      if (kind == PackageFileKind::DraftSource) contains_draft = true;
      continue;
    }
    if (!std::filesystem::is_directory(status) || is_hidden_leaf(entry)) {
      continue;
    }
    const std::optional<std::filesystem::path> canonical = canonical_directory(
        entry, "workspace child", diagnostics);
    if (!canonical.has_value()) return false;
    if (is_excluded_root(*canonical, excluded)) continue;
    children.push_back(*canonical);
  }
  if (contains_draft) candidates.push_back(directory);
  for (const std::filesystem::path &child : children) {
    if (!collect_candidate_directories(
            child, search_root, package_options, excluded, candidates, diagnostics)) {
      return false;
    }
  }
  return true;
}

} // namespace

bool locate_command_scope(const std::filesystem::path &search_directory,
                          std::filesystem::path &workspace_directory,
                          std::filesystem::path &canonical_search_directory,
                          std::filesystem::path &manifest_path,
                          DiagnosticSink &diagnostics) {
  const std::optional<std::filesystem::path> canonical_search =
      canonical_directory(search_directory, "command", diagnostics);
  if (!canonical_search.has_value())
    return false;
  canonical_search_directory = *canonical_search;

  std::filesystem::path current = *canonical_search;
  for (;;) {
    bool marker_present = false;
    if (!inspect_workspace_marker(current, marker_present, diagnostics)) {
      return false;
    }
    if (marker_present) {
      workspace_directory = current;
      manifest_path = current / WorkspaceManifestName;
      return true;
    }
    const std::filesystem::path parent = current.parent_path();
    if (parent.empty() || parent == current)
      break;
    current = parent;
  }

  workspace_directory = *canonical_search;
  manifest_path.clear();
  return true;
}

bool locate_command_package(const std::filesystem::path &package_directory,
                            CommandPackageSelection &selection,
                            DiagnosticSink &diagnostics) {
  std::filesystem::path canonical_package;
  if (!locate_command_scope(package_directory, selection.workspace_directory,
                            canonical_package, selection.manifest_path,
                            diagnostics)) {
    return false;
  }
  return identify_workspace_package(selection.workspace_directory,
                                    canonical_package, selection.package,
                                    diagnostics);
}

bool select_workspace_package(
    const std::filesystem::path &workspace_directory,
    std::string_view root_relative_path,
    WorkspacePackageSelection &selection,
    DiagnosticSink &diagnostics) {
  const std::optional<std::filesystem::path> workspace = canonical_directory(
      workspace_directory, "workspace", diagnostics);
  if (!workspace.has_value()) return false;

  std::filesystem::path selector;
  std::string selector_error;
  if (!valid_root_selector(root_relative_path, selector, selector_error)) {
    selection_error(diagnostics, selector_error);
    return false;
  }
  const std::filesystem::path requested = root_relative_path == "."
      ? *workspace
      : *workspace / selector;
  const std::optional<std::filesystem::path> package = canonical_directory(
      requested, "root package", diagnostics);
  if (!package.has_value()) return false;

  if (!identify_workspace_package(
          *workspace, *package, selection, diagnostics)) {
    return false;
  }
  if (selection.identity.root_relative_path != root_relative_path) {
    selection_error(
        diagnostics,
        "root package selector '" + std::string(root_relative_path) +
            "' resolves to canonical workspace package '" +
            selection.identity.root_relative_path + "'");
    return false;
  }
  return true;
}

bool identify_workspace_package(
    const std::filesystem::path &workspace_directory,
    const std::filesystem::path &package_directory,
    WorkspacePackageSelection &selection,
    DiagnosticSink &diagnostics) {
  const std::optional<std::filesystem::path> workspace = canonical_directory(
      workspace_directory, "workspace", diagnostics);
  if (!workspace.has_value()) return false;
  const std::optional<std::filesystem::path> package = canonical_directory(
      package_directory, "root package", diagnostics);
  if (!package.has_value()) return false;

  std::filesystem::path canonical_relative;
  if (!path_is_within(*workspace, *package, canonical_relative)) {
    selection_error(
        diagnostics,
        "root package is outside the selected workspace: '" +
            package->string() + "'");
    return false;
  }
  const std::string identity_path = *package == *workspace
      ? "."
      : canonical_relative.generic_string();
  selection.identity = {"workspace", identity_path};
  selection.physical_directory = *package;
  return true;
}

ExecutableRootDiscoveryResult discover_executable_roots(
    SourceManager &sources,
    const std::filesystem::path &search_directory,
    const WorkspaceLoadOptions &options,
    DiagnosticSink &diagnostics,
    std::span<const std::filesystem::path> excluded_directories) {
  ExecutableRootDiscoveryResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  const std::optional<std::filesystem::path> workspace = canonical_directory(
      options.workspace_directory, "workspace", diagnostics);
  if (!workspace.has_value()) return result;
  const std::optional<std::filesystem::path> search =
      canonical_directory(search_directory, "build search", diagnostics);
  if (!search.has_value())
    return result;
  std::filesystem::path search_relative;
  if (!path_is_within(*workspace, *search, search_relative)) {
    selection_error(
        diagnostics,
        "build search directory is outside the selected workspace: '" +
            search->string() + "'");
    return result;
  }
  if (options.package_options.file_tag.empty()) {
    selection_error(diagnostics, "target file tag must not be empty");
    return result;
  }

  std::vector<std::filesystem::path> excluded;
  excluded.reserve(excluded_directories.size() + options.dependencies.size() +
                   1);
  for (const std::filesystem::path &directory : excluded_directories) {
    const std::optional<std::filesystem::path> root =
        canonical_directory(directory, "excluded", diagnostics);
    if (!root.has_value())
      return result;
    std::filesystem::path relative;
    if (!path_is_within(*workspace, *root, relative)) {
      selection_error(
          diagnostics,
          "excluded directory is outside the selected workspace: '" +
              root->string() + "'");
      return result;
    }
    if (*root != *workspace)
      excluded.push_back(*root);
  }
  if (!options.core_directory.empty()) {
    const std::optional<std::filesystem::path> core = canonical_directory(
        options.core_directory, "core", diagnostics);
    if (!core.has_value()) return result;
    if (*core != *workspace) excluded.push_back(*core);
  }
  for (const DependencyMapping &dependency : options.dependencies) {
    const std::optional<std::filesystem::path> root = canonical_directory(
        dependency.physical_directory, "dependency", diagnostics);
    if (!root.has_value()) return result;
    if (*root != *workspace) excluded.push_back(*root);
  }
  std::sort(excluded.begin(), excluded.end());
  excluded.erase(std::unique(excluded.begin(), excluded.end()), excluded.end());

  std::vector<std::filesystem::path> candidates;
  if (!collect_candidate_directories(
          *search, *search,
          options.package_options,
          excluded,
          candidates,
          diagnostics)) {
    return result;
  }
  for (const std::filesystem::path &candidate : candidates) {
    PackageLoadResult loaded = load_package(
        sources,
        candidate.string(),
        options.package_options,
        diagnostics);
    if (!loaded.ok) continue;
    if (!package_contains_main(sources, loaded.package)) continue;
    const std::filesystem::path relative = candidate.lexically_relative(*workspace);
    result.roots.push_back({
        {"workspace", candidate == *workspace ? "." : relative.generic_string()},
        candidate,
    });
  }
  std::sort(
      result.roots.begin(),
      result.roots.end(),
      [](const WorkspacePackageSelection &left,
         const WorkspacePackageSelection &right) {
        return left.identity.root_relative_path <
            right.identity.root_relative_path;
      });
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

ExecutablePackageInspectionResult inspect_executable_package(
    SourceManager &sources,
    const WorkspacePackageSelection &package,
    const WorkspaceLoadOptions &options,
    DiagnosticSink &diagnostics) {
  ExecutablePackageInspectionResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  if (options.package_options.file_tag.empty()) {
    selection_error(diagnostics, "target file tag must not be empty");
    return result;
  }

  // load_package owns the same deterministic file selection and parser path as
  // recursive discovery. Inspecting the exact directory here avoids parsing
  // unrelated descendants merely because one named program overrides target.
  const PackageLoadResult loaded = load_package(
      sources,
      package.physical_directory.string(),
      options.package_options,
      diagnostics);
  if (!loaded.ok || diagnostics.error_count() != initial_errors)
    return result;
  result.contains_main = package_contains_main(sources, loaded.package);
  result.ok = true;
  return result;
}

} // namespace draft
