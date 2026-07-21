// Provider-free workspace compiler orchestration.
//
// This module turns one closed workspace into dependency-ordered semantic
// package rows, then optionally continues those same rows through validation
// discovery, parsed assembly, Draft MIR, and LLVM text. Inputs are exact source
// buffers owned by the caller's SourceManager plus explicit target,
// configuration, attachment, and foreign-summary facts. The result owns the
// workspace graph and every compiler representation but continues to borrow
// source bytes through stable FileIds; callers therefore keep SourceManager
// alive for the complete command.
//
// Target selection, source generations, parsed files, package imports, authored
// declaration types, nominal layouts, conditional choices, named constants,
// package name completeness, opaque interface synthesis sets, and package
// interfaces are explicit products in one dynamic command-local graph. The
// coordinator freezes ready waves and publishes task-owned diagnostics and
// payloads in stable product-ID order. Generic body demands still propagate
// consumer-first, and completed effects still publish dependency-first; later
// implementation slices move those remaining package loops into the same
// graph. Within an
// unchanged source generation, CompileWorkspaceProgress advances so native
// lowering can continue checked state without reloading source. A checked
// generated-source transition appends a successor generation and supersedes
// only the affected interface products while retaining unrelated products.
//
// No state is process-global or persisted as a compiler cache. Resolution
// orchestration consumes generated Draft only through ordinary source
// overrides and may not manufacture semantic or backend nodes. Relevant rules
// are specification sections 10 and 15 and the implementation architecture
// document.

#include "compile/compiler.h"

#include "base/sha256.h"
#include "base/timing.h"
#include "elaborator/generated_source.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"
#include "elaborator/resolved_program.h"
#include "sema/denial.h"
#include "sema/type_resolver.h"
#include "workspace/selection.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

// Package detail names are useful in --timings=all but should create no string
// or clock overhead during ordinary compilation or the compact report. Keeping
// this policy in one helper prevents every package loop from accidentally
// formatting identities before it knows that detailed output is enabled.
[[nodiscard]] TimingScope time_package_phase(
    TimingRecorder *timings,
    std::string_view phase,
    const PackageIdentity &identity) {
  if (timings == nullptr || timings->output() != TimingOutput::All) return {};
  return timings->scope(
      std::string(phase) + display_package_identity(identity),
      TimingVisibility::Detail);
}

// Merges task-local source-order SymbolIds without changing the order already
// published by an earlier product. Constant tasks may reach the same helper
// procedure through several expressions; one semantic body product is enough.
void append_unique_symbols(
    std::vector<SymbolId> &destination,
    std::span<const SymbolId> additions) {
  for (SymbolId symbol : additions) {
    if (std::find(destination.begin(), destination.end(), symbol) ==
        destination.end()) {
      destination.push_back(symbol);
    }
  }
}

// Converts semantic command selection into the exact package-loader facts used
// by both initial graph construction and later interface-to-body continuation.
// Continuations receive the driver's semantic options, not the private copy
// normalized inside an earlier compile_workspace call, so this operation must
// be idempotent and shared rather than relying on incidental mutation.
void configure_package_selection(CompileWorkspaceOptions &options) {
  if (options.validation_kind == ValidationKind::Test) {
    options.workspace.package_options.include_tests = true;
    options.workspace.package_options.include_benchmarks = false;
  } else if (options.validation_kind == ValidationKind::Benchmark) {
    options.workspace.package_options.include_tests = false;
    options.workspace.package_options.include_benchmarks = true;
  }
  options.workspace.package_options.file_tag = options.target.facts.file_tag;
  options.workspace.package_options.timings = options.timings;
}

