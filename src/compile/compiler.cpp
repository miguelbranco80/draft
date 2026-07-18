// Provider-free workspace compiler orchestration.

#include "compile/compiler.h"

#include "base/sha256.h"
#include "elaborator/generated_source.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"
#include "elaborator/resolved_program.h"
#include "sema/denial.h"
#include "sema/type_resolver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
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

[[nodiscard]] bool has_synthesis_record(
    const AgentMetadataResult &metadata) {
  for (const AgentRecord &record : metadata.records) {
    if (is_synthesis_obligation(record.kind)) return true;
  }
  return false;
}

// A package with a ready interface-stage synthesis obligation cannot yet
// publish public declaration types. In particular, a synthesized array count
// leaves its public alias temporarily invalid. Consumers are suspended by
// has_interface_synthesis(), so retain only package identity until the overlay
// is installed and a clean round can build the complete canonical interface.
[[nodiscard]] PackageInterface withheld_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package) {
  PackageInterface result;
  result.identity = identity;
  result.short_name = package.short_name;
  return result;
}

// Only the interface scheduler may turn an evaluator stop into a provider
// obligation. The complete body pass runs after all interface overlays and
// therefore retains the ordinary rejecting semantic behavior.
[[nodiscard]] CompileTimeSynthesisMode compile_time_synthesis_mode(
    CompileWorkspaceStage stage) {
  return stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis
      ? CompileTimeSynthesisMode::Discover
      : CompileTimeSynthesisMode::Reject;
}

[[nodiscard]] bool has_semantic_site(
    const SemanticPackage &package, SemanticSiteKind kind) {
  for (const SemanticSite &site : package.sites) {
    if (site.kind == kind) return true;
  }
  return false;
}

// Package declarations may introduce any name used by a compile-time body, so
// they remain an opaque prerequisite. Aggregate-member sites are narrower: try
// the selected bodies against the current incomplete graph. A successful check
// proves those bodies independent enough to join the same provider round. A
// failure is deferred without mutating the authoritative graph; after member
// expansions land, the next clean round either discovers the sites or reports
// the real body error. Evaluator-owned direct sites need no speculative body
// check and remain in the current opaque set in every case.
[[nodiscard]] bool append_compile_time_body_synthesis_sites(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    SemanticAnalysisResult &semantics,
    DiagnosticSink &diagnostics) {
  if (semantics.compile_time_synthesis_procedures.empty() ||
      has_semantic_site(
          semantics.package, SemanticSiteKind::SynthesisDeclaration)) {
    return true;
  }

  const bool speculate = has_semantic_site(
      semantics.package, SemanticSiteKind::SynthesisMember);
  if (speculate) {
    SemanticPackage candidate_package = semantics.package;
    ConstantTable candidate_constants = semantics.constants;
    DiagnosticSink deferred_diagnostics;
    const BodyCheckResult candidate = check_compile_time_procedure_bodies(
        sources,
        loaded,
        semantics.selections,
        candidate_package,
        candidate_constants,
        target,
        semantics.compile_time_synthesis_procedures,
        deferred_diagnostics);
    if (!candidate.ok) return true;
    semantics.package = std::move(candidate_package);
    semantics.constants = std::move(candidate_constants);
    return true;
  }

  const BodyCheckResult checked = check_compile_time_procedure_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target,
      semantics.compile_time_synthesis_procedures,
      diagnostics);
  return checked.ok;
}

// Validation files stay outside the ordinary workspace graph, so handwritten
// builds do not acquire test-only imports or declarations. A package that has
// synthesis sites gets one parallel target-selected load with both validation
// roles enabled. Only its canonical test/benchmark rows survive this helper;
// host paths and the duplicate ordinary syntax do not enter obligations.
[[nodiscard]] std::vector<AgentValidationContext> load_validation_context(
    SourceManager &sources,
    const WorkspacePackage &workspace_package,
    const WorkspaceLoadOptions &workspace_options,
    DiagnosticSink &diagnostics) {
  PackageLoadOptions context_options = workspace_options.package_options;
  context_options.include_tests = true;
  context_options.include_benchmarks = true;
  context_options.source_overrides.clear();
  for (const WorkspaceSourceOverride &source_override :
       workspace_options.source_overrides) {
    if (source_override.identity == workspace_package.identity) {
      context_options.source_overrides.push_back(source_override.source);
    }
  }
  PackageLoadResult context_package = load_package(
      sources,
      workspace_package.loaded.physical_directory,
      context_options,
      diagnostics);
  if (!context_package.ok) return {};
  return collect_agent_validation_context(sources, context_package.package);
}

