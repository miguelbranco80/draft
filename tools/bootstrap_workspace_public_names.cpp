// Bootstrap oracle for public declaration names and qualified import lookup.
//
// This non-installed qualification executable combines two production C++
// boundaries without changing either one. The workspace loader and declaration
// collector produce the exact graph, scopes, symbols, and early diagnostics
// used by the previous migration gate. A separate complete provider-free
// compilation then supplies canonical PackageInterface declarations and the
// ImportedSymbol rows created by production interface binding. The dump keeps
// the intersection with the raw collector's direct public symbols and maps
// those names back to their source declaration SymbolIds. This qualifies the
// unconditional public-name seam without pretending that the Draft stage has
// already selected conditional declarations or reconstructed typed interfaces.
//
// The early SourceManager owns every byte referenced by the graph and raw
// semantic packages until their dump and diagnostics finish. The complete
// compilation uses an independent SourceManager because its typed products are
// consulted only for public-name acceptance; no FileId crosses between the two
// lifetimes. Fixtures are fully valid and may contain public conditional
// declarations, but the dump deliberately filters those names from both the
// final interface and imported-symbol views because conditional selection and
// typed interface reconstruction remain later self-hosting gates.

#include "compile/compiler.h"
#include "sema/analyzer.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/workspace.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
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

// FileId is SourceManager-global while the self-hosted graph uses one package-
// local file-row index. Relative names are the stable display and ordering
// domain shared by both implementations.
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

// This index is queried by exact syntax identity only. Map iteration never
// contributes output ordering; graph and semantic vectors remain authoritative.
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

// The declaration portion remains byte-identical to the preceding phase gate.
// Keeping it in this oracle proves that public-name binding consumed the same
// raw package symbols rather than reconstructing a second declaration table.
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

[[nodiscard]] std::optional<draft::SymbolId> public_source_symbol(
    const draft::SemanticPackage &package, std::string_view name) {
  const std::optional<draft::SymbolId> found =
      package.symbols.lookup_direct(package.package_scope, name);
  if (!found.has_value() ||
      package.symbols.symbol(*found).visibility != draft::Visibility::Public) {
    return std::nullopt;
  }
  return found;
}

