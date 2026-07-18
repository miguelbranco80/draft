// Direct sequential resolver implementation over typed surface obligations.
//
// State is represented as three plain vectors: surface package borrows, the new
// pin map, and unique expansion objects. A site is processed in deterministic
// package/obligation order. Parallel providers may be added only as a scheduling
// optimization after dependency-ready sets are explicit.

#include "compile/resolver.h"

#include "elaborator/resolved_program.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"

#include <cstddef>
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

} // namespace

ResolveWorkspaceResult resolve_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    ResolveWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  ResolveWorkspaceResult result;
  const std::size_t initial_errors = diagnostics.error_count();

  // Surface compilation owns typing the holes. Resolution never starts from an
  // existing overlay because that would let old generated declarations create
  // dependency edges unavailable to a clean first resolution.
  CompileWorkspaceOptions surface_options = options.compile;
  surface_options.lower_mir = false;
  surface_options.emit_llvm = false;
  surface_options.workspace.source_overrides.clear();
  CompileWorkspaceResult surface = compile_workspace(
      sources,
      root_package_directory,
      std::move(surface_options),
      diagnostics);
  if (!surface.ok) return result;

  const ResolutionManifestLoadResult loaded = load_resolution_manifest(
      options.compile.workspace.workspace_directory, diagnostics);
  if (loaded.state == ResolutionManifestLoadState::Invalid) return result;

  std::vector<ResolutionSurfacePackage> surface_packages;
  std::vector<GeneratedExpansion> expansions;
  ResolutionManifest manifest;
  manifest.target_identity = options.compile.target.facts.identity;

  // Process sites in the compiler's deterministic package and metadata order.
  // A fresh pin contributes its exact stored bytes; revalidation retains stale
  // bytes; normal resolution calls the provider for stale or missing pins.
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
      const ResolutionPin *existing = find_pin(loaded, obligation.site_identity);
      const bool fresh = existing != nullptr &&
          existing->kind == obligation.kind &&
          existing->input_digest == obligation.input_digest;

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
          return result;
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
          return result;
        }
        if (!provider_is_configured(options.provider, diagnostics)) return result;
        const AgentRecord *record = find_record(package, obligation.syntax);
        if (record == nullptr) {
          diagnostics.error(
              SourceRange::invalid(),
              "synthesis obligation has no provider metadata record");
          return result;
        }
        SynthesisRequest request;
        if (!build_request(obligation, *record, request, diagnostics)) return result;
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
          return result;
        }
        expansion.source = std::move(response.source);
        expansion.digest = sha256(expansion.source);
        pin.expansion_digest = expansion.digest;
        pin.provider_identity = options.provider.provider_identity;
        pin.model_identity = options.provider.model_identity;
        pin.configuration_identity = options.provider.configuration_identity;
        ++result.synthesized_sites;
      }
      if (!add_expansion(expansions, std::move(expansion), diagnostics)) {
        return result;
      }
      manifest.pins.push_back(std::move(pin));
    }
  }

  // With no sites and no prior manifest there is no transaction to perform.
  // An existing manifest still runs through the pipeline so obsolete pins are
  // replaced by the current empty map and program identity.
  if (manifest.pins.empty() &&
      loaded.state == ResolutionManifestLoadState::Missing) {
    result.ok = diagnostics.error_count() == initial_errors;
    return result;
  }

  const ResolutionOverlayResult overlays = build_resolution_overlays(
      sources,
      surface_packages,
      manifest,
      options.compile.target.facts.identity,
      options.compile.workspace.workspace_directory,
      expansions,
      diagnostics);
  if (!overlays.ok) return result;

  options.compile.lower_mir = false;
  options.compile.emit_llvm = false;
  options.compile.workspace.source_overrides = overlays.sources;
  CompileWorkspaceResult resolved = compile_workspace(
      sources,
      root_package_directory,
      options.compile,
      diagnostics);
  if (!resolved.ok ||
      !validate_resolved_agent_boundaries(surface, resolved, diagnostics)) {
    return result;
  }

  manifest.resolved_program_digest = hash_resolved_program(
      sources,
      resolved.graph,
      options.compile.target,
      manifest,
      options.compile.compiler_content_identity);
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