// Builds every package-topology view in one linear edge pass, canonicalizes the
// identity and reverse-adjacency rows, then performs a deterministic Kahn
// traversal. A min-heap keeps the smallest ready PackageId visible without
// shifting a vector on every pop. Because duplicate syntax imports remain in
// the reverse rows until their comparison sorts complete, the conservative
// bound is O((packages + imports) log(packages + imports)); later interface
// lookups and source invalidation use the finished adjacency rows rather than
// rescanning all imports for every package.
[[nodiscard]] WorkspaceDependencyIndex build_dependency_index(
    const WorkspaceGraph &graph) {
  WorkspaceDependencyIndex result;
  const std::size_t package_count = graph.packages.size();
  result.import_edges_by_consumer.resize(package_count);
  result.consumers_by_dependency.resize(package_count);
  result.package_indices_by_identity.reserve(package_count);
  for (std::size_t package_index = 0; package_index < package_count;
       ++package_index) {
    result.package_indices_by_identity.push_back(package_index);
  }
  std::sort(
      result.package_indices_by_identity.begin(),
      result.package_indices_by_identity.end(),
      [&graph](std::size_t left, std::size_t right) {
        const PackageIdentity &left_identity = graph.packages[left].identity;
        const PackageIdentity &right_identity = graph.packages[right].identity;
        if (left_identity.root_identity != right_identity.root_identity) {
          return left_identity.root_identity < right_identity.root_identity;
        }
        return left_identity.root_relative_path <
            right_identity.root_relative_path;
      });

  std::vector<std::size_t> importer_count(package_count, 0);
  for (std::size_t edge_index = 0; edge_index < graph.imports.size();
       ++edge_index) {
    const PackageImport &edge = graph.imports[edge_index];
    if (!edge.importing_package.is_valid() ||
        !edge.imported_package.is_valid()) {
      result.valid = false;
      continue;
    }
    const std::size_t consumer =
        static_cast<std::size_t>(edge.importing_package.value);
    const std::size_t dependency =
        static_cast<std::size_t>(edge.imported_package.value);
    if (consumer >= package_count || dependency >= package_count) {
      result.valid = false;
      continue;
    }
    result.import_edges_by_consumer[consumer].push_back(edge_index);
    result.consumers_by_dependency[dependency].push_back(consumer);
    ++importer_count[dependency];
  }

  for (std::vector<std::size_t> &consumers :
       result.consumers_by_dependency) {
    std::sort(consumers.begin(), consumers.end());
    consumers.erase(
        std::unique(consumers.begin(), consumers.end()), consumers.end());
  }

  std::priority_queue<
      std::size_t,
      std::vector<std::size_t>,
      std::greater<std::size_t>> ready;
  for (std::size_t package_index = 0; package_index < package_count;
       ++package_index) {
    if (importer_count[package_index] == 0) ready.push(package_index);
  }

  result.consumer_first_order.reserve(package_count);
  while (!ready.empty()) {
    const std::size_t consumer = ready.top();
    ready.pop();
    result.consumer_first_order.push_back(consumer);
    for (std::size_t edge_index :
         result.import_edges_by_consumer[consumer]) {
      const std::size_t dependency = static_cast<std::size_t>(
          graph.imports[edge_index].imported_package.value);
      if (importer_count[dependency] == 0) {
        result.valid = false;
        continue;
      }
      --importer_count[dependency];
      if (importer_count[dependency] == 0) ready.push(dependency);
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

// Resolves one canonical package identity through the index's sorted
// indirection. The returned value is still the stable PackageId table index;
// callers never observe or persist the sorted position itself.
[[nodiscard]] std::optional<std::size_t> package_index_for(
    const WorkspaceGraph &graph,
    const WorkspaceDependencyIndex &dependencies,
    std::string_view root_identity,
    std::string_view root_relative_path) {
  const auto position = std::lower_bound(
      dependencies.package_indices_by_identity.begin(),
      dependencies.package_indices_by_identity.end(),
      std::pair(root_identity, root_relative_path),
      [&graph](
          std::size_t package_index,
          const std::pair<std::string_view, std::string_view> &key) {
        const PackageIdentity &identity = graph.packages[package_index].identity;
        if (identity.root_identity != key.first) {
          return identity.root_identity < key.first;
        }
        return identity.root_relative_path < key.second;
      });
  if (position == dependencies.package_indices_by_identity.end()) {
    return std::nullopt;
  }
  const std::size_t package_index = *position;
  const PackageIdentity &identity = graph.packages[package_index].identity;
  if (identity.root_identity == root_identity &&
      identity.root_relative_path == root_relative_path) {
    return package_index;
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

[[nodiscard]] bool has_agent_obligation_record(
    const AgentMetadataResult &metadata) {
  for (const AgentRecord &record : metadata.records) {
    if (record.kind != AgentConstructKind::Documentation) return true;
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
    const SemanticAnalysisResult &semantics,
    SemanticPackage &context_package,
    ConstantTable &context_constants,
    DiagnosticSink &diagnostics) {
  context_package = semantics.package;
  context_constants = semantics.constants;
  if (semantics.compile_time_synthesis_procedures.empty() ||
      has_semantic_site(
          semantics.package, SemanticSiteKind::SynthesisDeclaration)) {
    return true;
  }

  const bool speculate = has_semantic_site(
      semantics.package, SemanticSiteKind::SynthesisMember);
  if (speculate) {
    DiagnosticSink deferred_diagnostics;
    BodyCheckResult candidate = check_compile_time_procedure_bodies(
        sources,
        loaded,
        semantics.selections,
        semantics.package,
        semantics.constants,
        target,
        semantics.compile_time_synthesis_procedures,
        deferred_diagnostics);
    if (!candidate.ok) return true;
    context_package = std::move(candidate.package);
    context_constants = std::move(candidate.constants);
    return true;
  }

  BodyCheckResult checked = check_compile_time_procedure_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target,
      semantics.compile_time_synthesis_procedures,
      diagnostics);
  if (checked.ok) {
    context_package = std::move(checked.package);
    context_constants = std::move(checked.constants);
  }
  return checked.ok;
}

// Validation files stay outside the ordinary workspace graph, so handwritten
// builds do not acquire test-only imports or declarations. A package that has
// synthesis or judgment obligations gets one parallel target-selected load
// with both validation roles enabled. Only its canonical test/benchmark rows
// survive this helper;
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
       requester.declarations.package.imported_type_instantiation_requests) {
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
          requester.declarations.package,
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
    const WorkspaceDependencyIndex &schedule,
    std::size_t package_index,
    AvailablePackageImports &available,
    DiagnosticSink &diagnostics) {
  if (package_index >= result.graph.packages.size() ||
      package_index >= schedule.import_edges_by_consumer.size()) {
    return false;
  }
  available.consumer_identity =
      result.graph.packages[package_index].identity;
  for (std::size_t edge_index :
       schedule.import_edges_by_consumer[package_index]) {
    const PackageImport &import = result.graph.imports[edge_index];
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
    const WorkspaceDependencyIndex &schedule,
    std::size_t package_index,
    const CompileWorkspaceOptions &options,
    DiagnosticSink &diagnostics) {
  if (package_index >= result.packages.size() ||
      !result.packages[package_index].has_value()) {
    return false;
  }
  AvailablePackageImports available;
  if (!collect_package_imports(
          result, schedule, package_index, available, diagnostics)) {
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
  SemanticPackage interface_context_package;
  ConstantTable interface_context_constants;
  if (!append_compile_time_body_synthesis_sites(
          sources,
          workspace_package.loaded,
          options.target.facts,
          semantics,
          interface_context_package,
          interface_context_constants,
          diagnostics)) {
    return false;
  }
  AgentMetadataResult metadata = collect_agent_metadata(
      sources,
      workspace_package.loaded,
      interface_context_package,
      options.attachments,
      diagnostics);
  if (!metadata.ok) return false;

  std::vector<AgentValidationContext> validation_context =
      package.validation_context;
  if (options.validation_kind == ValidationKind::None &&
      validation_context.empty() && has_agent_obligation_record(metadata)) {
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
        interface_context_package,
        interface_context_constants,
        metadata,
        options.target,
        diagnostics,
        validation_context);
    if (!obligations.ok) return false;
  }

  package.declarations = std::move(semantics);
  ++package.declaration_generation;
  package.body_work_key = {};
  BodyCheckResult empty_bodies;
  package.bodies = std::move(empty_bodies);
  package.semantic_progress = PackageSemanticProgress::InterfaceReady;
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
      const WorkspaceDependencyIndex &schedule,
      const CompileWorkspaceOptions &options,
      DiagnosticSink &diagnostics)
      : sources_(sources),
        result_(result),
        schedule_(schedule),
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
         requester.declarations.package.imported_type_instantiation_requests) {
      ok = publish_request(requester, request, owner_stack) && ok;
    }
    return ok;
  }

  [[nodiscard]] bool publish_request(
      const CompiledPackage &requester,
      const ImportedTypeInstantiationRequest &request,
      std::vector<std::size_t> &owner_stack) {
    const std::optional<std::size_t> owner_index = package_index_for(
        result_.graph,
        schedule_,
        request.root_identity,
        request.root_relative_path);
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
          owner.declarations.package.symbols.lookup_direct(
              owner.declarations.package.package_scope,
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
          owner.declarations.package.symbols.symbol(*source);
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
            requester.declarations.package,
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
            graph, owner.declarations.package, diagnostics_);
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
          owner.declarations.package,
          owner.declarations.selections,
          *source,
          std::move(transferred_arguments),
          source_symbol.name_range,
          options_.target.facts,
          diagnostics_);
      if (!concrete.is_valid() ||
          owner.declarations.package.types.type(concrete).kind ==
              TypeKind::Invalid) {
        owner_stack.pop_back();
        return false;
      }
      const InterfaceTypeGraph graph = export_interface_type_application(
          owner.identity,
          owner.declarations.package,
          concrete,
          *source,
          publication_arguments,
          diagnostics_);
      if (owner_result_is_concrete(graph)) {
        append_instantiated_type(owner.interface, graph);
        owner_stack.pop_back();
        return true;
      }

      if (owner.declarations.package
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
              schedule_,
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
  const WorkspaceDependencyIndex &schedule_;
  const CompileWorkspaceOptions &options_;
  DiagnosticSink &diagnostics_;
};

[[nodiscard]] bool publish_type_instantiation_requests(
    SourceManager &sources,
    CompileWorkspaceResult &result,
    const WorkspaceDependencyIndex &schedule,
    const CompiledPackage &requester,
    const CompileWorkspaceOptions &options,
    DiagnosticSink &diagnostics) {
  TypeInstantiationPublisher publisher(
      sources, result, schedule, options, diagnostics);
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

// Only persistent site identity and construct kind cross the body-source
// transition. Retaining this compact boundary avoids copying a complete
// semantic graph merely to prove that checked generated source removed every
// synthesis site and preserved the authored judgment set.
[[nodiscard]] ResolvedAgentBoundary capture_agent_boundary(
    const CompileWorkspaceResult &surface) {
  ResolvedAgentBoundary result;
  for (const std::optional<CompiledPackage> &package : surface.packages) {
    if (!package.has_value()) continue;
    for (const AgentObligation &obligation : package->obligations.obligations) {
      if (obligation.kind == AgentConstructKind::Judgment) {
        result.judgment_sites.push_back(obligation.site_identity);
      }
    }
  }
  std::sort(result.judgment_sites.begin(), result.judgment_sites.end());
  result.judgment_sites.erase(
      std::unique(
          result.judgment_sites.begin(), result.judgment_sites.end()),
      result.judgment_sites.end());
  return result;
}

[[nodiscard]] bool snapshot_contains_judgment(
    const ResolvedAgentBoundary &surface,
    std::string_view site_identity) {
  return std::binary_search(
      surface.judgment_sites.begin(),
      surface.judgment_sites.end(),
      site_identity);
}

[[nodiscard]] bool validate_resolved_agent_boundary_snapshot(
    const ResolvedAgentBoundary &surface,
    const CompileWorkspaceResult &resolved,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  std::vector<std::string> resolved_judgments;
  for (std::size_t index = 0; index < resolved.packages.size(); ++index) {
    if (!resolved.packages[index].has_value()) continue;
    for (const AgentObligation &obligation :
         resolved.packages[index]->obligations.obligations) {
      if (is_synthesis_obligation(obligation.kind)) {
        diagnostics.error(
            obligation_range(resolved.graph.packages[index], obligation),
            "generated source may not contain a synthesis site");
      } else if (obligation.kind == AgentConstructKind::Judgment) {
        resolved_judgments.push_back(obligation.site_identity);
        if (!snapshot_contains_judgment(surface, obligation.site_identity)) {
          diagnostics.error(
              obligation_range(resolved.graph.packages[index], obligation),
              "generated source may not introduce a judgment");
        }
      }
    }
  }
  std::sort(resolved_judgments.begin(), resolved_judgments.end());
  resolved_judgments.erase(
      std::unique(resolved_judgments.begin(), resolved_judgments.end()),
      resolved_judgments.end());
  for (const std::string &site : surface.judgment_sites) {
    if (!std::binary_search(
            resolved_judgments.begin(), resolved_judgments.end(), site)) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolved source displaced a surface judgment site");
    }
  }
  return diagnostics.error_count() == initial_errors;
}

// Interface reanalysis can stop before consumers when generated source itself
// contains a declaration/member synthesis site. Diagnose those ready producer
// rows before semantic closure so the user sees the violated source boundary,
// not the downstream scheduler symptom that a package remained suspended.
[[nodiscard]] bool reject_generated_synthesis(
    const CompileWorkspaceResult &resolved,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  for (std::size_t index = 0; index < resolved.packages.size(); ++index) {
    if (!resolved.packages[index].has_value()) continue;
    for (const AgentObligation &obligation :
         resolved.packages[index]->obligations.obligations) {
      if (!is_synthesis_obligation(obligation.kind)) continue;
      diagnostics.error(
          obligation_range(resolved.graph.packages[index], obligation),
          "generated source may not contain a synthesis site");
    }
  }
  return diagnostics.error_count() == initial_errors;
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
// final dependency interface. A concrete generic proxy must use its exact
// specialization row: dependent `when` can deliberately make two instances of
// the same source template expose different effects.
void refresh_imported_effects(
    SemanticPackage &package,
    const CompileWorkspaceResult &result,
    const WorkspaceDependencyIndex &schedule,
    DiagnosticSink &diagnostics) {
  package.imported_effects.clear();
  package.imported_returns.clear();
  package.imported_writes.clear();
  for (ImportedSymbol &imported : package.imported_symbols) {
    const ImportedProcedureInstance *requested_instance = nullptr;
    for (const ImportedProcedureInstance &instance :
         package.imported_procedure_instances) {
      if (instance.instance_proxy == imported.proxy) {
        requested_instance = &instance;
        break;
      }
    }
    const std::optional<std::size_t> dependency = package_index_for(
        result.graph,
        schedule,
        imported.root_identity,
        imported.root_relative_path);
    if (!dependency.has_value() ||
        !result.packages[*dependency].has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot refresh effects for an unavailable imported package");
      continue;
    }
    const PackageInterface &dependency_interface =
        result.packages[*dependency]->interface;
    bool has_effect_summary = false;
    const std::vector<InterfaceDeclaration::Effect> *interface_effects = nullptr;
    const std::vector<InterfaceDeclaration::ReturnValue> *interface_returns = nullptr;
    const std::vector<InterfaceDeclaration::FieldWrite> *interface_writes = nullptr;
    if (requested_instance != nullptr) {
      const InterfaceProcedureInstance *concrete = nullptr;
      for (const InterfaceProcedureInstance &candidate :
           dependency_interface.procedure_instances) {
        if (candidate.template_name ==
                requested_instance->public_template_name &&
            candidate.instance_name == imported.public_name) {
          concrete = &candidate;
          break;
        }
      }
      if (concrete == nullptr) {
        diagnostics.error(
            SourceRange::invalid(),
            "cannot refresh effects for missing imported procedure instance '" +
                imported.public_name + "'");
        continue;
      }
      has_effect_summary = concrete->has_effect_summary;
      interface_effects = &concrete->effects;
      interface_returns = &concrete->return_values;
      interface_writes = &concrete->field_writes;
    } else {
      const InterfaceDeclaration *declaration = find_interface_declaration(
          dependency_interface, imported.public_name);
      if (declaration == nullptr) {
        diagnostics.error(
            SourceRange::invalid(),
            "cannot refresh effects for missing imported declaration '" +
                imported.public_name + "'");
        continue;
      }
      has_effect_summary = declaration->has_effect_summary;
      interface_effects = &declaration->effects;
      interface_returns = &declaration->return_values;
      interface_writes = &declaration->field_writes;
    }
    imported.has_effect_summary = has_effect_summary;
    for (const InterfaceDeclaration::Effect &effect : *interface_effects) {
      package.imported_effects.push_back(
          import_interface_effect(imported.proxy, effect));
    }
    for (const InterfaceDeclaration::ReturnValue &returned :
         *interface_returns) {
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
         *interface_writes) {
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
  result.root_package = full.root_package;
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
  empty_manifest.root_package =
      result.graph.package(result.graph.root_package).identity;
  result.resolved_program_digest = hash_resolved_program(
      sources,
      result.graph,
      options.target,
      empty_manifest,
      options.compiler_content_identity,
      options.configuration);
}

// A source change can affect every package that imports the changed package,
// directly or transitively, but never an unrelated dependency. The schedule's
// reverse adjacency rows turn that closure into an O(packages + imports)
// worklist rather than repeatedly scanning all edges to a fixed point. Changed
// PackageIds and each consumer row are sorted, so discovery remains
// deterministic even when several consumers become affected together.
[[nodiscard]] std::vector<bool> affected_packages(
    const WorkspaceDependencyIndex &schedule,
    const std::vector<PackageId> &changed_packages) {
  std::vector<bool> affected(
      schedule.consumers_by_dependency.size(), false);
  std::vector<std::size_t> worklist;
  for (PackageId package : changed_packages) {
    if (package.is_valid() &&
        static_cast<std::size_t>(package.value) < affected.size() &&
        !affected[package.value]) {
      affected[package.value] = true;
      worklist.push_back(package.value);
    }
  }
  std::sort(worklist.begin(), worklist.end());
  for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor) {
    const std::size_t dependency = worklist[cursor];
    for (std::size_t consumer :
         schedule.consumers_by_dependency[dependency]) {
      if (affected[consumer]) continue;
      affected[consumer] = true;
      worklist.push_back(consumer);
    }
  }
  return affected;
}

// Invalidates effect/obligation closure for one changed package and every
// transitive consumer without discarding reusable body HIR. InterfaceReady rows
// already need body work and therefore remain at their earlier phase.
void invalidate_package_closure(
    const WorkspaceDependencyIndex &schedule,
    std::span<const PackageId> changed_packages,
    CompileWorkspaceResult &result) {
  const std::vector<bool> affected =
      affected_packages(schedule, std::vector<PackageId>(
          changed_packages.begin(), changed_packages.end()));
  for (std::size_t index = 0; index < affected.size(); ++index) {
    if (!affected[index] || !result.packages[index].has_value()) continue;
    CompiledPackage &package = *result.packages[index];
    if (package.semantic_progress == PackageSemanticProgress::ClosureReady) {
      package.semantic_progress = PackageSemanticProgress::BodiesReady;
    }
  }
}

// Appends one workspace-owned scheduling row while preserving the direct
// product-to-package owner table. Eager inputs use an invalid owner. The owner
// vector must already match the graph because losing that parallel-array
// invariant would make a later ready product impossible to dispatch safely.
[[nodiscard]] SemanticProductId append_workspace_semantic_product(
    CompileWorkspaceResult &result, SemanticProductKind kind,
    std::span<const SemanticProductId> dependencies, PackageId owner,
    bool completed, DiagnosticSink &diagnostics) {
  if (result.semantic_products.package_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.constant_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.declaration_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.type_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.condition_by_product.size() !=
          result.semantic_graph.products.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "semantic product owner table is inconsistent with its graph");
    return {};
  }
  std::string reason;
  const SemanticProductId product =
      completed ? append_completed_semantic_product(result.semantic_graph, kind,
                                                    dependencies, reason)
                : append_semantic_product(result.semantic_graph, kind,
                                          dependencies, reason);
  if (!product.is_valid()) {
    diagnostics.error(SourceRange::invalid(),
                      "cannot append " +
                          std::string(semantic_product_kind_name(kind)) + ": " +
                          reason);
    return {};
  }
  result.semantic_products.package_by_product.push_back(owner);
  result.semantic_products.constant_by_product.push_back({});
  result.semantic_products.declaration_by_product.push_back({});
  result.semantic_products.type_by_product.push_back({});
  result.semantic_products.condition_by_product.push_back({});
  return product;
}

// Appends the eager products for one already parsed package in the current
// source generation. Assembly-only files are intentionally excluded: parsed
// assembly becomes its own target product later, while every Draft file has a
// SyntaxTree now and receives one ParsedFile row in canonical package order.
[[nodiscard]] bool append_parsed_package_products(
    const WorkspacePackage &package, std::size_t package_index,
    CompileWorkspaceResult &result, DiagnosticSink &diagnostics) {
  PackageSemanticProducts &products =
      result.semantic_products.packages[package_index];
  products.parsed_files.clear();
  for (const LoadedPackageFile &file : package.loaded.files) {
    if (!file.syntax.has_value())
      continue;
    const std::array dependencies{result.semantic_products.source_generation};
    const SemanticProductId parsed = append_workspace_semantic_product(
        result, SemanticProductKind::ParsedFile, dependencies, {}, true,
        diagnostics);
    if (!parsed.is_valid())
      return false;
    products.parsed_files.push_back(parsed);
  }
  products.imports = append_workspace_semantic_product(
      result, SemanticProductKind::PackageImports, products.parsed_files,
      PackageId{static_cast<std::uint32_t>(package_index)}, true, diagnostics);
  return products.imports.is_valid();
}

// Creates one successor source generation and its package interface tasks.
// Initial construction records all parsed packages. A later source transition
// records new eager file/import products only for reparsed packages, reuses the
// immutable inputs of untouched packages, supersedes the selected packages'
// earlier name/interface products, and appends new tasks dependency-first.
[[nodiscard]] bool prepare_workspace_interface_products(
    const WorkspaceDependencyIndex &schedule, const std::vector<bool> &selected,
    std::span<const PackageId> reparsed_packages,
    CompileWorkspaceResult &result, DiagnosticSink &diagnostics) {
  const bool initial = !result.semantic_products.target.is_valid();
  if (initial) {
    result.semantic_products.packages.resize(result.graph.packages.size());
    result.semantic_products.target = append_workspace_semantic_product(
        result, SemanticProductKind::TargetProfile, {}, {}, true, diagnostics);
    if (!result.semantic_products.target.is_valid())
      return false;
  } else if (result.semantic_products.packages.size() !=
             result.graph.packages.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "semantic package product table no longer matches the workspace graph");
    return false;
  }

  result.semantic_products.source_generation =
      append_workspace_semantic_product(result,
                                        SemanticProductKind::SourceGeneration,
                                        {}, {}, true, diagnostics);
  if (!result.semantic_products.source_generation.is_valid())
    return false;

  std::vector<bool> reparsed(result.graph.packages.size(), initial);
  for (PackageId package : reparsed_packages) {
    if (!package.is_valid() ||
        static_cast<std::size_t>(package.value) >= reparsed.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "source transition names an out-of-range semantic package");
      return false;
    }
    reparsed[package.value] = true;
  }
  for (std::size_t package_index = 0;
       package_index < result.graph.packages.size(); ++package_index) {
    if (!reparsed[package_index])
      continue;
    if (!append_parsed_package_products(result.graph.packages[package_index],
                                        package_index, result, diagnostics)) {
      return false;
    }
  }

  // Supersede the entire selected old interface slice before appending any
  // successor. selected is transitively closed for an interface change, so no
  // active consumer remains blocked on a superseded dependency.
  std::vector<SemanticProductId> superseded;
  for (std::size_t package_index = 0; package_index < selected.size();
       ++package_index) {
    if (!selected[package_index])
      continue;
    const PackageSemanticProducts &products =
        result.semantic_products.packages[package_index];
    if (products.name_set.is_valid())
      superseded.push_back(products.name_set);
    superseded.insert(
        superseded.end(),
        products.declaration_types.begin(),
        products.declaration_types.end());
    superseded.insert(
        superseded.end(),
        products.natural_layouts.begin(),
        products.natural_layouts.end());
    superseded.insert(
        superseded.end(),
        products.conditions.begin(),
        products.conditions.end());
    superseded.insert(
        superseded.end(), products.constants.begin(), products.constants.end());
    if (products.opaque_synthesis_set.is_valid())
      superseded.push_back(products.opaque_synthesis_set);
    if (products.package_interface.is_valid()) {
      superseded.push_back(products.package_interface);
    }
  }
  if (!superseded.empty()) {
    std::string reason;
    if (!supersede_semantic_products(result.semantic_graph, superseded,
                                     reason)) {
      diagnostics.error(SourceRange::invalid(),
                        "cannot advance semantic source generation: " + reason);
      return false;
    }
  }

  for (auto position = schedule.consumer_first_order.rbegin();
       position != schedule.consumer_first_order.rend(); ++position) {
    const std::size_t package_index = *position;
    if (!selected[package_index])
      continue;
    PackageSemanticProducts &products =
        result.semantic_products.packages[package_index];
    products.declaration_types.clear();
    products.natural_layouts.clear();
    products.conditions.clear();
    products.constants.clear();
    std::vector<SemanticProductId> dependencies;
    dependencies.push_back(result.semantic_products.target);
    dependencies.push_back(products.imports);
    dependencies.insert(dependencies.end(), products.parsed_files.begin(),
                        products.parsed_files.end());
    for (std::size_t edge_index :
         schedule.import_edges_by_consumer[package_index]) {
      const PackageImport &import = result.graph.imports[edge_index];
      const std::size_t dependency_index =
          static_cast<std::size_t>(import.imported_package.value);
      const SemanticProductId dependency_interface =
          result.semantic_products.packages[dependency_index].package_interface;
      if (!dependency_interface.is_valid()) {
        diagnostics.error(
            SourceRange::invalid(),
            "package interface product is missing before its consumer");
        return false;
      }
      dependencies.push_back(dependency_interface);
    }
    const PackageId owner{static_cast<std::uint32_t>(package_index)};
    products.name_set = append_workspace_semantic_product(
        result, SemanticProductKind::PackageNameSet, dependencies, owner, false,
        diagnostics);
    if (!products.name_set.is_valid())
      return false;
    products.opaque_synthesis_set = {};
    const std::array interface_dependencies{products.name_set};
    products.package_interface = append_workspace_semantic_product(
        result, SemanticProductKind::PackageInterface, interface_dependencies,
        owner, false, diagnostics);
    if (!products.package_interface.is_valid())
      return false;
  }
  return true;
}

// Completes the package-interface payload after every named constant product
// has published. Interface-discovery commands reach this helper with aggregate
// declaration semantics already complete; ordinary complete compilation first
// consumes the staged discovery payload without reevaluating its constants.
[[nodiscard]] bool finalize_workspace_package_interface(
    SourceManager &sources,
    const CompileWorkspaceOptions &options,
    WorkspacePackage &workspace_package,
    CompiledPackage &package,
    DiagnosticSink &diagnostics) {
  if (package.declaration_discovery.terminal) {
    package.declarations = finish_package_semantics_from_products(
        sources,
        workspace_package.loaded,
        options.target.facts,
        CompileTimeSynthesisMode::Reject,
        std::move(package.declaration_discovery),
        diagnostics);
    PackageDeclarationDiscovery empty_discovery;
    package.declaration_discovery = std::move(empty_discovery);
    if (!package.declarations.ok) return false;
  }

  SemanticPackage interface_context_package;
  ConstantTable interface_context_constants;
  if (!append_compile_time_body_synthesis_sites(
          sources,
          workspace_package.loaded,
          options.target.facts,
          package.declarations,
          interface_context_package,
          interface_context_constants,
          diagnostics)) {
    return false;
  }
  package.metadata = collect_agent_metadata(
      sources,
      workspace_package.loaded,
      interface_context_package,
      options.attachments,
      diagnostics);
  if (!package.metadata.ok) return false;
  if (options.validation_kind == ValidationKind::None &&
      package.validation_context.empty() &&
      has_agent_obligation_record(package.metadata)) {
    package.validation_context = load_validation_context(
        sources, workspace_package, options.workspace, diagnostics);
    package.validation_context_is_typed = false;
  }
  package.interface =
      options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis &&
              has_synthesis_record(package.metadata)
          ? withheld_package_interface(
                workspace_package.identity, package.declarations.package)
          : build_package_interface(
                workspace_package.identity,
                package.declarations.package,
                package.declarations.constants,
                package.metadata,
                diagnostics);
  if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
    package.obligations = build_agent_obligations(
        workspace_package.identity,
        sources,
        workspace_package.loaded,
        interface_context_package,
        interface_context_constants,
        package.metadata,
        options.target,
        diagnostics,
        package.validation_context);
    if (!package.obligations.ok) return false;
  }
  return !diagnostics.has_errors();
}

// Computes one PackageNameSet transition. Complete compilation first publishes
// eager authored declarations and later re-enters only to close an already
// completed product set. Interface-synthesis discovery retains the aggregate
// compatibility path until opaque declaration/member waits move to the same
// product representation.
[[nodiscard]] std::optional<CompiledPackage> analyze_workspace_package_names(
    SourceManager &sources, const CompileWorkspaceOptions &options,
    const WorkspaceDependencyIndex &schedule, std::size_t package_index,
    std::uint64_t declaration_generation,
    std::vector<AgentValidationContext> validation_context,
    bool validation_context_is_typed, CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  WorkspacePackage &workspace_package = result.graph.packages[package_index];
  AvailablePackageImports available;
  available.consumer_identity = workspace_package.identity;
  for (std::size_t edge_index :
       schedule.import_edges_by_consumer[package_index]) {
    const PackageImport &import = result.graph.imports[edge_index];
    const std::size_t dependency_index =
        static_cast<std::size_t>(import.imported_package.value);
    if (dependency_index >= result.packages.size() ||
        !result.packages[dependency_index].has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "package product ran before its dependency interface was published");
      return std::nullopt;
    }
    available.entries.push_back({
        {import.file, import.syntax},
        &result.packages[dependency_index]->interface,
    });
  }

  const bool schedule_constant_products =
      options.stage == CompileWorkspaceStage::Complete;
  const bool resume_product_discovery =
      schedule_constant_products &&
      result.packages[package_index].has_value() &&
      !result.packages[package_index]->declaration_discovery.terminal;
  CompiledPackage package;
  if (resume_product_discovery) {
    package = *result.packages[package_index];
  }
  if (!resume_product_discovery) {
    package.identity = workspace_package.identity;
    package.declaration_generation = declaration_generation;
    package.semantic_progress = PackageSemanticProgress::InterfaceReady;
    package.validation_context = std::move(validation_context);
    package.validation_context_is_typed = validation_context_is_typed;
  }
  TimingScope package_timing = time_package_phase(
      options.timings, "package declarations: ", workspace_package.identity);
  if (!resume_product_discovery) {
    for (const LoadedPackageFile &file : workspace_package.loaded.files) {
      if (file.kind != PackageFileKind::AssemblySource)
        continue;
      package.assembly_sources.push_back({
          file.relative_name,
          std::string(sources.text(file.source)),
      });
    }
  }
  if (schedule_constant_products) {
    if (resume_product_discovery) {
      if (!finish_package_declaration_discovery(package.declaration_discovery,
                                                diagnostics)) {
        return std::nullopt;
      }
    } else {
      package.declaration_discovery = begin_package_declaration_discovery(
          sources, workspace_package.loaded, available, diagnostics);
    }
    package.declarations.package = package.declaration_discovery.package;
    package.declarations.selections = package.declaration_discovery.selections;
    package.declarations.ok = package.declaration_discovery.terminal &&
        package.declaration_discovery.discovery_ok;
  } else {
    package.declarations = analyze_package_semantics(
        sources, workspace_package.loaded, options.target.facts, available,
        compile_time_synthesis_mode(options.stage), diagnostics);
  }
  if (options.timings != nullptr) {
    options.timings->add_counter("package semantic analyses", 1);
  }

  // The first PackageNameSet transition deliberately publishes a nonterminal
  // payload. The coordinator appends declaration, constant, layout, and
  // condition products from that exact table and blocks this barrier on them.
  if (schedule_constant_products && !package.declaration_discovery.terminal) {
    return package;
  }

  // Imported procedure-dependent layouts still form the legacy package-owner
  // retry. Its deletion gate is plan step 4. Keeping it visibly inside this
  // transitional task prevents the new coordinator from pretending that the
  // resulting dependency mutation is already safe to execute concurrently.
  std::vector<Sha256Digest> attempted_type_request_sets;
  while (package.declarations.ok &&
         !package.declarations.package.imported_type_instantiation_requests
              .empty()) {
    const Sha256Digest request_digest =
        hash_type_instantiation_requests(package, diagnostics);
    if (std::find(attempted_type_request_sets.begin(),
                  attempted_type_request_sets.end(),
                  request_digest) != attempted_type_request_sets.end()) {
      break;
    }
    attempted_type_request_sets.push_back(request_digest);
    if (!publish_type_instantiation_requests(sources, result, schedule, package,
                                             options, diagnostics)) {
      break;
    }
    if (schedule_constant_products) {
      package.declaration_discovery = discover_package_declarations(
          sources,
          workspace_package.loaded,
          options.target.facts,
          available,
          CompileTimeSynthesisMode::Reject,
          diagnostics);
      SemanticAnalysisResult empty_semantics;
      package.declarations = std::move(empty_semantics);
      package.declarations.package = package.declaration_discovery.package;
      package.declarations.selections =
          package.declaration_discovery.selections;
      package.declarations.ok = package.declaration_discovery.terminal &&
          package.declaration_discovery.discovery_ok;
    } else {
      package.declarations = analyze_package_semantics(
          sources,
          workspace_package.loaded,
          options.target.facts,
          available,
          diagnostics);
    }
    if (options.timings != nullptr) {
      options.timings->add_counter("package semantic analyses", 1);
    }
  }
  if (package.declarations.ok &&
      !package.declarations.package.imported_type_instantiation_requests
           .empty()) {
    diagnostics.error(SourceRange::invalid(),
                      "generic type layout requests made no semantic progress");
    package.declarations.ok = false;
  }
  if (!package.declarations.ok)
    return std::nullopt;

  // Complete compilation now lets the workspace graph own every named
  // ConstantValue. Interface discovery retains the aggregate evaluator until
  // synthesis waits themselves move to product outcomes in the next slice.
  if (schedule_constant_products) return package;
  return finalize_workspace_package_interface(
             sources, options, workspace_package, package, diagnostics)
      ? std::optional<CompiledPackage>(std::move(package))
      : std::nullopt;
}