// A concrete owner result may contain several nested nominal applications.
// Every procedural count and full procedural value marker must be gone before
// the graph is published to a consumer. Compact integer expressions are also
// symbolic interface state; a concrete request must have reduced them to exact
// scalar values.
[[nodiscard]] bool owner_result_is_concrete(
    const InterfaceTypeGraph &graph) {
  for (const InterfaceType &type : graph.types) {
    if (type.kind == TypeKind::TypeParameter ||
        type.owner_evaluated_element_count ||
        type.owner_evaluated_type_application ||
        type.element_count_expression.is_valid()) {
      return false;
    }
    for (const InterfaceNominalArgument &argument : type.nominal_arguments) {
      if (!argument.is_type &&
          (argument.owner_evaluated_value ||
           argument.value_expression.is_valid())) {
        return false;
      }
    }
  }
  return true;
}

// Requests are process-local rows, so progress cannot be judged by TypeId or
// vector length across a semantic rebuild. This digest uses the same canonical
// interface graphs which cross package boundaries and therefore remains stable
// when a clean analysis assigns different local IDs.
[[nodiscard]] Sha256Digest hash_type_instantiation_requests(
    const CompiledPackage &requester,
    DiagnosticSink &diagnostics) {
  Sha256 hash;
  hash_field(hash, "draft.type-instantiation-requests.v1");
  for (const ImportedTypeInstantiationRequest &request :
       requester.semantics.package.imported_type_instantiation_requests) {
    hash_field(hash, request.root_identity);
    hash_field(hash, request.root_relative_path);
    hash_field(hash, request.public_template_name);
    hash_u64(hash, static_cast<std::uint64_t>(request.arguments.size()));
    for (const ParametricArgument &argument : request.arguments) {
      hash_u64(hash, argument.is_type ? 1 : 0);
      const TypeId type = argument.is_type
          ? argument.type
          : argument.value_type;
      const InterfaceTypeGraph graph = export_interface_type(
          requester.identity,
          requester.semantics.package,
          type,
          diagnostics);
      hash.update(hash_interface_type_graph(graph).bytes);
      if (argument.is_type) continue;
      hash_u64(hash, static_cast<std::uint64_t>(argument.value.kind));
      hash_field(hash, argument.value.integer.to_decimal());
      hash_u64(hash, argument.owner_evaluated_value ? 1 : 0);
      hash_u64(hash, argument.value_expression.is_valid() ? 1 : 0);
    }
  }
  return hash.finalize();
}

[[nodiscard]] bool collect_package_imports(
    const CompileWorkspaceResult &result,
    std::size_t package_index,
    AvailablePackageImports &available,
    DiagnosticSink &diagnostics) {
  if (package_index >= result.graph.packages.size()) return false;
  available.consumer_identity =
      result.graph.packages[package_index].identity;
  for (const PackageImport &import : result.graph.imports) {
    if (static_cast<std::size_t>(import.importing_package.value) !=
        package_index) {
      continue;
    }
    const std::size_t dependency_index =
        static_cast<std::size_t>(import.imported_package.value);
    if (dependency_index >= result.packages.size() ||
        !result.packages[dependency_index].has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "generic type owner dependency is unavailable during "
          "semantic rebuild");
      return false;
    }
    available.entries.push_back({
        {import.file, import.syntax},
        &result.packages[dependency_index]->interface,
    });
  }
  return true;
}

void append_instantiated_type(
    PackageInterface &interface,
    const InterfaceTypeGraph &graph) {
  const Sha256Digest digest = hash_interface_type_graph(graph);
  for (const InterfaceTypeGraph &existing : interface.instantiated_types) {
    if (hash_interface_type_graph(existing) == digest) return;
  }
  interface.instantiated_types.push_back(graph);
}

