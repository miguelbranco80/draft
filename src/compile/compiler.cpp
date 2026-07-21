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
// declaration types, nominal and canonical generic layouts, conditional
// choices, named constants, package name completeness, opaque interface
// synthesis sets, and package interfaces are explicit products in one dynamic
// command-local graph. The coordinator freezes ready waves and publishes
// task-owned diagnostics and payloads in stable product-ID order. Procedure
// template and instance bodies are live dynamic products: authored roots are
// appended before checking, and nested or locally instantiated roots are
// appended between frozen waves with exact discovery edges. Their current
// worker-owned payload is one local HIR arena plus semantic append packet. A
// bounded executor checks a frozen ready wave against one shared prefix, then
// the coordinator remaps and publishes results in canonical product order;
// all packages contribute independent roots to the same ready set, and a new
// cross-package generic body depends on the exact completed consumer body which
// requested it. Direct effects and denials run as bounded procedure-owned
// waves; legal flow/effect SCCs publish dependency-first with their exact
// component edges. Package static data and assembly are explicit barriers, and
// all concrete runtime procedures lower in one bounded workspace MIR wave and
// own product-indexed MIR payloads. Machine functions and artifact layouts then
// publish in separate bounded native waves. Within an unchanged source
// generation, CompileWorkspaceProgress advances so native lowering can continue
// checked state without reloading source. A checked generated-source transition
// appends a successor generation and supersedes only the affected interface
// products while retaining unrelated products.
//
// No state is process-global or persisted as a compiler cache. Resolution
// orchestration consumes generated Draft only through ordinary source
// overrides and may not manufacture semantic or backend nodes. Relevant rules
// are specification sections 10 and 15 and the implementation architecture
// document.

#include "compile/compiler.h"

#include "base/sha256.h"
#include "base/timing.h"
#include "base/work_graph.h"
#include "elaborator/generated_source.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"
#include "elaborator/resolved_program.h"
#include "mir/lower.h"
#include "sema/body_publication.h"
#include "sema/denial.h"
#include "sema/runtime_context.h"
#include "sema/type_resolver.h"
#include "workspace/selection.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
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