// One private worker result for a frozen interface-scheduling wave. package is
// present for name/interface tasks, while constant and constant_package travel
// together for ConstantValue tasks so task-local TypeIds can be rewritten
// before publication. No field aliases coordinator state; after the wave joins,
// the coordinator alone moves accepted payloads into CompiledPackage rows.
struct WorkspaceInterfaceTaskSlot {
  SemanticProductOutcome outcome;
  std::optional<CompiledPackage> package;
  std::optional<DeclarationTypeProductAttempt> declaration_type;
  std::optional<NaturalLayoutProductAttempt> natural_layout;
  std::optional<ConditionalProductAttempt> condition;
  std::optional<ConstantProductAttempt> constant;
  std::optional<SemanticPackage> constant_package;
  std::size_t constant_shared_type_count = 0;
};

// Canonicalizes a structural TypeId created in one constant task. Every task
// begins with the coordinator's exact TypeStore prefix; IDs below shared_count
// are therefore already canonical. Rows after that prefix must be structural
// constructors supported by constant evaluation and are interned recursively
// into the coordinator store during ordered publication.
[[nodiscard]] std::optional<TypeId> publish_constant_task_type(
    const TypeStore &task,
    TypeId source,
    std::size_t shared_count,
    TypeStore &canonical,
    std::vector<TypeId> &published,
    DiagnosticSink &diagnostics) {
  if (!source.is_valid()) return TypeId{};
  if (source.value < shared_count) return source;
  if (source.value >= task.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "constant product returned an out-of-range task-local type");
    return std::nullopt;
  }
  if (published[source.value].is_valid()) return published[source.value];

  const Type &type = task.type(source);
  const auto publish_element = [&]() -> std::optional<TypeId> {
    return publish_constant_task_type(
        task,
        type.element,
        shared_count,
        canonical,
        published,
        diagnostics);
  };
  TypeId result;
  switch (type.kind) {
  case TypeKind::Pointer: {
    const std::optional<TypeId> element = publish_element();
    if (!element.has_value()) return std::nullopt;
    result = canonical.pointer(*element);
    break;
  }
  case TypeKind::MultiPointer: {
    const std::optional<TypeId> element = publish_element();
    if (!element.has_value()) return std::nullopt;
    result = canonical.multi_pointer(*element);
    break;
  }
  case TypeKind::Slice: {
    const std::optional<TypeId> element = publish_element();
    if (!element.has_value()) return std::nullopt;
    result = canonical.slice(*element);
    break;
  }
  case TypeKind::Array: {
    if (type.owner_evaluated_element_count ||
        type.element_count_expression.is_valid()) {
      diagnostics.error(
          type.declaration,
          "constant product returned a non-concrete array type");
      return std::nullopt;
    }
    const std::optional<TypeId> element = publish_element();
    if (!element.has_value()) return std::nullopt;
    result = canonical.array(*element, type.element_count);
    break;
  }
  case TypeKind::Simd: {
    if (type.owner_evaluated_element_count ||
        type.element_count_expression.is_valid()) {
      diagnostics.error(
          type.declaration,
          "constant product returned a non-concrete SIMD type");
      return std::nullopt;
    }
    const std::optional<TypeId> element = publish_element();
    if (!element.has_value()) return std::nullopt;
    result = canonical.simd(
        *element, type.element_count, type.declaration);
    break;
  }
  case TypeKind::Tuple: {
    std::vector<TypeId> members;
    members.reserve(type.members.size());
    for (TypeId member : type.members) {
      const std::optional<TypeId> published_member =
          publish_constant_task_type(
              task,
              member,
              shared_count,
              canonical,
              published,
              diagnostics);
      if (!published_member.has_value()) return std::nullopt;
      members.push_back(*published_member);
    }
    result = canonical.tuple(members);
    break;
  }
  case TypeKind::Procedure: {
    if (type.members.empty()) {
      diagnostics.error(
          type.declaration,
          "constant product returned a procedure type without a result");
      return std::nullopt;
    }
    std::vector<TypeId> members;
    members.reserve(type.members.size());
    for (TypeId member : type.members) {
      const std::optional<TypeId> published_member =
          publish_constant_task_type(
              task,
              member,
              shared_count,
              canonical,
              published,
              diagnostics);
      if (!published_member.has_value()) return std::nullopt;
      members.push_back(*published_member);
    }
    const TypeId procedure_result = members.back();
    members.pop_back();
    result = canonical.procedure(
        members, procedure_result, type.c_calling_convention);
    break;
  }
  default:
    diagnostics.error(
        type.declaration,
        "constant product attempted to publish a non-structural task-local type");
    return std::nullopt;
  }
  published[source.value] = result;
  return result;
}

// Rewrites every nested first-class type value in one constant payload. Other
// payload identities are mathematical values, strings, source-order variants,
// or stable SymbolIds from the shared semantic prefix and need no remapping.
[[nodiscard]] bool publish_constant_task_value(
    const TypeStore &task,
    ConstantValue &value,
    std::size_t shared_count,
    TypeStore &canonical,
    std::vector<TypeId> &published,
    DiagnosticSink &diagnostics) {
  if (value.kind == ConstantKind::Type) {
    const std::optional<TypeId> type = publish_constant_task_type(
        task,
        TypeId{value.type_index},
        shared_count,
        canonical,
        published,
        diagnostics);
    if (!type.has_value()) return false;
    value.type_index = type->value;
  }
  for (ConstantValue &element : value.elements) {
    if (!publish_constant_task_value(
            task,
            element,
            shared_count,
            canonical,
            published,
            diagnostics)) {
      return false;
    }
  }
  return true;
}

// Returns whether a package vector already contains a product for one stable
// symbol. Declaration branches append symbols monotonically, so a direct scan
// keeps duplicate prevention explicit without another mutable index.
[[nodiscard]] bool
has_symbol_product(std::span<const SemanticProductId> products,
                   std::span<const SymbolId> symbols, SymbolId wanted) {
  for (SemanticProductId product : products) {
    if (symbols[product.value] == wanted)
      return true;
  }
  return false;
}

