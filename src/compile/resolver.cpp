// Direct sequential resolver implementation over typed surface obligations.
//
// State is represented as three plain vectors: surface package borrows, the new
// pin map, and unique expansion objects. A site is processed in deterministic
// package/obligation order. Parallel providers may be added only as a scheduling
// optimization after dependency-ready sets are explicit.

#include "compile/resolver.h"

#include "elaborator/generated_source.h"
#include "elaborator/resolved_program.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

// Keeps evidence-only obligations out of the source-generation transaction.
[[nodiscard]] bool is_synthesis(AgentConstructKind kind) {
  return kind == AgentConstructKind::SynthesisDeclaration ||
      kind == AgentConstructKind::SynthesisMember ||
      kind == AgentConstructKind::SynthesisStatement ||
      kind == AgentConstructKind::SynthesisExpression ||
      kind == AgentConstructKind::SynthesisAssembly;
}

// Manifest parsing guarantees uniqueness, so a linear lookup has one result.
// Site counts are small compared with syntax/type work in this bootstrap; a
// sorted merge can replace this without changing semantics if measurement asks.
[[nodiscard]] const ResolutionPin *find_pin(
    const ResolutionManifestLoadResult &loaded,
    std::string_view site_identity) {
  if (loaded.state != ResolutionManifestLoadState::Loaded) return nullptr;
  for (const ResolutionPin &pin : loaded.manifest.pins) {
    if (pin.site_identity == site_identity) return &pin;
  }
  return nullptr;
}

// Joins the persistent obligation view back to its process-local prompt and
// attachment bytes through the syntax route retained by both rows.
[[nodiscard]] const AgentRecord *find_record(
    const CompiledPackage &package,
    SyntaxReference syntax) {
  for (const AgentRecord &record : package.metadata.records) {
    if (record.syntax == syntax) return &record;
  }
  return nullptr;
}

// Resolves the process-local syntax route while its surface package is alive.
// Only the returned byte offsets enter the persistent map; FileId and NodeId
// remain compiler-invocation details.
[[nodiscard]] SourceRange obligation_range(
    const LoadedPackage &loaded,
    const AgentObligation &obligation) {
  for (const LoadedPackageFile &file : loaded.files) {
    if (file.source != obligation.syntax.file || !file.syntax.has_value() ||
        !obligation.syntax.node.is_valid()) {
      continue;
    }
    return file.syntax->node(obligation.syntax.node).range;
  }
  return SourceRange::invalid();
}

// Keeps one copy of shared content-addressed bytes. Two sites may legitimately
// select identical source; conflicting bytes under one digest are diagnosed as
// an impossible hash/store input rather than resolved by vector order.
[[nodiscard]] bool add_expansion(
    std::vector<GeneratedExpansion> &expansions,
    GeneratedExpansion expansion,
    DiagnosticSink &diagnostics) {
  for (const GeneratedExpansion &existing : expansions) {
    if (existing.digest != expansion.digest) continue;
    if (existing.source != expansion.source) {
      diagnostics.error(
          SourceRange::invalid(),
          "two generated expansions conflict under one content identity");
      return false;
    }
    return true;
  }
  expansions.push_back(std::move(expansion));
  return true;
}

// Copies provider-visible context into an owned request. Attachment order is
// already canonical from metadata collection and is preserved exactly.
[[nodiscard]] bool build_request(
    const AgentObligation &obligation,
    const AgentRecord &record,
    SynthesisRequest &request,
    DiagnosticSink &diagnostics) {
  if (record.files.size() != record.file_contents.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "synthesis record attachment identities and bytes are inconsistent");
    return false;
  }
  request.obligation = obligation;
  // SyntaxReference is retained on compiler-owned obligations solely for
  // diagnostics and overlays. A provider receives persistent identities only.
  request.obligation.syntax = {};
  request.prompt = record.text;
  for (std::size_t index = 0; index < record.files.size(); ++index) {
    const AttachedFile &file = record.files[index];
    request.attachments.push_back({
        file.relative_path,
        file.size,
        file.digest,
        record.file_contents[index],
    });
  }
  return true;
}

// Provider identities are checked at invocation time so a resolver may reuse
// all fresh pins successfully with no provider installed.
[[nodiscard]] bool provider_is_configured(
    const SynthesisProvider &provider,
    DiagnosticSink &diagnostics) {
  if (provider.synthesize == nullptr) {
    diagnostics.error(
        SourceRange::invalid(), "synthesis provider is not configured");
    return false;
  }
  if (provider.provider_identity.empty() || provider.model_identity.empty() ||
      provider.configuration_identity.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "synthesis provider identities must not be empty");
    return false;
  }
  return true;
}