// PackageInterface is the production authority for which public declarations
// a valid dependency actually exports. ImportedSymbol is the authority for the
// names bound beneath each consumer alias. The raw source symbol lookup maps
// both products back into the ID domain compared with Draft.
void dump_public_names(
    const draft::WorkspaceGraph &raw_graph,
    const std::vector<draft::SemanticPackage> &raw_packages,
    const draft::CompileWorkspaceResult &compiled,
    std::ostream &output) {
  assert(compiled.ok);
  assert(compiled.packages.size() == raw_packages.size());
  assert(compiled.graph.packages.size() == raw_packages.size());
  const std::map<ImportSite, std::size_t> imports =
      index_import_sites(raw_graph);

  for (std::size_t package_index = 0;
       package_index < raw_packages.size(); ++package_index) {
    assert(compiled.packages[package_index].has_value());
    const draft::CompiledPackage &compiled_package =
        *compiled.packages[package_index];
    const draft::SemanticPackage &raw_package = raw_packages[package_index];
    const draft::PackageInterface &interface = compiled_package.interface;

    std::size_t public_name_count = 0;
    for (const draft::InterfaceDeclaration &declaration :
         interface.declarations) {
      if (public_source_symbol(raw_package, declaration.name).has_value()) {
        ++public_name_count;
      }
    }
    output << "public-name-set " << package_index << ' '
           << public_name_count << '\n';
    for (const draft::InterfaceDeclaration &declaration :
         interface.declarations) {
      const std::optional<draft::SymbolId> source =
          public_source_symbol(raw_package, declaration.name);
      if (!source.has_value()) continue;
      output << "public-name " << package_index << ' ' << source->value << ' '
             << declaration.name << '\n';
    }

    const draft::SemanticPackage &bound =
        compiled_package.declarations.package;
    const std::vector<draft::ImportBinding> &bound_imports =
        bound.imports_for_read();
    assert(bound_imports.size() == raw_package.imports.size());
    for (std::size_t binding_index = 0;
         binding_index < raw_package.imports.size(); ++binding_index) {
      const draft::ImportBinding &raw_binding =
          raw_package.imports[binding_index];
      const draft::ImportBinding &bound_binding =
          bound_imports[binding_index];
      assert(raw_binding.package_path == bound_binding.package_path);
      const ImportSite key{
          static_cast<std::uint32_t>(package_index),
          raw_binding.syntax.file.value,
          raw_binding.syntax.node.value,
      };
      const auto found_edge = imports.find(key);
      assert(found_edge != imports.end());
      const draft::PackageImport &edge = raw_graph.imports[found_edge->second];

      std::size_t member_count = 0;
      for (const draft::ImportedSymbol &imported :
           bound.imported_symbols_for_read()) {
        if (imported.import_symbol == bound_binding.symbol &&
            public_source_symbol(
                raw_packages[edge.imported_package.value],
                imported.public_name).has_value()) {
          ++member_count;
        }
      }
      output << "imported-package " << package_index << ' '
             << raw_binding.symbol.value << ' '
             << edge.imported_package.value << ' ' << member_count << '\n';

      for (const draft::ImportedSymbol &imported :
           bound.imported_symbols_for_read()) {
        if (imported.import_symbol != bound_binding.symbol) continue;
        const draft::SemanticPackage &target =
            raw_packages[edge.imported_package.value];
        const std::optional<draft::SymbolId> source =
            public_source_symbol(target, imported.public_name);
        if (!source.has_value()) continue;
        output << "imported-name " << package_index << ' '
               << raw_binding.symbol.value << ' '
               << edge.imported_package.value << ' ' << source->value << ' '
               << imported.public_name << '\n';
      }
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
      std::string_view(argv[1]) != "workspace-public-names" ||
      std::string_view(argv[3]) != "--workspace" ||
      std::string_view(argv[5]) != "--core" ||
      std::string_view(argv[7]) != "--core-identity" ||
      std::string_view(argv[9]) != "--target" ||
      !valid_target_selector(argv[10])) {
    std::cerr
        << "usage:\n"
           "  draft-bootstrap-workspace-public-names workspace-public-names "
           "<root-package> --workspace <workspace> --core <core-root> "
           "--core-identity <identity> --target <selector> [--dependency "
           "<prefix> <root> <identity>]...\n";
    return EXIT_FAILURE;
  }

  draft::WorkspaceLoadOptions workspace_options;
  workspace_options.workspace_directory = argv[4];
  workspace_options.core_directory = argv[6];
  workspace_options.core_content_identity = argv[8];
  workspace_options.package_options.file_tag = argv[10];
  for (int index = 11; index < argc; index += 4) {
    if (std::string_view(argv[index]) != "--dependency") {
      std::cerr << "error: malformed dependency mapping\n";
      return EXIT_FAILURE;
    }
    workspace_options.dependencies.push_back(
        {argv[index + 1], argv[index + 2], argv[index + 3]});
  }

  draft::SourceManager raw_sources;
  draft::DiagnosticSink raw_diagnostics;
  const draft::WorkspaceLoadResult loaded = draft::load_workspace(
      raw_sources, argv[2], workspace_options, raw_diagnostics);

  std::vector<draft::SemanticPackage> raw_packages;
  if (loaded.ok) {
    raw_packages.reserve(loaded.graph.packages.size());
    for (const draft::WorkspacePackage &package : loaded.graph.packages) {
      raw_packages.push_back(draft::collect_package_declarations(
          raw_sources, package.loaded, raw_diagnostics));
    }
  }

  std::optional<draft::CompileWorkspaceResult> compiled;
  draft::SourceManager compiled_sources;
  draft::DiagnosticSink compiled_diagnostics;
  if (loaded.ok && !raw_diagnostics.has_errors()) {
    draft::CompileWorkspaceOptions compile_options;
    std::string reason;
    if (!draft::select_builtin_target_profile(
            argv[10], compile_options.target, reason)) {
      std::cerr << "error: " << reason << '\n';
      return EXIT_FAILURE;
    }
    compile_options.workspace = workspace_options;
    compiled.emplace(draft::compile_workspace(
        compiled_sources, argv[2], compile_options, compiled_diagnostics));
  }

  dump_workspace_graph(loaded.graph, std::cout);
  if (loaded.ok) dump_declarations(loaded.graph, raw_packages, std::cout);
  if (compiled.has_value() && compiled->ok) {
    dump_public_names(loaded.graph, raw_packages, *compiled, std::cout);
  }

  const draft::DiagnosticSink normalized_raw =
      normalized_diagnostics(raw_diagnostics);
  std::cerr << draft::render_diagnostics(raw_sources, normalized_raw);
  const draft::DiagnosticSink normalized_compiled =
      normalized_diagnostics(compiled_diagnostics);
  std::cerr << draft::render_diagnostics(
      compiled_sources, normalized_compiled);
  if (!std::cout || !std::cerr) return EXIT_FAILURE;
  return loaded.ok && !raw_diagnostics.has_errors() &&
          compiled.has_value() && compiled->ok &&
          !compiled_diagnostics.has_errors()
      ? EXIT_SUCCESS
      : EXIT_FAILURE;
}