// Concrete parametric type instances are discoveries of the declaration task
// that needed them. Their existing synchronous owner will be replaced by the
// canonical generic-demand product in step 4; until then they must not be
// mistaken for another authored declaration merely because their owner symbol
// copies the template's SyntaxReference.
[[nodiscard]] bool is_parametric_type_instance(const SemanticPackage &package,
                                               SymbolId symbol) {
  for (const ParametricTypeInstanceRecord &instance :
       package.parametric_type_instances) {
    if (instance.instance == symbol)
      return true;
  }
  return false;
}

// Appends the declaration/type-facet products revealed by the current
// append-only package declaration table. Nominal identity already exists after
// collection and receives one completed TypeIdentity row; its independently
// scheduled member-types and natural-layout rows consume that identity. Other
// typed declarations receive one pending TypeIdentity row. All rows depend on
// the same immutable inputs as PackageNameSet, never on the barrier itself.
[[nodiscard]] bool append_package_type_products(
    CompileWorkspaceResult &result,
    std::span<const SemanticProductId> base_dependencies, PackageId owner,
    const CompiledPackage &package, DiagnosticSink &diagnostics) {
  PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  const SemanticPackage &semantic = package.declaration_discovery.package;
  const std::vector<SymbolId> package_symbols =
      semantic.symbols.scope(semantic.package_scope).symbols;
  for (SymbolId symbol : package_symbols) {
    if (has_symbol_product(products.declaration_types,
                           result.semantic_products.declaration_by_product,
                           symbol)) {
      continue;
    }
    const Symbol &declaration = semantic.symbols.symbol(symbol);
    // Interface reconstruction may install a compiler-owned package-scope
    // symbol solely to own the member scope of a nominal nested in an imported
    // procedure signature. Its type graph, including layout, arrived as one
    // already-complete immutable interface packet. Only source declarations
    // have SyntaxReference identities and therefore become declaration work
    // products in this package.
    if (!declaration.syntax.file.is_valid() ||
        !declaration.syntax.node.is_valid() ||
        is_parametric_type_instance(semantic, symbol)) {
      continue;
    }
    const bool nominal =
        declaration.kind == SymbolKind::Type && declaration.type.is_valid() &&
        (semantic.types.type(declaration.type).kind == TypeKind::Struct ||
         semantic.types.type(declaration.type).kind == TypeKind::Enum ||
         semantic.types.type(declaration.type).kind == TypeKind::TaggedUnion ||
         semantic.types.type(declaration.type).kind == TypeKind::RawUnion);
    const bool needs_declaration_type =
        nominal || declaration.kind == SymbolKind::Type ||
        declaration.kind == SymbolKind::Procedure ||
        declaration.kind == SymbolKind::Variable ||
        declaration.kind == SymbolKind::UnresolvedDeclaration;
    if (!needs_declaration_type)
      continue;

    SemanticProductId declaration_product;
    if (nominal) {
      const SemanticProductId identity = append_workspace_semantic_product(
          result, SemanticProductKind::TypeIdentity, base_dependencies, owner,
          true, diagnostics);
      if (!identity.is_valid())
        return false;
      result.semantic_products.type_by_product[identity.value] =
          declaration.type;
      const std::array dependencies{identity};
      declaration_product = append_workspace_semantic_product(
          result, SemanticProductKind::TypeMemberTypes, dependencies, owner,
          false, diagnostics);
    } else {
      declaration_product = append_workspace_semantic_product(
          result, SemanticProductKind::TypeIdentity, base_dependencies, owner,
          false, diagnostics);
    }
    if (!declaration_product.is_valid())
      return false;
    result.semantic_products.declaration_by_product[declaration_product.value] =
        symbol;
    if (declaration.type.is_valid()) {
      result.semantic_products.type_by_product[declaration_product.value] =
          declaration.type;
    }
    products.declaration_types.push_back(declaration_product);

    // A parametric nominal is a symbolic template, not a runtime-bearing type
    // application. Its identity and member-type pattern must be checked now,
    // but no target size exists until a canonical argument tuple creates a
    // concrete instance. Step 4 gives that instance its own layout product;
    // scheduling the template here would invent an impossible dependency on a
    // TypeParameter natural layout.
    if (!nominal || declaration.flags.parametric)
      continue;
    const std::array layout_dependencies{declaration_product};
    const SemanticProductId layout = append_workspace_semantic_product(
        result, SemanticProductKind::TypeNaturalLayout, layout_dependencies,
        owner, false, diagnostics);
    if (!layout.is_valid())
      return false;
    result.semantic_products.declaration_by_product[layout.value] = symbol;
    result.semantic_products.type_by_product[layout.value] = declaration.type;
    products.natural_layouts.push_back(layout);
  }
  return true;
}

// Appends one ConstantValue row for every currently visible package-scope
// constant or still-ambiguous `::` declaration. Symbol order is deterministic;
// later selected branches append only their new rows. Dependencies between
// constants are discovered by evaluators, not inferred from source order.
[[nodiscard]] bool append_package_constant_products(
    CompileWorkspaceResult &result,
    std::span<const SemanticProductId> base_dependencies, PackageId owner,
    const CompiledPackage &package, DiagnosticSink &diagnostics) {
  PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  const SemanticPackage &semantic = package.declaration_discovery.package;
  const std::size_t symbol_count = semantic.symbols.symbol_count();
  for (std::size_t index = 0; index < symbol_count; ++index) {
    const SymbolId symbol{static_cast<std::uint32_t>(index)};
    const Symbol &declaration = semantic.symbols.symbol(symbol);
    if (declaration.scope != semantic.package_scope ||
        (declaration.kind != SymbolKind::Constant &&
         declaration.kind != SymbolKind::UnresolvedDeclaration)) {
      continue;
    }
    if (has_symbol_product(products.constants,
                           result.semantic_products.constant_by_product,
                           symbol)) {
      continue;
    }
    // Collection cannot yet distinguish `Alias :: Some_Type` from an ordinary
    // constant when its payload is name/bracket syntax. The declaration-type
    // product owns that classification, so make it an explicit prerequisite
    // of the provisional ConstantValue row. A classified type completes this
    // row without evaluation; a classified constant proceeds normally.
    std::vector<SemanticProductId> dependencies(base_dependencies.begin(),
                                                base_dependencies.end());
    for (SemanticProductId declaration_product : products.declaration_types) {
      if (result.semantic_products
              .declaration_by_product[declaration_product.value] == symbol) {
        dependencies.push_back(declaration_product);
        break;
      }
    }
    const SemanticProductId product = append_workspace_semantic_product(
        result,
        SemanticProductKind::ConstantValue,
        dependencies,
        owner,
        false,
        diagnostics);
    if (!product.is_valid()) return false;
    result.semantic_products.constant_by_product[product.value] = symbol;
    products.constants.push_back(product);
  }
  return true;
}

// Appends one ConstantValue-shaped branch-choice product for every currently
// discovered package/member `when`. SyntaxReference is the stable typed key;
// conditions carry no Constant SymbolId and are dispatched through the
// condition side table.
[[nodiscard]] bool append_package_condition_products(
    CompileWorkspaceResult &result,
    std::span<const SemanticProductId> base_dependencies, PackageId owner,
    const CompiledPackage &package, DiagnosticSink &diagnostics) {
  PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  for (const SemanticSite &site : package.declaration_discovery.package.sites) {
    if (site.kind != SemanticSiteKind::ConditionalDeclaration &&
        site.kind != SemanticSiteKind::ConditionalMember) {
      continue;
    }
    bool present = false;
    for (SemanticProductId product : products.conditions) {
      if (result.semantic_products.condition_by_product[product.value] ==
          site.syntax) {
        present = true;
        break;
      }
    }
    if (present)
      continue;
    const SemanticProductId product = append_workspace_semantic_product(
        result, SemanticProductKind::ConstantValue, base_dependencies, owner,
        false, diagnostics);
    if (!product.is_valid())
      return false;
    result.semantic_products.condition_by_product[product.value] = site.syntax;
    products.conditions.push_back(product);
  }
  return true;
}

// Detects source appended by a completed condition before PackageNameSet tries
// to close. The coordinator must first append products for every new symbol and
// nested condition, then block the barrier on those rows. This read-only check
// keeps graph mutation between frozen waves.
[[nodiscard]] bool
package_has_unindexed_declaration_work(const CompileWorkspaceResult &result,
                                       PackageId owner,
                                       const CompiledPackage &package) {
  const PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  const SemanticPackage &semantic = package.declaration_discovery.package;
  const std::vector<SymbolId> package_symbols =
      semantic.symbols.scope(semantic.package_scope).symbols;
  for (SymbolId symbol : package_symbols) {
    const Symbol &declaration = semantic.symbols.symbol(symbol);
    // Keep this predicate identical to append_package_type_products. Imported
    // interface nominals can own member scopes in the package scope, but they
    // are complete interface inputs rather than unindexed authored work.
    if (!declaration.syntax.file.is_valid() ||
        !declaration.syntax.node.is_valid() ||
        is_parametric_type_instance(semantic, symbol)) {
      continue;
    }
    const bool needs_type =
        declaration.kind == SymbolKind::Type ||
        declaration.kind == SymbolKind::Procedure ||
        declaration.kind == SymbolKind::Variable ||
        declaration.kind == SymbolKind::UnresolvedDeclaration;
    if (needs_type &&
        !has_symbol_product(products.declaration_types,
                            result.semantic_products.declaration_by_product,
                            symbol)) {
      return true;
    }
    const bool needs_constant =
        declaration.kind == SymbolKind::Constant ||
        declaration.kind == SymbolKind::UnresolvedDeclaration;
    if (needs_constant &&
        !has_symbol_product(products.constants,
                            result.semantic_products.constant_by_product,
                            symbol)) {
      return true;
    }
  }
  for (const SemanticSite &site : semantic.sites) {
    if (site.kind != SemanticSiteKind::ConditionalDeclaration &&
        site.kind != SemanticSiteKind::ConditionalMember) {
      continue;
    }
    bool present = false;
    for (SemanticProductId product : products.conditions) {
      if (result.semantic_products.condition_by_product[product.value] ==
          site.syntax) {
        present = true;
        break;
      }
    }
    if (!present)
      return true;
  }
  return false;
}

// Finds the typed product row for one local constant blocker. Products are few
// at this stage and remain in SymbolId order, so a direct scan preserves the
// representation without adding a second package-local index.
[[nodiscard]] std::optional<SemanticProductId> package_constant_product(
    const CompileWorkspaceResult &result,
    PackageId owner,
    SymbolId symbol) {
  const PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  for (SemanticProductId product : products.constants) {
    if (result.semantic_products.constant_by_product[product.value] == symbol) {
      return product;
    }
  }
  return std::nullopt;
}

// Finds the owning declaration-type row for an explicit SymbolId blocker.
[[nodiscard]] std::optional<SemanticProductId>
package_declaration_product(const CompileWorkspaceResult &result,
                            PackageId owner, SymbolId symbol) {
  const PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  for (SemanticProductId product : products.declaration_types) {
    if (result.semantic_products.declaration_by_product[product.value] ==
        symbol) {
      return product;
    }
  }
  return std::nullopt;
}

// Maps one exact semantic type-facet blocker to its workspace product. Identity
// is eager for nominal types and complete structural types never report it as a
// blocker. MemberTypes is owned by the declaration row; NaturalLayout has its
// own typed vector.
[[nodiscard]] std::optional<SemanticProductId>
package_type_facet_product(const CompileWorkspaceResult &result,
                           PackageId owner, TypeFacetDependency dependency) {
  const PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  const std::span<const SemanticProductId> candidates =
      dependency.facet == TypeFacet::NaturalLayout
          ? std::span<const SemanticProductId>(products.natural_layouts)
          : std::span<const SemanticProductId>(products.declaration_types);
  for (SemanticProductId product : candidates) {
    if (result.semantic_products.type_by_product[product.value] ==
        dependency.type) {
      return product;
    }
  }
  return std::nullopt;
}

// Returns the retained semantic site named by one condition product. Sites are
// append-only and few; a direct source-identity scan avoids storing a pointer
// across branch materialization.
[[nodiscard]] const SemanticSite *
find_semantic_site(const SemanticPackage &package, SyntaxReference syntax) {
  for (const SemanticSite &site : package.sites) {
    if (site.syntax == syntax)
      return &site;
  }
  return nullptr;
}

[[nodiscard]] bool
package_condition_needs_materialization(const SemanticPackage &package,
                                        SyntaxReference syntax) {
  for (const ConditionalDeclarationRegion &region :
       package.conditional_declarations) {
    if (region.syntax == syntax)
      return !region.materialized;
  }
  return false;
}