// Publishes a private diagnostic packet into the coordinator sink without
// exposing DiagnosticSink's storage for mutation. Interface synthesis body
// attempts sometimes remain deliberately deferred behind an opaque declaration
// or member set; only a terminal attempt calls this operation.
void append_diagnostics(DiagnosticSink &destination,
                        const DiagnosticSink &source) {
  for (const Diagnostic &diagnostic : source.diagnostics()) {
    destination.report(diagnostic.severity, diagnostic.range,
                       diagnostic.message);
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

// Interface discovery may return either a complete declaration payload or a
// deliberately suspended package with at least one typed synthesis obligation.
// Both are successful semantic surfaces. The latter must not pretend that its
// incomplete canonical declaration package is complete merely to satisfy a
// generic pipeline success check.
[[nodiscard]] bool is_valid_interface_surface(const CompiledPackage &package) {
  if (package.declarations.ok)
    return true;
  for (const AgentObligation &obligation : package.obligations.obligations) {
    if (is_synthesis_obligation(obligation.kind))
      return true;
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
// the OpaqueSynthesisSet graph row, so retain only package identity until the
// overlay is installed and a clean successor generation can build the complete
// canonical interface.
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
  for (const SemanticSite &site : package.sites_for_read()) {
    if (site.kind == kind) return true;
  }
  return false;
}

// Graph products discover semantic sites in dependency-ready order, which is
// deliberately unrelated to source order. Provider occurrence identity and
// opaque sibling presentation are source constructs, so restore the package's
// canonical filename/byte order before metadata assigns occurrences. The sort
// is stable for malformed or equal locations, preserving deterministic task
// publication order as the recovery tiebreaker.
void sort_semantic_sites_in_source_order(
    const LoadedPackage &loaded, SemanticPackage &package) {
  const auto source_position = [&](const SemanticSite &site) {
    std::size_t file_index = loaded.files.size();
    std::uint32_t byte_offset = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0; index < loaded.files.size(); ++index) {
      const LoadedPackageFile &file = loaded.files[index];
      if (file.source != site.syntax.file) continue;
      file_index = index;
      if (file.syntax.has_value() && site.syntax.node.is_valid() &&
          site.syntax.node.value < file.syntax->nodes().size()) {
        byte_offset = file.syntax->node(site.syntax.node).range.begin.offset;
      }
      break;
    }
    return std::pair{file_index, byte_offset};
  };
  std::stable_sort(
      package.sites.begin(), package.sites.end(),
      [&](const SemanticSite &left, const SemanticSite &right) {
        return source_position(left) < source_position(right);
      });
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

// Builds the immutable final dependency contracts consumed by effect closure.
// Preliminary interface binding deliberately leaves body-derived contracts out
// of SemanticPackage. Once dependency bodies close, this operation reads their
// final interfaces and returns a separate payload; it never clears or rewrites
// the semantic generation already addressed by procedure products. A concrete
// generic proxy uses its exact specialization row because dependent `when` can
// make two instances of the same source template expose different effects.
[[nodiscard]] ImportedProcedureContracts build_imported_procedure_contracts(
    const SemanticPackage &package,
    const CompileWorkspaceResult &result,
    const WorkspaceDependencyIndex &schedule,
    std::span<const SymbolId> active_instance_proxies,
    DiagnosticSink &diagnostics) {
  ImportedProcedureContracts contracts;
  for (const ImportedSymbol &imported : package.imported_symbols_for_read()) {
    if (!imported.proxy.is_valid() ||
        package.symbols.symbol(imported.proxy).kind != SymbolKind::Procedure) {
      continue;
    }
    const ImportedProcedureInstance *requested_instance = nullptr;
    for (const ImportedProcedureInstance &instance :
         package.imported_procedure_instances_for_read()) {
      if (instance.instance_proxy == imported.proxy) {
        requested_instance = &instance;
        break;
      }
    }
    if (requested_instance != nullptr &&
        !std::binary_search(
            active_instance_proxies.begin(),
            active_instance_proxies.end(),
            imported.proxy,
            [](SymbolId left, SymbolId right) {
              return left.value < right.value;
            })) {
      // The proxy belongs to a completed body product which is no longer in
      // the selected program. Its dependency specialization is intentionally
      // absent from the current interface, so refreshing it would manufacture
      // either a false missing-instance diagnostic or stale effect rows.
      continue;
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
    contracts.procedures.push_back(
        {imported.proxy, has_effect_summary});
    for (const InterfaceDeclaration::Effect &effect : *interface_effects) {
      contracts.effects.push_back(
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
      contracts.returns.push_back(std::move(imported_return));
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
      contracts.writes.push_back(std::move(imported_write));
    }
  }
  return contracts;
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

// Returns whether one package's exact current-program closure products and
// non-product payloads are complete. This is deliberately derived state: the
// product graph remains the lifecycle authority, so source invalidation cannot
// leave a second package phase enum disagreeing with superseded rows.
[[nodiscard]] bool package_semantic_closure_is_current(
    const CompileWorkspaceResult &result,
    std::size_t package_index) {
  if (package_index >= result.packages.size() ||
      package_index >= result.semantic_products.packages.size() ||
      !result.packages[package_index].has_value()) {
    return false;
  }
  const CompiledPackage &package = *result.packages[package_index];
  const PackageSemanticProducts &products =
      result.semantic_products.packages[package_index];
  if (!package.bodies.ok || !package.metadata.ok || !package.obligations.ok ||
      !package.native_interop.ok ||
      products.effect_body_work_indices.size() !=
          products.direct_effect_summaries.size() ||
      products.effect_body_work_indices.size() !=
          products.denial_results.size() ||
      package.direct_effects.procedures.size() !=
          products.direct_effect_summaries.size() ||
      package.effects.components.size() !=
          products.closed_effect_sccs.size()) {
    return false;
  }
  const auto complete = [&](SemanticProductId product) {
    return product.is_valid() &&
        product.value < result.semantic_graph.products.size() &&
        result.semantic_graph.products[product.value].state ==
            SemanticProductState::Complete;
  };
  return std::all_of(
             products.direct_effect_summaries.begin(),
             products.direct_effect_summaries.end(), complete) &&
      std::all_of(
          products.closed_effect_sccs.begin(),
          products.closed_effect_sccs.end(), complete) &&
      std::all_of(
          products.denial_results.begin(),
          products.denial_results.end(), complete);
}

// Invalidates effect/obligation closure for one changed package and every
// transitive consumer without discarding reusable body HIR. Exact product rows
// are superseded and the native-interop completion payload is cleared. The
// preliminary interface obligation payload remains available while the
// aggregate result is intentionally back at InterfaceDiscovery; semantic
// continuation deterministically replaces it before closure can become current.
[[nodiscard]] bool invalidate_package_closure(
    const WorkspaceDependencyIndex &schedule,
    std::span<const PackageId> changed_packages,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  const std::vector<bool> affected =
      affected_packages(schedule, std::vector<PackageId>(
          changed_packages.begin(), changed_packages.end()));
  std::vector<SemanticProductId> superseded;
  for (std::size_t index = 0; index < affected.size(); ++index) {
    if (!affected[index] || !result.packages[index].has_value()) continue;
    const PackageSemanticProducts &products =
        result.semantic_products.packages[index];
    superseded.insert(
        superseded.end(),
        products.direct_effect_summaries.begin(),
        products.direct_effect_summaries.end());
    superseded.insert(
        superseded.end(),
        products.closed_effect_sccs.begin(),
        products.closed_effect_sccs.end());
    superseded.insert(
        superseded.end(),
        products.denial_results.begin(),
        products.denial_results.end());
    if (products.package_static_data.is_valid()) {
      superseded.push_back(products.package_static_data);
    }
    if (products.package_assembly.is_valid()) {
      superseded.push_back(products.package_assembly);
    }
    superseded.insert(
        superseded.end(),
        products.mir_procedures.begin(),
        products.mir_procedures.end());
    superseded.insert(
        superseded.end(),
        products.machine_functions.begin(),
        products.machine_functions.end());
    if (products.artifact_layout.is_valid()) {
      superseded.push_back(products.artifact_layout);
    }
  }
  if (!superseded.empty()) {
    std::string reason;
    if (!supersede_semantic_products(
            result.semantic_graph, superseded, reason)) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot invalidate semantic closure products: " + reason);
      return false;
    }
  }
  for (std::size_t index = 0; index < affected.size(); ++index) {
    if (!affected[index] || !result.packages[index].has_value()) continue;
    CompiledPackage &package = *result.packages[index];
    PackageSemanticProducts &products =
        result.semantic_products.packages[index];
    products.effect_body_work_indices.clear();
    products.direct_effect_summaries.clear();
    products.closed_effect_sccs.clear();
    products.denial_results.clear();
    products.package_static_data = {};
    products.package_assembly = {};
    products.mir_body_work_indices.clear();
    products.mir_procedures.clear();
    products.machine_functions.clear();
    products.artifact_layout = {};
    package.direct_effects = {};
    package.effects = {};
    package.native_interop = {};
    package.assembly = {};
    package.llvm = {};
    package.artifact_layout = {};
  }
  return true;
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
      result.semantic_products.procedure_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.type_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.condition_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.generic_type_demand_by_product.size() !=
          result.semantic_graph.products.size() ||
      result.semantic_products.mir_procedure_by_product.size() !=
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
  result.semantic_products.procedure_by_product.push_back({});
  result.semantic_products.type_by_product.push_back({});
  result.semantic_products.condition_by_product.push_back({});
  result.semantic_products.generic_type_demand_by_product.push_back({});
  result.semantic_products.mir_procedure_by_product.push_back(std::nullopt);
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
        superseded.end(), products.type_members.begin(),
        products.type_members.end());
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
        products.abi_classifications.begin(),
        products.abi_classifications.end());
    superseded.insert(
        superseded.end(),
        products.generic_type_demands.begin(),
        products.generic_type_demands.end());
    superseded.insert(
        superseded.end(),
        products.conditions.begin(),
        products.conditions.end());
    superseded.insert(
        superseded.end(), products.constants.begin(), products.constants.end());
    superseded.insert(
        superseded.end(), products.procedure_bodies.begin(),
        products.procedure_bodies.end());
    superseded.insert(
        superseded.end(),
        products.direct_effect_summaries.begin(),
        products.direct_effect_summaries.end());
    superseded.insert(
        superseded.end(),
        products.closed_effect_sccs.begin(),
        products.closed_effect_sccs.end());
    superseded.insert(
        superseded.end(),
        products.denial_results.begin(),
        products.denial_results.end());
    if (products.package_static_data.is_valid()) {
      superseded.push_back(products.package_static_data);
    }
    if (products.package_assembly.is_valid()) {
      superseded.push_back(products.package_assembly);
    }
    superseded.insert(
        superseded.end(),
        products.mir_procedures.begin(),
        products.mir_procedures.end());
    superseded.insert(
        superseded.end(),
        products.machine_functions.begin(),
        products.machine_functions.end());
    if (products.artifact_layout.is_valid()) {
      superseded.push_back(products.artifact_layout);
    }
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
    products.type_members.clear();
    products.declaration_types.clear();
    products.natural_layouts.clear();
    products.abi_classifications.clear();
    products.generic_type_demands.clear();
    products.conditions.clear();
    products.constants.clear();
    products.procedure_bodies.clear();
    products.selected_procedure_bodies.clear();
    products.effect_body_work_indices.clear();
    products.direct_effect_summaries.clear();
    products.closed_effect_sccs.clear();
    products.denial_results.clear();
    products.package_static_data = {};
    products.package_assembly = {};
    products.mir_body_work_indices.clear();
    products.mir_procedures.clear();
    products.machine_functions.clear();
    products.artifact_layout = {};
    products.body_type_producer.clear();
    products.declaration_inputs.clear();
    products.declaration_inputs.push_back(result.semantic_products.target);
    products.declaration_inputs.push_back(products.imports);
    products.declaration_inputs.insert(products.declaration_inputs.end(),
                                       products.parsed_files.begin(),
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
      products.declaration_inputs.push_back(dependency_interface);
    }
    const PackageId owner{static_cast<std::uint32_t>(package_index)};
    products.name_set = append_workspace_semantic_product(
        result, SemanticProductKind::PackageNameSet,
        products.declaration_inputs, owner, false, diagnostics);
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

// Completes the package-interface payload after every declaration and named
// constant product has published. An opaque synthesis wait never reaches this
// operation: the scheduler joins its retained task packets directly and returns
// an intentionally withheld interface. Reaching the barrier with a nonterminal
// declaration product set is therefore an integration error, not an alternate
// provider-context path.
[[nodiscard]] bool finalize_workspace_package_interface(
    SourceManager &sources,
    const CompileWorkspaceOptions &options,
    const WorkspacePackage &workspace_package,
    CompiledPackage &package,
    DiagnosticSink &diagnostics) {
  if (!package.declaration_discovery.terminal) {
    diagnostics.error(
        SourceRange::invalid(),
        "package interface reached nonterminal declaration discovery");
    return false;
  }
  package.declarations = finish_package_semantics_from_products(
      sources, workspace_package.loaded, options.target.facts,
      std::move(package.declaration_discovery), diagnostics);
  PackageDeclarationDiscovery empty_discovery;
  package.declaration_discovery = std::move(empty_discovery);
  if (!package.declarations.ok)
    return false;

  SemanticPackage interface_context_package = package.declarations.package;
  ConstantTable interface_context_constants = package.declarations.constants;
  sort_semantic_sites_in_source_order(workspace_package.loaded,
                                      interface_context_package);
  package.metadata = collect_agent_metadata(sources, workspace_package.loaded,
                                            interface_context_package,
                                            options.attachments, diagnostics);
  if (!package.metadata.ok)
    return false;
  if (options.validation_kind == ValidationKind::None &&
      package.validation_context.empty() &&
      has_agent_obligation_record(package.metadata)) {
    package.validation_context = load_validation_context(
        sources, workspace_package, options.workspace, diagnostics);
    package.validation_context_is_typed = false;
  }
  package.interface = build_package_interface(
      workspace_package.identity, package.declarations.package,
      package.declarations.constants, package.metadata, diagnostics);
  if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
    const ImportedProcedureContracts imported_contracts =
        imported_procedure_contracts(interface_context_package);
    package.obligations = build_agent_obligations(
        workspace_package.identity,
        sources,
        workspace_package.loaded,
        interface_context_package,
        interface_context_constants,
        package.metadata,
        imported_contracts,
        options.target,
        diagnostics,
        package.validation_context);
    if (!package.obligations.ok) return false;
  }
  return !diagnostics.has_errors();
}

// Computes one PackageNameSet transition. Every compiler stage publishes eager
// authored declarations first and later re-enters only to close an already
// completed product set. Synthesis discovery and complete compilation therefore
// share one declaration graph; only task evaluation's synthesis mode differs.
[[nodiscard]] std::optional<CompiledPackage> analyze_workspace_package_names(
    SourceManager &sources, const CompileWorkspaceOptions &options,
    const WorkspaceDependencyIndex &schedule, std::size_t package_index,
    std::vector<AgentValidationContext> validation_context,
    bool validation_context_is_typed, const CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  const WorkspacePackage &workspace_package =
      result.graph.packages[package_index];
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

  const bool resume_product_discovery =
      result.packages[package_index].has_value() &&
      !result.packages[package_index]->declaration_discovery.terminal;
  CompiledPackage package;
  if (resume_product_discovery) {
    package = *result.packages[package_index];
  }
  if (!resume_product_discovery) {
    package.identity = workspace_package.identity;
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
  if (resume_product_discovery) {
    if (!finish_package_declaration_discovery(package.declaration_discovery,
                                              diagnostics)) {
      return std::nullopt;
    }
  } else {
    package.declaration_discovery = begin_package_declaration_discovery(
        sources, workspace_package.loaded, available, diagnostics);
  }
  if (!resume_product_discovery && options.timings != nullptr) {
    options.timings->add_counter("package semantic analyses", 1);
  }

  // The first PackageNameSet transition deliberately publishes a nonterminal
  // payload. The coordinator appends declaration, constant, layout, and
  // condition products from that exact table and blocks this barrier on them.
  if (!package.declaration_discovery.terminal) {
    return package;
  }

  if (!package.declaration_discovery.discovery_ok)
    return std::nullopt;

  return package;
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
  std::optional<SemanticTaskAppend> declaration_semantic;
  // Blocked declaration attempts retain their private package until the
  // coordinator exports requester-local generic arguments. A synthesis wait
  // retains the same package as the task-owned semantic context for its
  // provider constraint. Complete attempts return only declaration_semantic;
  // they never retain or replace a complete package successor.
  std::optional<SemanticPackage> declaration_package;
  std::optional<NaturalLayoutProductAttempt> natural_layout;
  // A generic owner task produces both the owner-local semantic append and the
  // package-independent graph consumed by requesters. On a nested owner wait,
  // generic_package instead supplies the local TypeIds needed to create the
  // dependency's canonical key; no mutation from that attempt is published.
  std::optional<SemanticPackage> generic_package;
  std::optional<SemanticTaskAppend> generic_semantic;
  std::optional<TypeId> generic_type;
  std::optional<InterfaceTypeGraph> generic_result;
  std::vector<ImportedTypeInstantiationRequest> generic_dependencies;
  std::optional<ConditionalProductAttempt> condition;
  std::optional<ConstantProductAttempt> constant;
  std::optional<SemanticPackage> constant_package;
  std::size_t constant_shared_type_count = 0;
};

// InterfaceSynthesisSurface is the immutable semantic side of one exact product
// which stopped at `...`. ProductId identifies the graph owner; package and
// constants are the task-private state at the first stop, optionally enriched
// by one successful check of the exact compile-time procedures reached by that
// task. No sibling task can mutate or observe this state. Metadata is collected
// later because attachment I/O belongs to provider-surface composition, but it
// is derived only from this retained packet and never by replaying the semantic
// product.
struct InterfaceSynthesisSurface {
  SemanticProductId product;
  SemanticPackage package;
  ConstantTable constants;
  AgentMetadataResult metadata;
};

[[nodiscard]] bool is_synthesis_site(SemanticSiteKind kind) {
  return kind == SemanticSiteKind::SynthesisDeclaration ||
         kind == SemanticSiteKind::SynthesisMember ||
         kind == SemanticSiteKind::SynthesisStatement ||
         kind == SemanticSiteKind::SynthesisExpression ||
         kind == SemanticSiteKind::SynthesisAssembly;
}

// Completes the provider constraint owned by one stopped interface task. A
// direct declaration/member/expression site is already in task_package. When
// constant execution reached a procedure, that procedure is checked exactly
// once here so its lexical locals, expected types, and branch facts become part
// of the same private packet. An incomplete declaration/member set may make
// that body uncheckable in the current opaque round; in that case the body
// sites stay deferred until accepted structural source creates a successor
// generation, while direct sites from this task remain ready. Other body
// failures are real source failures and their private diagnostics publish now.
[[nodiscard]] bool prepare_interface_synthesis_surface(
    const SourceManager &sources, const LoadedPackage &loaded,
    const ConditionalSelections &selections, const TargetFacts &target,
    SemanticProductId product, SemanticPackage task_package,
    ConstantTable task_constants, std::vector<SymbolId> compile_time_procedures,
    bool declaration_set_is_incomplete, InterfaceSynthesisSurface &surface,
    DiagnosticSink &diagnostics) {
  if (!compile_time_procedures.empty()) {
    DiagnosticSink body_diagnostics;
    PackageBodyWorkState checked = check_compile_time_procedure_bodies(
        sources, loaded, selections, task_package, task_constants, target,
        compile_time_procedures, body_diagnostics);
    if (checked.ok) {
      task_package = std::move(checked.package);
      task_constants = std::move(checked.constants);
    } else if (!declaration_set_is_incomplete) {
      append_diagnostics(diagnostics, body_diagnostics);
      if (!diagnostics.has_errors()) {
        diagnostics.error(
            SourceRange::invalid(),
            "compile-time procedure synthesis constraint could not be typed");
      }
      return false;
    }
  }

  ensure_runtime_context_type(task_package, diagnostics);
  if (diagnostics.has_errors())
    return false;
  sort_semantic_sites_in_source_order(loaded, task_package);

  bool has_ready_site = false;
  for (const SemanticSite &site : task_package.sites_for_read()) {
    has_ready_site = has_ready_site || is_synthesis_site(site.kind);
  }
  // A procedure-only task may be intentionally deferred behind a declaration or
  // member site owned by another task in this opaque set. Every other wait must
  // expose the site which caused it to stop.
  if (!has_ready_site &&
      !(declaration_set_is_incomplete && !compile_time_procedures.empty())) {
    diagnostics.error(
        SourceRange::invalid(),
        "interface synthesis task produced no typed provider site");
    return false;
  }

  surface.product = product;
  surface.package = std::move(task_package);
  surface.constants = std::move(task_constants);
  return true;
}

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

// Ordinary local parametric type instances remain discoveries of the
// declaration task that needs them; cross-package owner-evaluated applications
// use the separate canonical demand table. Neither kind is another authored
// declaration merely because its owner symbol copies the template's
// SyntaxReference.
[[nodiscard]] bool is_parametric_type_instance(const SemanticPackage &package,
                                               SymbolId symbol) {
  for (const ParametricTypeInstanceRecord &instance :
       package.parametric_type_instances_for_read()) {
    if (instance.instance == symbol)
      return true;
  }
  return false;
}

// Finds the condition row for one exact reachable source site. An initial site
// is appended before its member product; a nested `else when` site is appended
// after its parent selects that syntax but before the blocked member attempt is
// retried. Every returned ID therefore belongs to the current source generation
// and is safe to attach as the consumer's exact prerequisite.
[[nodiscard]] std::optional<SemanticProductId> package_condition_product(
    const CompileWorkspaceResult &result,
    PackageId owner,
    SyntaxReference syntax) {
  const PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  for (SemanticProductId product : products.conditions) {
    if (result.semantic_products.condition_by_product[product.value] == syntax) {
      return product;
    }
  }
  return std::nullopt;
}

// Appends the declaration/type-facet products revealed by the current
// append-only package declaration table. Nominal identity already exists after
// collection and receives one completed TypeIdentity row. TypeMembers consumes
// that identity plus branch choices, TypeMemberTypes consumes the stable member
// namespace, and NaturalLayout consumes the declared types. Other typed
// declarations receive one pending TypeIdentity row. All roots depend on the
// same immutable inputs as PackageNameSet, never on the barrier itself.
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
      std::vector<SemanticProductId> member_dependencies{identity};
      // A member-name task may inspect only the selected branch of each
      // reachable `when`. Initial sites were discovered without declaring
      // members, so their exact condition rows can precede the first attempt.
      for (const SemanticSite &site : semantic.sites_for_read()) {
        if (site.kind != SemanticSiteKind::ConditionalMember ||
            site.anchor != symbol) {
          continue;
        }
        const std::optional<SemanticProductId> condition =
            package_condition_product(result, owner, site.syntax);
        if (!condition.has_value()) {
          diagnostics.error(
              SourceRange::invalid(),
              "nominal member condition has no semantic product");
          return false;
        }
        member_dependencies.push_back(*condition);
      }
      const SemanticProductId members = append_workspace_semantic_product(
          result, SemanticProductKind::TypeMembers, member_dependencies, owner,
          false, diagnostics);
      if (!members.is_valid())
        return false;
      result.semantic_products.declaration_by_product[members.value] = symbol;
      result.semantic_products.type_by_product[members.value] = declaration.type;
      products.type_members.push_back(members);
      const std::array member_type_dependencies{members};
      declaration_product = append_workspace_semantic_product(
          result, SemanticProductKind::TypeMemberTypes,
          member_type_dependencies, owner, false, diagnostics);
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
    // concrete instance. A cross-package owner-evaluated application receives
    // its own canonical layout product when demanded; scheduling the symbolic
    // template here would invent an impossible dependency on a TypeParameter
    // natural layout.
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
  for (const SemanticSite &site :
       package.declaration_discovery.package.sites_for_read()) {
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
  for (const SemanticSite &site : semantic.sites_for_read()) {
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
// blocker. Members, MemberTypes, and NaturalLayout have separate typed vectors.
[[nodiscard]] std::optional<SemanticProductId>
package_type_facet_product(const CompileWorkspaceResult &result,
                           PackageId owner, TypeFacetDependency dependency) {
  const PackageSemanticProducts &products =
      result.semantic_products.packages[owner.value];
  std::span<const SemanticProductId> candidates;
  switch (dependency.facet) {
  case TypeFacet::Members:
    candidates = products.type_members;
    break;
  case TypeFacet::NaturalLayout:
    candidates = products.natural_layouts;
    break;
  case TypeFacet::Identity:
  case TypeFacet::MemberTypes:
    candidates = products.declaration_types;
    break;
  }
  for (SemanticProductId product : candidates) {
    if (result.semantic_products.type_by_product[product.value] ==
        dependency.type) {
      return product;
    }
  }
  return std::nullopt;
}

// Imports every completed canonical generic result named by product's current
// dependency set. A declaration or outer generic task receives a fresh private
// package snapshot; importing here makes the result available through the
// ordinary ImportedType provenance lookup without mutating shared state or
// teaching the type resolver about command-level product IDs.
[[nodiscard]] bool import_completed_generic_dependencies(
    const CompileWorkspaceResult &result, SemanticProductId product,
    SemanticPackage &task_package, DiagnosticSink &diagnostics) {
  for (SemanticProductId dependency :
       result.semantic_graph.products[product.value].dependencies) {
    if (dependency.value >=
        result.semantic_products.generic_type_demand_by_product.size()) {
      diagnostics.error(SourceRange::invalid(),
                        "generic dependency has no typed product index");
      return false;
    }
    const GenericTypeDemandId demand_id =
        result.semantic_products
            .generic_type_demand_by_product[dependency.value];
    if (!demand_id.is_valid())
      continue;
    if (demand_id.value >=
        result.semantic_products.generic_type_demands.size()) {
      diagnostics.error(SourceRange::invalid(),
                        "generic dependency names an invalid demand row");
      return false;
    }
    const GenericTypeDemand &demand =
        result.semantic_products.generic_type_demands[demand_id.value];
    if (!demand.result.has_value()) {
      diagnostics.error(SourceRange::invalid(),
                        "completed generic dependency has no result graph");
      return false;
    }
    const TypeId imported =
        import_interface_type(*demand.result, task_package, diagnostics);
    if (!imported.is_valid() ||
        task_package.types.type(imported).kind == TypeKind::Invalid) {
      return false;
    }
  }
  return true;
}

// Collects the authored requester products needed before a concrete argument
// can cross into a generic owner. A type graph is not concrete merely because
// it contains no symbolic parameters: every runtime-bearing row exported to the
// owner must also have its natural layout. Structural rows reduce recursively
// to the nominal layout products that can make progress. requester_package is
// the exact task snapshot which produced the request; consulting a retained
// package by phase would make TypeIds depend on hidden coordinator state.
[[nodiscard]] bool collect_generic_argument_layout_dependencies(
    const CompileWorkspaceResult &result, PackageId requester,
    const SemanticPackage &requester_package, TypeId type,
    std::vector<TypeId> &active,
    std::vector<SemanticProductId> &dependencies,
    DiagnosticSink &diagnostics) {
  if (!type.is_valid() ||
      std::find(active.begin(), active.end(), type) != active.end()) {
    return true;
  }
  const TypeFacetState state =
      requester_package.types.facet_state(type, TypeFacet::NaturalLayout);
  if (state == TypeFacetState::Complete)
    return true;
  if (state != TypeFacetState::Waiting) {
    diagnostics.error(SourceRange::invalid(),
                      "generic type argument has no runtime natural layout");
    return false;
  }
  const std::optional<SemanticProductId> direct =
      package_type_facet_product(
          result, requester, {type, TypeFacet::NaturalLayout});
  if (direct.has_value()) {
    dependencies.push_back(*direct);
    return true;
  }

  const std::size_t dependency_begin = dependencies.size();
  active.push_back(type);
  const Type value = requester_package.types.type(type);
  bool ok = true;
  if (value.element.is_valid()) {
    ok = collect_generic_argument_layout_dependencies(
             result, requester, requester_package, value.element, active,
             dependencies, diagnostics) && ok;
  }
  for (TypeId member : value.members) {
    ok = collect_generic_argument_layout_dependencies(
             result, requester, requester_package, member, active,
             dependencies, diagnostics) && ok;
  }
  active.pop_back();
  if (ok && dependencies.size() == dependency_begin) {
    diagnostics.error(
        SourceRange::invalid(),
        "waiting generic type argument has no producing layout product");
    return false;
  }
  return ok;
}

struct GenericTypeDemandAppendResult {
  bool ok = false;
  std::optional<SemanticProductId> product;
  std::vector<SemanticProductId> dependencies;
};

// Converts one requester-local owner-evaluation row into the canonical key used
// by the defining package. Every argument type crosses the package boundary as
// an InterfaceTypeGraph, then interns into the owner's append-only TypeStore.
// Equal requests therefore converge through exact owner-local ParametricArgument
// equality; no digest, progress set, or requester TypeId is part of identity.
//
// Graph mutation occurs only between frozen waves. A new TypeNaturalLayout row
// depends on the owner's completed package interface, which proves that its
// public template declaration and private evaluation bodies are available.
[[nodiscard]] GenericTypeDemandAppendResult append_generic_type_demand(
    CompileWorkspaceResult &result,
    const WorkspaceDependencyIndex &schedule,
    PackageId requester,
    const SemanticPackage &requester_package,
    const ImportedTypeInstantiationRequest &request,
    DiagnosticSink &diagnostics) {
  GenericTypeDemandAppendResult appended;
  const std::optional<std::size_t> owner_index = package_index_for(
      result.graph, schedule, request.root_identity,
      request.root_relative_path);
  if (!owner_index.has_value() ||
      !result.packages[*owner_index].has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "generic type owner is unavailable during layout demand creation");
    return appended;
  }
  CompiledPackage &owner_package = *result.packages[*owner_index];
  SemanticPackage &owner_semantic = owner_package.declarations.package;
  const std::optional<SymbolId> source = owner_semantic.symbols.lookup_direct(
      owner_semantic.package_scope, request.public_template_name);
  if (!source.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "generic type demand names no public declaration '" +
            request.public_template_name + "'");
    return appended;
  }
  const Symbol &source_symbol = owner_semantic.symbols.symbol(*source);
  if (source_symbol.kind != SymbolKind::Type ||
      !source_symbol.flags.parametric ||
      source_symbol.visibility != Visibility::Public) {
    diagnostics.error(
        source_symbol.name_range,
        "generic type demand does not name a public parametric type");
    return appended;
  }

  for (const ParametricArgument &argument : request.arguments) {
    const TypeId requester_type =
        argument.is_type ? argument.type : argument.value_type;
    std::vector<TypeId> active;
    if (!collect_generic_argument_layout_dependencies(
            result, requester, requester_package, requester_type, active,
            appended.dependencies, diagnostics)) {
      return appended;
    }
  }
  if (!appended.dependencies.empty()) {
    appended.ok = true;
    return appended;
  }

  std::vector<ParametricArgument> arguments;
  arguments.reserve(request.arguments.size());
  for (const ParametricArgument &argument : request.arguments) {
    if (!argument.is_type &&
        (argument.value.kind != ConstantKind::Integer ||
         argument.owner_evaluated_value ||
         argument.value_expression.is_valid())) {
      diagnostics.error(
          source_symbol.name_range,
          "generic type demand contains a symbolic value argument");
      return appended;
    }
    const TypeId requester_type =
        argument.is_type ? argument.type : argument.value_type;
    const InterfaceTypeGraph graph = export_interface_type(
        result.packages[requester.value]->identity, requester_package,
        requester_type, diagnostics);
    if (!owner_result_is_concrete(graph)) {
      diagnostics.error(
          source_symbol.name_range,
          "generic type demand contains a symbolic type argument");
      return appended;
    }
    const TypeId owner_type =
        import_interface_type(graph, owner_semantic, diagnostics);
    if (!owner_type.is_valid() ||
        owner_semantic.types.type(owner_type).kind == TypeKind::Invalid) {
      return appended;
    }
    ParametricArgument transferred;
    transferred.is_type = argument.is_type;
    if (argument.is_type) {
      transferred.type = owner_type;
    } else {
      transferred.value_type = owner_type;
      transferred.value = argument.value;
    }
    arguments.push_back(std::move(transferred));
  }

  const PackageId owner{static_cast<std::uint32_t>(*owner_index)};
  const PackageSemanticProducts &owner_products =
      result.semantic_products.packages[*owner_index];
  for (SemanticProductId existing_product :
       owner_products.generic_type_demands) {
    const GenericTypeDemandId existing_id =
        result.semantic_products
            .generic_type_demand_by_product[existing_product.value];
    if (!existing_id.is_valid() ||
        existing_id.value >=
            result.semantic_products.generic_type_demands.size()) {
      diagnostics.error(SourceRange::invalid(),
                        "generic type demand index is inconsistent");
      return appended;
    }
    const GenericTypeDemand &existing =
        result.semantic_products.generic_type_demands[existing_id.value];
    if (existing.owner == owner && existing.source == *source &&
        existing.arguments == arguments) {
      appended.ok = true;
      appended.product = existing.product;
      return appended;
    }
  }

  if (result.semantic_products.generic_type_demands.size() >=
      std::numeric_limits<std::uint32_t>::max()) {
    diagnostics.error(SourceRange::invalid(),
                      "too many command-local generic type demands");
    return appended;
  }
  const SemanticProductId owner_interface =
      result.semantic_products.packages[*owner_index].package_interface;
  if (!owner_interface.is_valid() ||
      result.semantic_graph.products[owner_interface.value].state !=
          SemanticProductState::Complete) {
    diagnostics.error(
        SourceRange::invalid(),
        "generic type demand ran before its owner interface completed");
    return appended;
  }
  const std::array dependencies{owner_interface};
  const SemanticProductId product = append_workspace_semantic_product(
      result, SemanticProductKind::TypeNaturalLayout, dependencies, owner,
      false, diagnostics);
  if (!product.is_valid())
    return appended;
  const GenericTypeDemandId demand_id{
      static_cast<std::uint32_t>(
          result.semantic_products.generic_type_demands.size())};
  result.semantic_products.generic_type_demands.push_back(
      {owner, *source, std::move(arguments), product, std::nullopt});
  result.semantic_products.generic_type_demand_by_product[product.value] =
      demand_id;
  result.semantic_products.packages[*owner_index]
      .generic_type_demands.push_back(product);
  appended.ok = true;
  appended.product = product;
  return appended;
}

// Returns the retained semantic site named by one condition product. Sites are
// append-only and few; a direct source-identity scan avoids storing a pointer
// across branch materialization.
[[nodiscard]] const SemanticSite *
find_semantic_site(const SemanticPackage &package, SyntaxReference syntax) {
  for (const SemanticSite &site : package.sites_for_read()) {
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

// Returns the declaration products which are both explicit prerequisites of
// product and already published. The frozen package prefix can contain other
// completed signatures, but those incidental rows do not authorize this task
// to consume a name or type. The returned SymbolIds are the exact semantic
// visibility frontier supplied to declaration resolution.
[[nodiscard]] std::vector<SymbolId>
completed_declaration_dependencies(const CompileWorkspaceResult &result,
                                   PackageId owner, SemanticProductId product) {
  std::vector<SymbolId> completed;
  const PackageSemanticProducts &package_products =
      result.semantic_products.packages[owner.value];
  const std::vector<SemanticProductId> &dependencies =
      result.semantic_graph.products[product.value].dependencies;
  for (SemanticProductId candidate : package_products.declaration_types) {
    if (result.semantic_graph.products[candidate.value].state ==
            SemanticProductState::Complete &&
        std::find(dependencies.begin(), dependencies.end(), candidate) !=
            dependencies.end()) {
      completed.push_back(
          result.semantic_products.declaration_by_product[candidate.value]);
    }
  }
  return completed;
}

// Composes one package's provider view from the immutable surface packets owned
// by its stopped semantic tasks. SemanticPackage IDs are meaningful only inside
// their originating packet, so packets are never merged. Instead metadata rows
// are ordered and deduplicated by source identity, then each obligation is
// built against the exact package which typed that site.
// append_agent_obligation uses the shared result to reserve occurrence
// identities across sibling packets. This is the transactional join for the
// input side of an opaque set; accepted source remains private until the
// resolver has checked every sibling proposal.
[[nodiscard]] bool compose_interface_synthesis_surfaces(
    SourceManager &sources, const CompileWorkspaceOptions &options,
    std::vector<InterfaceSynthesisSurface> &surfaces,
    CompileWorkspaceResult &result, DiagnosticSink &diagnostics) {
  for (std::size_t package_index = 0; package_index < result.packages.size();
       ++package_index) {
    std::vector<std::size_t> package_surfaces;
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
      const SemanticProductId product = surfaces[index].product;
      if (product.is_valid() &&
          product.value < result.semantic_products.package_by_product.size() &&
          result.semantic_products.package_by_product[product.value].value ==
              package_index) {
        package_surfaces.push_back(index);
      }
    }
    if (package_surfaces.empty())
      continue;
    if (!result.packages[package_index].has_value()) {
      diagnostics.error(SourceRange::invalid(),
                        "task-owned synthesis surface has no retained package");
      return false;
    }

    CompiledPackage &compiled = *result.packages[package_index];
    const LoadedPackage &loaded = result.graph.packages[package_index].loaded;
    struct SurfaceRecord {
      std::size_t surface = 0;
      std::size_t record = 0;
    };
    std::vector<SurfaceRecord> records;
    for (std::size_t surface_index : package_surfaces) {
      InterfaceSynthesisSurface &surface = surfaces[surface_index];
      surface.metadata = collect_agent_metadata(
          sources, loaded, surface.package, options.attachments, diagnostics);
      if (!surface.metadata.ok)
        return false;
      for (std::size_t record_index = 0;
           record_index < surface.metadata.records.size(); ++record_index) {
        records.push_back({surface_index, record_index});
      }
    }

    const auto source_position = [&](const AgentRecord &record) {
      std::size_t file_index = loaded.files.size();
      std::uint32_t byte_offset = std::numeric_limits<std::uint32_t>::max();
      for (std::size_t index = 0; index < loaded.files.size(); ++index) {
        const LoadedPackageFile &file = loaded.files[index];
        if (file.source != record.syntax.file)
          continue;
        file_index = index;
        if (file.syntax.has_value() && record.syntax.node.is_valid() &&
            record.syntax.node.value < file.syntax->nodes().size()) {
          byte_offset =
              file.syntax->node(record.syntax.node).range.begin.offset;
        }
        break;
      }
      return std::pair{file_index, byte_offset};
    };
    std::stable_sort(
        records.begin(), records.end(),
        [&](const SurfaceRecord &left, const SurfaceRecord &right) {
          const AgentRecord &left_record =
              surfaces[left.surface].metadata.records[left.record];
          const AgentRecord &right_record =
              surfaces[right.surface].metadata.records[right.record];
          return source_position(left_record) < source_position(right_record);
        });

    // The same authored package docs and eager agent sites occur in several
    // task snapshots. Keep their first source-ordered owner; equal syntax and
    // kind are one semantic site, while different kinds remain distinct even if
    // malformed recovery assigned the same syntax node.
    std::vector<SurfaceRecord> unique_records;
    for (SurfaceRecord candidate : records) {
      const AgentRecord &record =
          surfaces[candidate.surface].metadata.records[candidate.record];
      const bool duplicate = std::any_of(
          unique_records.begin(), unique_records.end(),
          [&](const SurfaceRecord &existing) {
            const AgentRecord &other =
                surfaces[existing.surface].metadata.records[existing.record];
            return other.kind == record.kind && other.syntax == record.syntax;
          });
      if (!duplicate)
        unique_records.push_back(candidate);
    }

    compiled.metadata = {};
    compiled.metadata.ok = true;
    for (SurfaceRecord selected : unique_records) {
      compiled.metadata.records.push_back(
          surfaces[selected.surface].metadata.records[selected.record]);
    }
    if (options.validation_kind == ValidationKind::None &&
        compiled.validation_context.empty() &&
        has_agent_obligation_record(compiled.metadata)) {
      compiled.validation_context =
          load_validation_context(sources, result.graph.packages[package_index],
                                  options.workspace, diagnostics);
      compiled.validation_context_is_typed = false;
    }

    compiled.interface = withheld_package_interface(
        compiled.identity, compiled.declaration_discovery.package);
    compiled.obligations = {};
    compiled.obligations.ok = true;
    for (SurfaceRecord selected : unique_records) {
      InterfaceSynthesisSurface &surface = surfaces[selected.surface];
      const ImportedProcedureContracts imported_contracts =
          imported_procedure_contracts(surface.package);
      if (!append_agent_obligation(compiled.identity, sources, loaded,
                                   surface.package, surface.constants,
                                   surface.metadata, selected.record,
                                   imported_contracts,
                                   options.target, compiled.obligations,
                                   diagnostics, compiled.validation_context)) {
        return false;
      }
    }

    bool has_synthesis = false;
    for (const AgentObligation &obligation :
         compiled.obligations.obligations) {
      has_synthesis = has_synthesis || is_synthesis_obligation(obligation.kind);
    }
    if (!has_synthesis) {
      diagnostics.error(
          SourceRange::invalid(),
          "opaque synthesis set produced no provider obligation");
      return false;
    }
  }
  return true;
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
  for (std::size_t index = 0; index < result.packages.size(); ++index) {
    if (!selected[index])
      continue;
    if (result.packages[index].has_value()) {
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

  // Product IDs are appended and waves are published in ascending order. Keep
  // the immutable task-owned semantic packet for each product which reached
  // synthesis during this source generation. When ordinary work is exhausted,
  // provider obligations are built directly from these packets; no declaration,
  // condition, or constant product is invoked a second time.
  std::vector<InterfaceSynthesisSurface> synthesis_surfaces;

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
        return compose_interface_synthesis_surfaces(
            sources, options, synthesis_surfaces, result, diagnostics);
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
    // Every read-only interface task starts from a frozen canonical prefix and
    // returns only its private payload. Declaration and generic-owner products
    // use SemanticTaskAppend packets; declaration packets additionally contain
    // row-granular patches for the collected TypeId/SymbolId they refine.
    // PackageNameSet and PackageInterface can load validation source into
    // SourceManager, so those tasks run after every read-only product and in one
    // stable global chain. This keeps workers away from canonical package state
    // while allowing every already-isolated product to execute concurrently.
    struct InterfaceWaveExecution {
      SourceManager *sources = nullptr;
      const CompileWorkspaceOptions *options = nullptr;
      const WorkspaceDependencyIndex *schedule = nullptr;
      const SemanticReadyWave *wave = nullptr;
      const CompileWorkspaceResult *result = nullptr;
      std::vector<std::vector<AgentValidationContext>> *retained_validation =
          nullptr;
      std::vector<bool> *retained_validation_is_typed = nullptr;
      std::vector<WorkspaceInterfaceTaskSlot> *slots = nullptr;
      const std::vector<std::size_t> *task_indices = nullptr;
    };
    InterfaceWaveExecution execution{
        &sources,
        &options,
        &schedule,
        &wave,
        &result,
        &retained_validation,
        &retained_validation_is_typed,
        &slots,
        nullptr,
    };
    const auto execute_interface_task = [](
        void *opaque, WorkTaskId scheduled_task,
        std::string &failure) -> bool {
      (void)failure;
      auto &execution = *static_cast<InterfaceWaveExecution *>(opaque);
      SourceManager &sources = *execution.sources;
      const CompileWorkspaceOptions &options = *execution.options;
      const WorkspaceDependencyIndex &schedule = *execution.schedule;
      const SemanticReadyWave &wave = *execution.wave;
      const CompileWorkspaceResult &result = *execution.result;
      auto &retained_validation = *execution.retained_validation;
      auto &retained_validation_is_typed =
          *execution.retained_validation_is_typed;
      auto &slots = *execution.slots;
      const std::size_t task_index =
          (*execution.task_indices)[static_cast<std::size_t>(scheduled_task)];
      do {
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
          kind == SemanticProductKind::TypeMembers ||
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
        const SemanticPackage &canonical_package =
            result.packages[package_index]->declaration_discovery.package;
        const ConstantTable &canonical_constants =
            result.packages[package_index]
                ->declaration_discovery.published_constants;
        const SemanticTaskPrefix prefix = capture_semantic_task_prefix(
            canonical_package, canonical_constants);
        SemanticPackage task_package =
            canonical_package.fork_declaration_task_view();
        ConstantTable task_constants = canonical_constants.fork_append_only();
        if (!import_completed_generic_dependencies(
                result, product, task_package, slot.outcome.diagnostics)) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure =
              "declaration generic dependency import failed";
          continue;
        }
        if (kind == SemanticProductKind::TypeMembers) {
          slot.declaration_type = resolve_package_type_members_product(
              sources, result.graph.packages[package_index].loaded, task_package,
              result.packages[package_index]
                  ->declaration_discovery.selections,
              root, compile_time_synthesis_mode(options.stage),
              slot.outcome.diagnostics);
        } else {
          // A valid payload in the frozen prefix is not proof that this task
          // owns the corresponding prerequisite. Only an explicit completed
          // edge authorizes consumption. A previously unseen source reference
          // therefore blocks once, records its exact product edge, and becomes
          // available on the retry.
          const std::vector<SymbolId> completed_declarations =
              completed_declaration_dependencies(result, owner, product);
          slot.declaration_type = resolve_package_declaration_type_product(
              sources, result.graph.packages[package_index].loaded, task_package,
              result.packages[package_index]
                  ->declaration_discovery.selections,
              root, completed_declarations,
              task_constants,
              options.target.facts, compile_time_synthesis_mode(options.stage),
              slot.outcome.diagnostics);
        }
        if (slot.declaration_type->status == TypeProductStatus::Complete) {
          slot.declaration_semantic = extract_semantic_task_append(
              prefix, task_package, task_constants);
          continue;
        }
        if (slot.declaration_type->status == TypeProductStatus::Error) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "declaration type product failed";
          continue;
        }
        if (slot.declaration_type->status ==
            TypeProductStatus::WaitingForSynthesis) {
          slot.declaration_package = task_package.materialize_task_view();
          slot.outcome.kind = SemanticProductOutcomeKind::WaitingForSynthesis;
          continue;
        }
        if (!slot.declaration_type->generic_type_dependencies.empty()) {
          slot.declaration_package = task_package.materialize_task_view();
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
        for (SyntaxReference dependency :
             slot.declaration_type->condition_dependencies) {
          const std::optional<SemanticProductId> blocker =
              package_condition_product(result, owner, dependency);
          if (!blocker.has_value()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "declaration member condition has no product";
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
        const GenericTypeDemandId demand_id =
            result.semantic_products
                .generic_type_demand_by_product[product.value];
        if (demand_id.is_valid()) {
          if (demand_id.value >=
              result.semantic_products.generic_type_demands.size()) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "generic layout product names an invalid demand";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            continue;
          }
          if (!result.packages[package_index]->declarations.ok) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "generic owner product ran before its package interface";
            slot.outcome.diagnostics.error(SourceRange::invalid(),
                                           slot.outcome.failure);
            continue;
          }
          const GenericTypeDemand &demand =
              result.semantic_products.generic_type_demands[demand_id.value];
          const SemanticPackage &canonical_package =
              result.packages[package_index]->declarations.package;
          const ConstantTable &canonical_constants =
              result.packages[package_index]->declarations.constants;
          const SemanticTaskPrefix prefix = capture_semantic_task_prefix(
              canonical_package, canonical_constants);
          SemanticPackage task_package =
              canonical_package.fork_body_task_view();
          ConstantTable task_constants =
              canonical_constants.fork_append_only();
          if (!import_completed_generic_dependencies(
                  result, product, task_package, slot.outcome.diagnostics)) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure = "generic layout dependency import failed";
            continue;
          }
          const Symbol source_symbol =
              task_package.symbols.symbol(demand.source);
          const std::size_t request_begin =
              task_package.imported_type_instantiation_requests_for_read()
                  .size();
          const TypeId concrete = instantiate_parametric_type_application(
              sources, result.graph.packages[package_index].loaded,
              task_package,
              result.packages[package_index]->declarations.selections,
              demand.source, demand.arguments, source_symbol.name_range,
              &task_constants,
              options.target.facts, slot.outcome.diagnostics);
          if (!concrete.is_valid() ||
              task_package.types.type(concrete).kind == TypeKind::Invalid) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure = "generic layout product failed";
            continue;
          }
          slot.generic_type = concrete;
          slot.generic_result = export_interface_type_application(
              result.packages[package_index]->identity, task_package, concrete,
              demand.source, demand.arguments, slot.outcome.diagnostics);
          const AppendOnlyTableView<ImportedTypeInstantiationRequest> requests =
              task_package.imported_type_instantiation_requests_for_read();
          for (std::size_t index = request_begin;
               index < requests.size(); ++index) {
            slot.generic_dependencies.push_back(
                requests[index]);
          }
          if (!slot.generic_dependencies.empty()) {
            slot.generic_package = task_package.materialize_task_view();
            slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
            continue;
          }
          if (!owner_result_is_concrete(*slot.generic_result)) {
            slot.outcome.kind = SemanticProductOutcomeKind::Error;
            slot.outcome.failure =
                "generic layout product produced no concrete owner result";
            slot.outcome.diagnostics.error(source_symbol.name_range,
                                           slot.outcome.failure);
            continue;
          }
          slot.generic_semantic = extract_semantic_task_append(
              prefix, task_package, task_constants);
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
        if (result.packages[package_index].has_value() &&
            !result.packages[package_index]->declaration_discovery.terminal &&
            package_has_unindexed_declaration_work(
                result, owner, *result.packages[package_index])) {
          slot.package = *result.packages[package_index];
          continue;
        }
        slot.package = analyze_workspace_package_names(
            sources, options, schedule, package_index,
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
        const CompiledPackage &package = *result.packages[package_index];
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
              compile_time_synthesis_mode(options.stage),
              slot.outcome.diagnostics);
          if (slot.condition->status == CompileTimeProductStatus::Complete) {
            continue;
          }
          if (slot.condition->status ==
              CompileTimeProductStatus::WaitingForSynthesis) {
            slot.constant_package = std::move(task_package);
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
            compile_time_synthesis_mode(options.stage),
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
      } while (false);
      return true;
    };

    std::vector<std::size_t> safe_tasks;
    std::vector<std::size_t> source_mutating_tasks;
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      const SemanticProductId product = wave.products[task_index];
      const SemanticProductKind kind =
          result.semantic_graph.products[product.value].kind;
      const bool may_mutate_sources =
          kind == SemanticProductKind::PackageNameSet ||
          kind == SemanticProductKind::PackageInterface;
      if (may_mutate_sources) {
        source_mutating_tasks.push_back(task_index);
        continue;
      }
      safe_tasks.push_back(task_index);
    }
    std::vector<std::size_t> task_indices = safe_tasks;
    task_indices.insert(task_indices.end(), source_mutating_tasks.begin(),
                        source_mutating_tasks.end());
    execution.task_indices = &task_indices;

    WorkGraph execution_graph;
    execution_graph.tasks.resize(task_indices.size());
    for (std::size_t index = 0; index < source_mutating_tasks.size(); ++index) {
      const WorkTaskId task =
          static_cast<WorkTaskId>(safe_tasks.size() + index);
      execution_graph.tasks[task].dependencies.reserve(
          safe_tasks.size() + (index == 0 ? 0 : 1));
      for (std::size_t safe_index = 0; safe_index < safe_tasks.size();
           ++safe_index) {
        execution_graph.tasks[task].dependencies.push_back(
            static_cast<WorkTaskId>(safe_index));
      }
      if (index != 0) {
        execution_graph.tasks[task].dependencies.push_back(task - 1);
      }
    }
    const WorkGraphRunResult scheduled = run_work_graph(
        execution_graph,
        WorkGraphRunOptions{options.semantic_worker_count},
        execute_interface_task,
        &execution);
    if (options.timings != nullptr) {
      std::size_t generic_owner_tasks = 0;
      std::size_t declaration_tasks = 0;
      for (SemanticProductId product : wave.products) {
        if (result.semantic_products
                .generic_type_demand_by_product[product.value]
                .is_valid()) {
          ++generic_owner_tasks;
        }
        const SemanticProductKind kind =
            result.semantic_graph.products[product.value].kind;
        if (kind == SemanticProductKind::TypeIdentity ||
            kind == SemanticProductKind::TypeMembers ||
            kind == SemanticProductKind::TypeMemberTypes) {
          ++declaration_tasks;
        }
      }
      options.timings->add_counter("interface semantic ready waves", 1);
      options.timings->add_counter(
          "interface semantic tasks scheduled", wave.products.size());
      options.timings->add_counter(
          "interface semantic worker slots", scheduled.workers_used);
      if (generic_owner_tasks > 1) {
        options.timings->add_counter(
            "generic owner tasks in shared ready waves",
            generic_owner_tasks);
      }
      if (declaration_tasks > 1) {
        options.timings->add_counter(
            "declaration tasks in shared ready waves",
            declaration_tasks);
      }
    }
    if (!scheduled.ok) {
      std::string failure = "interface semantic worker scheduling failed";
      for (std::size_t index = 0; index < scheduled.tasks.size(); ++index) {
        if (scheduled.tasks[index].state != WorkTaskState::Failed) continue;
        failure += " at task " + std::to_string(index) + ": " +
            scheduled.tasks[index].failure;
        break;
      }
      diagnostics.error(SourceRange::invalid(), std::move(failure));
      return false;
    }

    {
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
        const std::vector<SemanticProductId> &base_dependencies =
            result.semantic_products.packages[owner.value].declaration_inputs;
        // Reachable conditions precede the member packets which consume their
        // selections. Constants follow type products so ambiguous type/value
        // declarations can depend on their classification row.
        const bool appended =
            append_package_condition_products(result, base_dependencies, owner,
                                              *slot.package,
                                              slot.outcome.diagnostics) &&
            append_package_type_products(result, base_dependencies, owner,
                                         *slot.package,
                                         slot.outcome.diagnostics) &&
            append_package_constant_products(result, base_dependencies, owner,
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

    // A declaration, member, condition, constant, or declaration-owned integer
    // task can discover one package-wide opaque synthesis set. Append the set
    // only after every task in the frozen wave has produced its private result,
    // then block each discovering product on that shared row. PackageNameSet
    // already depends on all indexed declaration work, so consumers remain
    // suspended transitively without publishing a partial package interface.
    if (options.stage == CompileWorkspaceStage::DiscoverInterfaceSynthesis) {
      // A procedure constraint which names an incomplete package declaration or
      // sibling member cannot be typed until that generated source lands.
      // Determine this package fact from every retained task plus the complete
      // frozen wave; worker order must not decide whether a body site joins the
      // opaque set.
      std::vector<bool> incomplete_declaration_set(result.packages.size(),
                                                   false);
      for (const InterfaceSynthesisSurface &surface : synthesis_surfaces) {
        if (!surface.product.is_valid() ||
            surface.product.value >=
                result.semantic_products.package_by_product.size()) {
          continue;
        }
        const PackageId owner =
            result.semantic_products.package_by_product[surface.product.value];
        for (const SemanticSite &site : surface.package.sites_for_read()) {
          if (site.kind == SemanticSiteKind::SynthesisDeclaration ||
              site.kind == SemanticSiteKind::SynthesisMember) {
            incomplete_declaration_set[owner.value] = true;
            break;
          }
        }
      }
      for (std::size_t task_index = 0; task_index < wave.products.size();
           ++task_index) {
        WorkspaceInterfaceTaskSlot &slot = slots[task_index];
        const SemanticProductId product = wave.products[task_index];
        const SemanticProductKind kind =
            result.semantic_graph.products[product.value].kind;
        const SemanticPackage *task_package = nullptr;
        if (kind == SemanticProductKind::PackageNameSet &&
            slot.package.has_value()) {
          task_package = &slot.package->declaration_discovery.package;
        } else if (slot.declaration_package.has_value()) {
          task_package = &*slot.declaration_package;
        } else if (slot.constant_package.has_value()) {
          task_package = &*slot.constant_package;
        }
        if (task_package == nullptr)
          continue;
        for (const SemanticSite &site : task_package->sites_for_read()) {
          if (site.kind != SemanticSiteKind::SynthesisDeclaration &&
              site.kind != SemanticSiteKind::SynthesisMember) {
            continue;
          }
          const PackageId owner =
              result.semantic_products.package_by_product[product.value];
          incomplete_declaration_set[owner.value] = true;
          break;
        }
      }

      for (std::size_t task_index = 0; task_index < wave.products.size();
           ++task_index) {
        WorkspaceInterfaceTaskSlot &slot = slots[task_index];
        const SemanticProductId product = wave.products[task_index];
        const SemanticProductKind kind =
            result.semantic_graph.products[product.value].kind;
        bool waits = slot.outcome.kind ==
                SemanticProductOutcomeKind::WaitingForSynthesis &&
            kind != SemanticProductKind::OpaqueSynthesisSet;
        if (kind == SemanticProductKind::PackageNameSet &&
            slot.package.has_value() &&
            has_semantic_site(
                slot.package->declaration_discovery.package,
                SemanticSiteKind::SynthesisDeclaration)) {
          waits = true;
        }
        if (!waits)
          continue;

        const PackageId owner =
            result.semantic_products.package_by_product[product.value];
        CompiledPackage *compiled = result.packages[owner.value].has_value()
                                        ? &*result.packages[owner.value]
                                        : nullptr;
        SemanticPackage task_package;
        ConstantTable task_constants;
        std::vector<SymbolId> compile_time_procedures;
        bool retained_context = false;
        if (kind == SemanticProductKind::PackageNameSet &&
            slot.package.has_value()) {
          task_package = slot.package->declaration_discovery.package;
          task_constants = package_product_constant_inputs(
              task_package,
              slot.package->declaration_discovery.published_constants);
          retained_context = true;
        } else if (slot.declaration_package.has_value() &&
                   slot.declaration_type.has_value() && compiled != nullptr) {
          task_package = *slot.declaration_package;
          task_constants = package_product_constant_inputs(
              task_package,
              compiled->declaration_discovery.published_constants);
          compile_time_procedures =
              slot.declaration_type->compile_time_procedures;
          retained_context = true;
        } else if (slot.constant_package.has_value() && compiled != nullptr) {
          task_package = *slot.constant_package;
          task_constants = package_product_constant_inputs(
              task_package,
              compiled->declaration_discovery.published_constants);
          if (slot.condition.has_value()) {
            compile_time_procedures = slot.condition->compile_time_procedures;
          } else if (slot.constant.has_value()) {
            compile_time_procedures = slot.constant->compile_time_procedures;
          }
          retained_context = true;
        }
        if (!retained_context) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure =
              "synthesis wait lost its task-owned semantic context";
          slot.outcome.diagnostics.error(SourceRange::invalid(),
                                         slot.outcome.failure);
          continue;
        }

        InterfaceSynthesisSurface surface;
        const ConditionalSelections &selections =
            kind == SemanticProductKind::PackageNameSet &&
                    slot.package.has_value()
                ? slot.package->declaration_discovery.selections
                : compiled->declaration_discovery.selections;
        if (!prepare_interface_synthesis_surface(
                sources, result.graph.packages[owner.value].loaded, selections,
                options.target.facts, product, std::move(task_package),
                std::move(task_constants), std::move(compile_time_procedures),
                incomplete_declaration_set[owner.value], surface,
                slot.outcome.diagnostics)) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure =
              "interface synthesis constraint preparation failed";
          continue;
        }

        PackageSemanticProducts &package_products =
            result.semantic_products.packages[owner.value];
        if (!package_products.opaque_synthesis_set.is_valid()) {
          package_products.opaque_synthesis_set =
              append_workspace_semantic_product(
                  result, SemanticProductKind::OpaqueSynthesisSet,
                  package_products.declaration_inputs, owner, false,
                  slot.outcome.diagnostics);
        }
        if (!package_products.opaque_synthesis_set.is_valid()) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "cannot publish opaque package synthesis set";
          continue;
        }
        slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
        if (std::find(slot.outcome.dependencies.begin(),
                      slot.outcome.dependencies.end(),
                      package_products.opaque_synthesis_set) ==
            slot.outcome.dependencies.end()) {
          slot.outcome.dependencies.push_back(
              package_products.opaque_synthesis_set);
        }
        const auto existing =
            std::find_if(synthesis_surfaces.begin(), synthesis_surfaces.end(),
                         [&](const InterfaceSynthesisSurface &candidate) {
                           return candidate.product == product;
                         });
        if (existing == synthesis_surfaces.end()) {
          synthesis_surfaces.push_back(std::move(surface));
        }
      }
    }

    // Publish successful declaration packets in ProductId order. Each packet
    // appends private semantic rows, interns equal structural discoveries, and
    // applies only the collected type/symbol rows owned by that product. No
    // complete package successor crosses the worker boundary.
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      WorkspaceInterfaceTaskSlot &slot = slots[task_index];
      if (!slot.declaration_semantic.has_value() ||
          slot.outcome.kind != SemanticProductOutcomeKind::Complete) {
        continue;
      }
      const SemanticProductId product = wave.products[task_index];
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      CompiledPackage &package = *result.packages[owner.value];
      SemanticTaskPublication publication;
      if (!publish_semantic_task_append(
              package.declaration_discovery.package,
              package.declaration_discovery.published_constants,
              *slot.declaration_semantic,
              publication,
              slot.outcome.diagnostics)) {
        slot.outcome.kind = SemanticProductOutcomeKind::Error;
        slot.outcome.failure = "declaration semantic publication failed";
      }
    }

    // Generic owners contain only appended semantic rows. Publish each packet
    // in ProductId order, intern equal types/instances against earlier packets,
    // and translate the task-local concrete TypeId retained by its product.
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      WorkspaceInterfaceTaskSlot &slot = slots[task_index];
      if (!slot.generic_semantic.has_value() ||
          !slot.generic_type.has_value() ||
          slot.outcome.kind != SemanticProductOutcomeKind::Complete) {
        continue;
      }
      const SemanticProductId product = wave.products[task_index];
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      CompiledPackage &package = *result.packages[owner.value];
      const TypeId task_type = *slot.generic_type;
      SemanticTaskPublication publication;
      if (!publish_semantic_task_append(
              package.declarations.package,
              package.declarations.constants,
              *slot.generic_semantic,
              publication,
              slot.outcome.diagnostics)) {
        slot.outcome.kind = SemanticProductOutcomeKind::Error;
        slot.outcome.failure = "generic layout publication failed";
        continue;
      }
      slot.generic_type = publication.canonical_type(task_type);
    }

    // Turn every requester-local owner row into a canonical command product in
    // stable task and request order. The blocked attempt itself is disposable;
    // only exported argument graphs cross into the owner's canonical tables.
    // An existing equal key is reused directly, including when its product is
    // already complete from an earlier requester.
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      WorkspaceInterfaceTaskSlot &slot = slots[task_index];
      if (slot.outcome.kind == SemanticProductOutcomeKind::Error)
        continue;
      const std::vector<ImportedTypeInstantiationRequest> *requests = nullptr;
      const SemanticPackage *requester_package = nullptr;
      if (slot.declaration_type.has_value() &&
          !slot.declaration_type->generic_type_dependencies.empty()) {
        requests = &slot.declaration_type->generic_type_dependencies;
        requester_package = slot.declaration_package.has_value()
            ? &*slot.declaration_package
            : nullptr;
      } else if (!slot.generic_dependencies.empty()) {
        requests = &slot.generic_dependencies;
        requester_package = slot.generic_package.has_value()
            ? &*slot.generic_package
            : nullptr;
      }
      if (requests == nullptr)
        continue;
      if (requester_package == nullptr) {
        slot.outcome.kind = SemanticProductOutcomeKind::Error;
        slot.outcome.failure =
            "generic dependency lost its requester-local package";
        slot.outcome.diagnostics.error(SourceRange::invalid(),
                                       slot.outcome.failure);
        continue;
      }
      const PackageId requester =
          result.semantic_products
              .package_by_product[wave.products[task_index].value];
      for (const ImportedTypeInstantiationRequest &request : *requests) {
        GenericTypeDemandAppendResult appended = append_generic_type_demand(
            result, schedule, requester, *requester_package, request,
            slot.outcome.diagnostics);
        if (!appended.ok) {
          slot.outcome.kind = SemanticProductOutcomeKind::Error;
          slot.outcome.failure = "cannot append generic type demand";
          break;
        }
        slot.outcome.dependencies.insert(
            slot.outcome.dependencies.end(), appended.dependencies.begin(),
            appended.dependencies.end());
        if (appended.product.has_value()) {
          slot.outcome.dependencies.push_back(*appended.product);
        }
      }
      if (slot.outcome.kind != SemanticProductOutcomeKind::Error) {
        slot.outcome.kind = SemanticProductOutcomeKind::Blocked;
      }
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
           kind != SemanticProductKind::TypeMembers &&
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

    // A completed generic owner task publishes one immutable transport graph
    // beside the TypeNaturalLayout product. The owner-local instance rows were
    // installed in the canonical declaration package before graph publication;
    // requesters consume only this package-independent result on their retry.
    for (std::size_t task_index = 0; task_index < wave.products.size();
         ++task_index) {
      const SemanticProductId product = wave.products[task_index];
      const GenericTypeDemandId demand_id =
          result.semantic_products
              .generic_type_demand_by_product[product.value];
      if (!demand_id.is_valid() ||
          result.semantic_graph.products[product.value].state !=
              SemanticProductState::Complete ||
          !slots[task_index].generic_result.has_value() ||
          !slots[task_index].generic_type.has_value()) {
        continue;
      }
      GenericTypeDemand &demand =
          result.semantic_products.generic_type_demands[demand_id.value];
      if (demand.result.has_value()) {
        diagnostics.error(SourceRange::invalid(),
                          "generic type demand was published more than once");
        return false;
      }
      demand.result = std::move(*slots[task_index].generic_result);
      result.semantic_products.type_by_product[product.value] =
          *slots[task_index].generic_type;
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
      // A selected package branch can introduce a nominal with its first
      // member condition, while a selected member branch can reveal the next
      // `else when` in its opaque chain. Discover that new frontier now and
      // append its products before another semantic wave is frozen.
      discover_package_member_condition_sites(
          sources, result.graph.packages[owner.value].loaded,
          package.declaration_discovery.selections,
          package.declaration_discovery.package, diagnostics);
      const PackageSemanticProducts &package_products =
          result.semantic_products.packages[owner.value];
      if (!append_package_condition_products(
              result, package_products.declaration_inputs, owner, package,
              diagnostics)) {
        return false;
      }
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
      published.bindings.insert(
          position, {symbol, std::move(constant.value), constant.type});
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

// External procedure keys are already canonicalized before this comparison.
// The full digest is semantic identity; the native name remains in the check so
// an inconsistent shortened spelling cannot silently reuse a completed body.
[[nodiscard]] bool same_external_procedure_demand(
    const ProcedureInstantiationDemand &left,
    const ProcedureInstantiationDemand &right) {
  return left.instance_name == right.instance_name &&
      left.digest == right.digest;
}

[[nodiscard]] std::optional<std::size_t> body_work_index_for_symbol(
    const PackageBodyWorkState &state,
    SymbolId symbol) {
  for (std::size_t index = 0; index < state.work.size(); ++index) {
    if (state.work[index].symbol == symbol) return index;
  }
  return std::nullopt;
}

// Finds the body row named by one external seed after the body initializer has
// either created a new instance or promoted an equal locally discovered one.
// ParametricInstanceRecord is canonical semantic identity; linkage_name is the
// exact cross-package spelling installed by promotion.
[[nodiscard]] std::optional<std::size_t> external_body_work_index(
    const PackageBodyWorkState &state,
    const ProcedureInstantiationDemand &demand) {
  for (const ParametricInstanceRecord &instance :
       state.package.parametric_instances_for_read()) {
    if (!instance.externally_requested) continue;
    const Symbol &symbol = state.package.symbols.symbol(instance.instance);
    const std::string_view linkage =
        symbol.linkage_name.empty() ? std::string_view(symbol.name)
                                    : std::string_view(symbol.linkage_name);
    if (linkage != demand.instance_name) continue;
    return body_work_index_for_symbol(state, instance.instance);
  }
  return std::nullopt;
}

// SelectedPackageBodyWork separates semantic checking reachability from public
// cross-package demand. A specialization called by its own package can remain
// in procedures after its last external consumer disappears, while external
// roots must become empty so the completed private body is not re-exported.
struct SelectedPackageBodyWork {
  std::vector<std::size_t> procedures;
  std::vector<std::size_t> external_roots;
};

// Computes the current program selection over one append-only body work table.
// Authored roots are unconditional. Current external demands name additional
// roots. Every discovered child has a smaller prerequisite index, so one
// source-order pass computes the complete transitive closure deterministically.
[[nodiscard]] SelectedPackageBodyWork select_package_body_work(
    const CompiledPackage &package,
    std::span<const ProcedureInstantiationDemand> current_demands,
    DiagnosticSink &diagnostics) {
  SelectedPackageBodyWork result;
  std::vector<bool> selected(package.bodies.work.size(), false);
  for (std::size_t index = 0; index < package.bodies.work.size(); ++index) {
    if (package.bodies.work[index].origin ==
        ProcedureBodyWorkOrigin::Authored) {
      selected[index] = true;
    }
  }

  for (const ProcedureInstantiationDemand &demand : current_demands) {
    const auto product = std::find_if(
        package.external_procedure_products.begin(),
        package.external_procedure_products.end(),
        [&](const ExternalProcedureBodyProduct &candidate) {
          return same_external_procedure_demand(candidate.demand, demand);
        });
    if (product == package.external_procedure_products.end() ||
        product->work_index >= selected.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "selected external procedure demand has no completed body product");
      continue;
    }
    selected[product->work_index] = true;
    result.external_roots.push_back(product->work_index);
  }

  std::sort(result.external_roots.begin(), result.external_roots.end());
  result.external_roots.erase(
      std::unique(
          result.external_roots.begin(), result.external_roots.end()),
      result.external_roots.end());
  result.procedures.reserve(selected.size());
  for (std::size_t index = 0; index < selected.size(); ++index) {
    const std::optional<std::size_t> prerequisite =
        package.bodies.work[index].prerequisite;
    if (prerequisite.has_value()) {
      if (*prerequisite >= index) {
        diagnostics.error(
            SourceRange::invalid(),
            "procedure body prerequisite is not an earlier product");
        continue;
      }
      selected[index] = selected[index] || selected[*prerequisite];
    }
    if (selected[index]) result.procedures.push_back(index);
  }
  return result;
}

[[nodiscard]] std::vector<SemanticSite> selected_package_semantic_sites(
    const CompiledPackage &package) {
  std::vector<SemanticSite> result;
  const AppendOnlyTableView<SemanticSite> declaration_sites =
      package.declarations.package.sites_for_read();
  const AppendOnlyTableView<SemanticSite> body_sites =
      package.bodies.package.sites_for_read();
  result.reserve(declaration_sites.size());
  for (const SemanticSite &site : declaration_sites) result.push_back(site);
  for (std::size_t index : package.selected_procedure_work) {
    if (index >= package.bodies.procedures.size()) continue;
    for (std::size_t site_index :
         package.bodies.procedures[index].semantic_site_indices) {
      if (site_index < body_sites.size()) result.push_back(body_sites[site_index]);
    }
  }
  return result;
}

[[nodiscard]] std::vector<SymbolId> selected_external_instance_symbols(
    const CompiledPackage &package) {
  std::vector<SymbolId> result;
  for (std::size_t work_index : package.selected_external_procedure_work) {
    if (work_index < package.bodies.work.size()) {
      result.push_back(package.bodies.work[work_index].symbol);
    }
  }
  std::sort(result.begin(), result.end(), [](SymbolId left, SymbolId right) {
    return left.value < right.value;
  });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

// One selected cross-package request and the completed body-work index which
// proves it is live. request is copied because later canonical demand export
// names its imported symbol proxy in the retained consumer package. The index
// addresses that package's append-only work/procedure/product domain.
struct SelectedImportedProcedureRequest {
  ImportedProcedureInstance request;
  std::size_t requester_work = 0;
};

// Returns every cross-package concrete call required by the selected HIR.
// Product-owned discovery rows cover work first created by that body. The HIR
// scan additionally covers reuse of a proxy created by an earlier product: if
// the earlier product later becomes unselected, the still-selected reference
// must continue selecting the owner body. Results preserve selected body and
// expression order; instance-proxy identity removes duplicates directly.
[[nodiscard]] std::vector<SelectedImportedProcedureRequest>
selected_imported_procedure_requests(const CompiledPackage &package) {
  std::vector<SelectedImportedProcedureRequest> result;
  std::vector<bool> seen(
      package.bodies.package.symbols.symbol_count(), false);
  const auto append_request = [&](
      const ImportedProcedureInstance &request,
      std::size_t requester_work) {
    if (!request.instance_proxy.is_valid() ||
        request.instance_proxy.value >= seen.size() ||
        seen[request.instance_proxy.value]) {
      return;
    }
    seen[request.instance_proxy.value] = true;
    result.push_back({request, requester_work});
  };
  const AppendOnlyTableView<ImportedProcedureInstance> package_requests =
      package.bodies.package.imported_procedure_instances_for_read();
  std::vector<const ImportedProcedureInstance *> request_by_proxy;
  request_by_proxy.reserve(package_requests.size());
  for (const ImportedProcedureInstance &request : package_requests) {
    request_by_proxy.push_back(&request);
  }
  std::sort(
      request_by_proxy.begin(),
      request_by_proxy.end(),
      [](const ImportedProcedureInstance *left,
         const ImportedProcedureInstance *right) {
        return left->instance_proxy.value < right->instance_proxy.value;
      });
  for (std::size_t index : package.selected_procedure_work) {
    if (index >= package.bodies.procedures.size()) continue;
    const ProcedureBodyHirResult &procedure = package.bodies.procedures[index];
    for (const ImportedProcedureInstance &request :
         procedure.imported_procedure_instances) {
      append_request(request, index);
    }
    for (std::size_t expression_index = 0;
         expression_index < procedure.program.expression_count();
         ++expression_index) {
      const HirExpression &expression = procedure.program.expression(
          HirExpressionId{static_cast<std::uint32_t>(expression_index)});
      if (!expression.symbol.is_valid()) continue;
      const auto request = std::lower_bound(
          request_by_proxy.begin(),
          request_by_proxy.end(),
          expression.symbol.value,
          [](const ImportedProcedureInstance *candidate, std::uint32_t value) {
            return candidate->instance_proxy.value < value;
          });
      if (request != request_by_proxy.end() &&
          (*request)->instance_proxy == expression.symbol) {
        append_request(**request, index);
      }
    }
  }
  return result;
}

[[nodiscard]] std::vector<SymbolId> selected_imported_instance_proxies(
    const CompiledPackage &package) {
  std::vector<SymbolId> result;
  for (const SelectedImportedProcedureRequest &request :
       selected_imported_procedure_requests(package)) {
    result.push_back(request.request.instance_proxy);
  }
  std::sort(result.begin(), result.end(), [](SymbolId left, SymbolId right) {
    return left.value < right.value;
  });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

// ProcedureDemandDiscovery keeps one portable owner demand beside the exact
// completed consumer-body product which exposed it. The requester is graph
// provenance, not demand identity: several bodies may request the same concrete
// specialization, but the owner still creates and checks one immutable product.
struct ProcedureDemandDiscovery {
  ProcedureInstantiationDemand demand;
  SemanticProductId requester;
};

// Translates one selected imported call from the consumer's TypeStore into the
// package-independent demand packet understood by its owner. The content hash
// is also the native monomorphization spelling. Naming the consumer proxy here
// keeps HIR lowering and owner materialization on the same exact symbol without
// leaking owner-local TypeIds across the package boundary.
[[nodiscard]] std::optional<std::size_t> export_procedure_demand(
    std::size_t consumer_index,
    const SelectedImportedProcedureRequest &selected,
    const WorkspaceDependencyIndex &schedule,
    CompileWorkspaceResult &result,
    ProcedureDemandDiscovery &discovery,
    DiagnosticSink &diagnostics) {
  CompiledPackage &consumer = *result.packages[consumer_index];
  const ImportedProcedureInstance &request = selected.request;
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
    return std::nullopt;
  }

  const std::vector<SemanticProductId> &consumer_products =
      result.semantic_products.packages[consumer_index].procedure_bodies;
  if (selected.requester_work >= consumer_products.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "generic procedure request has no completed consumer body product");
    return std::nullopt;
  }
  discovery.requester = consumer_products[selected.requester_work];
  if (!discovery.requester.is_valid() ||
      discovery.requester.value >= result.semantic_graph.products.size() ||
      result.semantic_graph.products[discovery.requester.value].state !=
          SemanticProductState::Complete) {
    diagnostics.error(
        SourceRange::invalid(),
        "generic procedure requester product is not complete");
    return std::nullopt;
  }

  ProcedureInstantiationDemand &demand = discovery.demand;
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
        consumer.identity,
        consumer.bodies.package,
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
  hash_u64(name_hash, static_cast<std::uint64_t>(request.pack_types.size()));
  for (TypeId pack_type : request.pack_types) {
    InterfaceTypeGraph graph = export_interface_type(
        consumer.identity,
        consumer.bodies.package,
        pack_type,
        diagnostics);
    const Sha256Digest digest = hash_interface_type_graph(graph);
    name_hash.update(digest.bytes);
    demand.pack_types.push_back(std::move(graph));
  }
  demand.digest = name_hash.finalize();
  demand.instance_name = request.public_template_name + "$mono$" +
      demand.digest.hex().substr(0, 24);

  bool named_proxy = false;
  for (ImportedSymbol &imported : consumer.bodies.package.imported_symbols) {
    if (imported.proxy != request.instance_proxy) continue;
    imported.public_name = demand.instance_name;
    named_proxy = true;
    break;
  }
  if (!named_proxy) {
    diagnostics.error(
        SourceRange::invalid(),
        "generic procedure instance has no imported symbol proxy");
    return std::nullopt;
  }
  return owner_index;
}

// Canonicalization makes demand discovery independent of worker completion and
// consumer traversal order. Equal concrete packets choose the lowest requester
// ProductId solely to give a newly created owner product one deterministic
// prerequisite. A shortened native-name collision remains an ordinary compiler
// diagnostic rather than allowing one request to select another request's code.
[[nodiscard]] bool canonicalize_procedure_discoveries(
    std::vector<ProcedureDemandDiscovery> &discoveries,
    DiagnosticSink &diagnostics) {
  std::sort(
      discoveries.begin(),
      discoveries.end(),
      [](const ProcedureDemandDiscovery &left,
         const ProcedureDemandDiscovery &right) {
        if (left.demand.instance_name != right.demand.instance_name) {
          return left.demand.instance_name < right.demand.instance_name;
        }
        if (left.demand.digest.bytes != right.demand.digest.bytes) {
          return left.demand.digest.bytes < right.demand.digest.bytes;
        }
        return left.requester.value < right.requester.value;
      });
  std::vector<ProcedureDemandDiscovery> canonical;
  canonical.reserve(discoveries.size());
  for (ProcedureDemandDiscovery &discovery : discoveries) {
    if (!canonical.empty() &&
        canonical.back().demand.instance_name ==
            discovery.demand.instance_name) {
      if (canonical.back().demand.digest != discovery.demand.digest) {
        diagnostics.error(
            SourceRange::invalid(),
            "generic procedure instances have a native-name hash collision");
        return false;
      }
      continue;
    }
    canonical.push_back(std::move(discovery));
  }
  discoveries = std::move(canonical);
  return true;
}

// Compares only semantic demand identity. The deterministic requester is a
// dependency chosen when a product is first created; changing which selected
// consumer now exposes a retained demand does not require another fixed-point
// iteration or mutate the completed owner product.
[[nodiscard]] bool same_procedure_discovery_set(
    std::span<const ProcedureDemandDiscovery> left,
    std::span<const ProcedureDemandDiscovery> right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!same_external_procedure_demand(
            left[index].demand, right[index].demand)) {
      return false;
    }
  }
  return true;
}

// Publishes the current procedure selection into the graph side table. This is
// called only at a body fixed point, when every selected work row has a complete
// product; intermediate selection uses work indices directly so a just-
// discovered pending child never masquerades as completed semantic output.
[[nodiscard]] bool publish_selected_body_products(
    std::size_t package_index,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  CompiledPackage &package = *result.packages[package_index];
  PackageSemanticProducts &products =
      result.semantic_products.packages[package_index];
  products.selected_procedure_bodies.clear();
  products.selected_procedure_bodies.reserve(
      package.selected_procedure_work.size());
  for (std::size_t work_index : package.selected_procedure_work) {
    if (work_index >= products.procedure_bodies.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "selected procedure has no semantic product row");
      return false;
    }
    const SemanticProductId product = products.procedure_bodies[work_index];
    if (!product.is_valid() ||
        product.value >= result.semantic_graph.products.size() ||
        result.semantic_graph.products[product.value].state !=
            SemanticProductState::Complete) {
      diagnostics.error(
          SourceRange::invalid(),
          "selected procedure body product is not complete");
      return false;
    }
    products.selected_procedure_bodies.push_back(product);
  }
  return true;
}