// A package imports a snapshot of each dependency interface into package-local
// semantic IDs. When a deeper owner publishes a new concrete type graph, the
// intermediate package must be rebuilt before retrying its outer request. The
// public graphs already produced for earlier consumers are self-contained, so
// they are retained across that clean declaration rebuild.
[[nodiscard]] bool rebuild_declaration_package(
    SourceManager &sources,
    CompileWorkspaceResult &result,
    std::size_t package_index,
    const CompileWorkspaceOptions &options,
    DiagnosticSink &diagnostics) {
  if (package_index >= result.packages.size() ||
      !result.packages[package_index].has_value()) {
    return false;
  }
  AvailablePackageImports available;
  if (!collect_package_imports(
          result, package_index, available, diagnostics)) {
    return false;
  }

  WorkspacePackage &workspace_package = result.graph.packages[package_index];
  CompiledPackage &package = *result.packages[package_index];
  std::vector<InterfaceTypeGraph> retained_instances =
      package.interface.instantiated_types;
  SemanticAnalysisResult semantics = analyze_package_semantics(
      sources,
      workspace_package.loaded,
      options.target.facts,
      available,
      compile_time_synthesis_mode(options.stage),
      diagnostics);
  if (!semantics.ok) return false;
  if (!append_compile_time_body_synthesis_sites(
          sources,
          workspace_package.loaded,
          options.target.facts,
          semantics,
          diagnostics)) {
    return false;
  }
  AgentMetadataResult metadata = collect_agent_metadata(
      sources,
      workspace_package.loaded,
      semantics.package,
      options.attachments,
      diagnostics);
  if (!metadata.ok) return false;

  std::vector<AgentValidationContext> validation_context =
      package.validation_context;
  if (validation_context.empty() && has_synthesis_record(metadata)) {
    validation_context = load_validation_context(
        sources, workspace_package, options.workspace, diagnostics);
  }
  PackageInterface interface =
      options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis &&
          has_synthesis_record(metadata)
      ? withheld_package_interface(
            workspace_package.identity, semantics.package)
      : build_package_interface(
            workspace_package.identity,
            semantics.package,
            semantics.constants,
            metadata,
            diagnostics);
  for (const InterfaceTypeGraph &graph : retained_instances) {
    append_instantiated_type(interface, graph);
  }

  AgentObligationResult obligations;
  if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
    obligations = build_agent_obligations(
        workspace_package.identity,
        sources,
        workspace_package.loaded,
        semantics.package,
        semantics.constants,
        metadata,
        options.target,
        diagnostics,
        validation_context);
    if (!obligations.ok) return false;
  }

  package.semantics = std::move(semantics);
  package.metadata = std::move(metadata);
  package.validation_context = std::move(validation_context);
  package.interface = std::move(interface);
  if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
    package.obligations = std::move(obligations);
  }
  return true;
}

// Executes concrete generic-type requests in the package which owns the
// template source and its private compile-time procedures. A concrete owner may
// itself request a deeper owner. Because package imports form an acyclic graph,
// recursively publishing those requests and rebuilding the intermediate
// declaration graph reaches a deterministic fixed point.
class TypeInstantiationPublisher {
public:
  TypeInstantiationPublisher(
      SourceManager &sources,
      CompileWorkspaceResult &result,
      const CompileWorkspaceOptions &options,
      DiagnosticSink &diagnostics)
      : sources_(sources),
        result_(result),
        options_(options),
        diagnostics_(diagnostics) {}

  [[nodiscard]] bool publish(const CompiledPackage &requester) {
    std::vector<std::size_t> owner_stack;
    return publish_requests(requester, owner_stack);
  }

private:
  [[nodiscard]] bool publish_requests(
      const CompiledPackage &requester,
      std::vector<std::size_t> &owner_stack) {
    bool ok = true;
    for (const ImportedTypeInstantiationRequest &request :
         requester.semantics.package.imported_type_instantiation_requests) {
      ok = publish_request(requester, request, owner_stack) && ok;
    }
    return ok;
  }