// Builds declaration semantics and preliminary interfaces through the dynamic
// product graph. Workers are deliberately sequential until package tasks stop
// mutating shared generic-layout owner state. Even in this oracle mode, every
// task owns diagnostics and its new package payload; the coordinator publishes
// both in stable SemanticProductId order after the complete wave joins.
[[nodiscard]] bool analyze_workspace_interfaces(
    SourceManager &sources, const CompileWorkspaceOptions &options,
    const WorkspaceDependencyIndex &schedule, const std::vector<bool> &selected,
    std::span<const PackageId> reparsed_packages,
    WorkspaceSemanticChange change, CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  std::vector<std::vector<AgentValidationContext>> retained_validation(
      result.packages.size());
  std::vector<bool> retained_validation_is_typed(result.packages.size(), false);
  std::vector<std::uint64_t> next_declaration_generation(result.packages.size(),
                                                         1);
  for (std::size_t index = 0; index < result.packages.size(); ++index) {
    if (!selected[index])
      continue;
    if (result.packages[index].has_value()) {
      next_declaration_generation[index] =
          result.packages[index]->declaration_generation + 1;
      retained_validation[index] =
          std::move(result.packages[index]->validation_context);
      retained_validation_is_typed[index] =
          change == WorkspaceSemanticChange::Body &&
          result.packages[index]->validation_context_is_typed;
    }
    result.packages[index].reset();
  }

  if (!prepare_workspace_interface_products(
          schedule, selected, reparsed_packages, result, diagnostics)) {
    return false;
  }

  TimingScope declaration_timing =
      options.timings != nullptr
          ? options.timings->scope("declaration semantics")
          : TimingScope{};
  while (true) {
    const SemanticReadyWave wave =
        freeze_semantic_ready_wave(result.semantic_graph);
    if (wave.status == SemanticReadyWaveStatus::Complete)
      return true;
    if (wave.status == SemanticReadyWaveStatus::WaitingForSynthesis) {
      if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
        return true;
      }
      diagnostics.error(
          SourceRange::invalid(),
          "complete semantic analysis is suspended on interface synthesis");
      return false;
    }
    if (wave.status == SemanticReadyWaveStatus::Failed)
      return false;
    if (wave.status == SemanticReadyWaveStatus::Stalled) {
      diagnostics.error(SourceRange::invalid(),
                        "semantic product scheduling stalled: " + wave.failure);
      return false;
    }

    std::vector<WorkspaceInterfaceTaskSlot> slots(wave.products.size());
    // Declaration products still publish through a sequential coordinator
    // oracle. Each successful task advances this wave-local package snapshot in
    // stable product order; blockers never expose their provisional mutation.
    // Step 9 replaces this with task-local deltas plus canonical interning.
    std::vector<std::optional<SemanticPackage>> declaration_wave_packages(
        result.packages.size());
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      const SemanticProductId product = wave.products[task_index];
      WorkspaceInterfaceTaskSlot &slot = slots[task_index];
      if (static_cast<std::size_t>(product.value) >=
          result.semantic_products.package_by_product.size()) {
        slot.outcome.kind = SemanticProductOutcomeKind::Error;
        slot.outcome.failure = "semantic product has no package owner row";
        slot.outcome.diagnostics.error(SourceRange::invalid(),
                                       slot.outcome.failure);
        continue;
      }
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      if (!owner.is_valid() ||
          static_cast<std::size_t>(owner.value) >= result.packages.size()) {
        slot.outcome.kind = SemanticProductOutcomeKind::Error;
        slot.outcome.failure = "semantic package product has no valid owner";
        slot.outcome.diagnostics.error(SourceRange::invalid(),
                                       slot.outcome.failure);
        continue;
      }
      const std::size_t package_index = owner.value;
      const SemanticProductKind kind =
          result.semantic_graph.products[product.value].kind;
      if (kind == SemanticProductKind::TypeIdentity ||
          kind == SemanticProductKind::TypeMemberTypes) {
        if (!result.packages[package_index].has_value()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure =
              "declaration type product ran before eager package discovery";
          slot.outcome.diagnostics.error(SourceRange::invalid(),
                                         slot.outcome.failure);
          continue;
        }
        const SymbolId root =
            result.semantic_products.declaration_by_product[product.value];
        SemanticPackage task_package =
            declaration_wave_packages[package_index].has_value()
                ? *declaration_wave_packages[package_index]
                : result.packages[package_index]->declaration_discovery.package;
        std::vector<SymbolId> completed_declarations;
        const PackageSemanticProducts &package_products =
            result.semantic_products.packages[package_index];
        for (SemanticProductId candidate : package_products.declaration_types) {
          if (result.semantic_graph.products[candidate.value].state ==
              SemanticProductState::Complete) {
            completed_declarations.push_back(
                result.semantic_products
                    .declaration_by_product[candidate.value]);
          }
        }
        slot.declaration_type = resolve_package_declaration_type_product(
            sources, result.graph.packages[package_index].loaded, task_package,
            result.packages[package_index]->declaration_discovery.selections,
            root, completed_declarations,
            result.packages[package_index]
                ->declaration_discovery.published_constants,
            result.packages[package_index]
                ->declaration_discovery.resolved_integers,
            options.target.facts, slot.outcome.diagnostics);
        if (slot.declaration_type->status == TypeProductStatus::Complete) {
          declaration_wave_packages[package_index] = std::move(task_package);
          continue;
        }
        if (slot.declaration_type->status == TypeProductStatus::Error) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "declaration type product failed";
          continue;
        }
        for (SymbolId dependency :
             slot.declaration_type->declaration_dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_declaration_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure = "declaration type dependency has no product";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            break;
          }
          slot.outcome.dependencies.push_back(*blocker);
        }
        for (SymbolId dependency :
             slot.declaration_type->constant_dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_constant_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "declaration constant dependency has no product";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            break;
          }
          slot.outcome.dependencies.push_back(*blocker);
        }
        for (TypeFacetDependency dependency :
             slot.declaration_type->type_dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_type_facet_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "declaration layout expression dependency has no type product";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            break;
          }
          slot.outcome.dependencies.push_back(*blocker);
        }
        if (slot.outcome.kind != SemanticProductOutcomeKind::Error) {
          slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
        }
        continue;
      }
      if (kind == SemanticProductKind::TypeNaturalLayout) {
        if (!result.packages[package_index].has_value()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure =
              "natural-layout product ran before package discovery";
          slot.outcome.diagnostics.error(SourceRange::invalid(),
                                         slot.outcome.failure);
          continue;
        }
        const TypeId nominal =
            result.semantic_products.type_by_product[product.value];
        slot.natural_layout = evaluate_natural_layout_product(
            result.packages[package_index]->declaration_discovery.package.types,
            nominal, slot.outcome.diagnostics);
        if (slot.natural_layout->status == TypeProductStatus::Complete) {
          continue;
        }
        if (slot.natural_layout->status == TypeProductStatus::Error) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "natural-layout product failed";
          continue;
        }
        for (TypeFacetDependency dependency :
             slot.natural_layout->dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_type_facet_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "natural-layout dependency has no type product";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            break;
          }
          slot.outcome.dependencies.push_back(*blocker);
        }
        if (slot.outcome.kind != SemanticProductOutcomeKind::Error) {
          slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
        }
        continue;
      }
      if (kind == SemanticProductKind::PackageNameSet) {
        if (options.stage == CompileWorkspaceStage::Complete &&
            result.packages[package_index].has_value() &&
            !result.packages[package_index]->declaration_discovery.terminal &&
            package_has_unindexed_declaration_work(
                result, owner, *result.packages[package_index])) {
          slot.package = *result.packages[package_index];
          continue;
        }
        slot.package = analyze_workspace_package_names(
            sources, options, schedule, package_index,
            next_declaration_generation[package_index],
            std::move(retained_validation[package_index]),
            retained_validation_is_typed[package_index], result,
            slot.outcome.diagnostics);
        if (!slot.package.has_value()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "package name-set analysis failed";
          if (!slot.outcome.diagnostics.has_errors()) {
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
          }
        }
        continue;
      }
      if (kind == SemanticProductKind::ConstantValue) {
        if (!result.packages[package_index].has_value()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure =
              "constant product ran before its package name set";
          slot.outcome.diagnostics.error(
              SourceRange::invalid(), slot.outcome.failure);
          continue;
        }
        CompiledPackage &package = *result.packages[package_index];
        const SyntaxReference condition_syntax =
            result.semantic_products.condition_by_product[product.value];
        if (condition_syntax.node.is_valid()) {
          const SemanticSite *site = find_semantic_site(
              package.declaration_discovery.package, condition_syntax);
          if (site == nullptr) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure = "conditional product site is unavailable";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            continue;
          }
          SemanticPackage task_package = package.declaration_discovery.package;
          slot.condition = evaluate_conditional_product(
              sources, result.graph.packages[package_index].loaded,
              task_package, options.target.facts, *site,
              package.declaration_discovery.published_constants,
              CompileTimeSynthesisMode::Reject, slot.outcome.diagnostics);
          if (slot.condition->status == CompileTimeProductStatus::Complete) {
            continue;
          }
          if (slot.condition->status ==
              CompileTimeProductStatus::WaitingForSynthesis) {
            slot.outcome.kind = SemanticProductOutcomeKind::WaitingForSynthesis;
            continue;
          }
          if (slot.condition->status == CompileTimeProductStatus::Error) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure = "conditional product evaluation failed";
            continue;
          }
          for (SymbolId dependency : slot.condition->declaration_dependencies) {
            const std::optional<SemanticProductId> blocker =
                package_declaration_product(result, owner, dependency);
            if (!blocker.has_value()) {
              slot.outcome.kind = SemanticProductOutcomeKind::Error;
              slot.outcome.failure =
                  "conditional declaration dependency has no product";
              slot.outcome.diagnostics.error(SourceRange::invalid(),
                                             slot.outcome.failure);
              break;
            }
            slot.outcome.dependencies.push_back(*blocker);
          }
          for (SymbolId dependency : slot.condition->constant_dependencies) {
            const std::optional<SemanticProductId> blocker =
                package_constant_product(result, owner, dependency);
            if (!blocker.has_value()) {
              slot.outcome.kind = SemanticProductOutcomeKind::Error;
              slot.outcome.failure =
                  "conditional constant dependency has no product";
              slot.outcome.diagnostics.error(SourceRange::invalid(),
                                             slot.outcome.failure);
              break;
            }
            slot.outcome.dependencies.push_back(*blocker);
          }
          for (TypeFacetDependency dependency :
               slot.condition->type_dependencies) {
            const std::optional<SemanticProductId> blocker =
                package_type_facet_product(result, owner, dependency);
            if (!blocker.has_value()) {
              slot.outcome.kind = SemanticProductOutcomeKind::Error;
              slot.outcome.failure =
                  "conditional type dependency has no product";
              slot.outcome.diagnostics.error(SourceRange::invalid(),
                                             slot.outcome.failure);
              break;
            }
            slot.outcome.dependencies.push_back(*blocker);
          }
          if (slot.outcome.kind != SemanticProductOutcomeKind::Error) {
            slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
          }
          continue;
        }
        const SymbolId root =
            result.semantic_products.constant_by_product[product.value];
        // The declaration prerequisite may have resolved an ambiguous `::`
        // row as a type alias. The provisional ConstantValue product then has
        // the terminal answer "not a constant" and publishes no value. This
        // is classification, not a failed compile-time evaluation.
        if (package.declaration_discovery.package.symbols.symbol(root).kind ==
            SymbolKind::Type) {
          continue;
        }
        slot.constant_package = package.declaration_discovery.package;
        slot.constant_shared_type_count =
            slot.constant_package->types.size();
        slot.constant = evaluate_package_constant_product(
            sources,
            result.graph.packages[package_index].loaded,
            *slot.constant_package,
            options.target.facts,
            root,
            package.declaration_discovery.published_constants,
            CompileTimeSynthesisMode::Reject,
            slot.outcome.diagnostics);
        if (slot.constant->status == CompileTimeProductStatus::Complete) {
          continue;
        }
        if (slot.constant->status ==
            CompileTimeProductStatus::WaitingForSynthesis) {
          slot.outcome.kind =
              SemanticProductOutcomeKind::WaitingForSynthesis;
          continue;
        }
        if (slot.constant->status == CompileTimeProductStatus::Error) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "constant product evaluation failed";
          if (!slot.outcome.diagnostics.has_errors()) {
            slot.outcome.diagnostics.error(
                SourceRange::invalid(), slot.outcome.failure);
          }
          continue;
        }
        for (SymbolId dependency : slot.constant->declaration_dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_declaration_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "constant declaration dependency has no product";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            break;
          }
          slot.outcome.dependencies.push_back(*blocker);
        }
        for (SymbolId dependency : slot.constant->constant_dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_constant_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "constant product names a dependency without a product";
            slot.outcome.diagnostics.error(
                SourceRange::invalid(), slot.outcome.failure);
            break;
          }
          slot.outcome.dependencies.push_back(*blocker);
        }
        for (TypeFacetDependency dependency :
             slot.constant->type_dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_type_facet_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure = "constant type dependency has no product";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            break;
          }
          slot.outcome.dependencies.push_back(*blocker);
        }
        if (slot.outcome.kind != SemanticProductOutcomeKind::Error) {
          slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
        }
        continue;
      }
      if (kind == SemanticProductKind::OpaqueSynthesisSet) {
        slot.outcome.kind = SemanticProductOutcomeKind::WaitingForSynthesis;
        continue;
      }
      if (kind == SemanticProductKind::PackageInterface) {
        if (!result.packages[package_index].has_value()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure =
              "package interface ran before its name-set payload was published";
          slot.outcome.diagnostics.error(SourceRange::invalid(),
                                         slot.outcome.failure);
          continue;
        }
        const PackageSemanticProducts &products =
            result.semantic_products.packages[package_index];
        for (SemanticProductId constant : products.constants) {
          if (result.semantic_graph.products[constant.value].state !=
              SemanticProductState::Complete) {
            slot.outcome.dependencies.push_back(constant);
          }
        }
        if (!slot.outcome.dependencies.empty()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
          continue;
        }
        if (result.packages[package_index]
                ->declaration_discovery.terminal) {
          slot.package = *result.packages[package_index];
          if (!finalize_workspace_package_interface(
                  sources,
                  options,
                  result.graph.packages[package_index],
                  *slot.package,
                  slot.outcome.diagnostics)) {
            slot.package.reset();
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure = "package interface finalization failed";
            if (!slot.outcome.diagnostics.has_errors()) {
              slot.outcome.diagnostics.error(
                  SourceRange::invalid(), slot.outcome.failure);
            }
          }
        }
        continue;
      }
      slot.outcome.kind = SemanticProductOutcomeKind::Error;
      slot.outcome.failure =
          "interface scheduler received an unexpected product kind";
      slot.outcome.diagnostics.error(SourceRange::invalid(),
                                     slot.outcome.failure);
    }

    // A successful name-set task may discover an opaque declaration/member
    // synthesis set. Append those set products only after every task in the
    // frozen wave has produced its private payload, and do so in wave order.
    // The name-set outcome then names the exact new blocker; the package
    // payload remains available to the resolver while neither its name barrier
    // nor its interface is falsely marked complete.
    if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
      for (std::size_t task_index = 0; task_index < wave.products.size();
           ++task_index) {
        WorkspaceInterfaceTaskSlot &slot = slots[task_index];
        if (!slot.package.has_value() ||
            !has_interface_synthesis(*slot.package)) {
          continue;
        }
        const SemanticProductId name_set = wave.products[task_index];
        if (result.semantic_graph.products[name_set.value].kind !=
            SemanticProductKind::PackageNameSet) {
          continue;
        }
        const PackageId owner =
            result.semantic_products.package_by_product[name_set.value];
        PackageSemanticProducts &package_products =
            result.semantic_products.packages[owner.value];
        package_products.opaque_synthesis_set =
            append_workspace_semantic_product(
                result, SemanticProductKind::OpaqueSynthesisSet,
                result.semantic_graph.products[name_set.value].dependencies,
                owner, false, slot.outcome.diagnostics);
        if (!package_products.opaque_synthesis_set.is_valid()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "cannot publish opaque package synthesis set";
          continue;
        }
        slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
        slot.outcome.dependencies = {
            package_products.opaque_synthesis_set,
        };
      }
    }

    if (options.stage == CompileWorkspaceStage::Complete) {
      // The initial PackageNameSet task publishes only eager declarations.
      // Append every product revealed by that stable table before allowing the
      // barrier to close. Later selected branches use the same idempotent
      // helpers, so only newly appended symbols/sites receive rows.
      for (std::size_t task_index = 0; task_index < wave.products.size();
           ++task_index) {
        WorkspaceInterfaceTaskSlot &slot = slots[task_index];
        if (!slot.package.has_value() ||
            slot.outcome.kind != SemanticProductOutcomeKind::Complete ||
            slot.package->declaration_discovery.terminal) {
          continue;
        }
        const SemanticProductId name_set = wave.products[task_index];
        if (result.semantic_graph.products[name_set.value].kind !=
            SemanticProductKind::PackageNameSet) {
          continue;
        }
        const PackageId owner =
            result.semantic_products.package_by_product[name_set.value];
        const std::vector<SemanticProductId> base_dependencies =
            result.semantic_graph.products[name_set.value].dependencies;
        const bool appended =
            append_package_type_products(result, base_dependencies, owner,
                                         *slot.package,
                                         slot.outcome.diagnostics) &&
            append_package_constant_products(result, base_dependencies, owner,
                                             *slot.package,
                                             slot.outcome.diagnostics) &&
            append_package_condition_products(result, base_dependencies, owner,
                                              *slot.package,
                                              slot.outcome.diagnostics);
        if (!appended) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "cannot append package declaration products";
          continue;
        }
        const PackageSemanticProducts &package_products =
            result.semantic_products.packages[owner.value];
        slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
        slot.outcome.dependencies = package_products.declaration_types;
        slot.outcome.dependencies.insert(
            slot.outcome.dependencies.end(),
            package_products.natural_layouts.begin(),
            package_products.natural_layouts.end());
        slot.outcome.dependencies.insert(slot.outcome.dependencies.end(),
                                         package_products.conditions.begin(),
                                         package_products.conditions.end());
        slot.outcome.dependencies.insert(slot.outcome.dependencies.end(),
                                         package_products.constants.begin(),
                                         package_products.constants.end());
      }

      for (std::size_t task_index = 0; task_index < wave.products.size();
           ++task_index) {
        WorkspaceInterfaceTaskSlot &slot = slots[task_index];
        if (!slot.package.has_value() ||
            slot.outcome.kind != SemanticProductOutcomeKind::Complete) {
          continue;
        }
        const SemanticProductId name_set = wave.products[task_index];
        if (result.semantic_graph.products[name_set.value].kind !=
            SemanticProductKind::PackageNameSet ||
            !slot.package->declaration_discovery.terminal) {
          continue;
        }
        const PackageId owner =
            result.semantic_products.package_by_product[name_set.value];
        const std::array constant_dependencies{name_set};
        if (!append_package_constant_products(result, constant_dependencies,
                                              owner, *slot.package,
                                              slot.outcome.diagnostics)) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "cannot append package constant products";
        }
      }
    }

    // Fix the declaration prefix before publishing any task-local structural
    // type. A frozen wave may contain both declaration and constant products.
    // Declaration tasks currently advance one sequential package snapshot,
    // whereas constant tasks retain the prefix they observed at wave start. If
    // constants were interned first and this snapshot then replaced TypeStore,
    // a returned tuple TypeId could silently become an unrelated nominal row.
    // Step 9 replaces the whole-snapshot oracle with task deltas; until then,
    // this coordinator order establishes the one canonical prefix for the wave.
    for (std::size_t package_index = 0;
         package_index < declaration_wave_packages.size(); ++package_index) {
      if (!declaration_wave_packages[package_index].has_value() ||
          !result.packages[package_index].has_value()) {
        continue;
      }
      result.packages[package_index]->declaration_discovery.package =
          std::move(*declaration_wave_packages[package_index]);
      result.packages[package_index]->declarations.package =
          result.packages[package_index]->declaration_discovery.package;
    }

    // Canonical structural type interning is coordinator-owned and follows
    // stable product order after the declaration prefix is fixed. A task row
    // below constant_shared_type_count still names the unchanged wave-start
    // prefix; every later row is structurally rebuilt in the now-final store.
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      WorkspaceInterfaceTaskSlot &slot = slots[task_index];
      if (!slot.constant.has_value() ||
          slot.constant->status != CompileTimeProductStatus::Complete ||
          slot.outcome.kind != SemanticProductOutcomeKind::Complete ||
          !slot.constant->result.has_value() ||
          !slot.constant_package.has_value()) {
        continue;
      }
      const SemanticProductId product = wave.products[task_index];
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      CompiledPackage &package = *result.packages[owner.value];
      TypeStore &canonical = package.declaration_discovery.package.types;
      std::vector<TypeId> published(slot.constant_package->types.size());
      ConstantValue &value = slot.constant->result->value;
      const bool value_ok = publish_constant_task_value(
          slot.constant_package->types,
          value,
          slot.constant_shared_type_count,
          canonical,
          published,
          slot.outcome.diagnostics);
      const std::optional<TypeId> value_type = publish_constant_task_type(
          slot.constant_package->types,
          slot.constant->result->type,
          slot.constant_shared_type_count,
          canonical,
          published,
          slot.outcome.diagnostics);
      if (!value_ok || !value_type.has_value()) {
        slot.outcome.kind = SemanticProductOutcomeKind::Error;
        slot.outcome.failure =
            "constant product type publication failed";
        continue;
      }
      slot.constant->result->type = *value_type;
    }

    std::vector<SemanticProductOutcome> outcomes;
    outcomes.reserve(slots.size());
    for (WorkspaceInterfaceTaskSlot &slot : slots) {
      outcomes.push_back(std::move(slot.outcome));
    }
    std::string publication_error;
    if (!publish_semantic_ready_wave(result.semantic_graph, wave, outcomes,
                                     diagnostics, publication_error)) {
      diagnostics.error(SourceRange::invalid(),
                        "cannot publish semantic product wave: " +
                            publication_error);
      return false;
    }

    // Publish the stable declaration-to-type side table after the graph states
    // become Complete. The semantic payload itself was installed above so
    // constant type interning could consume its final prefix.
    for (SemanticProductId product : wave.products) {
      const SemanticProductKind kind =
          result.semantic_graph.products[product.value].kind;
      if ((kind != SemanticProductKind::TypeIdentity &&
           kind != SemanticProductKind::TypeMemberTypes) ||
          result.semantic_graph.products[product.value].state !=
              SemanticProductState::Complete) {
        continue;
      }
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      const SymbolId symbol =
          result.semantic_products.declaration_by_product[product.value];
      result.semantic_products.type_by_product[product.value] =
          result.packages[owner.value]
              ->declaration_discovery.package.symbols.symbol(symbol)
              .type;
    }

    // Natural-layout packets mutate exactly one canonical facet after their
    // graph state becomes Complete. The owner SymbolId side table preserves the
    // source-order AggregateMember offset mapping.
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      const SemanticProductId product = wave.products[task_index];
      if (result.semantic_graph.products[product.value].kind !=
              SemanticProductKind::TypeNaturalLayout ||
          result.semantic_graph.products[product.value].state !=
              SemanticProductState::Complete ||
          !slots[task_index].natural_layout.has_value()) {
        continue;
      }
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      CompiledPackage &package = *result.packages[owner.value];
      if (!publish_natural_layout_product(
              package.declaration_discovery.package,
              result.semantic_products.declaration_by_product[product.value],
              result.semantic_products.type_by_product[product.value],
              std::move(*slots[task_index].natural_layout), diagnostics)) {
        return false;
      }
      package.declarations.package = package.declaration_discovery.package;
    }

    // A completed package-level condition publishes its boolean selection and
    // appends only the chosen branch to the retained declaration table. New
    // symbols and nested `else when` sites receive products when PackageNameSet
    // next re-enters; no authored declaration or earlier branch is rebuilt.
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      const SemanticProductId product = wave.products[task_index];
      if (result.semantic_graph.products[product.value].kind !=
              SemanticProductKind::ConstantValue ||
          result.semantic_graph.products[product.value].state !=
              SemanticProductState::Complete ||
          !slots[task_index].condition.has_value()) {
        continue;
      }
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      CompiledPackage &package = *result.packages[owner.value];
      const SyntaxReference site =
          result.semantic_products.condition_by_product[product.value];
      if (package.declaration_discovery.selections.find(site) == nullptr) {
        package.declaration_discovery.selections.entries.push_back(
            {site, slots[task_index].condition->selected_true});
      }
      if (package_condition_needs_materialization(
              package.declaration_discovery.package, site) &&
          !materialize_conditional_declaration(
              sources, result.graph.packages[owner.value].loaded,
              package.declaration_discovery.selections, site,
              package.declaration_discovery.package, diagnostics)) {
        return false;
      }
      package.declarations.package = package.declaration_discovery.package;
      package.declarations.selections =
          package.declaration_discovery.selections;
    }
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      const SemanticProductId product = wave.products[task_index];
      if (result.semantic_graph.products[product.value].kind !=
              SemanticProductKind::ConstantValue ||
          result.semantic_graph.products[product.value].state !=
              SemanticProductState::Complete ||
          !slots[task_index].constant.has_value() ||
          !slots[task_index].constant->result.has_value()) {
        continue;
      }
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      CompiledPackage &package = *result.packages[owner.value];
      const SymbolId symbol =
          result.semantic_products.constant_by_product[product.value];
      EvaluatedConstant &constant = *slots[task_index].constant->result;
      // An eagerly ambiguous `Alias :: Existing_Type` has both a provisional
      // ConstantValue row and a declaration TypeIdentity row. Once the latter
      // classifies the symbol as Type, the value result was only classification
      // evidence: publishing its meta-type over the represented alias type
      // would corrupt semantic identity. Type lookup consumes the Type symbol
      // directly, so no ConstantTable binding is needed.
      if (package.declaration_discovery.package.symbols.symbol(symbol).kind ==
          SymbolKind::Type) {
        continue;
      }
      package.declaration_discovery.package.symbols.symbol_mut(symbol).type =
          constant.type;
      ConstantTable &published =
          package.declaration_discovery.published_constants;
      const auto position = std::lower_bound(
          published.bindings.begin(),
          published.bindings.end(),
          symbol.value,
          [](const ConstantBinding &binding, std::uint32_t value) {
            return binding.symbol.value < value;
          });
      published.bindings.insert(position, {symbol, std::move(constant.value)});
      append_unique_symbols(
          package.declaration_discovery.compile_time_synthesis_procedures,
          slots[task_index].constant->compile_time_procedures);
    }
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      if (!slots[task_index].package.has_value())
        continue;
      const PackageId owner =
          result.semantic_products
              .package_by_product[wave.products[task_index].value];
      result.packages[owner.value] = std::move(*slots[task_index].package);
    }
  }
}

} // namespace