// Runs one workspace-wide ready set of explicit
// ProcedureTemplateBody/ProcedureInstanceBody products. Product rows are
// appended package-by-package in stable PackageId/work order before the global
// semantic wave is frozen. Roots discovered while that wave runs remain only
// in their PackageBodyWorkState until every worker has joined; the next call
// appends them with an exact dependency on the body which discovered their
// semantic environment.
//
// Task diagnostics are private and are merged only by
// publish_semantic_ready_wave in product-ID order. A source-invalid body still
// publishes a completed recoverable HIR row whose HirProcedure::valid is false;
// PackageBodyWorkState::ok prevents effects/lowering from consuming it. Graph
// Error is reserved here for scheduler/publication failure which produced no
// usable body result. This distinction lets independent and lexically nested
// authored bodies continue checking after an earlier source diagnostic.
//
// ProcedureBodyTaskSlot is one workspace task's complete package-local input
// context and isolated output. ProcedureBodyWaveExecution owns the two shared
// phase inputs plus fixed vectors of these slots and graph outcomes. One
// WorkTaskId moves exactly one input and writes one result/outcome. No vector or
// package is resized until the complete workspace wave joins. Source errors
// remain successful scheduler operations because recoverable HIR and private
// diagnostics are still valid products.
struct ProcedureBodyTaskSlot {
  const LoadedPackage *loaded = nullptr;
  const ConditionalSelections *selections = nullptr;
  ProcedureBodyTaskInput input;
  ProcedureBodyTaskResult result;
};