  [[nodiscard]] bool publish_request(
      const CompiledPackage &requester,
      const ImportedTypeInstantiationRequest &request,
      std::vector<std::size_t> &owner_stack) {
    const std::optional<std::size_t> owner_index = package_index_for(
        result_, request.root_identity, request.root_relative_path);
    if (!owner_index.has_value() ||
        !result_.packages[*owner_index].has_value()) {
      diagnostics_.error(
          SourceRange::invalid(),
          "generic type owner is unavailable during layout instantiation");
      return false;
    }
    if (std::find(
            owner_stack.begin(), owner_stack.end(), *owner_index) !=
        owner_stack.end()) {
      diagnostics_.error(
          SourceRange::invalid(),
          "generic type layout ownership forms a package cycle");
      return false;
    }
    owner_stack.push_back(*owner_index);

    std::vector<Sha256Digest> attempted_nested_requests;
    for (;;) {
      CompiledPackage &owner = *result_.packages[*owner_index];
      const std::optional<SymbolId> source =
          owner.semantics.package.symbols.lookup_direct(
              owner.semantics.package.package_scope,
              request.public_template_name);
      if (!source.has_value()) {
        diagnostics_.error(
            SourceRange::invalid(),
            "generic type request names no public declaration '" +
                request.public_template_name + "'");
        owner_stack.pop_back();
        return false;
      }
      const Symbol source_symbol =
          owner.semantics.package.symbols.symbol(*source);
      if (source_symbol.kind != SymbolKind::Type ||
          !source_symbol.flags.parametric ||
          source_symbol.visibility != Visibility::Public) {
        diagnostics_.error(
            source_symbol.name_range,
            "generic type layout request does not name a public parametric type");
        owner_stack.pop_back();
        return false;
      }

      std::vector<ParametricArgument> transferred_arguments;
      transferred_arguments.reserve(request.arguments.size());
      for (const ParametricArgument &argument : request.arguments) {
        ParametricArgument transferred;
        transferred.is_type = argument.is_type;
        if (!argument.is_type &&
            (argument.value.kind != ConstantKind::Integer ||
             argument.owner_evaluated_value ||
             argument.value_expression.is_valid())) {
          diagnostics_.error(
              source_symbol.name_range,
              "generic type owner request contains a symbolic value argument");
          owner_stack.pop_back();
          return false;
        }
        const TypeId argument_type = argument.is_type
            ? argument.type
            : argument.value_type;
        const InterfaceTypeGraph graph = export_interface_type(
            requester.identity,
            requester.semantics.package,
            argument_type,
            diagnostics_);
        if (!owner_result_is_concrete(graph)) {
          diagnostics_.error(
              source_symbol.name_range,
              "generic type owner request contains a symbolic type argument");
          owner_stack.pop_back();
          return false;
        }
        const TypeId imported = import_interface_type(
            graph, owner.semantics.package, diagnostics_);
        if (argument.is_type) {
          transferred.type = imported;
        } else {
          transferred.value_type = imported;
          transferred.value = argument.value;
        }
        transferred_arguments.push_back(std::move(transferred));
      }
      const std::vector<ParametricArgument> publication_arguments =
          transferred_arguments;

      const TypeId concrete = instantiate_parametric_type_application(
          sources_,
          result_.graph.packages[*owner_index].loaded,
          owner.semantics.package,
          owner.semantics.selections,
          *source,
          std::move(transferred_arguments),
          source_symbol.name_range,
          options_.target.facts,
          diagnostics_);
      if (!concrete.is_valid() ||
          owner.semantics.package.types.type(concrete).kind ==
              TypeKind::Invalid) {
        owner_stack.pop_back();
        return false;
      }
      const InterfaceTypeGraph graph = export_interface_type_application(
          owner.identity,
          owner.semantics.package,
          concrete,
          *source,
          publication_arguments,
          diagnostics_);
      if (owner_result_is_concrete(graph)) {
        append_instantiated_type(owner.interface, graph);
        owner_stack.pop_back();
        return true;
      }

      if (owner.semantics.package
              .imported_type_instantiation_requests.empty()) {
        diagnostics_.error(
            source_symbol.name_range,
            "generic type layout request did not produce a concrete owner result");
        owner_stack.pop_back();
        return false;
      }
      const Sha256Digest nested_digest =
          hash_type_instantiation_requests(owner, diagnostics_);
      if (std::find(
              attempted_nested_requests.begin(),
              attempted_nested_requests.end(),
              nested_digest) != attempted_nested_requests.end()) {
        diagnostics_.error(
            source_symbol.name_range,
            "transitive generic type layout requests made no semantic progress");
        owner_stack.pop_back();
        return false;
      }
      attempted_nested_requests.push_back(nested_digest);

      if (!publish_requests(owner, owner_stack) ||
          !rebuild_declaration_package(
              sources_,
              result_,
              *owner_index,
              options_,
              diagnostics_)) {
        owner_stack.pop_back();
        return false;
      }
    }
  }

  SourceManager &sources_;
  CompileWorkspaceResult &result_;
  const CompileWorkspaceOptions &options_;
  DiagnosticSink &diagnostics_;
};

[[nodiscard]] bool publish_type_instantiation_requests(
    SourceManager &sources,
    CompileWorkspaceResult &result,
    const CompiledPackage &requester,
    const CompileWorkspaceOptions &options,
    DiagnosticSink &diagnostics) {
  TypeInstantiationPublisher publisher(sources, result, options, diagnostics);
  return publisher.publish(requester);
}