CompileWorkspaceResult compile_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  TimingScope pipeline_timing = options.timings != nullptr
      ? options.timings->scope("compiler pipeline")
      : TimingScope{};
  if (options.timings != nullptr) {
    options.timings->add_counter("compiler passes", 1);
  }
  CompileWorkspaceResult result;
  result.compiler_content_identity = options.compiler_content_identity;
  result.target_identity = options.target.facts.identity;
  result.configuration = options.configuration;
  result.validation_kind = options.validation_kind;
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
  configure_package_selection(options);
  TimingScope workspace_timing = options.timings != nullptr
      ? options.timings->scope("workspace loading")
      : TimingScope{};
  WorkspaceLoadResult loaded = load_workspace(
      sources, root_package_directory, options.workspace, diagnostics);
  workspace_timing.finish();
  result.graph = std::move(loaded.graph);
  result.packages.resize(result.graph.packages.size());
  if (!loaded.ok) return result;
  if (options.timings != nullptr) {
    options.timings->add_counter("workspace loads", 1);
    options.timings->add_counter(
        "packages processed",
        static_cast<std::uint64_t>(result.graph.packages.size()));
    std::uint64_t file_count = 0;
    std::uint64_t source_bytes = 0;
    for (const WorkspacePackage &package : result.graph.packages) {
      for (const LoadedPackageFile &file : package.loaded.files) {
        file_count += 1;
        source_bytes += static_cast<std::uint64_t>(sources.text(file.source).size());
      }
    }
    options.timings->add_counter("source files processed", file_count);
    options.timings->add_counter("source bytes processed", source_bytes);
  }

  TimingScope ordering_timing = options.timings != nullptr
      ? options.timings->scope(
            "dependency ordering", TimingVisibility::Detail)
      : TimingScope{};
  result.dependencies = build_dependency_index(result.graph);
  const WorkspaceDependencyIndex &schedule = result.dependencies;
  ordering_timing.finish();
  if (!schedule.valid ||
      schedule.consumer_first_order.size() != result.graph.packages.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "workspace package graph is cyclic or malformed after successful loading");
    return result;
  }

  // Phase 1 publishes dependency interfaces. The initial load selects every
  // row; later source transitions call the same operation with only the
  // changed packages and their transitive consumers.
  const std::vector<bool> all_packages(result.graph.packages.size(), true);
  if (!analyze_workspace_interfaces(
          sources,
          options,
          schedule,
          all_packages,
          {},
          WorkspaceSemanticChange::Interface,
          result,
          diagnostics)) {
    return result;
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
          package->declarations.ok && package->metadata.ok && package->obligations.ok;
    }
    result.ok = every_ready_package_valid &&
        diagnostics.error_count() == initial_errors;
    if (result.ok) {
      result.progress = CompileWorkspaceProgress::InterfaceDiscovery;
    }
    return result;
  }

  bool every_package_ready = true;
  for (const std::optional<CompiledPackage> &package : result.packages) {
    every_package_ready = every_package_ready && package.has_value();
    if (package.has_value()) {
      every_package_ready = every_package_ready && package->declarations.ok &&
          package->metadata.ok;
    }
  }
  result.ok = every_package_ready &&
      diagnostics.error_count() == initial_errors;
  if (!result.ok) return result;
  result.progress = CompileWorkspaceProgress::InterfaceDiscovery;
  if (!continue_compiled_workspace_semantics(
          sources,
          root_package_directory,
          options,
          result,
          diagnostics)) {
    result.ok = false;
    return result;
  }
  const bool needs_target_continuation =
      options.validation_kind != ValidationKind::None ||
      options.lower_mir || options.emit_llvm;
  if (needs_target_continuation &&
      !continue_compiled_workspace(
          sources, options, result, diagnostics)) {
    result.ok = false;
  }
  return result;
}