struct ProcedureBodyWaveExecution {
  const SourceManager *sources = nullptr;
  const TargetFacts *target = nullptr;
  std::vector<ProcedureBodyTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_procedure_body_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context =
      *static_cast<ProcedureBodyWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.sources == nullptr || context.target == nullptr ||
      context.slots == nullptr || context.outcomes == nullptr ||
      index >= context.slots->size() || index >= context.outcomes->size() ||
      (*context.slots)[index].loaded == nullptr ||
      (*context.slots)[index].selections == nullptr) {
    failure = "procedure body worker received an invalid task slot";
    return false;
  }

  ProcedureBodyTaskSlot &slot = (*context.slots)[index];
  SemanticProductOutcome &outcome = (*context.outcomes)[index];
  slot.result = check_procedure_body_work(
      *context.sources,
      *slot.loaded,
      *slot.selections,
      *context.target,
      std::move(slot.input),
      outcome.diagnostics);
  outcome.kind = SemanticProductOutcomeKind::Complete;
  return true;
}

// Appends product rows for every package's unpublished work suffix. A locally
// discovered root depends on its package-local prerequisite. A newly
// materialized external root instead depends on the first completed consumer
// body which requested it. Promotion can point an external-demand record at an
// existing local product; that product already has its original exact edge and
// is never modified after completion.
[[nodiscard]] bool append_workspace_body_products(
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    PackageBodyWorkState &state = package.bodies;
    PackageSemanticProducts &package_products =
        result.semantic_products.packages[package_index];
    if (!package_products.package_interface.is_valid() ||
        package_products.package_interface.value >=
            result.semantic_graph.products.size() ||
        result.semantic_graph.products[package_products.package_interface.value]
                .state != SemanticProductState::Complete) {
      diagnostics.error(
          SourceRange::invalid(),
          "procedure body products require a completed package interface");
      return false;
    }
    if (package_products.procedure_bodies.size() != state.next_work ||
        state.procedures.size() != state.next_work) {
      diagnostics.error(
          SourceRange::invalid(),
          "procedure body work and product prefixes have different sizes");
      return false;
    }

    while (package_products.procedure_bodies.size() < state.work.size()) {
      const std::size_t work_index =
          package_products.procedure_bodies.size();
      const ProcedureBodyWorkItem &work = state.work[work_index];
      std::vector<SemanticProductId> dependencies{
          package_products.package_interface};
      if (work.prerequisite.has_value()) {
        if (*work.prerequisite >= package_products.procedure_bodies.size()) {
          diagnostics.error(
              SourceRange::invalid(),
              "procedure body work has an unpublished prerequisite");
          return false;
        }
        dependencies.push_back(
            package_products.procedure_bodies[*work.prerequisite]);
      } else if (work.origin == ProcedureBodyWorkOrigin::ExternalDemand) {
        const auto external = std::find_if(
            package.external_procedure_products.begin(),
            package.external_procedure_products.end(),
            [&](const ExternalProcedureBodyProduct &candidate) {
              return candidate.work_index == work_index;
            });
        if (external == package.external_procedure_products.end() ||
            !external->requester.is_valid()) {
          diagnostics.error(
              SourceRange::invalid(),
              "external procedure body has no requester product");
          return false;
        }
        dependencies.push_back(external->requester);
      }
      const SemanticProductKind kind = work.parametric_template
          ? SemanticProductKind::ProcedureTemplateBody
          : SemanticProductKind::ProcedureInstanceBody;
      const PackageId owner{static_cast<std::uint32_t>(package_index)};
      const SemanticProductId product = append_workspace_semantic_product(
          result, kind, dependencies, owner, false, diagnostics);
      if (!product.is_valid()) return false;
      result.semantic_products.procedure_by_product[product.value] =
          work.symbol;
      package_products.procedure_bodies.push_back(product);
    }
  }
  return true;
}

