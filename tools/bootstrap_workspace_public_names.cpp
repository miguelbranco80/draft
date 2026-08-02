// Bootstrap oracle for public names, target declarations, and typed interfaces.
//
// This non-installed qualification executable combines two production C++
// boundaries without changing either one. The workspace loader and declaration
// collector produce the exact graph, scopes, symbols, and early diagnostics
// used by the previous migration gate. A separate complete provider-free
// compilation then supplies canonical PackageInterface declarations,
// ImportedSymbol rows, and published ConditionalSelections. The unconditional
// command intersects interface rows with the raw collector. The target command
// replays those production selections through
// materialize_conditional_declaration on a fresh source-level package and
// compares the appended SymbolId suffix plus the selected public-name view.
// The interface command retains that complete prefix, then emits the reachable
// production scalar/closed-structural interface graph and structurally
// normalized consumer-local imported types and values.
//
// The early SourceManager owns every byte referenced by the graph and raw
// semantic packages until their dump and diagnostics finish. The complete
// compilation uses an independent SourceManager because its typed products are
// consulted only for public-name acceptance in unconditional mode. Target-mode
// selection replay uses only compiled-source FileIds with the matching compiled
// graph; no FileId crosses SourceManager lifetimes. The interface fixture is
// fully valid and intentionally stays inside Draft's diagnosed staging subset.

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