bool apply_compiled_workspace_source_overrides(
    SourceManager &sources,
    const std::vector<WorkspaceSourceOverride> &overrides,
    WorkspaceSemanticChange change,
    CompileWorkspaceOptions options,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  configure_package_selection(options);
  if (!result.ok ||
      (result.progress != CompileWorkspaceProgress::InterfaceDiscovery &&
       result.progress != CompileWorkspaceProgress::SemanticClosure)) {
    diagnostics.error(
        SourceRange::invalid(),
        "resolved source update requires a successful semantic graph");
    return false;
  }
  if (result.target_identity != options.target.facts.identity ||
      result.compiler_content_identity != options.compiler_content_identity ||
      result.configuration.runtime_assertions !=
          options.configuration.runtime_assertions ||
      result.validation_kind != options.validation_kind) {
    diagnostics.error(
        SourceRange::invalid(),
        "resolved source update options do not match the semantic graph");
    return false;
  }
  if (overrides.empty()) return true;

  TimingScope source_timing = options.timings != nullptr
      ? options.timings->scope("in-memory resolved source transition")
      : TimingScope{};
  const WorkspaceSourceUpdateResult update = apply_workspace_source_overrides(
      sources, overrides, result.graph, diagnostics);
  source_timing.finish();
  if (!update.ok) {
    result.ok = false;
    return false;
  }
  if (options.timings != nullptr) {
    options.timings->add_counter("workspace source transitions", 1);
  }

  const WorkspaceDependencyIndex &schedule = result.dependencies;
  if (!schedule.valid ||
      schedule.consumer_first_order.size() != result.graph.packages.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "workspace package graph became cyclic or malformed during resolved "
        "source update");
    result.ok = false;
    return false;
  }
  std::vector<bool> selected;
  if (change == WorkspaceSemanticChange::Interface) {
    selected = affected_packages(schedule, update.changed_packages);
  } else {
    selected.assign(result.graph.packages.size(), false);
    for (PackageId changed : update.changed_packages) {
      if (changed.is_valid() &&
          static_cast<std::size_t>(changed.value) < selected.size()) {
        selected[changed.value] = true;
      }
    }
  }

  // Every checked expansion first re-enters interface discovery. Even a body
  // expansion is parsed as a complete source file and must prove that it did
  // not create a new declaration/member synthesis prerequisite before body
  // checking resumes. This is a state transition on the existing graph, not a
  // second compilation of the workspace.
  options.stage = CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  options.lower_mir = false;
  options.emit_llvm = false;
  if (!analyze_workspace_interfaces(
          sources,
          options,
          schedule,
          selected,
          update.changed_packages,
          change,
          result,
          diagnostics)) {
    result.ok = false;
    return false;
  }
  invalidate_package_closure(schedule, update.changed_packages, result);

  bool every_ready_package_valid = true;
  for (const std::optional<CompiledPackage> &package : result.packages) {
    if (!package.has_value()) continue;
    every_ready_package_valid = every_ready_package_valid &&
        package->declarations.ok && package->metadata.ok &&
        package->obligations.ok;
  }
  result.ok = every_ready_package_valid &&
      diagnostics.error_count() == initial_errors;
  if (result.ok) {
    result.progress = CompileWorkspaceProgress::InterfaceDiscovery;
    result.validation_entries.clear();
    result.resolved_program_digest.reset();
    result.resolution_manifest.reset();
  }
  return result.ok;
}

bool continue_compiled_workspace_semantics(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  configure_package_selection(options);
  if (!result.ok ||
      result.progress != CompileWorkspaceProgress::InterfaceDiscovery) {
    diagnostics.error(
        SourceRange::invalid(),
        "semantic closure requires one successful interface graph");
    return false;
  }
  if (options.stage != CompileWorkspaceStage::Complete ||
      result.target_identity != options.target.facts.identity ||
      result.compiler_content_identity != options.compiler_content_identity ||
      result.configuration.runtime_assertions !=
          options.configuration.runtime_assertions ||
      result.validation_kind != options.validation_kind) {
    diagnostics.error(
        SourceRange::invalid(),
        "semantic-closure options do not match the interface graph");
    return false;
  }
  for (const std::optional<CompiledPackage> &package : result.packages) {
    if (!package.has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "semantic closure has a package blocked on interface synthesis");
      return false;
    }
  }
  const WorkspaceDependencyIndex &schedule = result.dependencies;
  if (!schedule.valid ||
      schedule.consumer_first_order.size() != result.graph.packages.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "interface workspace package graph became cyclic or malformed before "
        "body checking");
    return false;
  }

  // Phase 2: advance the deterministic body work graph from consumers toward
  // dependencies. Each package work key is its declaration generation plus
  // the canonical externally requested generic set. An equal key reuses the
  // complete body-owned semantic graph and HIR. A changed key starts from the
  // immutable declaration baseline; no checker ever re-enters retained body
  // state. Portable demands cross package TypeStore boundaries and are
  // materialized only inside the owner's new body generation.
  std::vector<std::vector<ProcedureInstantiationDemand>> demands(
      result.graph.packages.size());
  TimingScope body_timing = options.timings != nullptr
      ? options.timings->scope("body semantics")
      : TimingScope{};
  for (std::size_t package_index : schedule.consumer_first_order) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    const std::size_t package_error_count = diagnostics.error_count();
    if (!canonicalize_procedure_demands(
            demands[package_index], diagnostics)) {
      package.body_work_key = {};
      package.semantic_progress = PackageSemanticProgress::InterfaceReady;
      continue;
    }
    const bool stable_declarations =
        package.body_work_key.declaration_generation ==
            package.declaration_generation &&
        package.semantic_progress != PackageSemanticProgress::InterfaceReady &&
        package.bodies.ok;
    const bool reusable = stable_declarations && same_procedure_demands(
        package.body_work_key.procedure_demands, demands[package_index]);
    if (reusable) {
      if (options.timings != nullptr) {
        options.timings->add_counter("package body reuses", 1);
      }
    } else {
      // Until a complete replacement succeeds, the package owns only an
      // authoritative declaration baseline. Any partial BodyCheckResult is
      // diagnostic recovery state and must never satisfy a later reuse key.
      package.semantic_progress = PackageSemanticProgress::InterfaceReady;
      // A changed body can alter package effects and the completed interface
      // seen by every importer, even when signatures are unchanged. Retain
      // consumer HIR but mark its closure products stale before publishing the
      // replacement below.
      const PackageId changed{static_cast<std::uint32_t>(package_index)};
      invalidate_package_closure(
          schedule, std::span<const PackageId>(&changed, 1), result);

      std::vector<ProcedureInstantiationDemand> added_demands;
      const bool extendable = stable_declarations &&
          added_procedure_demands(
              package.body_work_key.procedure_demands,
              demands[package_index],
              added_demands) &&
          !added_demands.empty();
      if (extendable) {
        TimingScope package_timing = time_package_phase(
            options.timings, "package body extensions: ", package.identity);
        const std::size_t previous_checked =
            package.bodies.checked_procedures;
        const std::vector<ProcedureInstantiationSeed> additional_seeds =
            materialize_procedure_demands(
                added_demands, package.bodies.package, diagnostics);
        package.bodies = check_additional_package_instances(
            sources,
            workspace_package.loaded,
            package.declarations.selections,
            std::move(package.bodies),
            options.target.facts,
            diagnostics,
            additional_seeds);
        if (options.timings != nullptr) {
          options.timings->add_counter("package body extensions", 1);
          options.timings->add_counter(
              "procedure bodies checked",
              static_cast<std::uint64_t>(
                  package.bodies.checked_procedures - previous_checked));
        }
      } else {
        TimingScope package_timing = time_package_phase(
            options.timings, "package bodies: ", package.identity);
        SemanticPackage body_input = package.declarations.package;
        const std::vector<ProcedureInstantiationSeed> seeds =
            materialize_procedure_demands(
                demands[package_index], body_input, diagnostics);
        package.bodies = check_package_bodies(
            sources,
            workspace_package.loaded,
            package.declarations.selections,
            body_input,
            package.declarations.constants,
            options.target.facts,
            diagnostics,
            seeds);
        if (options.timings != nullptr) {
          options.timings->add_counter("package body checks", 1);
          options.timings->add_counter(
              "procedure bodies checked",
              static_cast<std::uint64_t>(package.bodies.checked_procedures));
        }
      }
      if (diagnostics.error_count() != package_error_count) {
        package.bodies.ok = false;
      }
      if (package.bodies.ok) {
        package.body_work_key.declaration_generation =
            package.declaration_generation;
        package.body_work_key.procedure_demands = demands[package_index];
        package.semantic_progress = PackageSemanticProgress::BodiesReady;
      } else {
        package.body_work_key = {};
      }
    }
    if (!package.bodies.ok) continue;

    for (const ImportedProcedureInstance &request :
         package.bodies.package.imported_procedure_instances) {
      const std::optional<std::size_t> owner_index = package_index_for(
          result.graph,
          schedule,
          request.root_identity,
          request.root_relative_path);
      if (!owner_index.has_value() ||
          !result.packages[*owner_index].has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "generic procedure owner is unavailable during instantiation");
        continue;
      }

      ProcedureInstantiationDemand demand;
      demand.public_template_name = request.public_template_name;
      Sha256 name_hash;
      hash_field(name_hash, "draft.procedure-instance.v2");
      hash_field(name_hash, request.root_identity);
      hash_field(name_hash, request.root_relative_path);
      hash_field(name_hash, request.public_template_name);
      for (const ParametricArgument &argument : request.arguments) {
        ProcedureInstantiationDemandArgument transferred;
        transferred.is_type = argument.is_type;
        hash_u64(name_hash, argument.is_type ? 1 : 0);
        const TypeId argument_type =
            argument.is_type ? argument.type : argument.value_type;
        transferred.type = export_interface_type(
            package.identity,
            package.bodies.package,
            argument_type,
            diagnostics);
        name_hash.update(hash_interface_type_graph(transferred.type).bytes);
        if (!argument.is_type) {
          hash_field(name_hash, argument.value.integer.to_decimal());
          transferred.value = argument.value;
        }
        demand.arguments.push_back(std::move(transferred));
      }
      hash_field(name_hash, "static-argument-pack");
      hash_u64(
          name_hash, static_cast<std::uint64_t>(request.pack_types.size()));
      for (TypeId pack_type : request.pack_types) {
        const InterfaceTypeGraph graph = export_interface_type(
            package.identity,
            package.bodies.package,
            pack_type,
            diagnostics);
        const Sha256Digest digest = hash_interface_type_graph(graph);
        name_hash.update(digest.bytes);
        demand.pack_types.push_back(graph);
      }
      demand.digest = name_hash.finalize();
      demand.instance_name = request.public_template_name + "$mono$" +
          demand.digest.hex().substr(0, 24);
      demands[*owner_index].push_back(demand);

      bool named_proxy = false;
      for (ImportedSymbol &imported :
           package.bodies.package.imported_symbols) {
        if (imported.proxy == request.instance_proxy) {
          imported.public_name = demand.instance_name;
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
  body_timing.finish();

  // Validation files are compiled in a parallel graph only after interface
  // synthesis has resolved every declaration they may name. Runtime body holes
  // remain legal checked HIR sites, so this pass can type tests before asking a
  // body synthesizer while still keeping test-only imports out of the ordinary
  // graph above. Its non-None validation kind is also the recursion guard: a
  // validation compile never asks for another validation-context compile.
  TimingScope validation_context_timing = options.timings != nullptr
      ? options.timings->scope(
            "validation context", TimingVisibility::Detail)
      : TimingScope{};
  bool needs_test_context = false;
  bool needs_benchmark_context = false;
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    if (!package.bodies.ok) continue;
    if (package.semantic_progress == PackageSemanticProgress::ClosureReady) {
      continue;
    }
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    package.metadata = collect_agent_metadata(
        sources,
        workspace_package.loaded,
        package.bodies.package,
        options.attachments,
        diagnostics);
    if (!package.metadata.ok ||
        !has_agent_obligation_record(package.metadata) ||
        options.validation_kind != ValidationKind::None) {
      continue;
    }
    if (package.validation_context.empty()) {
      package.validation_context = load_validation_context(
          sources, workspace_package, options.workspace, diagnostics);
      package.validation_context_is_typed = false;
    }
    if (package.validation_context_is_typed) continue;
    for (const AgentValidationContext &validation :
         package.validation_context) {
      needs_test_context = needs_test_context || validation.kind == "test";
      needs_benchmark_context =
          needs_benchmark_context || validation.kind == "benchmark";
    }
  }

  constexpr std::array typed_validation_kinds{
      ValidationKind::Test,
      ValidationKind::Benchmark,
  };
  for (ValidationKind kind : typed_validation_kinds) {
    const bool needed = kind == ValidationKind::Test
        ? needs_test_context
        : needs_benchmark_context;
    if (!needed) continue;
    CompileWorkspaceOptions validation_options = options;
    validation_options.validation_kind = kind;
    validation_options.lower_mir = false;
    validation_options.emit_llvm = false;
    CompileWorkspaceResult validation = compile_workspace(
        sources,
        root_package_directory,
        std::move(validation_options),
        diagnostics);
    if (!validation.ok) {
      result.ok = false;
      return false;
    }
    const WorkspaceDependencyIndex &validation_schedule =
        validation.dependencies;

    for (std::size_t package_index = 0;
         package_index < result.packages.size(); ++package_index) {
      if (!result.packages[package_index].has_value()) continue;
      CompiledPackage &package = *result.packages[package_index];
      const std::optional<std::size_t> validation_index = package_index_for(
          validation.graph,
          validation_schedule,
          package.identity.root_identity,
          package.identity.root_relative_path);
      if (!validation_index.has_value() ||
          !validation.packages[*validation_index].has_value()) {
        continue;
      }
      const CompiledPackage &typed_package =
          *validation.packages[*validation_index];
      if (!enrich_agent_validation_context(
              typed_package.identity,
              validation.graph.packages[*validation_index].loaded,
              typed_package.bodies.package,
              typed_package.bodies.constants,
              typed_package.bodies.program,
              kind,
              validation.validation_entries,
              package.validation_context,
              diagnostics)) {
        result.ok = false;
        return false;
      }
    }
  }
  for (std::optional<CompiledPackage> &package : result.packages) {
    if (!package.has_value() || package->validation_context.empty() ||
        options.validation_kind != ValidationKind::None) {
      continue;
    }
    package->validation_context_is_typed = true;
  }
  validation_context_timing.finish();

  // Phase 3: dependencies now have every requested concrete body. Publish
  // audited effects and complete interfaces dependency-first, then compose
  // consumer denials against those final summaries.
  TimingScope closure_timing = options.timings != nullptr
      ? options.timings->scope("semantic closure")
      : TimingScope{};
  for (auto position = schedule.consumer_first_order.rbegin();
       position != schedule.consumer_first_order.rend(); ++position) {
    const std::size_t package_index = *position;
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    if (!package.bodies.ok) continue;
    if (package.semantic_progress == PackageSemanticProgress::ClosureReady) {
      continue;
    }
    TimingScope package_timing = time_package_phase(
        options.timings, "package closure: ", package.identity);

    refresh_imported_effects(
        package.bodies.package, result, schedule, diagnostics);
    package.obligations = build_agent_obligations(
        workspace_package.identity,
        sources,
        workspace_package.loaded,
        package.bodies.package,
        package.bodies.constants,
        package.metadata,
        options.target,
        diagnostics,
        package.validation_context,
        &package.bodies.program);
    package.effects = summarize_package_effects(
        package.bodies.package,
        package.bodies.program,
        &options.target,
        options.foreign_provider_audits);
    package.native_interop = validate_native_interop(
        package.bodies.package,
        package.bodies.program,
        options.target.facts,
        diagnostics);
    const bool denials_ok = check_package_denials(
        sources,
        workspace_package.loaded,
        package.bodies.package,
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
        package.bodies.package,
        package.bodies.constants,
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
    package.semantic_progress = PackageSemanticProgress::ClosureReady;
  }
  closure_timing.finish();

  bool every_package_closed = true;
  for (const std::optional<CompiledPackage> &package : result.packages) {
    every_package_closed = every_package_closed && package.has_value() &&
        package->semantic_progress == PackageSemanticProgress::ClosureReady;
  }
  result.ok = every_package_closed &&
      diagnostics.error_count() == initial_errors;
  if (result.ok) {
    result.progress = CompileWorkspaceProgress::SemanticClosure;
  }
  return result.ok;
}

bool continue_compiled_workspace(
    SourceManager &sources,
    const CompileWorkspaceOptions &options,
    CompileWorkspaceResult &compiled,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  if (!compiled.ok ||
      (compiled.progress != CompileWorkspaceProgress::SemanticClosure &&
       compiled.progress != CompileWorkspaceProgress::ValidationDiscovery)) {
    diagnostics.error(
        SourceRange::invalid(),
        "target lowering requires one successful checked compiler graph");
    return false;
  }
  if (options.stage != CompileWorkspaceStage::Complete) {
    diagnostics.error(
        SourceRange::invalid(),
        "target lowering cannot continue an interface-discovery request");
    return false;
  }
  if (compiled.target_identity != options.target.facts.identity ||
      compiled.compiler_content_identity != options.compiler_content_identity ||
      compiled.configuration.runtime_assertions !=
          options.configuration.runtime_assertions ||
      compiled.validation_kind != options.validation_kind) {
    diagnostics.error(
        SourceRange::invalid(),
        "target lowering options do not match the checked semantic graph");
    return false;
  }

  const WorkspaceDependencyIndex &schedule = compiled.dependencies;
  if (!schedule.valid ||
      schedule.consumer_first_order.size() != compiled.graph.packages.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "checked workspace package graph became cyclic or malformed before "
        "lowering");
    return false;
  }

  // Phase 4: provider-free target lowering. No package reaches a backend until
  // every cross-package generic proxy has an exact defining symbol. Validation
  // discovery belongs between semantic closure and lowering: every selected
  // entry therefore has a checked body, exact core nominal parameter, and
  // target layout before the compiler constructs a native harness.
  TimingScope lowering_timing = options.timings != nullptr
      ? options.timings->scope("target lowering")
      : TimingScope{};
  if (compiled.progress == CompileWorkspaceProgress::SemanticClosure &&
      options.validation_kind != ValidationKind::None) {
    for (std::size_t package_index = 0;
         package_index < compiled.packages.size(); ++package_index) {
      if (!compiled.packages[package_index].has_value()) continue;
      CompiledPackage &package = *compiled.packages[package_index];
      if (!package.bodies.ok || !package.metadata.ok ||
          !package.obligations.ok || !package.native_interop.ok) {
        continue;
      }
      std::vector<ValidationEntry> discovered = discover_validation_entries(
          options.validation_kind,
          options.workspace.core_content_identity,
          package.identity,
          compiled.graph.packages[package_index].loaded,
          package.bodies.package,
          package.bodies.program,
          diagnostics);
      compiled.validation_entries.insert(
          compiled.validation_entries.end(),
          std::make_move_iterator(discovered.begin()),
          std::make_move_iterator(discovered.end()));
    }
    sort_validation_entries(compiled.validation_entries);
  }

  for (auto position = schedule.consumer_first_order.rbegin();
       position != schedule.consumer_first_order.rend(); ++position) {
    const std::size_t package_index = *position;
    if (!compiled.packages[package_index].has_value()) continue;
    CompiledPackage &package = *compiled.packages[package_index];
    WorkspacePackage &workspace_package =
        compiled.graph.packages[package_index];
    if (!package.bodies.ok || !package.metadata.ok || !package.obligations.ok ||
        !package.native_interop.ok) {
      continue;
    }
    TimingScope package_timing = options.lower_mir || options.emit_llvm
        ? time_package_phase(
              options.timings, "package lowering: ", package.identity)
        : TimingScope{};

    if (options.lower_mir || options.emit_llvm) {
      package.assembly = analyze_aarch64_assembly(
          sources,
          workspace_package.loaded,
          options.target,
          package.bodies.package,
          package.bodies.program,
          diagnostics);
      if (!package.assembly.ok) continue;
      package.mir = lower_package_to_mir(
          package.bodies.package,
          package.bodies.program,
          package.assembly,
          options.configuration.runtime_assertions,
          diagnostics);
      if (options.timings != nullptr) {
        options.timings->add_counter("MIR packages lowered", 1);
      }
      if (!package.mir.ok) continue;
    }
    if (options.emit_llvm) {
      LlvmIrOptions llvm_options;
      llvm_options.package = workspace_package.identity;
      llvm_options.emit_runtime_support =
          package_index ==
          static_cast<std::size_t>(compiled.graph.root_package.value);
      llvm_options.emit_program_entry =
          options.emit_program_entry && llvm_options.emit_runtime_support;
      if (llvm_options.emit_runtime_support) {
        llvm_options.validation_kind = options.validation_kind;
        llvm_options.validation_entries = compiled.validation_entries;
      }
      package.llvm = emit_llvm_ir(
          options.target,
          sources,
          llvm_options,
          package.bodies.package,
          package.declarations.global_initializers,
          package.mir.program,
          diagnostics);
      if (options.timings != nullptr) {
        options.timings->add_counter("LLVM modules emitted", 1);
        options.timings->add_counter(
            "LLVM IR bytes",
            static_cast<std::uint64_t>(package.llvm.text.size()));
      }
      if (!package.llvm.ok) continue;
    }
  }
  lowering_timing.finish();

  compiled.ok = diagnostics.error_count() == initial_errors;
  if (compiled.ok) {
    if (options.lower_mir || options.emit_llvm) {
      compiled.progress = CompileWorkspaceProgress::TargetLowering;
    } else if (options.validation_kind != ValidationKind::None) {
      compiled.progress = CompileWorkspaceProgress::ValidationDiscovery;
    }
  }
  return compiled.ok;
}