// One package-local publication packet for a workspace wave. task_slots are in
// that package's canonical body-work order and index the workspace task/result
// arrays. Rows are retained only until the wave joins and are never serialized.
struct PackageBodyWavePublication {
  std::size_t package_index = 0;
  std::size_t first_work = 0;
  std::vector<std::size_t> task_slots;
};

// Runs at most one currently pending workspace body wave. With no unpublished
// work it is a no-op. Otherwise every pending package suffix first receives
// graph rows, the complete semantic ready set is frozen, and one bounded worker
// batch checks all packages together. Package-local semantic appends publish in
// PackageId/work order, followed by graph outcomes and diagnostics in ProductId
// order. Any scheduler or invariant failure is diagnosed and returns false.
[[nodiscard]] bool run_workspace_body_ready_wave(
    const SourceManager &sources,
    const TargetFacts &target,
    std::size_t worker_count,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  bool has_ready_work = false;
  for (const std::optional<CompiledPackage> &package : result.packages) {
    has_ready_work = has_ready_work ||
        (package.has_value() &&
         package->bodies.next_work < package->bodies.work.size());
  }
  if (!has_ready_work) return true;
  if (!append_workspace_body_products(result, diagnostics)) return false;

  const SemanticReadyWave wave =
      freeze_semantic_ready_wave(result.semantic_graph);
  if (wave.status != SemanticReadyWaveStatus::Ready) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body products did not form a ready semantic wave" +
            (wave.failure.empty() ? std::string{} : ": " + wave.failure));
    return false;
  }

  const std::size_t no_task = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> task_by_product(
      result.semantic_graph.products.size(), no_task);
  for (std::size_t task_index = 0; task_index < wave.products.size();
       ++task_index) {
    const SemanticProductId product = wave.products[task_index];
    const SemanticProductKind kind =
        result.semantic_graph.products[product.value].kind;
    if (kind != SemanticProductKind::ProcedureTemplateBody &&
        kind != SemanticProductKind::ProcedureInstanceBody) {
      diagnostics.error(
          SourceRange::invalid(),
          "procedure body ready wave contains another semantic product kind");
      return false;
    }
    task_by_product[product.value] = task_index;
  }

  std::vector<ProcedureBodyTaskSlot> slots(wave.products.size());
  std::vector<PackageBodyWavePublication> publications;
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    PackageBodyWorkState &state = package.bodies;
    if (state.next_work >= state.work.size()) continue;
    const std::size_t first_work = state.next_work;
    std::vector<ProcedureBodyTaskInput> inputs =
        take_ready_procedure_body_wave(state, diagnostics);
    PackageBodyWavePublication publication;
    publication.package_index = package_index;
    publication.first_work = first_work;
    publication.task_slots.reserve(inputs.size());
    const std::vector<SemanticProductId> &products =
        result.semantic_products.packages[package_index].procedure_bodies;
    for (std::size_t offset = 0; offset < inputs.size(); ++offset) {
      const std::size_t work_index = first_work + offset;
      if (work_index >= products.size()) {
        diagnostics.error(
            SourceRange::invalid(),
            "procedure body task has no semantic product");
        return false;
      }
      const SemanticProductId product = products[work_index];
      const std::size_t task_index = task_by_product[product.value];
      if (task_index == no_task || slots[task_index].loaded != nullptr) {
        diagnostics.error(
            SourceRange::invalid(),
            "procedure body product is absent or duplicated in its ready wave");
        return false;
      }
      slots[task_index].loaded =
          &result.graph.packages[package_index].loaded;
      slots[task_index].selections = &package.declarations.selections;
      slots[task_index].input = std::move(inputs[offset]);
      publication.task_slots.push_back(task_index);
    }
    publications.push_back(std::move(publication));
  }
  for (const ProcedureBodyTaskSlot &slot : slots) {
    if (slot.loaded == nullptr || slot.selections == nullptr) {
      diagnostics.error(
          SourceRange::invalid(),
          "procedure body ready product has no package task slot");
      return false;
    }
  }

  std::vector<SemanticProductOutcome> outcomes(wave.products.size());
  WorkGraph execution_graph;
  execution_graph.tasks.resize(wave.products.size());
  ProcedureBodyWaveExecution execution{
      &sources,
      &target,
      &slots,
      &outcomes,
  };
  const WorkGraphRunResult scheduled = run_work_graph(
      execution_graph,
      WorkGraphRunOptions{worker_count},
      execute_procedure_body_task,
      &execution);
  if (timings != nullptr) {
    timings->add_counter("procedure body ready waves", 1);
    timings->add_counter(
        "procedure body tasks scheduled", wave.products.size());
    timings->add_counter(
        "procedure body worker slots", scheduled.workers_used);
  }
  if (!scheduled.ok) {
    std::string failure = "procedure body worker scheduling failed";
    for (std::size_t index = 0; index < scheduled.tasks.size(); ++index) {
      if (scheduled.tasks[index].state == WorkTaskState::Failed) {
        failure += " at task " + std::to_string(index) + ": " +
            scheduled.tasks[index].failure;
        break;
      }
    }
    diagnostics.error(SourceRange::invalid(), std::move(failure));
    return false;
  }

  // Package publishers remain independent canonical-ID domains. Publish them
  // in PackageId order, with each package's results in work order, only after
  // the complete workspace worker set has joined.
  for (PackageBodyWavePublication &publication : publications) {
    std::vector<ProcedureBodyTaskResult> package_results;
    package_results.reserve(publication.task_slots.size());
    for (std::size_t task_index : publication.task_slots) {
      package_results.push_back(std::move(slots[task_index].result));
    }
    PackageBodyWorkState &state =
        result.packages[publication.package_index]->bodies;
    PackageSemanticProducts &products =
        result.semantic_products.packages[publication.package_index];
    if (products.body_type_producer.size() > state.package.types.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "body type-producer table is longer than its TypeStore");
      return false;
    }
    products.body_type_producer.resize(state.package.types.size());
    if (!publish_procedure_body_wave(
            state, std::move(package_results), diagnostics)) {
      return false;
    }
    products.body_type_producer.resize(state.package.types.size());
    for (std::size_t offset = 0; offset < publication.task_slots.size();
         ++offset) {
      const std::size_t work_index = publication.first_work + offset;
      if (work_index >= state.procedures.size() ||
          work_index >= products.procedure_bodies.size()) {
        diagnostics.error(
            SourceRange::invalid(),
            "published body types have no procedure product");
        return false;
      }
      const SemanticProductId producer =
          products.procedure_bodies[work_index];
      for (TypeId type : state.procedures[work_index].published_types) {
        if (!type.is_valid() ||
            type.value >= products.body_type_producer.size() ||
            products.body_type_producer[type.value].is_valid()) {
          diagnostics.error(
              SourceRange::invalid(),
              "published body type has an invalid or duplicate producer");
          return false;
        }
        products.body_type_producer[type.value] = producer;
      }
    }
  }

  std::string publication_error;
  if (!publish_semantic_ready_wave(
          result.semantic_graph,
          wave,
          outcomes,
          diagnostics,
          publication_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish procedure body wave: " + publication_error);
    return false;
  }
  return true;
}

// One ABI task reads an immutable complete package TypeStore and writes one
// compact target classification. The table row is not written by the worker:
// product-ID publication remains the only mutation of CompiledPackage.
struct AbiClassificationTaskSlot {
  const TypeStore *types = nullptr;
  TypeId type;
  Aarch64CAbiType result;
};

struct AbiClassificationWaveExecution {
  const TargetFacts *target = nullptr;
  std::vector<AbiClassificationTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_abi_classification_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context =
      *static_cast<AbiClassificationWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.target == nullptr || context.slots == nullptr ||
      context.outcomes == nullptr || index >= context.slots->size() ||
      index >= context.outcomes->size() ||
      (*context.slots)[index].types == nullptr ||
      !(*context.slots)[index].type.is_valid()) {
    failure = "ABI classification worker received an invalid task slot";
    return false;
  }

  AbiClassificationTaskSlot &slot = (*context.slots)[index];
  slot.result =
      classify_aarch64_c_type(*slot.types, slot.type, *context.target);
  (*context.outcomes)[index].kind = SemanticProductOutcomeKind::Complete;
  return true;
}

// Appends the unpublished suffix of each package's TypeId-indexed ABI facet.
// A declaration-baseline type is available at PackageInterface. A type first
// installed by body publication instead names that exact procedure product.
// The target is explicit in both cases because ABI class is not natural layout
// and a future profile may classify the same layout differently.
[[nodiscard]] bool append_workspace_abi_products(
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    PackageSemanticProducts &products =
        result.semantic_products.packages[package_index];
    const std::size_t type_count = package.bodies.package.types.size();
    if (products.abi_classifications.size() != package.c_abi.rows.size() ||
        products.abi_classifications.size() > type_count ||
        products.body_type_producer.size() > type_count) {
      diagnostics.error(
          SourceRange::invalid(),
          "ABI product, payload, and TypeStore prefixes are inconsistent");
      return false;
    }
    products.body_type_producer.resize(type_count);

    while (products.abi_classifications.size() < type_count) {
      const std::size_t type_index = products.abi_classifications.size();
      if (type_index >= std::numeric_limits<std::uint32_t>::max()) {
        diagnostics.error(
            SourceRange::invalid(),
            "ABI classification exceeds the TypeId domain");
        return false;
      }
      const TypeId type{static_cast<std::uint32_t>(type_index)};
      std::vector<SemanticProductId> dependencies{
          result.semantic_products.target};
      const SemanticProductId body_producer =
          products.body_type_producer[type_index];
      if (body_producer.is_valid()) {
        dependencies.push_back(body_producer);
      } else {
        if (!products.package_interface.is_valid()) {
          diagnostics.error(
              SourceRange::invalid(),
              "declaration ABI classification has no package interface");
          return false;
        }
        dependencies.push_back(products.package_interface);
      }
      const PackageId owner{static_cast<std::uint32_t>(package_index)};
      const SemanticProductId product = append_workspace_semantic_product(
          result,
          SemanticProductKind::TypeAbiClassification,
          dependencies,
          owner,
          false,
          diagnostics);
      if (!product.is_valid()) return false;
      result.semantic_products.type_by_product[product.value] = type;
      products.abi_classifications.push_back(product);
    }
  }
  return true;
}

// Completes every newly appended ABI facet in one workspace ready wave. All
// body products are already at a fixed point, so classification is read-only and
// independent across TypeIds and packages. Results and graph states publish in
// ProductId order after the bounded worker set joins.
[[nodiscard]] bool run_workspace_abi_classifications(
    const TargetFacts &target,
    std::size_t worker_count,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  const std::size_t product_count_before = result.semantic_graph.products.size();
  if (!append_workspace_abi_products(result, diagnostics)) return false;
  if (result.semantic_graph.products.size() == product_count_before) return true;

  const SemanticReadyWave wave =
      freeze_semantic_ready_wave(result.semantic_graph);
  if (wave.status != SemanticReadyWaveStatus::Ready) {
    diagnostics.error(
        SourceRange::invalid(),
        "ABI classification products did not form a ready semantic wave" +
            (wave.failure.empty() ? std::string{} : ": " + wave.failure));
    return false;
  }

  std::vector<AbiClassificationTaskSlot> slots(wave.products.size());
  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    const SemanticProductId product = wave.products[index];
    const SemanticProduct &row = result.semantic_graph.products[product.value];
    const PackageId owner =
        result.semantic_products.package_by_product[product.value];
    const TypeId type = result.semantic_products.type_by_product[product.value];
    if (row.kind != SemanticProductKind::TypeAbiClassification ||
        !owner.is_valid() || owner.value >= result.packages.size() ||
        !result.packages[owner.value].has_value() || !type.is_valid() ||
        type.value >= result.packages[owner.value]->bodies.package.types.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "ABI ready wave contains an invalid classification product");
      return false;
    }
    slots[index].types = &result.packages[owner.value]->bodies.package.types;
    slots[index].type = type;
  }

  std::vector<SemanticProductOutcome> outcomes(wave.products.size());
  WorkGraph execution_graph;
  execution_graph.tasks.resize(wave.products.size());
  AbiClassificationWaveExecution execution{&target, &slots, &outcomes};
  const WorkGraphRunResult scheduled = run_work_graph(
      execution_graph,
      WorkGraphRunOptions{worker_count},
      execute_abi_classification_task,
      &execution);
  if (timings != nullptr) {
    timings->add_counter("ABI classification ready waves", 1);
    timings->add_counter("ABI classifications", wave.products.size());
    timings->add_counter(
        "ABI classification worker slots", scheduled.workers_used);
  }
  if (!scheduled.ok) {
    std::string failure = "ABI classification worker scheduling failed";
    for (std::size_t index = 0; index < scheduled.tasks.size(); ++index) {
      if (scheduled.tasks[index].state != WorkTaskState::Failed) continue;
      failure += " at task " + std::to_string(index) + ": " +
          scheduled.tasks[index].failure;
      break;
    }
    diagnostics.error(SourceRange::invalid(), std::move(failure));
    return false;
  }

  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    const SemanticProductId product = wave.products[index];
    const PackageId owner =
        result.semantic_products.package_by_product[product.value];
    const TypeId type = result.semantic_products.type_by_product[product.value];
    CompiledPackage &package = *result.packages[owner.value];
    if (package.c_abi.rows.empty()) {
      package.c_abi.target_identity = target.identity;
    }
    if (package.c_abi.target_identity != target.identity ||
        type.value != package.c_abi.rows.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "ABI classification payload is not in TypeId order");
      return false;
    }
    package.c_abi.rows.push_back(slots[index].result);
  }

  std::string publication_error;
  if (!publish_semantic_ready_wave(
          result.semantic_graph,
          wave,
          outcomes,
          diagnostics,
          publication_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish ABI classification wave: " + publication_error);
    return false;
  }
  return true;
}

// Returns the exact already-closed effect inputs imported by one package. A
// consumer's direct rows may substitute imported return/write contracts, so its
// products must name the dependency SCC rows rather than relying on the earlier
// preliminary PackageInterface barrier.
[[nodiscard]] std::vector<SemanticProductId> dependency_effect_products(
    std::size_t package_index,
    const WorkspaceDependencyIndex &schedule,
    const CompileWorkspaceResult &result) {
  std::vector<SemanticProductId> dependencies;
  for (std::size_t edge_index :
       schedule.import_edges_by_consumer[package_index]) {
    const PackageImport &edge = result.graph.imports[edge_index];
    const PackageSemanticProducts &imported =
        result.semantic_products.packages[edge.imported_package.value];
    dependencies.insert(
        dependencies.end(),
        imported.closed_effect_sccs.begin(),
        imported.closed_effect_sccs.end());
  }
  return dependencies;
}

struct DirectEffectTaskSlot {
  const CompiledPackage *package = nullptr;
  std::span<const std::size_t> selected_indices;
  std::size_t selected_position = 0;
  const TargetProfile *target = nullptr;
  std::span<const ForeignProviderAudit> provider_audits;
  DirectProcedureEffectSummary result;
};

struct DirectEffectWaveExecution {
  std::vector<DirectEffectTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_direct_effect_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context =
      *static_cast<DirectEffectWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.slots == nullptr || context.outcomes == nullptr ||
      index >= context.slots->size() || index >= context.outcomes->size()) {
    failure = "direct-effect worker received an invalid task slot";
    return false;
  }
  DirectEffectTaskSlot &slot = (*context.slots)[index];
  if (slot.package == nullptr || slot.target == nullptr ||
      slot.selected_position >= slot.selected_indices.size()) {
    failure = "direct-effect worker received an invalid package projection";
    return false;
  }
  const std::size_t work_index =
      slot.selected_indices[slot.selected_position];
  if (work_index >= slot.package->bodies.procedures.size()) {
    failure = "direct-effect body work index is outside the HIR product table";
    return false;
  }
  const std::size_t hir_procedure_count =
      slot.package->bodies.procedures[work_index].program.procedures().size();
  if (hir_procedure_count != 1) {
    failure = "direct-effect body product owns " +
        std::to_string(hir_procedure_count) + " HIR procedure rows";
    return false;
  }
  slot.result = collect_direct_procedure_effect(
      slot.package->bodies.package,
      slot.package->bodies.procedures,
      slot.selected_indices,
      slot.selected_position,
      slot.package->imported_contracts,
      slot.target,
      slot.provider_audits);
  (*context.outcomes)[index].kind = SemanticProductOutcomeKind::Complete;
  return true;
}