// Keep the oracle spelling independent of host enum ordinals and identical to
// the Draft dump's closed canonical kind vocabulary.
[[nodiscard]] const char *type_kind_text(draft::TypeKind kind) {
  switch (kind) {
  case draft::TypeKind::Invalid: return "invalid";
  case draft::TypeKind::Void: return "void";
  case draft::TypeKind::UntypedInteger: return "untyped-integer";
  case draft::TypeKind::UntypedFloat: return "untyped-float";
  case draft::TypeKind::Bool: return "bool";
  case draft::TypeKind::BooleanStorage: return "boolean-storage";
  case draft::TypeKind::SignedInteger: return "signed-integer";
  case draft::TypeKind::UnsignedInteger: return "unsigned-integer";
  case draft::TypeKind::Float: return "float";
  case draft::TypeKind::Rune: return "rune";
  case draft::TypeKind::EndianScalar: return "endian-scalar";
  case draft::TypeKind::RawPointer: return "raw-pointer";
  case draft::TypeKind::CString: return "c-string";
  case draft::TypeKind::String: return "string";
  case draft::TypeKind::Pointer: return "pointer";
  case draft::TypeKind::MultiPointer: return "multi-pointer";
  case draft::TypeKind::Slice: return "slice";
  case draft::TypeKind::Array: return "array";
  case draft::TypeKind::Tuple: return "tuple";
  case draft::TypeKind::Procedure: return "procedure";
  case draft::TypeKind::Simd: return "simd";
  case draft::TypeKind::Struct: return "struct";
  case draft::TypeKind::Enum: return "enum";
  case draft::TypeKind::Variant: return "variant";
  case draft::TypeKind::Union: return "union";
  case draft::TypeKind::Distinct: return "distinct";
  case draft::TypeKind::TypeParameter: return "type-parameter";
  case draft::TypeKind::MetaType: return "meta-type";
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

// Initial and selected declarations share one row schema. label is the only
// phase distinction; every identity, source, flag, and native spelling comes
// from the source-level SemanticPackage being qualified.
void dump_symbol(
    std::string_view label, const draft::WorkspacePackage &workspace,
    const draft::SemanticPackage &semantic, std::size_t package_index,
    std::size_t symbol_index, std::ostream &output) {
  const draft::SymbolId id{static_cast<std::uint32_t>(symbol_index)};
  const draft::Symbol &symbol = semantic.symbols.symbol(id);
  const draft::NativeBinding *native = native_binding(semantic, id);
  output << label << package_index << ' ' << symbol_index << ' '
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
      dump_symbol(
          "symbol ", workspace, semantic, package_index, symbol_index, output);
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

// Replaying production's completed selections through the production source
// collector isolates the exact phase Draft is replacing. The fresh package has
// no imported interface proxies or typed mutations, so its unconditional
// SymbolId prefix matches the previous gate and each chosen branch appends in
// the same materialization order as Draft. Selection replay is incremental:
// passing production's complete final table while materializing an outer false
// branch would let the collector consume a nested `else when` immediately,
// rather than expose it as the next product in the recorded frontier.
[[nodiscard]] bool rebuild_target_declarations(
    const draft::SourceManager &sources,
    const draft::CompileWorkspaceResult &compiled,
    std::vector<draft::SemanticPackage> &selected_packages,
    std::vector<std::vector<draft::ConditionalSelection>> &selected_conditions,
    draft::DiagnosticSink &diagnostics) {
  assert(compiled.ok);
  selected_packages.reserve(compiled.graph.packages.size());
  selected_conditions.resize(compiled.graph.packages.size());
  for (std::size_t package_index = 0;
       package_index < compiled.graph.packages.size(); ++package_index) {
    assert(compiled.packages[package_index].has_value());
    const draft::WorkspacePackage &workspace =
        compiled.graph.packages[package_index];
    const draft::ConditionalSelections &published =
        compiled.packages[package_index]->declarations.selections;
    selected_packages.push_back(draft::collect_package_declarations(
        sources, workspace.loaded, diagnostics));
    draft::SemanticPackage &selected = selected_packages.back();
    draft::ConditionalSelections replayed;

    std::size_t condition_index = 0;
    while (condition_index < selected.conditional_declarations.size()) {
      const draft::SyntaxReference site =
          selected.conditional_declarations[condition_index].syntax;
      const draft::ConditionalSelection *selection = published.find(site);
      if (selection == nullptr) {
        diagnostics.error(
            draft::SourceRange::invalid(),
            "production package has no selection for a target declaration");
        return false;
      }
      selected_conditions[package_index].push_back(*selection);
      replayed.entries.push_back(*selection);
      if (!draft::materialize_conditional_declaration(
              sources, workspace.loaded, replayed, site, selected,
              diagnostics)) {
        return false;
      }
      ++condition_index;
    }
  }
  return !diagnostics.has_errors();
}

void dump_target_declarations(
    const draft::TargetProfile &profile, const draft::WorkspaceGraph &graph,
    const std::vector<draft::SemanticPackage> &unconditional_packages,
    const std::vector<draft::SemanticPackage> &selected_packages,
    const std::vector<std::vector<draft::ConditionalSelection>> &conditions,
    std::ostream &output) {
  assert(unconditional_packages.size() == selected_packages.size());
  assert(selected_packages.size() == graph.packages.size());
  assert(conditions.size() == selected_packages.size());
  const draft::TargetFacts &facts = profile.facts;
  output << "target-profile " << facts.identity << ' ' << facts.arch << ' '
         << facts.os << ' ' << facts.abi << ' ' << facts.byte_order << ' '
         << facts.object_format << ' ' << facts.file_tag << ' '
         << facts.pointer_bits << ' ' << facts.page_size << '\n';

  for (std::size_t package_index = 0;
       package_index < selected_packages.size(); ++package_index) {
    const draft::WorkspacePackage &workspace = graph.packages[package_index];
    for (const draft::ConditionalSelection &selection :
         conditions[package_index]) {
      output << "condition " << package_index << ' '
             << relative_file_name(workspace, selection.site.file) << ' '
             << selection.site.node.value << ' '
             << (selection.select_true ? '1' : '0') << '\n';
    }

    const std::size_t prefix =
        unconditional_packages[package_index].symbols.symbol_count();
    const draft::SemanticPackage &selected = selected_packages[package_index];
    assert(prefix <= selected.symbols.symbol_count());
    output << "selected-symbol-set " << package_index << ' ' << prefix << ' '
           << selected.symbols.symbol_count() - prefix << '\n';
    for (std::size_t symbol_index = prefix;
         symbol_index < selected.symbols.symbol_count(); ++symbol_index) {
      dump_symbol(
          "selected-symbol ", workspace, selected, package_index,
          symbol_index, output);
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
// names bound beneath each consumer alias. The supplied source-only package
// table may be unconditional or target-materialized; its lookup maps both
// products back into the exact phase-local ID domain compared with Draft.
void dump_public_names(
    const draft::WorkspaceGraph &source_graph,
    const std::vector<draft::SemanticPackage> &source_packages,
    const draft::CompileWorkspaceResult &compiled,
    std::ostream &output) {
  assert(compiled.ok);
  assert(compiled.packages.size() == source_packages.size());
  assert(compiled.graph.packages.size() == source_packages.size());
  const std::map<ImportSite, std::size_t> imports =
      index_import_sites(source_graph);

  for (std::size_t package_index = 0;
       package_index < source_packages.size(); ++package_index) {
    assert(compiled.packages[package_index].has_value());
    const draft::CompiledPackage &compiled_package =
        *compiled.packages[package_index];
    const draft::SemanticPackage &source_package =
        source_packages[package_index];
    const draft::PackageInterface &interface = compiled_package.interface;

    std::size_t public_name_count = 0;
    for (const draft::InterfaceDeclaration &declaration :
         interface.declarations) {
      if (public_source_symbol(source_package, declaration.name).has_value()) {
        ++public_name_count;
      }
    }
    output << "public-name-set " << package_index << ' '
           << public_name_count << '\n';
    for (const draft::InterfaceDeclaration &declaration :
         interface.declarations) {
      const std::optional<draft::SymbolId> source =
          public_source_symbol(source_package, declaration.name);
      if (!source.has_value()) continue;
      output << "public-name " << package_index << ' ' << source->value << ' '
             << declaration.name << '\n';
    }

    const draft::SemanticPackage &bound =
        compiled_package.declarations.package;
    const std::vector<draft::ImportBinding> &bound_imports =
        bound.imports_for_read();
    assert(bound_imports.size() == source_package.imports.size());
    for (std::size_t binding_index = 0;
         binding_index < source_package.imports.size(); ++binding_index) {
      const draft::ImportBinding &source_binding =
          source_package.imports[binding_index];
      const draft::ImportBinding &bound_binding =
          bound_imports[binding_index];
      assert(source_binding.package_path == bound_binding.package_path);
      const ImportSite key{
          static_cast<std::uint32_t>(package_index),
          source_binding.syntax.file.value,
          source_binding.syntax.node.value,
      };
      const auto found_edge = imports.find(key);
      assert(found_edge != imports.end());
      const draft::PackageImport &edge =
          source_graph.imports[found_edge->second];

      std::size_t member_count = 0;
      for (const draft::ImportedSymbol &imported :
           bound.imported_symbols_for_read()) {
        if (imported.import_symbol == bound_binding.symbol &&
            public_source_symbol(
                source_packages[edge.imported_package.value],
                imported.public_name).has_value()) {
          ++member_count;
        }
      }
      output << "imported-package " << package_index << ' '
             << source_binding.symbol.value << ' '
             << edge.imported_package.value << ' ' << member_count << '\n';

      for (const draft::ImportedSymbol &imported :
           bound.imported_symbols_for_read()) {
        if (imported.import_symbol != bound_binding.symbol) continue;
        const draft::SemanticPackage &target =
            source_packages[edge.imported_package.value];
        const std::optional<draft::SymbolId> source =
            public_source_symbol(target, imported.public_name);
        if (!source.has_value()) continue;
        output << "imported-name " << package_index << ' '
               << source_binding.symbol.value << ' '
               << edge.imported_package.value << ' ' << source->value << ' '
               << imported.public_name << '\n';
      }
    }
  }
}

// Render only the constant payload facts compared by this migration gate.
// Type indices are already in the producer InterfaceTypeId domain.
void append_interface_constant(
    const draft::ConstantValue &value, std::ostream &output) {
  switch (value.kind) {
  case draft::ConstantKind::Bool:
    output << "bool " << (value.boolean ? '1' : '0');
    return;
  case draft::ConstantKind::Integer:
    output << "integer " << value.integer.to_decimal();
    return;
  case draft::ConstantKind::String:
    output << "string " << value.text.size() << ' ' << value.text;
    return;
  case draft::ConstantKind::Type:
    output << "type " << value.type_index;
    return;
  case draft::ConstantKind::EnumLabel:
    output << "target-category " << value.text;
    return;
  case draft::ConstantKind::Unavailable:
  case draft::ConstantKind::Nil:
  case draft::ConstantKind::Float:
  case draft::ConstantKind::Aggregate:
  case draft::ConstantKind::Procedure:
  case draft::ConstantKind::Target:
    output << "invalid";
    return;
  }
}

// Imported TypeIds are deliberately normalized structurally. A complete
// production compilation interns many private types which are outside this
// migration slice, so comparing process-local integers would measure unrelated
// work rather than consumer-side interface reconstruction. Structural children
// use canonical Draft syntax, while scalar leaves retain kind and spelling.
void append_local_type_shape(
    const draft::TypeStore &types, draft::TypeId id, std::ostream &output) {
  assert(id.is_valid());
  const draft::Type &type = types.type(id);
  if (type.kind == draft::TypeKind::Pointer ||
      type.kind == draft::TypeKind::MultiPointer ||
      type.kind == draft::TypeKind::Slice) {
    if (type.kind == draft::TypeKind::Pointer) {
      output << '^';
    } else if (type.kind == draft::TypeKind::MultiPointer) {
      output << "[^]";
    } else {
      output << "[]";
    }
    append_local_type_shape(types, type.element, output);
    return;
  }
  if (type.kind == draft::TypeKind::Array) {
    output << '[' << type.element_count << ']';
    append_local_type_shape(types, type.element, output);
    return;
  }
  if (type.kind == draft::TypeKind::Tuple) {
    output << '(';
    for (std::size_t index = 0; index < type.members.size(); ++index) {
      if (index != 0) output << ',';
      append_local_type_shape(types, type.members[index], output);
    }
    output << ')';
    return;
  }
  if (type.kind == draft::TypeKind::Procedure) {
    assert(!type.members.empty());
    output << "procedure(";
    for (std::size_t index = 0; index + 1 < type.members.size(); ++index) {
      if (index != 0) output << ',';
      append_local_type_shape(types, type.members[index], output);
    }
    output << ")->";
    append_local_type_shape(types, type.members.back(), output);
    return;
  }
  output << type_kind_text(type.kind) << ':'
         << (type.name.empty() ? "-" : type.name);
}

// Imported type constants have already been rewritten back to a consumer
// TypeId. Expand that ID structurally; every other scalar payload is domain-
// independent and shares the producer renderer.
void append_local_constant(
    const draft::TypeStore &types, const draft::ConstantValue &value,
    std::ostream &output) {
  if (value.kind != draft::ConstantKind::Type) {
    append_interface_constant(value, output);
    return;
  }
  output << "type ";
  append_local_type_shape(types, draft::TypeId{value.type_index}, output);
}

// The typed-interface dump is intentionally narrower than PackageInterface's
// final serialization contract. It compares the canonical scalar and closed
// structural graph moved so far and verifies that every dependency declaration
// was reconstructed as a consumer-local proxy type/value beneath the exact
// alias.
void dump_interfaces(
    const draft::WorkspaceGraph &source_graph,
    const std::vector<draft::SemanticPackage> &source_packages,
    const draft::CompileWorkspaceResult &compiled, std::ostream &output) {
  assert(compiled.ok);
  assert(source_packages.size() == compiled.packages.size());
  const std::map<ImportSite, std::size_t> imports =
      index_import_sites(source_graph);

  for (std::size_t package_index = 0;
       package_index < compiled.packages.size(); ++package_index) {
    assert(compiled.packages[package_index].has_value());
    const draft::CompiledPackage &compiled_package =
        *compiled.packages[package_index];
    const draft::PackageInterface &interface = compiled_package.interface;
    output << "interface-package " << package_index << ' '
           << interface.types.size() << ' ' << interface.declarations.size()
           << '\n';

    for (std::size_t type_index = 0; type_index < interface.types.size();
         ++type_index) {
      const draft::InterfaceType &type = interface.types[type_index];
      output << "interface-type " << package_index << ' ' << type_index << ' '
             << type_kind_text(type.kind) << ' '
             << (type.name.empty() ? "-" : type.name) << ' '
             << type.bit_width << ' ';
      if (type.element.is_valid()) {
        output << type.element.value;
      } else {
        output << "invalid";
      }
      output << ' ' << type.element_count << ' ' << type.members.size();
      for (draft::InterfaceTypeId member : type.members) {
        output << ' ' << member.value;
      }
      output << '\n';
    }

    for (std::size_t declaration_index = 0;
         declaration_index < interface.declarations.size();
         ++declaration_index) {
      const draft::InterfaceDeclaration &declaration =
          interface.declarations[declaration_index];
      output << "interface-declaration " << package_index << ' '
             << declaration_index << ' ' << declaration.name << ' '
             << symbol_kind_text(declaration.kind) << ' '
             << (declaration.flags.is_thread_local ? '1' : '0')
             << (declaration.flags.foreign ? '1' : '0')
             << (declaration.flags.exported ? '1' : '0')
             << (declaration.flags.parametric ? '1' : '0') << ' '
             << declaration.type.value << ' ';
      if (declaration.has_constant) {
        append_interface_constant(declaration.constant, output);
      } else {
        output << "none";
      }
      output << '\n';
    }

    const draft::SemanticPackage &source_package =
        source_packages[package_index];
    const draft::SemanticPackage &bound =
        compiled_package.declarations.package;
    const std::vector<draft::ImportBinding> &bound_imports =
        bound.imports_for_read();
    assert(bound_imports.size() == source_package.imports.size());
    for (std::size_t binding_index = 0;
         binding_index < source_package.imports.size(); ++binding_index) {
      const draft::ImportBinding &source_binding =
          source_package.imports[binding_index];
      const draft::ImportBinding &bound_binding = bound_imports[binding_index];
      const ImportSite key{
          static_cast<std::uint32_t>(package_index),
          source_binding.syntax.file.value,
          source_binding.syntax.node.value,
      };
      const auto found_edge = imports.find(key);
      assert(found_edge != imports.end());
      const draft::PackageImport &edge =
          source_graph.imports[found_edge->second];
      assert(compiled.packages[edge.imported_package.value].has_value());
      const draft::PackageInterface &target_interface =
          compiled.packages[edge.imported_package.value]->interface;

      output << "typed-import " << package_index << ' '
             << source_binding.symbol.value << ' '
             << target_interface.declarations.size() << '\n';
      for (std::size_t ordinal = 0;
           ordinal < target_interface.declarations.size(); ++ordinal) {
        const draft::InterfaceDeclaration &target_declaration =
            target_interface.declarations[ordinal];
        const draft::ImportedSymbol *matched = nullptr;
        for (const draft::ImportedSymbol &imported :
             bound.imported_symbols_for_read()) {
          if (imported.import_symbol == bound_binding.symbol &&
              imported.public_name == target_declaration.name) {
            matched = &imported;
            break;
          }
        }
        assert(matched != nullptr);
        const draft::Symbol &proxy = bound.symbols.symbol(matched->proxy);
        output << "typed-import-name " << package_index << ' '
               << source_binding.symbol.value << ' '
               << edge.imported_package.value << ' ' << ordinal << ' '
               << target_declaration.name << ' ';
        append_local_type_shape(bound.types, proxy.type, output);
        output << ' ';
        if (matched->has_constant) {
          append_local_constant(bound.types, matched->constant, output);
        } else {
          output << "none";
        }
        output << '\n';
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
  const bool public_names_command =
      argc >= 2 && std::string_view(argv[1]) == "workspace-public-names";
  const bool target_declarations_command =
      argc >= 2 &&
      std::string_view(argv[1]) == "workspace-target-declarations";
  const bool interfaces_command =
      argc >= 2 && std::string_view(argv[1]) == "workspace-interfaces";
  const bool selected_source_command =
      target_declarations_command || interfaces_command;
  if (argc < 11 || (argc - 11) % 4 != 0 ||
      (!public_names_command && !target_declarations_command &&
       !interfaces_command) ||
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
           "<prefix> <root> <identity>]...\n"
           "  draft-bootstrap-workspace-public-names "
           "workspace-target-declarations <root-package> --workspace "
           "<workspace> --core <core-root> --core-identity <identity> "
           "--target <selector> [--dependency <prefix> <root> "
           "<identity>]...\n"
           "  draft-bootstrap-workspace-public-names "
           "workspace-interfaces <root-package> --workspace <workspace> "
           "--core <core-root> --core-identity <identity> --target "
           "<selector> [--dependency <prefix> <root> <identity>]...\n";
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
  draft::TargetProfile target_profile;
  draft::SourceManager compiled_sources;
  draft::DiagnosticSink compiled_diagnostics;
  if (loaded.ok && !raw_diagnostics.has_errors()) {
    draft::CompileWorkspaceOptions compile_options;
    std::string reason;
    if (!draft::select_builtin_target_profile(
            argv[10], target_profile, reason)) {
      std::cerr << "error: " << reason << '\n';
      return EXIT_FAILURE;
    }
    compile_options.target = target_profile;
    compile_options.workspace = workspace_options;
    compiled.emplace(draft::compile_workspace(
        compiled_sources, argv[2], compile_options, compiled_diagnostics));
  }

  std::vector<draft::SemanticPackage> selected_packages;
  std::vector<std::vector<draft::ConditionalSelection>> selected_conditions;
  draft::DiagnosticSink selection_diagnostics;
  bool selected_ok = !selected_source_command;
  if (selected_source_command && compiled.has_value() && compiled->ok) {
    selected_ok = rebuild_target_declarations(
        compiled_sources, *compiled, selected_packages, selected_conditions,
        selection_diagnostics);
  }

  dump_workspace_graph(loaded.graph, std::cout);
  if (loaded.ok) dump_declarations(loaded.graph, raw_packages, std::cout);
  if (compiled.has_value() && compiled->ok) {
    if (selected_source_command && selected_ok) {
      dump_target_declarations(
          target_profile, compiled->graph, raw_packages, selected_packages,
          selected_conditions, std::cout);
      dump_public_names(
          compiled->graph, selected_packages, *compiled, std::cout);
      if (interfaces_command) {
        dump_interfaces(
            compiled->graph, selected_packages, *compiled, std::cout);
      }
    } else if (public_names_command) {
      dump_public_names(loaded.graph, raw_packages, *compiled, std::cout);
    }
  }

  const draft::DiagnosticSink normalized_raw =
      normalized_diagnostics(raw_diagnostics);
  std::cerr << draft::render_diagnostics(raw_sources, normalized_raw);
  const draft::DiagnosticSink normalized_compiled =
      normalized_diagnostics(compiled_diagnostics);
  std::cerr << draft::render_diagnostics(
      compiled_sources, normalized_compiled);
  std::cerr << draft::render_diagnostics(
      compiled_sources, selection_diagnostics);
  if (!std::cout || !std::cerr) return EXIT_FAILURE;
  return loaded.ok && !raw_diagnostics.has_errors() &&
          compiled.has_value() && compiled->ok &&
          !compiled_diagnostics.has_errors() && selected_ok &&
          !selection_diagnostics.has_errors()
      ? EXIT_SUCCESS
      : EXIT_FAILURE;
}
