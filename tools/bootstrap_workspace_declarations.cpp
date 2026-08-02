// Bootstrap oracle for declaration names and file-local import aliases.
//
// This non-installed qualification executable calls the production C++
// workspace loader and declaration collector, then prints the same canonical
// workspace graph used by the earlier migration gate followed by a stable view
// of package/file scopes, accepted symbols, native source flags, and import
// aliases bound to their already-resolved graph target. It intentionally stops
// before public-interface import, declaration typing, and conditional `when`
// materialization, matching the Draft staging boundary under qualification.
//
// SourceManager owns every byte referenced by the graph and SemanticPackage
// rows until dumping and diagnostic rendering finish. Physical package paths
// are absent from stdout. Native filesystem detail is normalized only for the
// same portable `cannot resolve` failure handled by the workspace-syntax oracle.

#include "sema/analyzer.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/workspace.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] bool valid_target_selector(std::string_view selector) {
  return selector == "aarch64-macos" || selector == "aarch64-linux" ||
         selector == "x86_64-linux" || selector == "x86_64-windows";
}

[[nodiscard]] const char *root_kind_text(draft::PackageRootKind kind) {
  switch (kind) {
  case draft::PackageRootKind::Workspace: return "workspace";
  case draft::PackageRootKind::Dependency: return "dependency";
  case draft::PackageRootKind::Core: return "core";
  }
  return "unknown";
}

[[nodiscard]] const char *symbol_kind_text(draft::SymbolKind kind) {
  switch (kind) {
  case draft::SymbolKind::Import: return "import";
  case draft::SymbolKind::UnresolvedDeclaration:
    return "unresolved-declaration";
  case draft::SymbolKind::Type: return "type";
  case draft::SymbolKind::Constant: return "constant";
  case draft::SymbolKind::Variable: return "variable";
  case draft::SymbolKind::Procedure: return "procedure";
  case draft::SymbolKind::Parameter: return "parameter";
  case draft::SymbolKind::Local: return "local";
  case draft::SymbolKind::Field: return "field";
  case draft::SymbolKind::EnumMember: return "enum-member";
  case draft::SymbolKind::VariantAlternative: return "variant-alternative";
  case draft::SymbolKind::TypeParameter: return "type-parameter";
  case draft::SymbolKind::ValueParameter: return "value-parameter";
  }
  return "unknown";
}

[[nodiscard]] const char *visibility_text(draft::Visibility visibility) {
  switch (visibility) {
  case draft::Visibility::Private: return "private";
  case draft::Visibility::Public: return "public";
  }
  return "unknown";
}

// FileId is SourceManager-global in C++ while the self-hosted graph uses the
// owning package's stable file-row index. Relative filenames are the common,
// semantic-order-preserving display domain.
[[nodiscard]] std::string_view relative_file_name(
    const draft::WorkspacePackage &package, draft::FileId file) {
  for (const draft::LoadedPackageFile &row : package.loaded.files) {
    if (row.source == file) return row.relative_name;
  }
  return "<invalid-file>";
}