// Publishes one DirectEffectSummary task per selected procedure. Every task
// reads the same immutable package/source domain and owns exactly one row; the
// bounded worker batch cannot observe or enrich a sibling result. Product and
// payload order both follow selected_procedure_bodies.
[[nodiscard]] bool run_package_direct_effect_products(
    std::size_t package_index,
    const WorkspaceDependencyIndex &schedule,
    const TargetProfile &target,
    std::span<const ForeignProviderAudit> provider_audits,
    std::size_t worker_count,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  CompiledPackage &package = *result.packages[package_index];
  PackageSemanticProducts &products =
      result.semantic_products.packages[package_index];
  if (!products.effect_body_work_indices.empty() ||
      !products.direct_effect_summaries.empty() ||
      !package.direct_effects.procedures.empty() ||
      products.selected_procedure_bodies.size() !=
          package.selected_procedure_work.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "direct-effect product slice is not empty or aligned before scheduling");
    return false;
  }

  for (std::size_t work_index : package.selected_procedure_work) {
    if (work_index >= package.bodies.procedures.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "selected effect body is outside the HIR product table");
      return false;
    }
    const std::size_t procedure_count =
        package.bodies.procedures[work_index].program.procedures().size();
    if (procedure_count == 0) continue;
    if (procedure_count != 1) {
      diagnostics.error(
          SourceRange::invalid(),
          "one procedure body product owns multiple HIR procedure rows");
      return false;
    }
    products.effect_body_work_indices.push_back(work_index);
  }

  const std::vector<SemanticProductId> imported_dependencies =
      dependency_effect_products(package_index, schedule, result);
  const PackageId owner{static_cast<std::uint32_t>(package_index)};
  for (std::size_t position = 0;
       position < products.effect_body_work_indices.size(); ++position) {
    const std::size_t work_index =
        products.effect_body_work_indices[position];
    std::vector<SemanticProductId> dependencies{
        result.semantic_products.target,
        products.procedure_bodies[work_index]};
    dependencies.insert(
        dependencies.end(),
        imported_dependencies.begin(),
        imported_dependencies.end());
    const SemanticProductId product = append_workspace_semantic_product(
        result,
        SemanticProductKind::DirectEffectSummary,
        dependencies,
        owner,
        false,
        diagnostics);
    if (!product.is_valid()) return false;
    result.semantic_products.procedure_by_product[product.value] =
        package.bodies.work[work_index].symbol;
    products.direct_effect_summaries.push_back(product);
  }
  if (products.direct_effect_summaries.empty()) return true;

  const SemanticReadyWave wave =
      freeze_semantic_ready_wave(result.semantic_graph);
  if (wave.status != SemanticReadyWaveStatus::Ready ||
      wave.products != products.direct_effect_summaries) {
    diagnostics.error(
        SourceRange::invalid(),
        "direct-effect products did not form their exact ready wave" +
            (wave.failure.empty() ? std::string{} : ": " + wave.failure));
    return false;
  }

  std::vector<DirectEffectTaskSlot> slots(wave.products.size());
  std::vector<SemanticProductOutcome> outcomes(wave.products.size());
  for (std::size_t index = 0; index < slots.size(); ++index) {
    slots[index].package = &package;
    slots[index].selected_indices = products.effect_body_work_indices;
    slots[index].selected_position = index;
    slots[index].target = &target;
    slots[index].provider_audits = provider_audits;
  }
  WorkGraph execution_graph;
  execution_graph.tasks.resize(slots.size());
  DirectEffectWaveExecution execution{&slots, &outcomes};
  const WorkGraphRunResult scheduled = run_work_graph(
      execution_graph,
      WorkGraphRunOptions{worker_count},
      execute_direct_effect_task,
      &execution);
  if (timings != nullptr) {
    timings->add_counter("direct effect ready waves", 1);
    timings->add_counter("direct effect tasks", slots.size());
    timings->add_counter(
        "direct effect worker slots", scheduled.workers_used);
  }
  if (!scheduled.ok) {
    std::string failure = "direct-effect worker scheduling failed";
    for (std::size_t index = 0; index < scheduled.tasks.size(); ++index) {
      if (scheduled.tasks[index].state != WorkTaskState::Failed) continue;
      failure += " at task " + std::to_string(index) + ": " +
          scheduled.tasks[index].failure;
      break;
    }
    diagnostics.error(SourceRange::invalid(), std::move(failure));
    return false;
  }
  package.direct_effects.procedures.reserve(slots.size());
  for (DirectEffectTaskSlot &slot : slots) {
    package.direct_effects.procedures.push_back(std::move(slot.result));
  }
  std::string publication_error;
  if (!publish_semantic_ready_wave(
          result.semantic_graph,
          wave,
          outcomes,
          diagnostics,
          publication_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish direct-effect wave: " + publication_error);
    return false;
  }
  return true;
}

// Appends the explicit legal-cycle products discovered by flow/effect closure.
// The SCC algorithm is itself the cycle-aware coordinator: ordinary DAG task
// rows cannot represent a legal recursive component before its membership is
// known. Once the task-owned EffectSummaryResult exists, these rows publish its
// immutable component payloads through normal waiting/ready/complete graph
// transitions. Exact component dependencies preserve all available parallelism.
[[nodiscard]] bool publish_package_effect_scc_products(
    std::size_t package_index,
    const WorkspaceDependencyIndex &schedule,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  CompiledPackage &package = *result.packages[package_index];
  PackageSemanticProducts &products =
      result.semantic_products.packages[package_index];
  if (!products.closed_effect_sccs.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "closed-effect SCC products were already published");
    return false;
  }
  const std::vector<SemanticProductId> imported_dependencies =
      dependency_effect_products(package_index, schedule, result);
  const PackageId owner{static_cast<std::uint32_t>(package_index)};
  for (std::size_t component_index = 0;
       component_index < package.effects.components.size(); ++component_index) {
    const ClosedEffectComponent &component =
        package.effects.components[component_index];
    std::vector<SemanticProductId> dependencies{
        result.semantic_products.target};
    dependencies.insert(
        dependencies.end(),
        imported_dependencies.begin(),
        imported_dependencies.end());
    for (std::size_t procedure_index : component.procedure_indices) {
      if (procedure_index < products.direct_effect_summaries.size()) {
        dependencies.push_back(
            products.direct_effect_summaries[procedure_index]);
      }
    }
    for (std::size_t dependency : component.dependencies) {
      if (dependency >= products.closed_effect_sccs.size()) {
        diagnostics.error(
            SourceRange::invalid(),
            "effect SCC dependency is not in dependency-first order");
        return false;
      }
      dependencies.push_back(products.closed_effect_sccs[dependency]);
    }
    const SemanticProductId product = append_workspace_semantic_product(
        result,
        SemanticProductKind::ClosedEffectScc,
        dependencies,
        owner,
        false,
        diagnostics);
    if (!product.is_valid()) return false;
    products.closed_effect_sccs.push_back(product);
  }

  std::size_t completed = 0;
  while (completed < products.closed_effect_sccs.size()) {
    const SemanticReadyWave wave =
        freeze_semantic_ready_wave(result.semantic_graph);
    if (wave.status != SemanticReadyWaveStatus::Ready) {
      diagnostics.error(
          SourceRange::invalid(),
          "closed-effect SCC products did not form a ready wave" +
              (wave.failure.empty() ? std::string{} : ": " + wave.failure));
      return false;
    }
    std::vector<SemanticProductOutcome> outcomes(wave.products.size());
    for (SemanticProductId product : wave.products) {
      if (product.value >= result.semantic_graph.products.size() ||
          result.semantic_graph.products[product.value].kind !=
              SemanticProductKind::ClosedEffectScc ||
          result.semantic_products.package_by_product[product.value] != owner) {
        diagnostics.error(
            SourceRange::invalid(),
            "closed-effect ready wave contains another product");
        return false;
      }
    }
    std::string publication_error;
    if (!publish_semantic_ready_wave(
            result.semantic_graph,
            wave,
            outcomes,
            diagnostics,
            publication_error)) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot publish closed-effect SCC wave: " + publication_error);
      return false;
    }
    completed += wave.products.size();
    if (timings != nullptr) {
      timings->add_counter("closed effect SCC ready waves", 1);
      timings->add_counter("closed effect SCC products", wave.products.size());
    }
  }
  return true;
}

struct DenialTaskSlot {
  const SourceManager *sources = nullptr;
  const LoadedPackage *loaded = nullptr;
  const SemanticPackage *package = nullptr;
  const ProcedureBodyHirResult *body = nullptr;
  const EffectSummaryResult *effects = nullptr;
};

struct DenialWaveExecution {
  std::vector<DenialTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_denial_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context = *static_cast<DenialWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.slots == nullptr || context.outcomes == nullptr ||
      index >= context.slots->size() || index >= context.outcomes->size()) {
    failure = "denial worker received an invalid task slot";
    return false;
  }
  const DenialTaskSlot &slot = (*context.slots)[index];
  if (slot.sources == nullptr || slot.loaded == nullptr ||
      slot.package == nullptr || slot.body == nullptr || slot.effects == nullptr) {
    failure = "denial worker received incomplete semantic inputs";
    return false;
  }
  SemanticProductOutcome &outcome = (*context.outcomes)[index];
  const std::span<const ProcedureBodyHirResult> one_body(slot.body, 1);
  const bool ok = check_procedure_denials(
      *slot.sources,
      *slot.loaded,
      *slot.package,
      one_body,
      *slot.effects,
      outcome.diagnostics);
  outcome.kind = ok ? SemanticProductOutcomeKind::Complete
                    : SemanticProductOutcomeKind::Error;
  if (!ok) outcome.failure = "procedure violates an active denial";
  return true;
}

// Checks every selected procedure as one independent DenialResult product.
// Its owning closed SCC already depends transitively on every callee component,
// so the denial row needs only that SCC and its exact body product.
[[nodiscard]] bool run_package_denial_products(
    const SourceManager &sources,
    std::size_t package_index,
    std::size_t worker_count,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics,
    bool &denials_ok) {
  denials_ok = true;
  CompiledPackage &package = *result.packages[package_index];
  PackageSemanticProducts &products =
      result.semantic_products.packages[package_index];
  if (!products.denial_results.empty() ||
      products.direct_effect_summaries.size() !=
          products.effect_body_work_indices.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "denial product slice is not empty or aligned before scheduling");
    return false;
  }
  const PackageId owner{static_cast<std::uint32_t>(package_index)};
  for (std::size_t position = 0;
       position < products.effect_body_work_indices.size(); ++position) {
    std::optional<std::size_t> component_index;
    for (std::size_t candidate = 0;
         candidate < package.effects.components.size(); ++candidate) {
      const std::vector<std::size_t> &members =
          package.effects.components[candidate].procedure_indices;
      if (std::find(members.begin(), members.end(), position) != members.end()) {
        component_index = candidate;
        break;
      }
    }
    if (!component_index.has_value() ||
        *component_index >= products.closed_effect_sccs.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "selected procedure has no closed effect SCC product");
      return false;
    }
    const std::array dependencies{
        products.procedure_bodies[products.effect_body_work_indices[position]],
        products.closed_effect_sccs[*component_index]};
    const SemanticProductId product = append_workspace_semantic_product(
        result,
        SemanticProductKind::DenialResult,
        dependencies,
        owner,
        false,
        diagnostics);
    if (!product.is_valid()) return false;
    const std::size_t work_index =
        products.effect_body_work_indices[position];
    result.semantic_products.procedure_by_product[product.value] =
        package.bodies.work[work_index].symbol;
    products.denial_results.push_back(product);
  }
  if (products.denial_results.empty()) return true;

  const SemanticReadyWave wave =
      freeze_semantic_ready_wave(result.semantic_graph);
  if (wave.status != SemanticReadyWaveStatus::Ready ||
      wave.products != products.denial_results) {
    diagnostics.error(
        SourceRange::invalid(),
        "denial products did not form their exact ready wave" +
            (wave.failure.empty() ? std::string{} : ": " + wave.failure));
    return false;
  }
  std::vector<DenialTaskSlot> slots(wave.products.size());
  std::vector<SemanticProductOutcome> outcomes(wave.products.size());
  for (std::size_t position = 0; position < slots.size(); ++position) {
    const std::size_t work_index =
        products.effect_body_work_indices[position];
    slots[position] = {
        &sources,
        &result.graph.packages[package_index].loaded,
        &package.bodies.package,
        &package.bodies.procedures[work_index],
        &package.effects};
  }
  WorkGraph execution_graph;
  execution_graph.tasks.resize(slots.size());
  DenialWaveExecution execution{&slots, &outcomes};
  const WorkGraphRunResult scheduled = run_work_graph(
      execution_graph,
      WorkGraphRunOptions{worker_count},
      execute_denial_task,
      &execution);
  if (timings != nullptr) {
    timings->add_counter("denial ready waves", 1);
    timings->add_counter("denial tasks", slots.size());
    timings->add_counter("denial worker slots", scheduled.workers_used);
  }
  if (!scheduled.ok) {
    diagnostics.error(SourceRange::invalid(), "denial worker scheduling failed");
    return false;
  }
  for (const SemanticProductOutcome &outcome : outcomes) {
    denials_ok = denials_ok &&
        outcome.kind == SemanticProductOutcomeKind::Complete;
  }
  std::string publication_error;
  if (!publish_semantic_ready_wave(
          result.semantic_graph,
          wave,
          outcomes,
          diagnostics,
          publication_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish denial wave: " + publication_error);
    return false;
  }
  return true;
}

enum class PackageLoweringBarrierKind {
  StaticData,
  ParsedAssembly,
};

// Freezes the exact selected concrete runtime body order once, before either
// package-static emission or MIR scheduling uses it. Symbolic templates and
// compile-time-only procedures remain checked semantic products but have no
// runtime artifact. The output order is the selected body-product order and is
// therefore independent of worker completion.
[[nodiscard]] bool select_runtime_procedure_work(
    const CompiledPackage &package,
    std::vector<std::size_t> &selected,
    DiagnosticSink &diagnostics) {
  selected.clear();
  for (std::size_t work_index : package.selected_procedure_work) {
    if (work_index >= package.bodies.procedures.size()) {
      diagnostics.error(
          SourceRange::invalid(),
          "selected runtime body is outside the HIR product table");
      return false;
    }
    const std::vector<HirProcedure> &procedures =
        package.bodies.procedures[work_index].program.procedures();
    if (procedures.empty()) continue;
    if (procedures.size() != 1) {
      diagnostics.error(
          SourceRange::invalid(),
          "one procedure body product owns multiple runtime candidates");
      return false;
    }
    if (procedures.front().parametric_template ||
        procedures.front().compile_time_only) {
      continue;
    }
    selected.push_back(work_index);
  }
  return true;
}

// One task slot owns either the package-static product or the complete assembly
// payload for one selected package. When LLVM is requested, the static task
// emits the unit which defines globals, runtime support, and any hosted entry;
// otherwise the same product is only the immutable global/constants barrier.
// Captured standalone assembly bytes already live in compiler state. Inline-
// assembly analysis visits each procedure-owned HIR arena separately and
// concatenates only its source-keyed AssemblyRegion rows; it never constructs a
// package HIR program.
struct PackageLoweringBarrierTaskSlot {
  PackageLoweringBarrierKind kind = PackageLoweringBarrierKind::StaticData;
  const SourceManager *sources = nullptr;
  const LoadedPackage *loaded = nullptr;
  const TargetProfile *target = nullptr;
  const CompiledPackage *package = nullptr;
  bool emit_llvm = false;
  LlvmIrOptions llvm_options;
  std::vector<SymbolId> runtime_procedures;
  LlvmIrResult static_data;
  AssemblyProgram assembly;
};

struct PackageLoweringBarrierWaveExecution {
  std::vector<PackageLoweringBarrierTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_package_lowering_barrier_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context =
      *static_cast<PackageLoweringBarrierWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.slots == nullptr || context.outcomes == nullptr ||
      index >= context.slots->size() || index >= context.outcomes->size()) {
    failure = "package-lowering worker received an invalid task slot";
    return false;
  }
  PackageLoweringBarrierTaskSlot &slot = (*context.slots)[index];
  SemanticProductOutcome &outcome = (*context.outcomes)[index];
  if (slot.kind == PackageLoweringBarrierKind::StaticData) {
    if (!slot.emit_llvm) {
      outcome.kind = SemanticProductOutcomeKind::Complete;
      return true;
    }
    if (slot.sources == nullptr || slot.target == nullptr ||
        slot.package == nullptr) {
      failure = "package-static worker received incomplete package inputs";
      return false;
    }
    slot.static_data = emit_llvm_package_static_data(
        *slot.target,
        *slot.sources,
        slot.llvm_options,
        slot.package->bodies.package,
        slot.package->c_abi,
        slot.package->declarations.global_initializers,
        slot.runtime_procedures,
        outcome.diagnostics);
    outcome.kind = slot.static_data.ok
        ? SemanticProductOutcomeKind::Complete
        : SemanticProductOutcomeKind::Error;
    if (!slot.static_data.ok) {
      outcome.failure = "package static data failed LLVM emission";
    }
    return true;
  }
  if (slot.sources == nullptr || slot.loaded == nullptr ||
      slot.target == nullptr || slot.package == nullptr) {
    failure = "parsed-assembly worker received incomplete package inputs";
    return false;
  }

  slot.assembly.ok = true;
  for (std::size_t work_index : slot.package->selected_procedure_work) {
    if (work_index >= slot.package->bodies.procedures.size()) {
      failure = "parsed-assembly body index is outside the HIR product table";
      return false;
    }
    AssemblyProgram local = analyze_aarch64_assembly(
        *slot.sources,
        *slot.loaded,
        *slot.target,
        slot.package->bodies.package,
        slot.package->bodies.procedures[work_index].program,
        outcome.diagnostics);
    slot.assembly.ok = slot.assembly.ok && local.ok;
    slot.assembly.regions.insert(
        slot.assembly.regions.end(),
        std::make_move_iterator(local.regions.begin()),
        std::make_move_iterator(local.regions.end()));
  }
  outcome.kind = slot.assembly.ok
      ? SemanticProductOutcomeKind::Complete
      : SemanticProductOutcomeKind::Error;
  if (!slot.assembly.ok) {
    outcome.failure = "package contains invalid parsed assembly";
  }
  return true;
}