[[nodiscard]] bool resolution_cancelled(
    const ResolveWorkspaceOptions &options,
    DiagnosticSink &diagnostics) {
  if (options.cancellation_requested == nullptr ||
      !options.cancellation_requested(options.cancellation_state)) {
    return false;
  }
  diagnostics.error(SourceRange::invalid(), "resolution cancelled");
  return true;
}

// One elaboration stage owns exactly the synthesis sites visible in its input
// compilation. Interface discovery contains declaration/member sites; the body
// compilation after those edits contains statement/expression/assembly sites.
// The stage manifest is intentionally separate because the overlay builder
// requires no unrelated pins.
struct ResolvedStage {
  bool ok = false;
  ResolutionManifest manifest;
  std::vector<WorkspaceSourceOverride> overrides;
};

// Resolves every synthesis obligation in one already checked stage and builds
// complete-file source overrides against that stage's exact source buffers.
// All obligations are computed before any proposal is installed, so sites in
// one interface completeness set cannot observe another site's generated names.
[[nodiscard]] ResolvedStage resolve_stage(
    SourceManager &sources,
    const CompileWorkspaceResult &surface,
    const ResolutionManifestLoadResult &loaded,
    const ResolveWorkspaceOptions &options,
    ResolveWorkspaceResult &result,
    std::vector<GeneratedExpansion> &expansions,
    DiagnosticSink &diagnostics) {
  ResolvedStage stage;
  stage.manifest.target_identity = options.compile.target.facts.identity;
  std::vector<ResolutionSurfacePackage> surface_packages;

  // Process sites in deterministic package and obligation order. Fresh pins
  // reuse verified store bytes; stale or missing pins call the provider unless
  // this is the provider-free revalidation mode.
  for (std::size_t package_index = 0;
       package_index < surface.packages.size(); ++package_index) {
    if (!surface.packages[package_index].has_value()) continue;
    const CompiledPackage &package = *surface.packages[package_index];
    surface_packages.push_back({
        &package.identity,
        &surface.graph.packages[package_index].loaded,
        &package.obligations,
    });
    for (const AgentObligation &obligation :
         package.obligations.obligations) {
      if (!is_synthesis(obligation.kind)) continue;
      if (resolution_cancelled(options, diagnostics)) return stage;
      const ResolutionPin *existing = find_pin(loaded, obligation.site_identity);
      // Provider-free operation intentionally accepts a content-fresh pin: it
      // is the offline path. Once a caller explicitly configures a provider,
      // that selection is semantic input and must match the pin exactly.
      const bool provider_matches = options.provider.synthesize == nullptr ||
          (existing != nullptr &&
           existing->provider_identity == options.provider.provider_identity &&
           existing->model_identity == options.provider.model_identity &&
           existing->configuration_identity ==
               options.provider.configuration_identity);
      const bool fresh = existing != nullptr &&
          existing->kind == obligation.kind &&
          existing->input_digest == obligation.input_digest &&
          provider_matches;

      GeneratedExpansion expansion;
      ResolutionPin pin;
      pin.site_identity = obligation.site_identity;
      pin.kind = obligation.kind;
      pin.input_digest = obligation.input_digest;
      if (fresh || (options.revalidate && existing != nullptr &&
                    existing->kind == obligation.kind)) {
        if (!load_generated_expansion(
                options.compile.workspace.workspace_directory,
                existing->expansion_digest,
                expansion.source,
                diagnostics)) {
          return stage;
        }
        expansion.digest = existing->expansion_digest;
        pin.expansion_digest = existing->expansion_digest;
        pin.provider_identity = existing->provider_identity;
        pin.model_identity = existing->model_identity;
        pin.configuration_identity = existing->configuration_identity;
        ++result.reused_sites;
      } else {
        if (options.revalidate) {
          diagnostics.error(
              SourceRange::invalid(),
              "revalidation cannot fill a missing synthesis pin");
          return stage;
        }
        if (!provider_is_configured(options.provider, diagnostics)) return stage;
        const AgentRecord *record = find_record(package, obligation.syntax);
        if (record == nullptr) {
          diagnostics.error(
              SourceRange::invalid(),
              "synthesis obligation has no provider metadata record");
          return stage;
        }
        SynthesisRequest request;
        if (!build_request(obligation, *record, request, diagnostics)) return stage;
        SynthesisResponse response;
        const std::size_t before_provider = diagnostics.error_count();
        if (!options.provider.synthesize(
                options.provider.state,
                request,
                response,
                diagnostics)) {
          if (diagnostics.error_count() == before_provider) {
            diagnostics.error(
                SourceRange::invalid(),
                "synthesis provider failed without a diagnostic");
          }
          return stage;
        }
        expansion.source = std::move(response.source);
        expansion.digest = sha256(expansion.source);
        pin.expansion_digest = expansion.digest;
        pin.provider_identity = options.provider.provider_identity;
        pin.model_identity = options.provider.model_identity;
        pin.configuration_identity = options.provider.configuration_identity;
        ++result.synthesized_sites;
      }

      const SourceRange surface_range = obligation_range(
          surface.graph.packages[package_index].loaded, obligation);
      if (!surface_range.is_valid()) {
        diagnostics.error(
            SourceRange::invalid(),
            "synthesis obligation has no source range for its persistent map");
        return stage;
      }
      pin.source_map.root_identity = obligation.root_identity;
      pin.source_map.root_relative_path = obligation.root_relative_path;
      pin.source_map.source_relative_path = obligation.source_relative_path;
      pin.source_map.surface_begin = surface_range.begin.offset;
      pin.source_map.surface_end = surface_range.end.offset;
      pin.source_map.expansion_bytes =
          static_cast<std::uint64_t>(expansion.source.size());

      // This check runs for reused bytes as well as new proposals. It prevents
      // an older or externally supplied store from smuggling provider work into
      // the next stage before the complete resolved-program check can run.
      const std::string display_name =
          "<generated/" + obligation.site_identity + ">";
      if (!validate_generated_source_boundary(
              sources,
              display_name,
              expansion.source,
              diagnostics)) {
        return stage;
      }
      if (!add_expansion(expansions, std::move(expansion), diagnostics)) {
        return stage;
      }
      stage.manifest.pins.push_back(std::move(pin));
    }
  }

  const ResolutionOverlayResult overlays = build_resolution_overlays(
      sources,
      surface_packages,
      stage.manifest,
      options.compile.target.facts.identity,
      options.compile.workspace.workspace_directory,
      expansions,
      diagnostics);
  if (!overlays.ok) return stage;
  stage.overrides = overlays.sources;
  stage.ok = true;
  return stage;
}