// A dependency with any obligation emitted by interface discovery cannot yet
// publish a complete interface to consumers. Besides declarations and members,
// this includes a statement/expression site in a procedure required by a
// constant, layout, or `when` decision. Discovery conservatively suspends every
// consumer until the dependency is overlaid and recompiled; readiness must not
// depend on which absent generated name or conditional branch a consumer uses.
[[nodiscard]] bool has_interface_synthesis(const CompiledPackage &package) {
  for (const AgentObligation &obligation : package.obligations.obligations) {
    if (is_synthesis_obligation(obligation.kind)) return true;
  }
  return false;
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

[[nodiscard]] ImportedEffect import_interface_effect(
    SymbolId proxy,
    const InterfaceDeclaration::Effect &source);

[[nodiscard]] ImportedFlowValue import_interface_flow_value(
    SymbolId proxy,
    const InterfaceDeclaration::FlowValue &source) {
  ImportedFlowValue result;
  result.unknown = source.unknown;
  for (const InterfaceDeclaration::ReturnFlowSlot &slot : source.flow_slots) {
    result.flow_slots.push_back(
        {slot.parameter, slot.path, slot.context});
  }
  for (const InterfaceDeclaration::Effect &effect :
       source.contract_effects) {
    result.contract_effects.push_back(
        import_interface_effect(proxy, effect));
  }
  return result;
}

[[nodiscard]] ImportedEffect import_interface_effect(
    SymbolId proxy,
    const InterfaceDeclaration::Effect &source) {
  ImportedEffect result;
  result.procedure_proxy = proxy;
  result.kind = source.kind;
  result.root_identity = source.root_identity;
  result.root_relative_path = source.root_relative_path;
  result.declaration = source.declaration;
  result.detail = source.detail;
  result.flow_parameter = source.flow_parameter;
  result.flow_path = source.flow_path;
  result.flow_context = source.flow_context;
  for (const InterfaceDeclaration::FlowArgument &argument :
       source.flow_arguments) {
    ImportedFlowArgument imported_argument;
    for (const InterfaceDeclaration::FlowField &field : argument.fields) {
      imported_argument.fields.push_back({
          field.path,
          import_interface_flow_value(proxy, field.value)});
    }
    result.flow_arguments.push_back(std::move(imported_argument));
  }
  return result;
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
  package.imported_returns.clear();
  package.imported_writes.clear();
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
      package.imported_effects.push_back(
          import_interface_effect(imported.proxy, effect));
    }
    for (const InterfaceDeclaration::ReturnValue &returned :
         declaration->return_values) {
      ImportedProcedureReturn imported_return;
      imported_return.procedure_proxy = imported.proxy;
      imported_return.path = returned.path;
      imported_return.unknown = returned.unknown;
      for (const InterfaceDeclaration::ReturnFlowSlot &slot :
           returned.flow_slots) {
        imported_return.flow_slots.push_back(
            {slot.parameter, slot.path, slot.context});
      }
      for (const InterfaceDeclaration::Effect &effect :
           returned.contract_effects) {
        imported_return.contract_effects.push_back(
            import_interface_effect(imported.proxy, effect));
      }
      package.imported_returns.push_back(std::move(imported_return));
    }
    for (const InterfaceDeclaration::FieldWrite &write :
         declaration->field_writes) {
      ImportedProcedureWrite imported_write;
      imported_write.procedure_proxy = imported.proxy;
      imported_write.parameter = write.parameter;
      imported_write.indirection = write.indirection;
      imported_write.path = write.path;
      imported_write.value_unknown = write.value_unknown;
      for (const InterfaceDeclaration::ReturnFlowSlot &slot :
           write.value_flow_slots) {
        imported_write.value_flow_slots.push_back(
            {slot.parameter, slot.path, slot.context});
      }
      for (const InterfaceDeclaration::Effect &effect :
           write.value_contract_effects) {
        imported_write.value_contract_effects.push_back(
            import_interface_effect(imported.proxy, effect));
      }
      package.imported_writes.push_back(std::move(imported_write));
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
    if (pin.source_map.expansion_bytes !=
        static_cast<std::uint64_t>(source.size())) {
      diagnostics.error(
          SourceRange::invalid(),
          "generated expansion length does not match its resolution source map");
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
        existing.source = std::move(candidate.source);
        replaced = true;
        break;
      }
    }
    if (!replaced) combined.push_back(std::move(candidate));
  }
}

void bind_handwritten_program_identity(
    const SourceManager &sources,
    const CompileWorkspaceOptions &options,
    CompileWorkspaceResult &result) {
  if (!result.ok) return;
  ResolutionManifest empty_manifest;
  empty_manifest.target_identity = options.target.facts.identity;
  result.resolved_program_digest = hash_resolved_program(
      sources,
      result.graph,
      options.target,
      empty_manifest,
      options.compiler_content_identity);
}

} // namespace