// Publishes package static-data and parsed-assembly barriers in one workspace
// wave. Both products name the selected body/denial generation explicitly;
// static data additionally names every ABI row because native global layout
// and the independently compilable LLVM unit consume that complete target
// facet. Independent packages may emit static data and analyze assembly
// concurrently, while publication remains PackageId/product ordered.
[[nodiscard]] bool run_workspace_lowering_barriers(
    const SourceManager &sources,
    const TargetProfile &target,
    bool emit_llvm,
    bool emit_program_entry,
    ValidationKind validation_kind,
    std::span<const ValidationEntry> validation_entries,
    std::size_t worker_count,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  std::vector<SemanticProductId> expected_wave;
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    PackageSemanticProducts &products =
        result.semantic_products.packages[package_index];
    if (products.package_static_data.is_valid() ||
        products.package_assembly.is_valid()) {
      diagnostics.error(
          SourceRange::invalid(),
          "package lowering barriers were already published");
      return false;
    }
    if (!select_runtime_procedure_work(
            *result.packages[package_index],
            products.mir_body_work_indices,
            diagnostics)) {
      return false;
    }
    std::vector<SemanticProductId> static_dependencies{
        result.semantic_products.target,
        products.package_interface};
    static_dependencies.insert(
        static_dependencies.end(),
        products.selected_procedure_bodies.begin(),
        products.selected_procedure_bodies.end());
    static_dependencies.insert(
        static_dependencies.end(),
        products.abi_classifications.begin(),
        products.abi_classifications.end());
    static_dependencies.insert(
        static_dependencies.end(),
        products.denial_results.begin(),
        products.denial_results.end());
    const PackageId owner{static_cast<std::uint32_t>(package_index)};
    products.package_static_data = append_workspace_semantic_product(
        result,
        SemanticProductKind::PackageStaticData,
        static_dependencies,
        owner,
        false,
        diagnostics);
    if (!products.package_static_data.is_valid()) return false;
    expected_wave.push_back(products.package_static_data);

    std::vector<SemanticProductId> assembly_dependencies{
        result.semantic_products.target,
        result.semantic_products.source_generation};
    assembly_dependencies.insert(
        assembly_dependencies.end(),
        products.selected_procedure_bodies.begin(),
        products.selected_procedure_bodies.end());
    assembly_dependencies.insert(
        assembly_dependencies.end(),
        products.denial_results.begin(),
        products.denial_results.end());
    products.package_assembly = append_workspace_semantic_product(
        result,
        SemanticProductKind::PackageAssembly,
        assembly_dependencies,
        owner,
        false,
        diagnostics);
    if (!products.package_assembly.is_valid()) return false;
    expected_wave.push_back(products.package_assembly);
  }
  if (expected_wave.empty()) return true;

  const SemanticReadyWave wave =
      freeze_semantic_ready_wave(result.semantic_graph);
  if (wave.status != SemanticReadyWaveStatus::Ready ||
      wave.products != expected_wave) {
    diagnostics.error(
        SourceRange::invalid(),
        "package lowering barriers did not form their exact ready wave" +
            (wave.failure.empty() ? std::string{} : ": " + wave.failure));
    return false;
  }
  std::vector<PackageLoweringBarrierTaskSlot> slots(wave.products.size());
  std::vector<SemanticProductOutcome> outcomes(wave.products.size());
  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    const SemanticProductId product = wave.products[index];
    const PackageId owner =
        result.semantic_products.package_by_product[product.value];
    if (!owner.is_valid() || owner.value >= result.packages.size() ||
        !result.packages[owner.value].has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "package lowering barrier has no retained package owner");
      return false;
    }
    const SemanticProductKind kind =
        result.semantic_graph.products[product.value].kind;
    slots[index].kind = kind == SemanticProductKind::PackageStaticData
        ? PackageLoweringBarrierKind::StaticData
        : PackageLoweringBarrierKind::ParsedAssembly;
    if (kind != SemanticProductKind::PackageStaticData &&
        kind != SemanticProductKind::PackageAssembly) {
      diagnostics.error(
          SourceRange::invalid(),
          "package lowering wave contains another product kind");
      return false;
    }
    slots[index].sources = &sources;
    slots[index].loaded = &result.graph.packages[owner.value].loaded;
    slots[index].target = &target;
    slots[index].package = &*result.packages[owner.value];
    slots[index].emit_llvm = emit_llvm;
    if (slots[index].kind == PackageLoweringBarrierKind::StaticData &&
        emit_llvm) {
      slots[index].llvm_options.package =
          result.graph.packages[owner.value].identity;
      slots[index].llvm_options.emit_runtime_support =
          owner.value == result.graph.root_package.value;
      slots[index].llvm_options.emit_program_entry =
          emit_program_entry && slots[index].llvm_options.emit_runtime_support;
      if (slots[index].llvm_options.emit_runtime_support) {
        slots[index].llvm_options.validation_kind = validation_kind;
        slots[index].llvm_options.validation_entries.assign(
            validation_entries.begin(), validation_entries.end());
      }
      const PackageSemanticProducts &products =
          result.semantic_products.packages[owner.value];
      slots[index].runtime_procedures.reserve(
          products.mir_body_work_indices.size());
      for (std::size_t work_index : products.mir_body_work_indices) {
        slots[index].runtime_procedures.push_back(
            slots[index]
                .package->bodies.procedures[work_index]
                .program.procedures()
                .front()
                .symbol);
      }
    }
  }
  WorkGraph execution_graph;
  execution_graph.tasks.resize(slots.size());
  PackageLoweringBarrierWaveExecution execution{&slots, &outcomes};
  const WorkGraphRunResult scheduled = run_work_graph(
      execution_graph,
      WorkGraphRunOptions{worker_count},
      execute_package_lowering_barrier_task,
      &execution);
  if (timings != nullptr) {
    timings->add_counter("package lowering barrier ready waves", 1);
    timings->add_counter(
        "package lowering barrier tasks", slots.size());
    timings->add_counter(
        "package lowering barrier worker slots", scheduled.workers_used);
  }
  if (!scheduled.ok) {
    std::string failure =
        "package lowering barrier worker scheduling failed";
    for (std::size_t index = 0; index < scheduled.tasks.size(); ++index) {
      if (scheduled.tasks[index].state != WorkTaskState::Failed) continue;
      failure += " at task " + std::to_string(index) + ": " +
          scheduled.tasks[index].failure;
      break;
    }
    diagnostics.error(SourceRange::invalid(), std::move(failure));
    return false;
  }
  bool barriers_ok = true;
  for (const SemanticProductOutcome &outcome : outcomes) {
    barriers_ok = barriers_ok &&
        outcome.kind == SemanticProductOutcomeKind::Complete;
  }
  std::string publication_error;
  if (!publish_semantic_ready_wave(
          result.semantic_graph,
          wave,
          outcomes,
          diagnostics,
          publication_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish package lowering barrier wave: " +
            publication_error);
    return false;
  }
  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    const PackageId owner = result.semantic_products.package_by_product[
        wave.products[index].value];
    CompiledPackage &package = *result.packages[owner.value];
    if (slots[index].kind == PackageLoweringBarrierKind::StaticData) {
      if (emit_llvm) {
        package.llvm.static_data = std::move(slots[index].static_data);
        if (timings != nullptr) {
          timings->add_counter("LLVM static data modules emitted", 1);
          timings->add_counter(
              "LLVM IR bytes", package.llvm.static_data.text.size());
        }
      }
    } else {
      package.assembly = std::move(slots[index].assembly);
    }
  }
  return barriers_ok;
}

struct MirProcedureTaskSlot {
  const SemanticPackage *semantic = nullptr;
  const ProcedureBodyHirResult *body = nullptr;
  const AssemblyProgram *assembly = nullptr;
  RuntimeAssertionMode runtime_assertions = RuntimeAssertionMode::On;
  MirProcedureLoweringResult result;
};

struct MirProcedureWaveExecution {
  std::vector<MirProcedureTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_mir_procedure_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context =
      *static_cast<MirProcedureWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.slots == nullptr || context.outcomes == nullptr ||
      index >= context.slots->size() || index >= context.outcomes->size()) {
    failure = "MIR worker received an invalid task slot";
    return false;
  }
  MirProcedureTaskSlot &slot = (*context.slots)[index];
  if (slot.semantic == nullptr || slot.body == nullptr ||
      slot.assembly == nullptr ||
      slot.body->program.procedures().size() != 1) {
    failure = "MIR worker received an invalid procedure product";
    return false;
  }
  SemanticProductOutcome &outcome = (*context.outcomes)[index];
  slot.result = lower_procedure_to_mir(
      *slot.semantic,
      slot.body->program,
      slot.body->program.procedures().front(),
      slot.assembly,
      slot.runtime_assertions,
      outcome.diagnostics);
  outcome.kind = slot.result.ok && slot.result.lowered
      ? SemanticProductOutcomeKind::Complete
      : SemanticProductOutcomeKind::Error;
  if (outcome.kind == SemanticProductOutcomeKind::Error) {
    outcome.failure = "procedure failed MIR lowering";
  }
  return true;
}

[[nodiscard]] std::optional<SemanticProductId> denial_product_for_body(
    const PackageSemanticProducts &products,
    std::size_t work_index) {
  for (std::size_t position = 0;
       position < products.effect_body_work_indices.size(); ++position) {
    if (products.effect_body_work_indices[position] == work_index &&
        position < products.denial_results.size()) {
      return products.denial_results[position];
    }
  }
  return std::nullopt;
}

// Appends and executes one MirProcedure product for every selected concrete
// runtime body in the workspace. Tasks read immutable semantic/HIR/assembly
// inputs and own one private MIR procedure plus diagnostics. After the global
// wave joins, the coordinator publishes each procedure directly into the side
// table row addressed by its semantic product; no package MIR is composed.
[[nodiscard]] bool run_workspace_mir_products(
    RuntimeAssertionMode runtime_assertions,
    std::size_t worker_count,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  std::vector<SemanticProductId> expected_wave;
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    PackageSemanticProducts &products =
        result.semantic_products.packages[package_index];
    if (!products.mir_procedures.empty()) {
      diagnostics.error(
          SourceRange::invalid(),
          "MIR product slice is not empty before scheduling");
      return false;
    }
    const PackageId owner{static_cast<std::uint32_t>(package_index)};
    for (std::size_t work_index : products.mir_body_work_indices) {
      if (work_index >= package.bodies.procedures.size()) {
        diagnostics.error(
            SourceRange::invalid(),
            "selected MIR body is outside the HIR product table");
        return false;
      }
      const std::vector<HirProcedure> &procedures =
          package.bodies.procedures[work_index].program.procedures();
      if (procedures.size() != 1) {
        diagnostics.error(
            SourceRange::invalid(),
            "one procedure body product owns multiple MIR candidates");
        return false;
      }
      const HirProcedure &procedure = procedures.front();
      const std::optional<SemanticProductId> denial =
          denial_product_for_body(products, work_index);
      if (!denial.has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "runtime procedure has no denial result product");
        return false;
      }
      const std::array dependencies{
          products.procedure_bodies[work_index],
          *denial,
          products.package_static_data,
          products.package_assembly};
      const SemanticProductId product = append_workspace_semantic_product(
          result,
          SemanticProductKind::MirProcedure,
          dependencies,
          owner,
          false,
          diagnostics);
      if (!product.is_valid()) return false;
      result.semantic_products.procedure_by_product[product.value] =
          procedure.symbol;
      products.mir_procedures.push_back(product);
      expected_wave.push_back(product);
    }
  }
  if (expected_wave.empty()) return true;

  const SemanticReadyWave wave =
      freeze_semantic_ready_wave(result.semantic_graph);
  if (wave.status != SemanticReadyWaveStatus::Ready ||
      wave.products != expected_wave) {
    diagnostics.error(
        SourceRange::invalid(),
        "MIR products did not form their exact workspace ready wave" +
            (wave.failure.empty() ? std::string{} : ": " + wave.failure));
    return false;
  }
  std::vector<MirProcedureTaskSlot> slots(wave.products.size());
  std::vector<SemanticProductOutcome> outcomes(wave.products.size());
  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    const SemanticProductId product = wave.products[index];
    const PackageId owner =
        result.semantic_products.package_by_product[product.value];
    if (!owner.is_valid() || owner.value >= result.packages.size() ||
        !result.packages[owner.value].has_value()) {
      diagnostics.error(
          SourceRange::invalid(), "MIR product has no retained package owner");
      return false;
    }
    CompiledPackage &package = *result.packages[owner.value];
    const PackageSemanticProducts &products =
        result.semantic_products.packages[owner.value];
    const auto position = std::find(
        products.mir_procedures.begin(),
        products.mir_procedures.end(),
        product);
    if (position == products.mir_procedures.end()) {
      diagnostics.error(
          SourceRange::invalid(), "MIR product is absent from its package index");
      return false;
    }
    const std::size_t local_index = static_cast<std::size_t>(
        position - products.mir_procedures.begin());
    const std::size_t work_index =
        products.mir_body_work_indices[local_index];
    slots[index].semantic = &package.bodies.package;
    slots[index].body = &package.bodies.procedures[work_index];
    slots[index].assembly = &package.assembly;
    slots[index].runtime_assertions = runtime_assertions;
  }
  WorkGraph execution_graph;
  execution_graph.tasks.resize(slots.size());
  MirProcedureWaveExecution execution{&slots, &outcomes};
  const WorkGraphRunResult scheduled = run_work_graph(
      execution_graph,
      WorkGraphRunOptions{worker_count},
      execute_mir_procedure_task,
      &execution);
  if (timings != nullptr) {
    timings->add_counter("MIR ready waves", 1);
    timings->add_counter("MIR procedure tasks", slots.size());
    timings->add_counter("MIR worker slots", scheduled.workers_used);
  }
  if (!scheduled.ok) {
    std::string failure = "MIR worker scheduling failed";
    for (std::size_t index = 0; index < scheduled.tasks.size(); ++index) {
      if (scheduled.tasks[index].state != WorkTaskState::Failed) continue;
      failure += " at task " + std::to_string(index) + ": " +
          scheduled.tasks[index].failure;
      break;
    }
    diagnostics.error(SourceRange::invalid(), std::move(failure));
    return false;
  }
  bool mir_ok = true;
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    mir_ok = mir_ok &&
        outcomes[index].kind == SemanticProductOutcomeKind::Complete;
    if (slots[index].result.ok && slots[index].result.lowered &&
        result.semantic_products
            .mir_procedure_by_product[wave.products[index].value]
            .has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "MIR product already owns a procedure payload");
      return false;
    }
  }
  std::string publication_error;
  if (!publish_semantic_ready_wave(
          result.semantic_graph,
          wave,
          outcomes,
          diagnostics,
          publication_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish MIR procedure wave: " + publication_error);
    return false;
  }
  for (std::size_t index = 0; index < wave.products.size(); ++index) {
    if (slots[index].result.ok && slots[index].result.lowered) {
      result.semantic_products
          .mir_procedure_by_product[wave.products[index].value] =
          std::move(slots[index].result.procedure);
    }
  }
  if (timings != nullptr) {
    timings->add_counter("MIR procedures lowered", wave.products.size());
  }
  return mir_ok;
}

// One worker owns the complete textual LLVM module for one MIR product. The
// input spans borrow immutable package state for the duration of the joined
// wave; the result and diagnostics remain task-local until product-ordered
// publication.
struct MachineFunctionTaskSlot {
  const SourceManager *sources = nullptr;
  const TargetProfile *target = nullptr;
  const SemanticPackage *semantic = nullptr;
  const Aarch64CAbiTable *abi = nullptr;
  const std::vector<SymbolId> *runtime_procedures = nullptr;
  const MirProcedure *procedure = nullptr;
  LlvmIrOptions options;
  std::size_t procedure_ordinal = 0;
  std::size_t package_function_index = 0;
  LlvmIrResult llvm;
};

struct MachineFunctionWaveExecution {
  std::vector<MachineFunctionTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_machine_function_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context =
      *static_cast<MachineFunctionWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.slots == nullptr || context.outcomes == nullptr ||
      index >= context.slots->size() || index >= context.outcomes->size()) {
    failure = "machine-function worker received an invalid task slot";
    return false;
  }
  MachineFunctionTaskSlot &slot = (*context.slots)[index];
  if (slot.sources == nullptr || slot.target == nullptr ||
      slot.semantic == nullptr || slot.abi == nullptr ||
      slot.runtime_procedures == nullptr || slot.procedure == nullptr) {
    failure = "machine-function worker received incomplete inputs";
    return false;
  }
  SemanticProductOutcome &outcome = (*context.outcomes)[index];
  slot.llvm = emit_llvm_machine_function(
      *slot.target,
      *slot.sources,
      slot.options,
      *slot.semantic,
      *slot.abi,
      *slot.runtime_procedures,
      slot.procedure_ordinal,
      *slot.procedure,
      outcome.diagnostics);
  outcome.kind = slot.llvm.ok
      ? SemanticProductOutcomeKind::Complete
      : SemanticProductOutcomeKind::Error;
  if (!slot.llvm.ok) {
    outcome.failure = "procedure failed LLVM machine-function emission";
  }
  return true;
}

// Artifact layout is a publication barrier rather than an emitter. Its worker
// validates the complete immutable input set and writes the canonical
// static/function/assembly sequence into one private slot. Later native I/O is
// allowed to consume only this sequence.
struct ArtifactLayoutTaskSlot {
  const CompiledPackage *package = nullptr;
  const PackageSemanticProducts *products = nullptr;
  PackageArtifactLayout layout;
};

struct ArtifactLayoutWaveExecution {
  std::vector<ArtifactLayoutTaskSlot> *slots = nullptr;
  std::vector<SemanticProductOutcome> *outcomes = nullptr;
};

[[nodiscard]] bool execute_artifact_layout_task(
    void *opaque_context,
    WorkTaskId task,
    std::string &failure) {
  auto &context =
      *static_cast<ArtifactLayoutWaveExecution *>(opaque_context);
  const std::size_t index = static_cast<std::size_t>(task);
  if (context.slots == nullptr || context.outcomes == nullptr ||
      index >= context.slots->size() || index >= context.outcomes->size()) {
    failure = "artifact-layout worker received an invalid task slot";
    return false;
  }
  ArtifactLayoutTaskSlot &slot = (*context.slots)[index];
  SemanticProductOutcome &outcome = (*context.outcomes)[index];
  if (slot.package == nullptr || slot.products == nullptr ||
      !slot.package->llvm.static_data.ok ||
      slot.package->llvm.machine_functions.size() !=
          slot.products->machine_functions.size()) {
    failure = "artifact-layout worker received incomplete native products";
    return false;
  }

  slot.layout.inputs.reserve(
      1 + slot.products->machine_functions.size() +
      slot.package->assembly_sources.size());
  slot.layout.inputs.push_back({
      PackageArtifactInputKind::PackageStaticData,
      0,
      slot.products->package_static_data});
  for (std::size_t function_index = 0;
       function_index < slot.products->machine_functions.size();
       ++function_index) {
    if (!slot.package->llvm.machine_functions[function_index].ok) {
      failure = "artifact layout contains an invalid machine function";
      return false;
    }
    slot.layout.inputs.push_back({
        PackageArtifactInputKind::MachineFunction,
        function_index,
        slot.products->machine_functions[function_index]});
  }
  for (std::size_t assembly_index = 0;
       assembly_index < slot.package->assembly_sources.size();
       ++assembly_index) {
    slot.layout.inputs.push_back({
        PackageArtifactInputKind::PackageAssembly,
        assembly_index,
        slot.products->package_assembly});
  }
  slot.layout.ok = true;
  outcome.kind = SemanticProductOutcomeKind::Complete;
  return true;
}