// Later-stage overrides contain complete files based on the already overlaid
// interface source. They replace an earlier row for the same semantic package
// and filename; unrelated early files remain present. Vector order stays the
// deterministic package/file discovery order established by the first stage.
void merge_overrides(
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

// Appends one stage's disjoint pin set into the coherent transaction manifest.
// A duplicate means site association changed between stages, which must be
// diagnosed instead of being resolved by stage order.
[[nodiscard]] bool append_stage_pins(
    ResolutionManifest &combined,
    ResolutionManifest stage,
    DiagnosticSink &diagnostics) {
  for (ResolutionPin &pin : stage.pins) {
    for (const ResolutionPin &existing : combined.pins) {
      if (existing.site_identity == pin.site_identity) {
        diagnostics.error(
            SourceRange::invalid(),
            "synthesis site appeared in more than one elaboration stage");
        return false;
      }
    }
    combined.pins.push_back(std::move(pin));
  }
  return true;
}

} // namespace

ResolveWorkspaceResult resolve_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    ResolveWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  ResolveWorkspaceResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  if (resolution_cancelled(options, diagnostics)) return result;

  const ResolutionManifestLoadResult loaded = load_resolution_manifest(
      options.compile.workspace.workspace_directory, diagnostics);
  if (loaded.state == ResolutionManifestLoadState::Invalid) return result;

  std::vector<GeneratedExpansion> expansions;
  ResolutionManifest manifest;
  manifest.target_identity = options.compile.target.facts.identity;
  if (options.external_inputs_configured) {
    manifest.external_inputs = std::move(options.external_inputs);
  } else if (loaded.state == ResolutionManifestLoadState::Loaded) {
    manifest.external_inputs = loaded.manifest.external_inputs;
  }

  // Validate caller-produced rows before compiling or invoking a provider.
  // The strict manifest parser is the single schema authority; round-tripping
  // here also canonicalizes row order for the returned in-memory manifest.
  ResolutionManifest external_input_check;
  DiagnosticSink external_input_diagnostics;
  if (!parse_resolution_manifest(
          serialize_resolution_manifest(manifest),
          external_input_check,
          external_input_diagnostics)) {
    for (const Diagnostic &diagnostic :
         external_input_diagnostics.diagnostics()) {
      diagnostics.report(
          diagnostic.severity, diagnostic.range, diagnostic.message);
    }
    return result;
  }
  manifest.external_inputs = std::move(external_input_check.external_inputs);
  std::vector<WorkspaceSourceOverride> interface_overrides;

  // Interface synthesis advances in dependency-ready rounds. Every package in
  // one round sees completed prerequisite package interfaces but none of its
  // own round's proposals. Merging a nonempty round removes at least one
  // provider site, and generated-source validation forbids adding another, so
  // the finite source graph guarantees termination without an iteration cap.
  while (true) {
    if (resolution_cancelled(options, diagnostics)) return result;
    CompileWorkspaceOptions interface_options = options.compile;
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
    if (!interface_surface.ok) return result;

    ResolvedStage interface_stage = resolve_stage(
        sources,
        interface_surface,
        loaded,
        options,
        result,
        expansions,
        diagnostics);
    if (!interface_stage.ok) return result;
    const bool made_progress = !interface_stage.manifest.pins.empty();
    if (!append_stage_pins(
            manifest, std::move(interface_stage.manifest), diagnostics)) {
      return result;
    }
    merge_overrides(
        interface_overrides, std::move(interface_stage.overrides));
    if (made_progress) continue;

    bool all_packages_ready = true;
    for (const std::optional<CompiledPackage> &package :
         interface_surface.packages) {
      all_packages_ready = all_packages_ready && package.has_value();
    }
    if (!all_packages_ready) {
      diagnostics.error(
          SourceRange::invalid(),
          "interface elaboration has suspended packages but no ready synthesis site");
      return result;
    }
    break;
  }

  // Stage 2 type-checks bodies only after all interface edits are installed.
  // Its obligations therefore include exact expected expression types and
  // visible locals together with the generated interface dependencies.
  if (resolution_cancelled(options, diagnostics)) return result;
  CompileWorkspaceOptions body_options = options.compile;
  body_options.stage = CompileWorkspaceStage::Complete;
  body_options.lower_mir = false;
  body_options.emit_llvm = false;
  body_options.workspace.source_overrides = interface_overrides;
  CompileWorkspaceResult body_surface = compile_workspace(
      sources,
      root_package_directory,
      body_options,
      diagnostics);
  if (!body_surface.ok) return result;

  ResolvedStage body_stage = resolve_stage(
      sources,
      body_surface,
      loaded,
      options,
      result,
      expansions,
      diagnostics);
  if (!body_stage.ok ||
      !append_stage_pins(
          manifest, std::move(body_stage.manifest), diagnostics)) {
    return result;
  }

  // With no sites and no prior manifest there is no transaction to perform.
  // An existing manifest still proceeds so obsolete pins become an empty map.
  if (manifest.pins.empty() && manifest.external_inputs.empty() &&
      loaded.state == ResolutionManifestLoadState::Missing &&
      options.judgment_runner.run == nullptr) {
    result.ok = diagnostics.error_count() == initial_errors;
    return result;
  }

  std::vector<WorkspaceSourceOverride> complete_overrides =
      std::move(interface_overrides);
  merge_overrides(complete_overrides, std::move(body_stage.overrides));
  options.compile.stage = CompileWorkspaceStage::Complete;
  options.compile.lower_mir = false;
  options.compile.emit_llvm = false;
  options.compile.workspace.source_overrides = std::move(complete_overrides);
  if (resolution_cancelled(options, diagnostics)) return result;
  CompileWorkspaceResult resolved = compile_workspace(
      sources, root_package_directory, options.compile, diagnostics);
  if (!resolved.ok || !validate_resolved_agent_boundaries(
          body_surface, resolved, diagnostics)) {
    return result;
  }

  manifest.resolved_program_digest = hash_resolved_program(
      sources,
      resolved.graph,
      options.compile.target,
      manifest,
      options.compile.compiler_content_identity);

  // Existing judgment evidence remains meaningful only when this exact
  // transaction reconstructed the same resolved program. Native validation
  // rows are regenerated below; judgment rows may be expensive provider work,
  // so an unchanged program preserves them unless a selected judgment runner
  // explicitly replaces some or all rows.
  if (loaded.state == ResolutionManifestLoadState::Loaded &&
      loaded.manifest.resolved_program_digest ==
          manifest.resolved_program_digest) {
    for (const ResolutionEvidencePin &pin : loaded.manifest.evidence) {
      if (pin.kind == "judgment") manifest.evidence.push_back(pin);
    }
  }

  // Validation-only files are deliberately absent from the ordinary surface
  // graph above. Select and compile each first-release suite only after the
  // complete candidate exists, using the same accepted in-memory overrides.
  // Evidence is written before resolution.json so failed attempts still revoke
  // their exact key; only passing evidence reaches the final manifest rename.
  constexpr std::array validation_kinds{
      ValidationKind::Test,
      ValidationKind::Benchmark,
  };
  for (ValidationKind validation_kind : validation_kinds) {
    if (resolution_cancelled(options, diagnostics)) return result;
    CompileWorkspaceOptions validation_options = options.compile;
    validation_options.validation_kind = validation_kind;
    validation_options.lower_mir = true;
    validation_options.emit_llvm = true;
    validation_options.emit_program_entry = true;
    CompileWorkspaceResult validation = compile_workspace(
        sources,
        root_package_directory,
        std::move(validation_options),
        diagnostics);
    if (!validation.ok) return result;
    validation.resolution_manifest = manifest;
    validation.resolved_program_digest = hash_resolved_program(
        sources,
        validation.graph,
        options.compile.target,
        manifest,
        options.compile.compiler_content_identity);
    if (validation_kind == ValidationKind::Test) {
      result.tested_procedures = validation.validation_entries.size();
    } else {
      result.benchmarked_procedures = validation.validation_entries.size();
    }
    if (validation.validation_entries.empty()) continue;
    if (options.validation_runner.run == nullptr) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolution candidate contains " +
              std::string(validation_kind_name(validation_kind)) +
              " procedures but no precommit validation runner is configured");
      return result;
    }
    const std::size_t before_validation = diagnostics.error_count();
    ResolutionValidationEvidence evidence;
    if (!options.validation_runner.run(
            options.validation_runner.state,
            options.compile.target,
            validation_kind,
            validation,
            evidence,
            diagnostics)) {
      if (diagnostics.error_count() == before_validation) {
        diagnostics.error(
            SourceRange::invalid(),
            "resolution candidate " +
                std::string(validation_kind_name(validation_kind)) +
                " validation failed without a diagnostic");
      }
      return result;
    }
    if (resolution_cancelled(options, diagnostics)) return result;
    if (!evidence.recorded) {
      diagnostics.error(
          SourceRange::invalid(),
          "precommit validation passed without persistent evidence");
      return result;
    }
    const WorkspacePackage &root =
        validation.graph.package(validation.graph.root_package);
    manifest.evidence.push_back({
        std::string(validation_kind_name(validation_kind)),
        root.identity.root_identity,
        root.identity.root_relative_path,
        evidence.key,
        evidence.content_digest,
    });
  }

  if (options.judgment_runner.run != nullptr) {
    if (resolution_cancelled(options, diagnostics)) return result;
    resolved.resolution_manifest = manifest;
    resolved.resolved_program_digest = manifest.resolved_program_digest;
    std::vector<ResolutionEvidencePin> judgment_evidence;
    const std::size_t before_judgment = diagnostics.error_count();
    if (!options.judgment_runner.run(
            options.judgment_runner.state,
            options.compile.target,
            resolved,
            judgment_evidence,
            result.judged_sites,
            diagnostics)) {
      if (diagnostics.error_count() == before_judgment) {
        diagnostics.error(
            SourceRange::invalid(),
            "resolution candidate judgment failed without a diagnostic");
      }
      return result;
    }
    for (const ResolutionEvidencePin &pin : judgment_evidence) {
      if (pin.kind != "judgment") {
        diagnostics.error(
            SourceRange::invalid(),
            "resolution judgment runner returned non-judgment evidence");
        return result;
      }
    }
    std::vector<ResolutionEvidencePin> retained;
    for (const ResolutionEvidencePin &pin : manifest.evidence) {
      if (pin.kind != "judgment") retained.push_back(pin);
    }
    retained.insert(
        retained.end(), judgment_evidence.begin(), judgment_evidence.end());
    manifest.evidence = std::move(retained);
  }

  // A requested judgment profile over a handwritten program with no judgment
  // sites is still a true no-op. Do not create resolution.json merely because
  // the caller asked the empty selector set to run.
  if (manifest.pins.empty() && manifest.external_inputs.empty() &&
      manifest.evidence.empty() &&
      loaded.state == ResolutionManifestLoadState::Missing) {
    result.ok = diagnostics.error_count() == initial_errors;
    return result;
  }
  // This is the final cancellation boundary. Once commit_resolution starts it
  // performs one crash-safe object-before-manifest transaction and must not be
  // interrupted by a cooperative flag halfway through its atomic publication.
  if (resolution_cancelled(options, diagnostics)) return result;
  if (!commit_resolution(
          options.compile.workspace.workspace_directory,
          manifest,
          expansions,
          diagnostics)) {
    return result;
  }
  result.manifest = std::move(manifest);
  result.committed = true;
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