ResolvedAgentBoundary capture_resolved_agent_boundary(
    const CompileWorkspaceResult &surface) {
  return capture_agent_boundary(surface);
}

bool validate_resolved_agent_boundaries(
    const ResolvedAgentBoundary &surface,
    const CompileWorkspaceResult &resolved,
    DiagnosticSink &diagnostics) {
  return validate_resolved_agent_boundary_snapshot(
      surface, resolved, diagnostics);
}

CompileWorkspaceResult compile_workspace_with_resolution(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  TimingScope orchestration_timing = options.timings != nullptr
      ? options.timings->scope("resolution orchestration")
      : TimingScope{};
  const std::size_t initial_errors = diagnostics.error_count();

  WorkspacePackageSelection selected_root;
  if (!identify_workspace_package(
          options.workspace.workspace_directory,
          root_package_directory,
          selected_root,
          diagnostics)) {
    return {};
  }
  const ResolutionStoreKey store_key{
      options.target.facts.identity,
      selected_root.identity,
  };

  TimingScope manifest_timing = options.timings != nullptr
      ? options.timings->scope(
            "resolution manifest loading", TimingVisibility::Detail)
      : TimingScope{};
  const ResolutionManifestLoadResult loaded_manifest = load_resolution_manifest(
      options.workspace.workspace_directory, store_key, diagnostics);
  manifest_timing.finish();
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

  // The recursive ordinary compilation above has already matched every pin to
  // the current typed synthesis input, including the current validation source
  // context. The derived validation graph deliberately loads command-only
  // declarations and uses a different context-enrichment mode, so its local
  // obligation digest is not the manifest's ordinary-program digest. Reuse the
  // authenticated pins here; site, kind, source-map, expansion hash, and final
  // validation typing remain checked by the overlay and compiler.
  const ResolutionInputVerification input_verification =
      loaded_manifest.state == ResolutionManifestLoadState::Loaded &&
          options.validation_kind != ValidationKind::None
      ? ResolutionInputVerification::AuthenticatedManifest
      : ResolutionInputVerification::RequireCurrentInput;

  std::vector<bool> matched_pins;
  std::vector<WorkspaceSourceOverride> interface_overrides;
  if (loaded_manifest.state == ResolutionManifestLoadState::Loaded) {
    matched_pins.resize(loaded_manifest.manifest.pins.size(), false);
  }

  // Reproduce dependency-ready interface rounds from pinned bytes. No body is
  // checked until every package interface is complete, and no round observes a
  // same-round expansion. Each nonempty round removes at least one site.
  CompileWorkspaceOptions interface_options = options;
  interface_options.stage =
      CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  interface_options.lower_mir = false;
  interface_options.emit_llvm = false;
  CompileWorkspaceResult interface_surface = compile_workspace(
      sources,
      root_package_directory,
      interface_options,
      diagnostics);
  if (!interface_surface.ok) return interface_surface;
  while (true) {
    if (options.timings != nullptr) {
      options.timings->add_counter("interface discovery rounds", 1);
    }

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
      TimingScope overlay_timing = options.timings != nullptr
          ? options.timings->scope(
                "interface resolution overlay", TimingVisibility::Detail)
          : TimingScope{};
      const ResolutionOverlayResult interface_overlay = build_resolution_overlays(
              sources,
              resolution_packages(interface_surface),
              interface_manifest,
              options.target.facts.identity,
              options.workspace.workspace_directory,
              input_verification,
              {},
              diagnostics);
      overlay_timing.finish();
      if (!interface_overlay.ok) {
        interface_surface.ok = false;
        return interface_surface;
      }
      if (site_count != 0 &&
          !apply_compiled_workspace_source_overrides(
              sources,
              interface_overlay.sources,
              WorkspaceSemanticChange::Interface,
              interface_options,
              interface_surface,
              diagnostics)) {
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

  // With early interfaces installed, advance their exact declarations, types,
  // parsed source, and dependency edges into bodies. The resulting surface
  // state is the authored judgment/synthesis boundary for body overlays.
  CompileWorkspaceOptions body_options = options;
  body_options.stage = CompileWorkspaceStage::Complete;
  body_options.lower_mir = false;
  body_options.emit_llvm = false;
  body_options.workspace.source_overrides = interface_overrides;
  TimingScope body_surface_timing = options.timings != nullptr
      ? options.timings->scope("body semantic continuation")
      : TimingScope{};
  if (!continue_compiled_workspace_semantics(
          sources,
          root_package_directory,
          body_options,
          interface_surface,
          diagnostics)) {
    interface_surface.ok = false;
  }
  body_surface_timing.finish();
  CompileWorkspaceResult body_surface = std::move(interface_surface);
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
    // body_surface owns the complete declarations, types, HIR, effects,
    // denials, and dependency graph. Native commands continue that exact
    // command-local state through MIR and LLVM; no persistent cache is involved.
    TimingScope identity_timing = options.timings != nullptr
        ? options.timings->scope(
              "resolved-program identity", TimingVisibility::Detail)
        : TimingScope{};
    bind_handwritten_program_identity(sources, options, body_surface);
    identity_timing.finish();
    const bool needs_target_continuation =
        options.validation_kind != ValidationKind::None ||
        options.lower_mir || options.emit_llvm;
    if (needs_target_continuation &&
        !continue_compiled_workspace(
            sources, options, body_surface, diagnostics)) {
      body_surface.ok = false;
    }
    return body_surface;
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
  TimingScope body_overlay_timing = options.timings != nullptr
      ? options.timings->scope(
            "body resolution overlay", TimingVisibility::Detail)
      : TimingScope{};
  const ResolutionOverlayResult body_overlay = build_resolution_overlays(
      sources,
      resolution_packages(body_surface),
      body_manifest,
      options.target.facts.identity,
      options.workspace.workspace_directory,
      input_verification,
      {},
      diagnostics);
  body_overlay_timing.finish();
  if (!body_overlay.ok) {
    body_surface.ok = false;
    return body_surface;
  }

  // Every full-manifest pin must belong to exactly one selected stage. An
  // obsolete pin from another graph cannot be silently ignored by an otherwise
  // successful provider-free build.
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

  const ResolvedAgentBoundary surface_boundary =
      capture_agent_boundary(body_surface);
  if (!body_overlay.sources.empty()) {
    CompileWorkspaceOptions update_options = options;
    update_options.stage =
        CompileWorkspaceStage::DiscoverInterfaceSynthesis;
    update_options.lower_mir = false;
    update_options.emit_llvm = false;
    if (!apply_compiled_workspace_source_overrides(
            sources,
            body_overlay.sources,
            WorkspaceSemanticChange::Body,
            update_options,
            body_surface,
            diagnostics) ||
        !reject_generated_synthesis(body_surface, diagnostics)) {
      body_surface.ok = false;
      return body_surface;
    }

    // Body-category expansions cannot alter declarations, so the source
    // transition retains already typed validation context while rebuilding the
    // affected package declarations and consumers. Resume body/effect closure
    // on those exact rows; no workspace or validation graph is loaded again.
    if (!continue_compiled_workspace_semantics(
            sources,
            root_package_directory,
            body_options,
            body_surface,
            diagnostics)) {
      body_surface.ok = false;
      return body_surface;
    }
  }

  // Generated source is allowed to contain ordinary docs but not another
  // provider operation. Every surface synthesis site was removed by the
  // overlay, so any remaining synthesis obligation necessarily came from an
  // expansion. Judgment identities must be exactly the surface set; input
  // digests may legitimately change after generated declarations become
  // visible and are therefore not compared here.
  (void)validate_resolved_agent_boundary_snapshot(
      surface_boundary, body_surface, diagnostics);
  if (diagnostics.error_count() == initial_errors) {
    const Sha256Digest program_digest = hash_resolved_program(
        sources,
        body_surface.graph,
        options.target,
        loaded_manifest.manifest,
        options.compiler_content_identity,
        options.configuration);
    body_surface.resolved_program_digest = program_digest;
    if (options.validation_kind == ValidationKind::None &&
        program_digest != loaded_manifest.manifest.resolved_program_digest) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolution manifest resolved-program identity is stale");
    }
  }
  body_surface.ok = diagnostics.error_count() == initial_errors;
  if (body_surface.ok) {
    body_surface.resolution_manifest = loaded_manifest.manifest;
    if (!body_surface.resolved_program_digest.has_value()) {
      body_surface.resolved_program_digest =
          loaded_manifest.manifest.resolved_program_digest;
    }
    const bool needs_target_continuation =
        options.validation_kind != ValidationKind::None ||
        options.lower_mir || options.emit_llvm;
    if (needs_target_continuation &&
        !continue_compiled_workspace(
            sources, options, body_surface, diagnostics)) {
      body_surface.ok = false;
    }
  }
  return body_surface;
}

} // namespace draft
