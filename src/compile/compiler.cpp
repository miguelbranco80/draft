// Provider-free workspace compiler orchestration.

#include "compile/compiler.h"

#include "base/sha256.h"
#include "sema/denial.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

// WorkspacePackage storage follows discovery order, which is deterministic but
// is not a topological order when two sibling packages share a dependency. This
// small Kahn traversal treats imports as consumer -> dependency edges. The
// resulting forward order lets every caller publish generic instance requests
// before the defining dependency is body-checked; reversing it gives the
// dependency-first order required by interface and effect publication.
[[nodiscard]] std::vector<std::size_t> consumer_first_order(
    const WorkspaceGraph &graph) {
  std::vector<std::size_t> importer_count(graph.packages.size(), 0);
  for (const PackageImport &edge : graph.imports) {
    if (edge.imported_package.is_valid() &&
        static_cast<std::size_t>(edge.imported_package.value) <
            importer_count.size()) {
      ++importer_count[edge.imported_package.value];
    }
  }

  std::vector<std::size_t> ready;
  for (std::size_t index = 0; index < importer_count.size(); ++index) {
    if (importer_count[index] == 0) ready.push_back(index);
  }
  std::vector<std::size_t> result;
  while (!ready.empty()) {
    const std::size_t current = ready.front();
    ready.erase(ready.begin());
    result.push_back(current);
    for (const PackageImport &edge : graph.imports) {
      if (static_cast<std::size_t>(edge.importing_package.value) != current) {
        continue;
      }
      const std::size_t dependency =
          static_cast<std::size_t>(edge.imported_package.value);
      if (dependency >= importer_count.size() || importer_count[dependency] == 0) {
        continue;
      }
      --importer_count[dependency];
      if (importer_count[dependency] == 0) {
        const auto position = std::lower_bound(
            ready.begin(), ready.end(), dependency);
        ready.insert(position, dependency);
      }
    }
  }
  return result;
}

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::uint8_t bytes[8];
  for (std::size_t index = 0; index < 8; ++index) {
    const std::size_t shift = (7 - index) * 8;
    bytes[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
  hash.update(std::span<const std::uint8_t>(bytes, 8));
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

[[nodiscard]] std::optional<std::size_t> package_index_for(
    const CompileWorkspaceResult &result,
    std::string_view root_identity,
    std::string_view root_relative_path) {
  for (std::size_t index = 0; index < result.graph.packages.size(); ++index) {
    const PackageIdentity &identity = result.graph.packages[index].identity;
    if (identity.root_identity == root_identity &&
        identity.root_relative_path == root_relative_path) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] const InterfaceDeclaration *find_interface_declaration(
    const PackageInterface &package, std::string_view name) {
  for (const InterfaceDeclaration &declaration : package.declarations) {
    if (declaration.name == name) return &declaration;
  }
  return nullptr;
}

// Preliminary interfaces intentionally contain no body-derived effects. Once
// dependency bodies are checked, refresh each already-bound proxy from the
// final dependency interface. Concrete generic proxies inherit the public
// template's audited summary, because their body is that template under a
// semantics-preserving type/value substitution.
void refresh_imported_effects(
    SemanticPackage &package,
    const CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  package.imported_effects.clear();
  for (ImportedSymbol &imported : package.imported_symbols) {
    std::string_view declaration_name = imported.public_name;
    for (const ImportedProcedureInstance &instance :
         package.imported_procedure_instances) {
      if (instance.instance_proxy == imported.proxy) {
        declaration_name = instance.public_template_name;
        break;
      }
    }
    const std::optional<std::size_t> dependency = package_index_for(
        result, imported.root_identity, imported.root_relative_path);
    if (!dependency.has_value() ||
        !result.packages[*dependency].has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot refresh effects for an unavailable imported package");
      continue;
    }
    const InterfaceDeclaration *declaration = find_interface_declaration(
        result.packages[*dependency]->interface, declaration_name);
    if (declaration == nullptr) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot refresh effects for missing imported declaration '" +
              std::string(declaration_name) + "'");
      continue;
    }
    imported.has_effect_summary = declaration->has_effect_summary;
    for (const InterfaceDeclaration::Effect &effect : declaration->effects) {
      package.imported_effects.push_back({
          imported.proxy,
          effect.kind,
          effect.root_identity,
          effect.root_relative_path,
          effect.declaration,
          effect.detail,
      });
    }
  }
}

} // namespace

