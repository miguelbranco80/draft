// Provider-free workspace compiler orchestration.

#include "compile/compiler.h"

#include "base/sha256.h"
#include "elaborator/generated_source.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"
#include "elaborator/resolved_program.h"
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

// Resolution pins apply only to grammar-producing sites. Documentation and
// judgments remain surface metadata and follow their separate evidence path.
[[nodiscard]] bool is_synthesis_obligation(AgentConstructKind kind) {
  return kind == AgentConstructKind::SynthesisDeclaration ||
      kind == AgentConstructKind::SynthesisMember ||
      kind == AgentConstructKind::SynthesisStatement ||
      kind == AgentConstructKind::SynthesisExpression ||
      kind == AgentConstructKind::SynthesisAssembly;
}

// Recovers the diagnostic range for a process-local obligation after either
// the surface or resolved package pass. Invalid compiler rows are reported at
// no source rather than turned into assertions on user-controlled input.
[[nodiscard]] SourceRange obligation_range(
    const WorkspacePackage &package,
    const AgentObligation &obligation) {
  for (const LoadedPackageFile &file : package.loaded.files) {
    if (file.source != obligation.syntax.file || !file.syntax.has_value() ||
        !obligation.syntax.node.is_valid()) {
      continue;
    }
    return file.syntax->node(obligation.syntax.node).range;
  }
  return SourceRange::invalid();
}

// Tests persistent site membership across the two compilation passes. Kind is
// explicit because synthesis and judgment identities inhabit different policy
// domains even though both use the same `site-<digest>` spelling.
[[nodiscard]] bool contains_site(
    const CompileWorkspaceResult &compiled,
    AgentConstructKind kind,
    std::string_view site_identity) {
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation : package->obligations.obligations) {
      if (obligation.kind == kind &&
          obligation.site_identity == site_identity) {
        return true;
      }
    }
  }
  return false;
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

// Builds the borrowed package view required by the byte-overlay layer. The
// compiler result owns every pointer for the duration of the caller's pass.
[[nodiscard]] std::vector<ResolutionSurfacePackage> resolution_packages(
    const CompileWorkspaceResult &compiled) {
  std::vector<ResolutionSurfacePackage> result;
  for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
    if (!compiled.packages[index].has_value()) continue;
    const CompiledPackage &package = *compiled.packages[index];
    result.push_back({
        &package.identity,
        &compiled.graph.packages[index].loaded,
        &package.obligations,
    });
  }
  return result;
}

[[nodiscard]] std::size_t synthesis_site_count(
    const CompileWorkspaceResult &compiled) {
  std::size_t result = 0;
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation : package->obligations.obligations) {
      if (is_synthesis_obligation(obligation.kind)) ++result;
    }
  }
  return result;
}

// Selects only pins belonging to one staged surface. The full manifest also
// contains the other stage, but build_resolution_overlays correctly rejects
// unrelated pins. matched is parallel to the full canonical manifest and lets
// the orchestrator reject obsolete rows after every stage has been observed.
[[nodiscard]] ResolutionManifest select_stage_manifest(
    const ResolutionManifest &full,
    const CompileWorkspaceResult &compiled,
    std::vector<bool> &matched) {
  ResolutionManifest result;
  result.target_identity = full.target_identity;
  result.resolved_program_digest = full.resolved_program_digest;
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation : package->obligations.obligations) {
      if (!is_synthesis_obligation(obligation.kind)) continue;
      for (std::size_t index = 0; index < full.pins.size(); ++index) {
        if (full.pins[index].site_identity != obligation.site_identity) continue;
        result.pins.push_back(full.pins[index]);
        matched[index] = true;
        break;
      }
    }
  }
  return result;
}

// Stored bytes are checked before they can create the next semantic stage.
// Hash verification belongs to load_generated_expansion; the lexical boundary
// independently rejects nested synthesis and generated judgment claims.
[[nodiscard]] bool validate_stage_expansions(
    SourceManager &sources,
    const std::filesystem::path &workspace_directory,
    const ResolutionManifest &manifest,
    DiagnosticSink &diagnostics) {
  for (const ResolutionPin &pin : manifest.pins) {
    std::string source;
    if (!load_generated_expansion(
            workspace_directory,
            pin.expansion_digest,
            source,
            diagnostics)) {
      return false;
    }
    if (!validate_generated_source_boundary(
            sources,
            "<generated/" + pin.site_identity + ">",
            source,
            diagnostics)) {
      return false;
    }
  }
  return true;
}