void dump_workspace_graph(
    const draft::WorkspaceGraph &graph, std::ostream &output) {
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

using ImportSite = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;

// Build one lookup index without exposing map iteration as semantic ordering.
// Every later query is by an exact package/file/node key retained by both the
// production graph edge and declaration ImportBinding.
[[nodiscard]] std::map<ImportSite, std::size_t> index_import_sites(
    const draft::WorkspaceGraph &graph) {
  std::map<ImportSite, std::size_t> result;
  for (std::size_t index = 0; index < graph.imports.size(); ++index) {
    const draft::PackageImport &edge = graph.imports[index];
    const ImportSite key{
        edge.importing_package.value, edge.file.value, edge.syntax.value};
    const auto [unused, inserted] = result.emplace(key, index);
    (void)unused;
    assert(inserted);
  }
  return result;
}

[[nodiscard]] const draft::NativeBinding *native_binding(
    const draft::SemanticPackage &package, draft::SymbolId symbol) {
  for (const draft::NativeBinding &binding : package.native_bindings) {
    if (binding.symbol == symbol) return &binding;
  }
  return nullptr;
}

void dump_declarations(
    const draft::WorkspaceGraph &graph,
    const std::vector<draft::SemanticPackage> &semantic_packages,
    std::ostream &output) {
  assert(semantic_packages.size() == graph.packages.size());
  const std::map<ImportSite, std::size_t> imports = index_import_sites(graph);

  for (std::size_t package_index = 0;
       package_index < semantic_packages.size(); ++package_index) {
    const draft::SemanticPackage &semantic = semantic_packages[package_index];
    const draft::WorkspacePackage &workspace = graph.packages[package_index];
    output << "semantic-package " << package_index << ' '
           << semantic.symbols.scope_count() << ' '
           << semantic.symbols.symbol_count() << ' '
           << semantic.imports.size() << '\n';

    for (const draft::FileSemanticScope &file : semantic.files) {
      output << "file-scope " << package_index << ' ' << file.scope.value << ' '
             << relative_file_name(workspace, file.file) << '\n';
    }

    for (std::size_t symbol_index = 0;
         symbol_index < semantic.symbols.symbol_count(); ++symbol_index) {
      const draft::SymbolId id{static_cast<std::uint32_t>(symbol_index)};
      const draft::Symbol &symbol = semantic.symbols.symbol(id);
      const draft::NativeBinding *native = native_binding(semantic, id);
      output << "symbol " << package_index << ' ' << symbol_index << ' '
             << symbol.scope.value << ' '
             << relative_file_name(workspace, symbol.syntax.file) << ' '
             << symbol.name << ' ' << symbol_kind_text(symbol.kind) << ' '
             << visibility_text(symbol.visibility) << ' '
             << (symbol.flags.is_thread_local ? '1' : '0')
             << (symbol.flags.foreign ? '1' : '0')
             << (symbol.flags.exported ? '1' : '0')
             << (symbol.flags.parametric ? '1' : '0') << ' ';
      if (native != nullptr && !native->provider.empty()) {
        output << native->provider;
      } else {
        output << '-';
      }
      output << ' ';
      if (native != nullptr) {
        output << native->linker_name_spelling;
      } else {
        output << '-';
      }
      output << '\n';
    }

    for (const draft::ImportBinding &binding : semantic.imports) {
      const ImportSite key{
          static_cast<std::uint32_t>(package_index),
          binding.syntax.file.value,
          binding.syntax.node.value,
      };
      const auto found = imports.find(key);
      assert(found != imports.end());
      const draft::PackageImport &edge = graph.imports[found->second];
      output << "import-binding " << package_index << ' '
             << binding.symbol.value << ' ' << edge.path << ' '
             << edge.imported_package.value << '\n';
    }
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
      std::string_view(argv[1]) != "workspace-declarations" ||
      std::string_view(argv[3]) != "--workspace" ||
      std::string_view(argv[5]) != "--core" ||
      std::string_view(argv[7]) != "--core-identity" ||
      std::string_view(argv[9]) != "--target" ||
      !valid_target_selector(argv[10])) {
    std::cerr
        << "usage:\n"
           "  draft-bootstrap-workspace-declarations workspace-declarations "
           "<root-package> --workspace <workspace> --core <core-root> "
           "--core-identity <identity> --target <selector> [--dependency "
           "<prefix> <root> <identity>]...\n";
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
    options.dependencies.push_back(
        {argv[index + 1], argv[index + 2], argv[index + 3]});
  }

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::WorkspaceLoadResult loaded =
      draft::load_workspace(sources, argv[2], options, diagnostics);

  std::vector<draft::SemanticPackage> semantic_packages;
  if (loaded.ok) {
    semantic_packages.reserve(loaded.graph.packages.size());
    for (const draft::WorkspacePackage &package : loaded.graph.packages) {
      semantic_packages.push_back(draft::collect_package_declarations(
          sources, package.loaded, diagnostics));
    }
  }

  dump_workspace_graph(loaded.graph, std::cout);
  if (loaded.ok) {
    dump_declarations(loaded.graph, semantic_packages, std::cout);
  }
  const draft::DiagnosticSink normalized = normalized_diagnostics(diagnostics);
  std::cerr << draft::render_diagnostics(sources, normalized);
  if (!std::cout || !std::cerr) return EXIT_FAILURE;
  return loaded.ok && !diagnostics.has_errors() ? EXIT_SUCCESS : EXIT_FAILURE;
}
