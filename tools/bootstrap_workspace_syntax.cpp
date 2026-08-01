// Bootstrap oracle for canonical workspace identity and recursive imports.
//
// This non-installed qualification executable calls the production C++
// workspace loader with the same explicit roots accepted by draftc-next, then
// prints a stable graph view containing semantic identities and import
// occurrences. Physical paths are deliberately absent. The process owns its
// SourceManager, diagnostics, graph, and argument-backed configuration until
// rendering completes.
//
// Native filesystem error details are removed only from `cannot resolve`
// diagnostics because Draft core/filesystem exposes a portable closed error
// set. All source ranges, excerpts, parser messages, graph messages, ordering,
// stdout bytes, and exit status otherwise come from the production loader.

#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/workspace.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

[[nodiscard]] bool valid_target_selector(std::string_view selector) {
  return selector == "aarch64-macos" || selector == "aarch64-linux" ||
         selector == "x86_64-linux" || selector == "x86_64-windows";
}

[[nodiscard]] const char *root_kind_text(draft::PackageRootKind kind) {
  switch (kind) {
  case draft::PackageRootKind::Workspace:
    return "workspace";
  case draft::PackageRootKind::Dependency:
    return "dependency";
  case draft::PackageRootKind::Core:
    return "core";
  }
  return "unknown";
}

// FileId is SourceManager-global in the bootstrap while the self-hosted package
// uses its canonical file-row index. The dump names the relative file, which is
// stable across both representations and more useful to a human reader.
[[nodiscard]] std::string_view relative_file_name(
    const draft::WorkspacePackage &package, draft::FileId file) {
  for (const draft::LoadedPackageFile &row : package.loaded.files) {
    if (row.source == file) return row.relative_name;
  }
  return "<invalid-file>";
}

void dump_workspace_graph(const draft::WorkspaceGraph &graph,
                          std::ostream &output) {
  output << "root-package ";
  if (graph.root_package.is_valid()) {
    output << graph.root_package.value;
  } else {
    output << "invalid";
  }
  output << '\n';

  for (std::size_t index = 0; index < graph.roots.size(); ++index) {
    const draft::PackageRoot &root = graph.roots[index];
    output << "root " << index << ' ' << root_kind_text(root.kind) << ' '
           << root.identity << ' '
           << (root.import_prefix.empty() ? "-" : root.import_prefix) << '\n';
  }
  for (std::size_t index = 0; index < graph.packages.size(); ++index) {
    const draft::WorkspacePackage &package = graph.packages[index];
    output << "package " << index << ' ' << package.root << ' '
           << draft::display_package_identity(package.identity) << ' '
           << package.loaded.short_name << '\n';
  }
  for (const draft::PackageImport &import : graph.imports) {
    const draft::WorkspacePackage &importing =
        graph.package(import.importing_package);
    output << "import " << import.importing_package.value << ' '
           << relative_file_name(importing, import.file) << ' ' << import.path
           << ' ' << import.imported_package.value << '\n';
  }
}

[[nodiscard]] draft::DiagnosticSink normalized_diagnostics(
    const draft::DiagnosticSink &source) {
  draft::DiagnosticSink result;
  constexpr std::string_view prefix = "cannot resolve ";
  for (const draft::Diagnostic &diagnostic : source.diagnostics()) {
    std::string message = diagnostic.message;
    if (message.starts_with(prefix)) {
      const std::size_t detail = message.rfind("': ");
      if (detail != std::string::npos) message.erase(detail + 1);
    }
    result.report(diagnostic.severity, diagnostic.range, std::move(message));
  }
  return result;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 11 || (argc - 11) % 4 != 0 ||
      std::string_view(argv[1]) != "workspace-syntax" ||
      std::string_view(argv[3]) != "--workspace" ||
      std::string_view(argv[5]) != "--core" ||
      std::string_view(argv[7]) != "--core-identity" ||
      std::string_view(argv[9]) != "--target" ||
      !valid_target_selector(argv[10])) {
    std::cerr
        << "usage:\n"
           "  draft-bootstrap-workspace-syntax workspace-syntax <root-package> "
           "--workspace <workspace> --core <core-root> --core-identity "
           "<identity> --target <selector> [--dependency <prefix> <root> "
           "<identity>]...\n";
    return EXIT_FAILURE;
  }

  draft::WorkspaceLoadOptions options;
  options.workspace_directory = argv[4];
  options.core_directory = argv[6];
  options.core_content_identity = argv[8];
  options.package_options.file_tag = argv[10];
  for (int index = 11; index < argc; index += 4) {
    if (std::string_view(argv[index]) != "--dependency") {
      std::cerr << "error: malformed dependency mapping\n";
      return EXIT_FAILURE;
    }
    options.dependencies.push_back({argv[index + 1], argv[index + 2],
                                    argv[index + 3]});
  }

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::WorkspaceLoadResult loaded =
      draft::load_workspace(sources, argv[2], options, diagnostics);
  dump_workspace_graph(loaded.graph, std::cout);
  const draft::DiagnosticSink normalized = normalized_diagnostics(diagnostics);
  std::cerr << draft::render_diagnostics(sources, normalized);
  if (!std::cout || !std::cerr) return EXIT_FAILURE;
  return loaded.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