// Publishes all backend-native semantic products in two explicit waves. First,
// every concrete MIR procedure in every package becomes one independently
// compilable MachineFunction module. Second, one ArtifactLayout barrier per
// package fixes linker input order. The static-data units were already emitted
// by their earlier package products, so no package-wide LLVM operation remains.
[[nodiscard]] bool run_workspace_native_products(
    const SourceManager &sources,
    const TargetProfile &target,
    std::size_t worker_count,
    TimingRecorder *timings,
    CompileWorkspaceResult &result,
    DiagnosticSink &diagnostics) {
  std::vector<std::vector<SymbolId>> runtime_procedures(
      result.packages.size());
  std::vector<SemanticProductId> expected_machine_wave;
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    PackageSemanticProducts &products =
        result.semantic_products.packages[package_index];
    if (!products.machine_functions.empty() ||
        products.artifact_layout.is_valid() ||
        !package.llvm.machine_functions.empty() ||
        package.artifact_layout.ok) {
      diagnostics.error(
          SourceRange::invalid(),
          "native product slice is not empty before scheduling");
      return false;
    }
    if (!package.llvm.static_data.ok) {
      diagnostics.error(
          SourceRange::invalid(),
          "native products require complete static data and MIR products");
      return false;
    }
    for (SemanticProductId mir_product : products.mir_procedures) {
      if (!mir_product.is_valid() ||
          mir_product.value >=
              result.semantic_products.mir_procedure_by_product.size() ||
          !result.semantic_products
               .mir_procedure_by_product[mir_product.value]
               .has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "native products require one payload per MIR product");
        return false;
      }
      runtime_procedures[package_index].push_back(
          result.semantic_products
              .mir_procedure_by_product[mir_product.value]
              ->symbol);
    }
    package.llvm.machine_functions.resize(products.mir_procedures.size());
    const PackageId owner{static_cast<std::uint32_t>(package_index)};
    for (std::size_t function_index = 0;
         function_index < products.mir_procedures.size();
         ++function_index) {
      const std::array dependencies{
          products.mir_procedures[function_index],
          products.package_static_data};
      const SemanticProductId product = append_workspace_semantic_product(
          result,
          SemanticProductKind::MachineFunction,
          dependencies,
          owner,
          false,
          diagnostics);
      if (!product.is_valid()) return false;
      result.semantic_products.procedure_by_product[product.value] =
          result.semantic_products
              .mir_procedure_by_product[
                  products.mir_procedures[function_index].value]
              ->symbol;
      products.machine_functions.push_back(product);
      expected_machine_wave.push_back(product);
    }
  }

  if (!expected_machine_wave.empty()) {
    const SemanticReadyWave wave =
        freeze_semantic_ready_wave(result.semantic_graph);
    if (wave.status != SemanticReadyWaveStatus::Ready ||
        wave.products != expected_machine_wave) {
      diagnostics.error(
          SourceRange::invalid(),
          "machine functions did not form their exact workspace ready wave" +
              (wave.failure.empty() ? std::string{} : ": " + wave.failure));
      return false;
    }
    std::vector<MachineFunctionTaskSlot> slots(wave.products.size());
    std::vector<SemanticProductOutcome> outcomes(wave.products.size());
    for (std::size_t index = 0; index < wave.products.size(); ++index) {
      const SemanticProductId product = wave.products[index];
      const PackageId owner =
          result.semantic_products.package_by_product[product.value];
      if (!owner.is_valid() || owner.value >= result.packages.size() ||
          !result.packages[owner.value].has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "machine function has no retained package owner");
        return false;
      }
      CompiledPackage &package = *result.packages[owner.value];
      const PackageSemanticProducts &products =
          result.semantic_products.packages[owner.value];
      const auto position = std::find(
          products.machine_functions.begin(),
          products.machine_functions.end(),
          product);
      if (position == products.machine_functions.end()) {
        diagnostics.error(
            SourceRange::invalid(),
            "machine function is absent from its package index");
        return false;
      }
      const std::size_t function_index = static_cast<std::size_t>(
          position - products.machine_functions.begin());
      MachineFunctionTaskSlot &slot = slots[index];
      slot.sources = &sources;
      slot.target = &target;
      slot.semantic = &package.bodies.package;
      slot.abi = &package.c_abi;
      slot.runtime_procedures = &runtime_procedures[owner.value];
      slot.procedure = &*result.semantic_products
          .mir_procedure_by_product[
              products.mir_procedures[function_index].value];
      slot.options.package = result.graph.packages[owner.value].identity;
      slot.procedure_ordinal = function_index;
      slot.package_function_index = function_index;
    }
    WorkGraph execution_graph;
    execution_graph.tasks.resize(slots.size());
    MachineFunctionWaveExecution execution{&slots, &outcomes};
    const WorkGraphRunResult scheduled = run_work_graph(
        execution_graph,
        WorkGraphRunOptions{worker_count},
        execute_machine_function_task,
        &execution);
    if (timings != nullptr) {
      timings->add_counter("machine-function ready waves", 1);
      timings->add_counter("machine-function tasks", slots.size());
      timings->add_counter(
          "machine-function worker slots", scheduled.workers_used);
    }
    if (!scheduled.ok) {
      std::string failure = "machine-function worker scheduling failed";
      for (std::size_t index = 0; index < scheduled.tasks.size(); ++index) {
        if (scheduled.tasks[index].state != WorkTaskState::Failed) continue;
        failure += " at task " + std::to_string(index) + ": " +
            scheduled.tasks[index].failure;
        break;
      }
      diagnostics.error(SourceRange::invalid(), std::move(failure));
      return false;
    }
    bool machine_functions_ok = true;
    for (const SemanticProductOutcome &outcome : outcomes) {
      machine_functions_ok = machine_functions_ok &&
          outcome.kind == SemanticProductOutcomeKind::Complete;
    }
    std::string publication_error;
    if (!publish_semantic_ready_wave(
            result.semantic_graph,
            wave,
            outcomes,
            diagnostics,
            publication_error)) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot publish machine-function wave: " + publication_error);
      return false;
    }
    for (std::size_t index = 0; index < wave.products.size(); ++index) {
      const PackageId owner = result.semantic_products.package_by_product[
          wave.products[index].value];
      CompiledPackage &package = *result.packages[owner.value];
      package.llvm.machine_functions[slots[index].package_function_index] =
          std::move(slots[index].llvm);
      if (timings != nullptr) {
        timings->add_counter("LLVM machine function modules emitted", 1);
        timings->add_counter(
            "LLVM IR bytes",
            static_cast<std::uint64_t>(package.llvm.machine_functions[
                slots[index].package_function_index].text.size()));
      }
    }
    if (!machine_functions_ok) return false;
  }

  std::vector<SemanticProductId> expected_layout_wave;
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    PackageSemanticProducts &products =
        result.semantic_products.packages[package_index];
    std::vector<SemanticProductId> dependencies{
        products.package_static_data,
        products.package_assembly};
    dependencies.insert(
        dependencies.end(),
        products.machine_functions.begin(),
        products.machine_functions.end());
    const PackageId owner{static_cast<std::uint32_t>(package_index)};
    products.artifact_layout = append_workspace_semantic_product(
        result,
        SemanticProductKind::ArtifactLayout,
        dependencies,
        owner,
        false,
        diagnostics);
    if (!products.artifact_layout.is_valid()) return false;
    expected_layout_wave.push_back(products.artifact_layout);
  }
  if (expected_layout_wave.empty()) return true;

  const SemanticReadyWave layout_wave =
      freeze_semantic_ready_wave(result.semantic_graph);
  if (layout_wave.status != SemanticReadyWaveStatus::Ready ||
      layout_wave.products != expected_layout_wave) {
    diagnostics.error(
        SourceRange::invalid(),
        "artifact layouts did not form their exact workspace ready wave" +
            (layout_wave.failure.empty()
                 ? std::string{}
                 : ": " + layout_wave.failure));
    return false;
  }
  std::vector<ArtifactLayoutTaskSlot> layout_slots(
      layout_wave.products.size());
  std::vector<SemanticProductOutcome> layout_outcomes(
      layout_wave.products.size());
  for (std::size_t index = 0;
       index < layout_wave.products.size(); ++index) {
    const PackageId owner = result.semantic_products.package_by_product[
        layout_wave.products[index].value];
    if (!owner.is_valid() || owner.value >= result.packages.size() ||
        !result.packages[owner.value].has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "artifact layout has no retained package owner");
      return false;
    }
    layout_slots[index].package = &*result.packages[owner.value];
    layout_slots[index].products =
        &result.semantic_products.packages[owner.value];
  }
  WorkGraph layout_execution_graph;
  layout_execution_graph.tasks.resize(layout_slots.size());
  ArtifactLayoutWaveExecution layout_execution{
      &layout_slots, &layout_outcomes};
  const WorkGraphRunResult layout_scheduled = run_work_graph(
      layout_execution_graph,
      WorkGraphRunOptions{worker_count},
      execute_artifact_layout_task,
      &layout_execution);
  if (timings != nullptr) {
    timings->add_counter("artifact-layout ready waves", 1);
    timings->add_counter("artifact-layout tasks", layout_slots.size());
    timings->add_counter(
        "artifact-layout worker slots", layout_scheduled.workers_used);
  }
  if (!layout_scheduled.ok) {
    std::string failure = "artifact-layout worker scheduling failed";
    for (std::size_t index = 0;
         index < layout_scheduled.tasks.size(); ++index) {
      if (layout_scheduled.tasks[index].state != WorkTaskState::Failed) {
        continue;
      }
      failure += " at task " + std::to_string(index) + ": " +
          layout_scheduled.tasks[index].failure;
      break;
    }
    diagnostics.error(SourceRange::invalid(), std::move(failure));
    return false;
  }
  std::string layout_publication_error;
  if (!publish_semantic_ready_wave(
          result.semantic_graph,
          layout_wave,
          layout_outcomes,
          diagnostics,
          layout_publication_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish artifact-layout wave: " + layout_publication_error);
    return false;
  }
  for (std::size_t index = 0;
       index < layout_wave.products.size(); ++index) {
    const PackageId owner = result.semantic_products.package_by_product[
        layout_wave.products[index].value];
    CompiledPackage &package = *result.packages[owner.value];
    package.artifact_layout = std::move(layout_slots[index].layout);
    package.llvm.ok = package.artifact_layout.ok;
  }
  return true;
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
      if (!package.has_value())
        continue;
      every_ready_package_valid =
          every_ready_package_valid && is_valid_interface_surface(*package) &&
          package->metadata.ok && package->obligations.ok;
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
  if (!invalidate_package_closure(
          schedule, update.changed_packages, result, diagnostics)) {
    result.ok = false;
    return false;
  }

  bool every_ready_package_valid = true;
  for (const std::optional<CompiledPackage> &package : result.packages) {
    if (!package.has_value()) continue;
    every_ready_package_valid = every_ready_package_valid &&
                                is_valid_interface_surface(*package) &&
                                package->metadata.ok && package->obligations.ok;
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

  // Phase 2: close one workspace-wide dynamic graph of exact body products.
  // Fresh packages expose authored roots before any external seed is admitted.
  // That first selection is semantically important: a local instantiation keeps
  // its Discovered origin even if another package later asks for the same body,
  // so removing the external request cannot deactivate locally required code.
  //
  // Every iteration runs one ready set across all packages, recomputes the
  // current selected-program closure, exports its cross-package calls, and
  // materializes only previously unseen owner products. Completed products are
  // immutable. Removing a request changes selection, while adding an equal
  // request reselects the retained HIR without rechecking it. The loop ends only
  // when no product is pending and demand plus selection have reached a fixed
  // point. Package import cycles are already rejected, so cross-package demand
  // expansion cannot form an uncollapsed recursive scheduler cycle.
  const std::size_t package_count = result.graph.packages.size();
  std::vector<std::vector<std::size_t>> previous_selected(package_count);
  std::vector<std::vector<std::size_t>> previous_external(package_count);
  std::vector<std::size_t> previous_checked(package_count, 0);
  std::vector<bool> body_changed(package_count, false);
  std::vector<std::vector<ProcedureDemandDiscovery>> current_discoveries(
      package_count);
  TimingScope body_timing = options.timings != nullptr
      ? options.timings->scope("body semantics")
      : TimingScope{};

  for (std::size_t package_index = 0; package_index < package_count;
       ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    previous_selected[package_index] = package.selected_procedure_work;
    previous_external[package_index] =
        package.selected_external_procedure_work;
    previous_checked[package_index] = package.bodies.checked_procedures;
    const bool fresh_bodies = !package.bodies.ok;

    if (fresh_bodies) {
      package.external_procedure_products.clear();
      package.bodies = begin_package_body_work(
          sources,
          workspace_package.loaded,
          package.declarations.selections,
          package.declarations.package,
          package.declarations.constants,
          options.target.facts,
          diagnostics);
      body_changed[package_index] = true;
      if (options.timings != nullptr) {
        options.timings->add_counter("package body starts", 1);
      }
    }

    // Reset command-current selection to authored roots before looking at any
    // retained external product. Old selection belongs to the preceding source
    // graph and must not keep a now-unreachable transitive demand alive.
    const std::vector<ProcedureInstantiationDemand> no_demands;
    const std::size_t selection_errors = diagnostics.error_count();
    SelectedPackageBodyWork authored =
        select_package_body_work(package, no_demands, diagnostics);
    if (diagnostics.error_count() != selection_errors) {
      package.bodies.ok = false;
      continue;
    }
    package.selected_procedure_work = std::move(authored.procedures);
    package.selected_external_procedure_work.clear();
  }

  for (;;) {
    if (!run_workspace_body_ready_wave(
            sources,
            options.target.facts,
            options.semantic_worker_count,
            options.timings,
            result,
            diagnostics)) {
      return false;
    }

    // Publication can add locally discovered children. Expand selection through
    // those prerequisite edges before reading outbound requests from HIR.
    for (std::size_t package_index = 0; package_index < package_count;
         ++package_index) {
      if (!result.packages[package_index].has_value()) continue;
      CompiledPackage &package = *result.packages[package_index];
      std::vector<ProcedureInstantiationDemand> demands;
      demands.reserve(current_discoveries[package_index].size());
      for (const ProcedureDemandDiscovery &discovery :
           current_discoveries[package_index]) {
        demands.push_back(discovery.demand);
      }
      const std::size_t selection_errors = diagnostics.error_count();
      SelectedPackageBodyWork selected =
          select_package_body_work(package, demands, diagnostics);
      if (diagnostics.error_count() != selection_errors) {
        package.bodies.ok = false;
        continue;
      }
      package.selected_procedure_work = std::move(selected.procedures);
      package.selected_external_procedure_work =
          std::move(selected.external_roots);
    }

    std::vector<std::vector<ProcedureDemandDiscovery>> next_discoveries(
        package_count);
    for (std::size_t consumer_index = 0; consumer_index < package_count;
         ++consumer_index) {
      if (!result.packages[consumer_index].has_value() ||
          !result.packages[consumer_index]->bodies.ok) {
        continue;
      }
      const std::vector<SelectedImportedProcedureRequest> requests =
          selected_imported_procedure_requests(
              *result.packages[consumer_index]);
      for (const SelectedImportedProcedureRequest &request : requests) {
        const std::size_t export_errors = diagnostics.error_count();
        ProcedureDemandDiscovery discovery;
        const std::optional<std::size_t> owner_index = export_procedure_demand(
            consumer_index,
            request,
            schedule,
            result,
            discovery,
            diagnostics);
        if (diagnostics.error_count() != export_errors) {
          result.packages[consumer_index]->bodies.ok = false;
          continue;
        }
        if (!owner_index.has_value()) {
          continue;
        }
        next_discoveries[*owner_index].push_back(std::move(discovery));
      }
    }

    bool demand_changed = false;
    for (std::size_t package_index = 0; package_index < package_count;
         ++package_index) {
      if (!result.packages[package_index].has_value()) continue;
      CompiledPackage &package = *result.packages[package_index];
      if (!canonicalize_procedure_discoveries(
              next_discoveries[package_index], diagnostics)) {
        package.bodies.ok = false;
        continue;
      }
      demand_changed = demand_changed || !same_procedure_discovery_set(
          current_discoveries[package_index],
          next_discoveries[package_index]);

      std::vector<ProcedureDemandDiscovery> missing;
      for (const ProcedureDemandDiscovery &discovery :
           next_discoveries[package_index]) {
        const auto retained = std::find_if(
            package.external_procedure_products.begin(),
            package.external_procedure_products.end(),
            [&](const ExternalProcedureBodyProduct &candidate) {
              return same_external_procedure_demand(
                  candidate.demand, discovery.demand);
            });
        if (retained == package.external_procedure_products.end()) {
          missing.push_back(discovery);
        }
      }
      if (!package.bodies.ok || missing.empty()) continue;

      std::vector<ProcedureInstantiationDemand> missing_demands;
      missing_demands.reserve(missing.size());
      for (const ProcedureDemandDiscovery &discovery : missing) {
        missing_demands.push_back(discovery.demand);
      }
      const std::vector<ProcedureInstantiationSeed> seeds =
          materialize_procedure_demands(
              missing_demands, package.bodies.package, diagnostics);
      if (!append_package_body_seeds(
              sources,
              result.graph.packages[package_index].loaded,
              package.declarations.selections,
              package.bodies,
              options.target.facts,
              diagnostics,
              seeds)) {
        package.bodies.ok = false;
      }
      std::size_t materialized = 0;
      for (ProcedureDemandDiscovery &discovery : missing) {
        const std::optional<std::size_t> work_index =
            external_body_work_index(package.bodies, discovery.demand);
        if (!work_index.has_value()) {
          diagnostics.error(
              SourceRange::invalid(),
              "external procedure demand did not materialize a body product");
          package.bodies.ok = false;
          continue;
        }
        package.external_procedure_products.push_back(
            {std::move(discovery.demand),
             *work_index,
             discovery.requester});
        ++materialized;
      }
      if (materialized != 0) {
        body_changed[package_index] = true;
        if (options.timings != nullptr) {
          options.timings->add_counter(
              "external procedure bodies materialized", materialized);
        }
      }
    }

    bool selection_changed = false;
    current_discoveries = std::move(next_discoveries);
    for (std::size_t package_index = 0; package_index < package_count;
         ++package_index) {
      if (!result.packages[package_index].has_value()) continue;
      CompiledPackage &package = *result.packages[package_index];
      std::vector<ProcedureInstantiationDemand> demands;
      demands.reserve(current_discoveries[package_index].size());
      for (const ProcedureDemandDiscovery &discovery :
           current_discoveries[package_index]) {
        demands.push_back(discovery.demand);
      }
      const std::vector<std::size_t> before_procedures =
          package.selected_procedure_work;
      const std::vector<std::size_t> before_external =
          package.selected_external_procedure_work;
      const std::size_t selection_errors = diagnostics.error_count();
      SelectedPackageBodyWork selected =
          select_package_body_work(package, demands, diagnostics);
      if (diagnostics.error_count() != selection_errors) {
        package.bodies.ok = false;
        continue;
      }
      package.selected_procedure_work = std::move(selected.procedures);
      package.selected_external_procedure_work =
          std::move(selected.external_roots);
      selection_changed = selection_changed ||
          before_procedures != package.selected_procedure_work ||
          before_external != package.selected_external_procedure_work;
    }

    bool has_pending_body = false;
    for (const std::optional<CompiledPackage> &package : result.packages) {
      has_pending_body = has_pending_body ||
          (package.has_value() &&
           package->bodies.next_work < package->bodies.work.size());
    }
    if (!has_pending_body && !demand_changed && !selection_changed) break;

    // A wave which produced no pending child and did not affect demand or
    // selection has already been fully observed above. Conversely, every other
    // continuation has a concrete monotone product append or a shrinking/
    // growing current-program set; no arbitrary iteration cap is required.
  }

  std::vector<PackageId> changed_packages;
  for (std::size_t package_index = 0; package_index < package_count;
       ++package_index) {
    if (!result.packages[package_index].has_value()) continue;
    CompiledPackage &package = *result.packages[package_index];
    if (!package.bodies.finalized) {
      (void)finalize_package_body_work(
          options.target.facts,
          package.bodies,
          diagnostics);
    }
    if (options.timings != nullptr &&
        package.bodies.checked_procedures > previous_checked[package_index]) {
      options.timings->add_counter(
          "procedure bodies checked",
          static_cast<std::uint64_t>(
              package.bodies.checked_procedures -
              previous_checked[package_index]));
    }
    if (!package.bodies.ok) continue;
    if (!publish_selected_body_products(
            package_index, result, diagnostics)) {
      package.bodies.ok = false;
      continue;
    }

    const bool current_program_changed = body_changed[package_index] ||
        package.selected_procedure_work != previous_selected[package_index] ||
        package.selected_external_procedure_work !=
            previous_external[package_index];
    if (current_program_changed) {
      changed_packages.push_back(
          PackageId{static_cast<std::uint32_t>(package_index)});
    }
  }
  if (!changed_packages.empty()) {
    if (!invalidate_package_closure(
            schedule, changed_packages, result, diagnostics)) {
      return false;
    }
  }
  body_timing.finish();

  TimingScope abi_timing = options.timings != nullptr
      ? options.timings->scope("ABI classification")
      : TimingScope{};
  if (!run_workspace_abi_classifications(
          options.target.facts,
          options.semantic_worker_count,
          options.timings,
          result,
          diagnostics)) {
    return false;
  }
  abi_timing.finish();

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
    if (package_semantic_closure_is_current(result, package_index)) {
      continue;
    }
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    const std::vector<SemanticSite> selected_sites =
        selected_package_semantic_sites(package);
    package.metadata = collect_agent_metadata(
        sources,
        workspace_package.loaded,
        package.bodies.package,
        selected_sites,
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
              typed_package.bodies.procedures,
              typed_package.selected_procedure_work,
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
    if (package_semantic_closure_is_current(result, package_index)) {
      continue;
    }
    TimingScope package_timing = time_package_phase(
        options.timings, "package closure: ", package.identity);
    const std::vector<SymbolId> active_imported_instances =
        selected_imported_instance_proxies(package);
    const std::vector<SymbolId> active_external_instances =
        selected_external_instance_symbols(package);

    package.imported_contracts = build_imported_procedure_contracts(
        package.bodies.package,
        result,
        schedule,
        active_imported_instances,
        diagnostics);
    package.obligations = build_selected_agent_obligations(
        workspace_package.identity,
        sources,
        workspace_package.loaded,
        package.bodies.package,
        package.bodies.constants,
        package.metadata,
        package.imported_contracts,
        options.target,
        diagnostics,
        package.validation_context,
        package.bodies.procedures,
        package.selected_procedure_work);
    if (!run_package_direct_effect_products(
            package_index,
            schedule,
            options.target,
            options.foreign_provider_audits,
            options.semantic_worker_count,
            options.timings,
            result,
            diagnostics)) {
      result.ok = false;
      return false;
    }
    package.effects = close_procedure_effects(
        package.bodies.package,
        package.bodies.procedures,
        package.selected_procedure_work,
        package.direct_effects,
        package.imported_contracts,
        &options.target,
        options.foreign_provider_audits);
    if (!publish_package_effect_scc_products(
            package_index,
            schedule,
            options.timings,
            result,
            diagnostics)) {
      result.ok = false;
      return false;
    }
    package.native_interop = validate_native_interop(
        package.bodies.package,
        package.bodies.procedures,
        package.selected_procedure_work,
        package.c_abi,
        options.target.facts,
        diagnostics);
    bool denials_ok = true;
    if (!run_package_denial_products(
            sources,
            package_index,
            options.semantic_worker_count,
            options.timings,
            result,
            diagnostics,
            denials_ok)) {
      result.ok = false;
      return false;
    }
    package.interface = build_package_interface(
        workspace_package.identity,
        package.bodies.package,
        package.bodies.constants,
        package.metadata,
        package.effects,
        active_external_instances,
        diagnostics);
    if (!package.metadata.ok || !package.obligations.ok || !denials_ok ||
        !package.native_interop.ok) {
      continue;
    }
  }
  closure_timing.finish();

  bool every_package_closed = true;
  for (std::size_t package_index = 0;
       package_index < result.packages.size(); ++package_index) {
    every_package_closed = every_package_closed &&
        package_semantic_closure_is_current(result, package_index);
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
          package.bodies.procedures,
          package.selected_procedure_work,
          diagnostics);
      compiled.validation_entries.insert(
          compiled.validation_entries.end(),
          std::make_move_iterator(discovered.begin()),
          std::make_move_iterator(discovered.end()));
    }
    sort_validation_entries(compiled.validation_entries);
  }

  // Native-bound package facts and executable procedures now continue the
  // same semantic graph. Parsed assembly is one package payload assembled from
  // procedure-owned HIR arenas; MIR itself is one immutable task per concrete
  // runtime body across the complete workspace. No package HIR projection or
  // semantic-table mutation participates in lowering.
  if (options.lower_mir || options.emit_llvm) {
    if (!run_workspace_lowering_barriers(
            sources,
            options.target,
            options.emit_llvm,
            options.emit_program_entry,
            options.validation_kind,
            compiled.validation_entries,
            options.semantic_worker_count,
            options.timings,
            compiled,
            diagnostics) ||
        !run_workspace_mir_products(
            options.configuration.runtime_assertions,
            options.semantic_worker_count,
            options.timings,
            compiled,
            diagnostics)) {
      compiled.ok = false;
      return false;
    }
    if (options.emit_llvm) {
      if (!run_workspace_native_products(
              sources,
              options.target,
              options.semantic_worker_count,
              options.timings,
              compiled,
              diagnostics)) {
        compiled.ok = false;
        return false;
      }
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