CompileWorkspaceResult compile_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  CompileWorkspaceResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  std::string profile_error;
  if (!validate_target_profile(options.target, profile_error)) {
    diagnostics.error(
        SourceRange::invalid(), "invalid target profile: " + profile_error);
    return result;
  }
  options.workspace.package_options.file_tag = options.target.facts.file_tag;
  WorkspaceLoadResult loaded = load_workspace(
      sources,
      root_package_directory,
      options.workspace,
      diagnostics);
  result.graph = std::move(loaded.graph);
  result.packages.resize(result.graph.packages.size());
  if (!loaded.ok) return result;

  const std::vector<std::size_t> consumer_order =
      consumer_first_order(result.graph);
  if (consumer_order.size() != result.graph.packages.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "workspace package graph is cyclic after successful loading");
    return result;
  }

  // Phase 1: dependency-first declaration semantics and preliminary public
  // interfaces. Bodies and effects are intentionally absent, but every type,
  // constant, and parametric signature needed by a consumer is now available.
  for (auto position = consumer_order.rbegin();
       position != consumer_order.rend(); ++position) {
    const std::size_t package_index = *position;
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    AvailablePackageImports available;
    bool dependencies_ready = true;
    for (const PackageImport &import : result.graph.imports) {
      if (static_cast<std::size_t>(import.importing_package.value) != package_index) {
        continue;
      }
      const std::size_t dependency_index =
          static_cast<std::size_t>(import.imported_package.value);
      if (dependency_index >= result.packages.size() ||
          !result.packages[dependency_index].has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "dependency did not produce a package interface before its consumer");
        dependencies_ready = false;
        continue;
      }
      available.entries.push_back({
          {import.file, import.syntax},
          &result.packages[dependency_index]->interface,
      });
    }
    if (!dependencies_ready) continue;

    CompiledPackage package;
    package.identity = workspace_package.identity;
    // Freeze native assembly beside the semantic phase products. Native build
    // invocation is deliberately separate from compilation and must consume
    // this checked snapshot rather than mutable workspace paths.
    for (const LoadedPackageFile &file : workspace_package.loaded.files) {
      if (file.kind != PackageFileKind::AssemblySource) continue;
      package.assembly_sources.push_back({
          file.relative_name,
          std::string(sources.text(file.source)),
      });
    }
    package.semantics = analyze_package_semantics(
        sources,
        workspace_package.loaded,
        options.target.facts,
        available,
        diagnostics);
    if (!package.semantics.ok) continue;
    package.interface = build_package_interface(
        workspace_package.identity,
        package.semantics.package,
        package.semantics.constants,
        diagnostics);
    result.packages[package_index] = std::move(package);
  }

  // Phase 2: body-check from consumers toward dependencies. A checked caller
  // can request a public generic body from a package that has semantic tables
  // but has not yet entered its body pass. The type graph transfer below is the
  // only place a concrete argument crosses TypeStore ownership.
  std::vector<std::vector<ProcedureInstantiationSeed>> seeds(
      result.graph.packages.size());
  for (std::size_t package_index : consumer_order) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    package.bodies = check_package_bodies(
        sources,
        workspace_package.loaded,
        package.semantics.selections,
        package.semantics.package,
        package.semantics.constants,
        options.target.facts,
        diagnostics,
        seeds[package_index]);
    if (!package.bodies.ok) continue;

    for (const ImportedProcedureInstance &request :
         package.semantics.package.imported_procedure_instances) {
      const std::optional<std::size_t> owner_index = package_index_for(
          result, request.root_identity, request.root_relative_path);
      if (!owner_index.has_value() ||
          !result.packages[*owner_index].has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "generic procedure owner is unavailable during instantiation");
        continue;
      }

      CompiledPackage &owner = *result.packages[*owner_index];
      ProcedureInstantiationSeed seed;
      seed.public_template_name = request.public_template_name;
      Sha256 name_hash;
      hash_field(name_hash, "draft.procedure-instance.v1");
      hash_field(name_hash, request.root_identity);
      hash_field(name_hash, request.root_relative_path);
      hash_field(name_hash, request.public_template_name);
      for (const ParametricArgument &argument : request.arguments) {
        ParametricArgument transferred;
        transferred.is_type = argument.is_type;
        hash_u64(name_hash, argument.is_type ? 1 : 0);
        if (argument.is_type) {
          const InterfaceTypeGraph graph = export_interface_type(
              package.identity,
              package.semantics.package,
              argument.type,
              diagnostics);
          const Sha256Digest digest = hash_interface_type_graph(graph);
          name_hash.update(digest.bytes);
          transferred.type = import_interface_type(
              graph, owner.semantics.package, diagnostics);
        } else {
          const InterfaceTypeGraph graph = export_interface_type(
              package.identity,
              package.semantics.package,
              argument.value_type,
              diagnostics);
          const Sha256Digest digest = hash_interface_type_graph(graph);
          name_hash.update(digest.bytes);
          hash_field(name_hash, argument.value.integer.to_decimal());
          transferred.value_type = import_interface_type(
              graph, owner.semantics.package, diagnostics);
          transferred.value = argument.value;
        }
        seed.arguments.push_back(std::move(transferred));
      }
      seed.instance_name = request.public_template_name + "$mono$" +
          name_hash.finalize().hex().substr(0, 24);
      seeds[*owner_index].push_back(seed);

      bool named_proxy = false;
      for (ImportedSymbol &imported :
           package.semantics.package.imported_symbols) {
        if (imported.proxy == request.instance_proxy) {
          imported.public_name = seed.instance_name;
          named_proxy = true;
          break;
        }
      }
      if (!named_proxy) {
        diagnostics.error(
            SourceRange::invalid(),
            "generic procedure instance has no imported symbol proxy");
      }
    }
  }

  // Phase 3: dependencies now have every requested concrete body. Publish
  // audited effects and complete interfaces dependency-first, then compose
  // consumer denials against those final summaries.
  for (auto position = consumer_order.rbegin();
       position != consumer_order.rend(); ++position) {
    const std::size_t package_index = *position;
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    if (!package.bodies.ok) continue;

    refresh_imported_effects(package.semantics.package, result, diagnostics);
    package.metadata = collect_agent_metadata(
        sources,
        workspace_package.loaded,
        package.semantics.package,
        options.attachments,
        diagnostics);
    package.effects = summarize_package_effects(
        package.semantics.package, package.bodies.program);
    package.native_interop = validate_native_interop(
        package.semantics.package, package.bodies.program, diagnostics);
    const bool denials_ok = check_package_denials(
        sources,
        workspace_package.loaded,
        package.semantics.package,
        package.bodies.program,
        package.effects,
        diagnostics);
    package.interface = build_package_interface(
        workspace_package.identity,
        package.semantics.package,
        package.semantics.constants,
        package.metadata,
        package.effects,
        diagnostics);
    if (!package.metadata.ok || !denials_ok || !package.native_interop.ok) {
      continue;
    }
  }

  // Phase 4: provider-free target lowering. No package reaches a backend until
  // every cross-package generic proxy has an exact defining symbol.
  for (auto position = consumer_order.rbegin();
       position != consumer_order.rend(); ++position) {
    const std::size_t package_index = *position;
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    if (!package.bodies.ok || !package.metadata.ok ||
        !package.native_interop.ok) {
      continue;
    }

    if (options.lower_mir || options.emit_llvm) {
      package.assembly = analyze_aarch64_assembly(
          sources,
          workspace_package.loaded,
          options.target,
          package.semantics.package,
          package.bodies.program,
          diagnostics);
      if (!package.assembly.ok) continue;
      package.mir = lower_package_to_mir(
          package.semantics.package,
          package.bodies.program,
          package.assembly,
          diagnostics);
      if (!package.mir.ok) continue;
    }
    if (options.emit_llvm) {
      LlvmIrOptions llvm_options;
      llvm_options.package = workspace_package.identity;
      llvm_options.emit_program_entry =
          package_index ==
          static_cast<std::size_t>(result.graph.root_package.value);
      package.llvm = emit_llvm_ir(
          options.target,
          sources,
          llvm_options,
          package.semantics.package,
          package.semantics.global_initializers,
          package.mir.program,
          diagnostics);
      if (!package.llvm.ok) continue;
    }
  }

  bool every_package_ready = true;
  for (const std::optional<CompiledPackage> &package : result.packages) {
    every_package_ready = every_package_ready && package.has_value();
  }
  result.ok = every_package_ready &&
      diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