CompileWorkspaceResult compile_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  CompileWorkspaceResult result;
  result.compiler_content_identity = options.compiler_content_identity;
  result.foreign_provider_audits = options.foreign_provider_audits;
  const std::size_t initial_errors = diagnostics.error_count();
  if (options.compiler_content_identity.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "compiler content identity must not be empty");
    return result;
  }
  for (const ForeignProviderAudit &audit :
       options.foreign_provider_audits) {
    if (audit.provider == "draft_runtime" ||
        audit.provider == "package_assembly" ||
        std::binary_search(
            options.target.system_link_providers.begin(),
            options.target.system_link_providers.end(),
            audit.provider)) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider audit '" + audit.provider +
              "' attempts to override a compiler- or target-owned provider");
      return result;
    }
  }
  std::string profile_error;
  if (!validate_target_profile(options.target, profile_error)) {
    diagnostics.error(
        SourceRange::invalid(), "invalid target profile: " + profile_error);
    return result;
  }
  if (options.validation_kind == ValidationKind::Test) {
    options.workspace.package_options.include_tests = true;
    options.workspace.package_options.include_benchmarks = false;
  } else if (options.validation_kind == ValidationKind::Benchmark) {
    options.workspace.package_options.include_tests = false;
    options.workspace.package_options.include_benchmarks = true;
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
    available.consumer_identity = workspace_package.identity;
    bool dependencies_ready = true;
    for (const PackageImport &import : result.graph.imports) {
      if (static_cast<std::size_t>(import.importing_package.value) != package_index) {
        continue;
      }
      const std::size_t dependency_index =
          static_cast<std::size_t>(import.imported_package.value);
      if (dependency_index >= result.packages.size() ||
          !result.packages[dependency_index].has_value()) {
        if (options.stage == CompileWorkspaceStage::Complete) {
          diagnostics.error(
              SourceRange::invalid(),
              "dependency did not produce a package interface before its consumer");
        }
        dependencies_ready = false;
        continue;
      }
      if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis &&
          has_interface_synthesis(*result.packages[dependency_index])) {
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
        compile_time_synthesis_mode(options.stage),
        diagnostics);
    // Imported procedure-dependent layouts are discovered only after the
    // consumer supplies concrete arguments. Publish each owner result, then
    // rebuild this package from its unchanged source and the enriched dependency
    // interfaces. A successful rebuild contains no placeholder requests.
    std::vector<Sha256Digest> attempted_type_request_sets;
    while (package.semantics.ok &&
           !package.semantics.package
                .imported_type_instantiation_requests.empty()) {
      const Sha256Digest request_digest =
          hash_type_instantiation_requests(package, diagnostics);
      if (std::find(
              attempted_type_request_sets.begin(),
              attempted_type_request_sets.end(),
              request_digest) != attempted_type_request_sets.end()) {
        break;
      }
      attempted_type_request_sets.push_back(request_digest);
      if (!publish_type_instantiation_requests(
              sources, result, package, options, diagnostics)) {
        break;
      }
      package.semantics = analyze_package_semantics(
          sources,
          workspace_package.loaded,
          options.target.facts,
          available,
          diagnostics);
    }
    if (package.semantics.ok &&
        !package.semantics.package
             .imported_type_instantiation_requests.empty()) {
      diagnostics.error(
          SourceRange::invalid(),
          "generic type layout requests made no semantic progress");
      package.semantics.ok = false;
    }
    if (!package.semantics.ok) continue;
    if (!append_compile_time_body_synthesis_sites(
            sources,
            workspace_package.loaded,
            options.target.facts,
            package.semantics,
            diagnostics)) {
      continue;
    }
    // Public package/declaration documentation is an interface input just like
    // public types and constants. Collect it before consumers bind dependency
    // interfaces; waiting for the body pass would make those exact docs and
    // attachment bytes permanently unavailable in the consumer semantic graph.
    // Body-local judgments and synthesis sites are added by the later complete
    // recollection after HIR checking.
    package.metadata = collect_agent_metadata(
        sources,
        workspace_package.loaded,
        package.semantics.package,
        options.attachments,
        diagnostics);
    if (package.metadata.ok && has_synthesis_record(package.metadata)) {
      package.validation_context = load_validation_context(
          sources, workspace_package, options.workspace, diagnostics);
    }
    package.interface =
        options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis &&
            has_synthesis_record(package.metadata)
        ? withheld_package_interface(
              workspace_package.identity, package.semantics.package)
        : build_package_interface(
              workspace_package.identity,
              package.semantics.package,
              package.semantics.constants,
              package.metadata,
              diagnostics);
    if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
      package.obligations = build_agent_obligations(
          workspace_package.identity,
          sources,
          workspace_package.loaded,
          package.semantics.package,
          package.semantics.constants,
          package.metadata,
          options.target,
          diagnostics,
          package.validation_context);
    }
    result.packages[package_index] = std::move(package);
  }

  // Declaration and member synthesis is an interface-stage operation. The
  // complete body pass cannot run yet because ordinary source is allowed to
  // name symbols and fields supplied by these sites. Metadata collection is
  // nevertheless valid: declaration collection and type skeleton resolution
  // have already installed the exact package/type scopes and visible symbols
  // available to each opaque completeness set.
  if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
    bool every_ready_package_valid = true;
    for (const std::optional<CompiledPackage> &package : result.packages) {
      if (!package.has_value()) continue;
      every_ready_package_valid = every_ready_package_valid &&
          package->semantics.ok && package->metadata.ok && package->obligations.ok;
    }
    result.ok = every_ready_package_valid &&
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
    // Expression, statement, and assembly sites are installed by body
    // checking, so a package with only later-stage synthesis was not visible to
    // the phase-1 guard above. Load its validation context now, before the final
    // obligations are hashed. Early declaration/member packages reuse the rows
    // they already collected.
    if (package.metadata.ok && package.validation_context.empty() &&
        has_synthesis_record(package.metadata)) {
      package.validation_context = load_validation_context(
          sources, workspace_package, options.workspace, diagnostics);
    }
    package.obligations = build_agent_obligations(
        workspace_package.identity,
        sources,
        workspace_package.loaded,
        package.semantics.package,
        package.semantics.constants,
        package.metadata,
        options.target,
        diagnostics,
        package.validation_context);
    package.effects = summarize_package_effects(
        package.semantics.package,
        package.bodies.program,
        &options.target,
        options.foreign_provider_audits);
    package.native_interop = validate_native_interop(
        package.semantics.package, package.bodies.program, diagnostics);
    const bool denials_ok = check_package_denials(
        sources,
        workspace_package.loaded,
        package.semantics.package,
        package.bodies.program,
        package.effects,
        diagnostics);
    // Body effects replace the preliminary declaration interface, but concrete
    // type applications published during the owner fixed point are independent
    // self-contained graphs. Keep them available to tooling and any later
    // consumer of this completed compilation result; rebuilding the ordinary
    // declarations must not silently erase generic layout products.
    std::vector<InterfaceTypeGraph> retained_type_instances =
        package.interface.instantiated_types;
    PackageInterface completed_interface = build_package_interface(
        workspace_package.identity,
        package.semantics.package,
        package.semantics.constants,
        package.metadata,
        package.effects,
        diagnostics);
    for (const InterfaceTypeGraph &graph : retained_type_instances) {
      append_instantiated_type(completed_interface, graph);
    }
    package.interface = std::move(completed_interface);
    if (!package.metadata.ok || !package.obligations.ok || !denials_ok ||
        !package.native_interop.ok) {
      continue;
    }
  }

  // Phase 4: provider-free target lowering. No package reaches a backend until
  // every cross-package generic proxy has an exact defining symbol.
  //
  // Validation discovery belongs between semantic closure and lowering. A
  // filename or spelling alone never becomes an executable call: every entry
  // below has a checked body, exact core nominal parameter, and target layout.
  if (options.validation_kind != ValidationKind::None) {
    for (std::size_t package_index = 0;
         package_index < result.packages.size(); ++package_index) {
      if (!result.packages[package_index].has_value()) continue;
      CompiledPackage &package = *result.packages[package_index];
      if (!package.bodies.ok || !package.metadata.ok ||
          !package.obligations.ok || !package.native_interop.ok) {
        continue;
      }
      std::vector<ValidationEntry> discovered = discover_validation_entries(
          options.validation_kind,
          options.workspace.core_content_identity,
          package.identity,
          result.graph.packages[package_index].loaded,
          package.semantics.package,
          package.bodies.program,
          diagnostics);
      result.validation_entries.insert(
          result.validation_entries.end(),
          std::make_move_iterator(discovered.begin()),
          std::make_move_iterator(discovered.end()));
    }
    sort_validation_entries(result.validation_entries);
  }

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
      llvm_options.emit_runtime_support =
          package_index ==
          static_cast<std::size_t>(result.graph.root_package.value);
      llvm_options.emit_program_entry =
          options.emit_program_entry && llvm_options.emit_runtime_support;
      if (llvm_options.emit_runtime_support) {
        llvm_options.validation_kind = options.validation_kind;
        llvm_options.validation_entries = result.validation_entries;
      }
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

  const ResolutionManifestLoadResult loaded_manifest =
      load_resolution_manifest(options.workspace.workspace_directory, diagnostics);
  if (loaded_manifest.state == ResolutionManifestLoadState::Invalid) {
    return {};
  }

  // A normal resolution manifest identifies the ordinary program graph, while
  // a validation command deliberately adds command-only files and imports.
  // Reproduce the ordinary graph once to authenticate the manifest before the
  // validation graph derives its separate, definition-inclusive identity.
  if (loaded_manifest.state == ResolutionManifestLoadState::Loaded &&
      options.validation_kind != ValidationKind::None) {
    CompileWorkspaceOptions base_options = options;
    base_options.validation_kind = ValidationKind::None;
    base_options.lower_mir = false;
    base_options.emit_llvm = false;
    const CompileWorkspaceResult base = compile_workspace_with_resolution(
        sources,
        root_package_directory,
        std::move(base_options),
        diagnostics);
    if (!base.ok) return base;
  }

  std::vector<bool> matched_pins;
  std::vector<WorkspaceSourceOverride> interface_overrides;
  if (loaded_manifest.state == ResolutionManifestLoadState::Loaded) {
    matched_pins.resize(loaded_manifest.manifest.pins.size(), false);
  }

  // Reproduce dependency-ready interface rounds from pinned bytes. No body is
  // checked until every package interface is complete, and no round observes a
  // same-round expansion. Each nonempty round removes at least one site.
  while (true) {
    CompileWorkspaceOptions interface_options = options;
    interface_options.stage =
        CompileWorkspaceStage::DiscoverInterfaceSynthesis;
    interface_options.lower_mir = false;
    interface_options.emit_llvm = false;
    interface_options.workspace.source_overrides = interface_overrides;
    CompileWorkspaceResult interface_surface = compile_workspace(
        sources,
        root_package_directory,
        std::move(interface_options),
        diagnostics);
    if (!interface_surface.ok) return interface_surface;

    const std::size_t site_count = synthesis_site_count(interface_surface);
    if (loaded_manifest.state == ResolutionManifestLoadState::Missing) {
      if (site_count != 0) {
        diagnostics.error(
            SourceRange::invalid(),
            "workspace has unresolved synthesis sites and no resolution manifest; "
            "run 'draftc resolve'");
        interface_surface.ok = false;
        return interface_surface;
      }
    } else {
      const ResolutionManifest interface_manifest = select_stage_manifest(
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
      merge_resolution_overrides(
          interface_overrides, interface_overlay.sources);
    }
    if (site_count != 0) continue;

    bool all_packages_ready = true;
    for (const std::optional<CompiledPackage> &package :
         interface_surface.packages) {
      all_packages_ready = all_packages_ready && package.has_value();
    }
    if (!all_packages_ready) {
      diagnostics.error(
          SourceRange::invalid(),
          "interface elaboration has suspended packages but no ready synthesis site");
      interface_surface.ok = false;
      return interface_surface;
    }
    break;
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
    if (!options.lower_mir && !options.emit_llvm) {
      bind_handwritten_program_identity(sources, options, body_surface);
      return body_surface;
    }
    options.stage = CompileWorkspaceStage::Complete;
    options.workspace.source_overrides.clear();
    CompileWorkspaceResult handwritten = compile_workspace(
        sources, root_package_directory, std::move(options), diagnostics);
    // compile_workspace retained the exact compiler identity, so reconstruct
    // the two option fields consumed by the moved value without a magic
    // duplicate version string.
    if (handwritten.ok) {
      ResolutionManifest empty_manifest;
      empty_manifest.target_identity = body_options.target.facts.identity;
      handwritten.resolved_program_digest = hash_resolved_program(
          sources,
          handwritten.graph,
          body_options.target,
          empty_manifest,
          handwritten.compiler_content_identity);
    }
    return handwritten;
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
    resolved.resolved_program_digest = program_digest;
    if (options.validation_kind == ValidationKind::None &&
        program_digest != loaded_manifest.manifest.resolved_program_digest) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolution manifest resolved-program identity is stale");
    }
  }
  resolved.ok = diagnostics.error_count() == initial_errors;
  if (resolved.ok) {
    resolved.resolution_manifest = loaded_manifest.manifest;
    if (!resolved.resolved_program_digest.has_value()) {
      resolved.resolved_program_digest =
          loaded_manifest.manifest.resolved_program_digest;
    }
  }
  return resolved;
}

} // namespace draft