// A body-stage file is composed from the already overlaid interface file, so
// its complete bytes supersede the earlier row. Files touched only by the
// interface stage stay in the combined override set.
void merge_resolution_overrides(
    std::vector<WorkspaceSourceOverride> &combined,
    std::vector<WorkspaceSourceOverride> later) {
  for (WorkspaceSourceOverride &candidate : later) {
    bool replaced = false;
    for (WorkspaceSourceOverride &existing : combined) {
      if (existing.identity == candidate.identity &&
          existing.source.relative_name == candidate.source.relative_name) {
        existing.source.contents = std::move(candidate.source.contents);
        replaced = true;
        break;
      }
    }
    if (!replaced) combined.push_back(std::move(candidate));
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
  if (options.compiler_content_identity.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "compiler content identity must not be empty");
    return result;
  }
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

  // Declaration and member synthesis is an interface-stage operation. The
  // complete body pass cannot run yet because ordinary source is allowed to
  // name symbols and fields supplied by these sites. Metadata collection is
  // nevertheless valid: declaration collection and type skeleton resolution
  // have already installed the exact package/type scopes and visible symbols
  // available to each opaque completeness set.
  if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
    for (auto position = consumer_order.rbegin();
         position != consumer_order.rend(); ++position) {
      const std::size_t package_index = *position;
      if (!result.packages[package_index].has_value()) continue;
      CompiledPackage &package = *result.packages[package_index];
      WorkspacePackage &workspace_package = result.graph.packages[package_index];
      package.metadata = collect_agent_metadata(
          sources,
          workspace_package.loaded,
          package.semantics.package,
          options.attachments,
          diagnostics);
      package.obligations = build_agent_obligations(
          workspace_package.identity,
          sources,
          workspace_package.loaded,
          package.semantics.package,
          package.metadata,
          options.target,
          diagnostics);
      package.interface = build_package_interface(
          workspace_package.identity,
          package.semantics.package,
          package.semantics.constants,
          package.metadata,
          diagnostics);
    }
    bool every_package_ready = true;
    for (const std::optional<CompiledPackage> &package : result.packages) {
      every_package_ready = every_package_ready && package.has_value() &&
          package->semantics.ok && package->metadata.ok &&
          package->obligations.ok;
    }
    result.ok = every_package_ready &&
        diagnostics.error_count() == initial_errors;
    return result;
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
    package.obligations = build_agent_obligations(
        workspace_package.identity,
        sources,
        workspace_package.loaded,
        package.semantics.package,
        package.metadata,
        options.target,
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
    if (!package.metadata.ok || !package.obligations.ok || !denials_ok ||
        !package.native_interop.ok) {
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
    if (!package.bodies.ok || !package.metadata.ok || !package.obligations.ok ||
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

bool validate_resolved_agent_boundaries(
    const CompileWorkspaceResult &surface,
    const CompileWorkspaceResult &resolved,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  for (std::size_t index = 0; index < resolved.packages.size(); ++index) {
    if (!resolved.packages[index].has_value()) continue;
    for (const AgentObligation &obligation :
         resolved.packages[index]->obligations.obligations) {
      if (is_synthesis_obligation(obligation.kind)) {
        diagnostics.error(
            obligation_range(resolved.graph.packages[index], obligation),
            "generated source may not contain a synthesis site");
      } else if (obligation.kind == AgentConstructKind::Judgment &&
                 !contains_site(
                     surface,
                     AgentConstructKind::Judgment,
                     obligation.site_identity)) {
        diagnostics.error(
            obligation_range(resolved.graph.packages[index], obligation),
            "generated source may not introduce a judgment");
      }
    }
  }
  for (const std::optional<CompiledPackage> &package : surface.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation : package->obligations.obligations) {
      if (obligation.kind == AgentConstructKind::Judgment &&
          !contains_site(
              resolved,
              AgentConstructKind::Judgment,
              obligation.site_identity)) {
        diagnostics.error(
            SourceRange::invalid(),
            "resolved source displaced a surface judgment site");
      }
    }
  }
  return diagnostics.error_count() == initial_errors;
}

CompileWorkspaceResult compile_workspace_with_resolution(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();

  // First discover declaration/member synthesis without checking dependent
  // bodies. Caller-supplied overrides are cleared so early site identity and
  // input hashes always originate in physical surface source, never old pins.
  CompileWorkspaceOptions interface_options = options;
  interface_options.stage = CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  interface_options.lower_mir = false;
  interface_options.emit_llvm = false;
  interface_options.workspace.source_overrides.clear();
  CompileWorkspaceResult interface_surface = compile_workspace(
      sources,
      root_package_directory,
      std::move(interface_options),
      diagnostics);
  if (!interface_surface.ok) return interface_surface;

  const ResolutionManifestLoadResult loaded_manifest =
      load_resolution_manifest(options.workspace.workspace_directory, diagnostics);
  if (loaded_manifest.state == ResolutionManifestLoadState::Invalid) {
    interface_surface.ok = false;
    return interface_surface;
  }

  // A missing manifest can proceed only when the early stage has no synthesis.
  // Otherwise bodies cannot be checked soundly because their missing names and
  // layouts are precisely what declaration/member expansion is meant to add.
  if (loaded_manifest.state == ResolutionManifestLoadState::Missing) {
    if (synthesis_site_count(interface_surface) != 0) {
      diagnostics.error(
          SourceRange::invalid(),
          "workspace has unresolved synthesis sites and no resolution manifest; "
          "run 'draftc resolve'");
      interface_surface.ok = false;
      return interface_surface;
    }
  }

  std::vector<bool> matched_pins;
  ResolutionManifest interface_manifest;
  std::vector<WorkspaceSourceOverride> interface_overrides;
  if (loaded_manifest.state == ResolutionManifestLoadState::Loaded) {
    matched_pins.resize(loaded_manifest.manifest.pins.size(), false);
    interface_manifest = select_stage_manifest(
        loaded_manifest.manifest, interface_surface, matched_pins);
    if (!validate_stage_expansions(
            sources,
            options.workspace.workspace_directory,
            interface_manifest,
            diagnostics)) {
      interface_surface.ok = false;
      return interface_surface;
    }
    const ResolutionOverlayResult interface_overlay =
        build_resolution_overlays(
            sources,
            resolution_packages(interface_surface),
            interface_manifest,
            options.target.facts.identity,
            options.workspace.workspace_directory,
            {},
            diagnostics);
    if (!interface_overlay.ok) {
      interface_surface.ok = false;
      return interface_surface;
    }
    interface_overrides = interface_overlay.sources;
  }

  // With early interfaces installed, the ordinary full front end can derive
  // expected types and visible locals for body, expression, and assembly sites.
  CompileWorkspaceOptions body_options = options;
  body_options.stage = CompileWorkspaceStage::Complete;
  body_options.lower_mir = false;
  body_options.emit_llvm = false;
  body_options.workspace.source_overrides = interface_overrides;
  CompileWorkspaceResult body_surface = compile_workspace(
      sources, root_package_directory, body_options, diagnostics);
  if (!body_surface.ok) return body_surface;

  if (loaded_manifest.state == ResolutionManifestLoadState::Missing) {
    if (synthesis_site_count(body_surface) != 0) {
      diagnostics.error(
          SourceRange::invalid(),
          "workspace has unresolved synthesis sites and no resolution manifest; "
          "run 'draftc resolve'");
      body_surface.ok = false;
      return body_surface;
    }
    if (!options.lower_mir && !options.emit_llvm) return body_surface;
    options.stage = CompileWorkspaceStage::Complete;
    options.workspace.source_overrides.clear();
    return compile_workspace(
        sources, root_package_directory, std::move(options), diagnostics);
  }

  ResolutionManifest body_manifest = select_stage_manifest(
      loaded_manifest.manifest, body_surface, matched_pins);
  if (!validate_stage_expansions(
          sources,
          options.workspace.workspace_directory,
          body_manifest,
          diagnostics)) {
    body_surface.ok = false;
    return body_surface;
  }
  const ResolutionOverlayResult body_overlay = build_resolution_overlays(
      sources,
      resolution_packages(body_surface),
      body_manifest,
      options.target.facts.identity,
      options.workspace.workspace_directory,
      {},
      diagnostics);
  if (!body_overlay.ok) {
    body_surface.ok = false;
    return body_surface;
  }

  // Every full-manifest pin must belong to exactly one selected stage. An
  // obsolete pin from another graph cannot be silently ignored by an otherwise
  // successful offline or locked build.
  for (std::size_t index = 0; index < matched_pins.size(); ++index) {
    if (!matched_pins[index]) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolution manifest pin does not match a selected synthesis site '" +
              loaded_manifest.manifest.pins[index].site_identity + "'");
    }
  }
  if (diagnostics.error_count() != initial_errors) {
    body_surface.ok = false;
    return body_surface;
  }

  std::vector<WorkspaceSourceOverride> complete_overrides =
      std::move(interface_overrides);
  merge_resolution_overrides(
      complete_overrides, body_overlay.sources);
  options.stage = CompileWorkspaceStage::Complete;
  options.workspace.source_overrides = std::move(complete_overrides);
  CompileWorkspaceResult resolved = compile_workspace(
      sources, root_package_directory, options, diagnostics);
  if (!resolved.ok) return resolved;

  // Generated source is allowed to contain ordinary docs but not another
  // provider operation. Every surface synthesis site was removed by the
  // overlay, so any remaining synthesis obligation necessarily came from an
  // expansion. Judgment identities must be exactly the surface set; input
  // digests may legitimately change after generated declarations become
  // visible and are therefore not compared here.
  (void)validate_resolved_agent_boundaries(
      body_surface, resolved, diagnostics);
  if (diagnostics.error_count() == initial_errors) {
    const Sha256Digest program_digest = hash_resolved_program(
        sources,
        resolved.graph,
        options.target,
        loaded_manifest.manifest,
        options.compiler_content_identity);
    if (program_digest != loaded_manifest.manifest.resolved_program_digest) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolution manifest resolved-program identity is stale");
    }
  }
  resolved.ok = diagnostics.error_count() == initial_errors;
  return resolved;
}

} // namespace draft
