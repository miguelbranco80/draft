// Deterministic resolver implementation over typed surface obligations.
//
// The authoritative CompileWorkspaceResult advances through interface rounds,
// body closure, and target lowering without reloading its workspace. Pins and
// unique expansion objects remain plain vectors. A site is processed in
// deterministic package/obligation order; its private proposal check copies
// the current graph so opaque siblings cannot observe each other, then uses the
// same in-memory source-transition operation as the authoritative stage.
// Independent provider calls run in bounded ready waves, then proposal checks
// and publication return to the resolver thread in stable site order.

#include "compile/resolver.h"

#include "base/timing.h"
#include "base/work_graph.h"
#include "elaborator/generated_source.h"
#include "elaborator/resolved_program.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"
#include "workspace/selection.h"

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
  if (provider.maximum_parallel_calls == 0 ||
      provider.maximum_parallel_calls > 64) {
    diagnostics.error(
        SourceRange::invalid(),
        "synthesis provider parallel-call bound must be between 1 and 64");
    return false;
  }
  return true;
}

// Performs provider-owned command setup exactly once and only when resolution
// reaches a site that actually needs generation. Fresh pins therefore remain a
// genuinely provider-free path. The prepared flag belongs to the whole resolve
// command, not one semantic stage, so interface rounds and the body stage share
// one immutable resource set.
[[nodiscard]] bool prepare_provider(
    const SynthesisProvider &provider,
    bool &prepared,
    DiagnosticSink &diagnostics) {
  if (prepared) return true;
  if (provider.prepare != nullptr &&
      !provider.prepare(provider.state, diagnostics)) {
    if (!diagnostics.has_errors()) {
      diagnostics.error(
          SourceRange::invalid(),
          "synthesis provider preparation failed without a diagnostic");
    }
    return false;
  }
  prepared = true;
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

// Returns whether an otherwise-fresh site was explicitly selected for new
// provider work. The caller records matches separately so an exact selector
// typo can fail the transaction instead of silently doing nothing.
[[nodiscard]] bool regeneration_selects(
    const ResolveWorkspaceOptions &options,
    std::string_view site_identity) {
  if (!options.regenerate) return false;
  if (options.regeneration_site_identities.empty()) return true;
  for (const std::string &selected : options.regeneration_site_identities) {
    if (selected == site_identity) return true;
  }
  return false;
}

void record_regeneration_match(
    std::string_view site_identity,
    std::vector<std::string> &matches) {
  for (const std::string &existing : matches) {
    if (existing == site_identity) return;
  }
  matches.emplace_back(site_identity);
}

// Later-stage or candidate overrides contain complete files based on an
// already overlaid source surface. A later row replaces an earlier row for the
// same semantic package and filename; unrelated files retain deterministic
// discovery order. This operation is used both by normal stage advancement and
// by private one-proposal compiler checks.
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

// Copies a private candidate diagnostic set into the resolver's authoritative
// sink. Rejected intermediate attempts stay private; only the final rejection
// is published so users do not see errors that a later proposal corrected.
void publish_diagnostics(
    const DiagnosticSink &source,
    DiagnosticSink &destination) {
  for (const Diagnostic &diagnostic : source.diagnostics()) {
    destination.report(
        diagnostic.severity, diagnostic.range, diagnostic.message);
  }
}

// Installs exactly one proposed expansion over the stage surface and runs the
// same provider-free compiler stage that discovered the obligation. Earlier
// interface-round overrides remain visible, while other proposals from this
// completeness set do not: this preserves the specification's opaque-set rule
// while still giving each provider response ordinary parser and type-checker
// authority before it enters the transaction.
[[nodiscard]] bool proposal_compiles(
    SourceManager &sources,
    const std::string &root_package_directory,
    const CompileWorkspaceOptions &stage_compile_options,
    const CompileWorkspaceResult &surface,
    const PackageIdentity &identity,
    const LoadedPackage &loaded,
    const AgentObligation &obligation,
    const ResolutionPin &pin,
    const GeneratedExpansion &expansion,
    DiagnosticSink &diagnostics) {
  AgentObligationResult candidate_obligations;
  candidate_obligations.ok = true;
  candidate_obligations.obligations.push_back(obligation);
  const ResolutionSurfacePackage candidate_package{
      &identity,
      &loaded,
      &candidate_obligations,
  };
  ResolutionManifest candidate_manifest;
  candidate_manifest.target_identity =
      stage_compile_options.target.facts.identity;
  candidate_manifest.pins.push_back(pin);
  ResolutionOverlayResult candidate_overlay =
      build_resolution_overlays(
          sources,
          std::span<const ResolutionSurfacePackage>(&candidate_package, 1),
          candidate_manifest,
          stage_compile_options.target.facts.identity,
          stage_compile_options.workspace.workspace_directory,
          ResolutionInputVerification::RequireCurrentInput,
          std::span<const GeneratedExpansion>(&expansion, 1),
          diagnostics);
  if (!candidate_overlay.ok) return false;

  // A proposal is speculative, but it does not need a second workspace load.
  // Copy the command-local semantic state as the isolation boundary, install
  // exactly this site's complete-file replacement, and resume the same graph
  // operations the authoritative stage will use after every sibling proposal
  // in the opaque set has been accepted.
  CompileWorkspaceResult compiled = surface;
  CompileWorkspaceOptions candidate_options = stage_compile_options;
  candidate_options.lower_mir = false;
  candidate_options.emit_llvm = false;
  bool candidate_ok = apply_compiled_workspace_source_overrides(
      sources,
      candidate_overlay.sources,
      stage_compile_options.stage == CompileWorkspaceStage::Complete
          ? WorkspaceSemanticChange::Body
          : WorkspaceSemanticChange::Interface,
      candidate_options,
      compiled,
      diagnostics);
  if (candidate_ok &&
      stage_compile_options.stage == CompileWorkspaceStage::Complete) {
    candidate_options.stage = CompileWorkspaceStage::Complete;
    candidate_ok = continue_compiled_workspace_semantics(
        sources,
        root_package_directory,
        candidate_options,
        compiled,
        diagnostics);
  }
  if ((!candidate_ok || !compiled.ok) && !diagnostics.has_errors()) {
    diagnostics.error(
        SourceRange::invalid(),
        "compiler rejected a synthesis proposal without a diagnostic");
  }
  return candidate_ok && compiled.ok && !diagnostics.has_errors();
}

// One site row owns all mutable resolution state for one obligation in stable
// package/obligation order. Pointers borrow the immutable stage surface for the
// duration of resolve_stage. Provider workers read only request and write no row;
// the resolver thread installs responses, rejection histories, pins, and source
// bytes after each wave joins. This separation is the concurrency invariant.
struct StageSite {
  std::size_t package_index = 0;
  const AgentObligation *obligation = nullptr;
  SourceRange surface_range;
  bool regenerate = false;
  bool requires_provider = false;
  bool accepted = false;
  bool expansion_boundary_checked = false;
  ResolutionPin pin;
  GeneratedExpansion expansion;
  SynthesisRequest request;
};

// One call slot is indexed by WorkTaskId within one provider wave. diagnostics
// and response are written by exactly one worker, then read only after join.
// site_index maps the transient task ID back to the persistent stage order.
struct ProviderWaveCall {
  std::size_t site_index = 0;
  // Set immediately before entering the provider callback. A queued task that
  // observes cancellation remains false, which lets timing distinguish work
  // submitted to the scheduler from a provider callback actually entered.
  bool invoked = false;
  bool ok = false;
  SynthesisResponse response;
  DiagnosticSink diagnostics;
};

// The scheduler erases operation types to a plain context pointer. This direct
// context records the two disjoint tables needed by invoke_provider_task: an
// immutable site table and task-indexed mutable result slots.
struct ProviderWaveContext {
  const SynthesisProvider *provider = nullptr;
  const std::vector<StageSite> *sites = nullptr;
  std::vector<ProviderWaveCall> *calls = nullptr;
  void *cancellation_state = nullptr;
  ResolutionCancellationRequested cancellation_requested = nullptr;
};

// Invokes one stateless proposal request. The callback may internally retry its
// own process transport, but it cannot check or publish Draft source here. A
// missing provider diagnostic is converted into a task-local compiler error so
// the main thread can always publish one stable lowest-site failure.
[[nodiscard]] bool invoke_provider_task(
    void *opaque,
    WorkTaskId task,
    std::string &failure) {
  auto *context = static_cast<ProviderWaveContext *>(opaque);
  ProviderWaveCall &call = (*context->calls)[task];
  const StageSite &site = (*context->sites)[call.site_index];
  if (context->cancellation_requested != nullptr &&
      context->cancellation_requested(context->cancellation_state)) {
    call.diagnostics.error(site.surface_range, "resolution cancelled");
    failure = "resolution cancelled before synthesis provider call for site " +
        site.obligation->site_identity;
    return false;
  }
  call.invoked = true;
  const bool callback_ok = context->provider->synthesize(
      context->provider->state,
      site.request,
      call.response,
      call.diagnostics);
  if (!callback_ok && !call.diagnostics.has_errors()) {
    call.diagnostics.error(
        site.surface_range,
        "synthesis provider failed without a diagnostic");
  }
  // A provider cannot report an error and simultaneously declare success. The
  // diagnostic remains authoritative at this boundary; treating the response
  // as usable would hide the error when task-local sinks are joined.
  call.ok = callback_ok && !call.diagnostics.has_errors();
  if (!call.ok) {
    failure = "synthesis provider failed for site " +
        site.obligation->site_identity;
  }
  return call.ok;
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
    const std::string &root_package_directory,
    const CompileWorkspaceOptions &stage_compile_options,
    const CompileWorkspaceResult &surface,
    const ResolutionManifestLoadResult &loaded,
    const ResolveWorkspaceOptions &options,
    ResolveWorkspaceResult &result,
    bool &provider_prepared,
    std::vector<std::string> &regeneration_matches,
    std::vector<GeneratedExpansion> &expansions,
    DiagnosticSink &diagnostics) {
  ResolvedStage stage;
  stage.manifest.target_identity = options.compile.target.facts.identity;
  std::vector<ResolutionSurfacePackage> surface_packages;
  std::vector<StageSite> sites;

  // Collect the complete semantic ready set before invoking any provider.
  // Fresh pins load verified store bytes immediately. Stale/missing rows retain
  // owned requests whose obligations were all computed against the same opaque
  // surface, so no completion timing can change a sibling request.
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
      // A site belongs to exactly one dependency-ready stage. Count it before
      // any provider, source-map, or proposal validation can return so failed
      // explicit Resolve commands still report discovered work accurately.
      ++result.site_count;
      if (resolution_cancelled(options, diagnostics)) return stage;
      const ResolutionPin *existing = find_pin(loaded, obligation.site_identity);
      const bool regenerate = regeneration_selects(
          options, obligation.site_identity);
      if (regenerate) {
        record_regeneration_match(
            obligation.site_identity, regeneration_matches);
      }
      const bool fresh = existing != nullptr &&
          existing->kind == obligation.kind &&
          existing->input_digest == obligation.input_digest &&
          !regenerate;

      StageSite site;
      site.package_index = package_index;
      site.obligation = &obligation;
      site.regenerate = regenerate;
      site.pin.site_identity = obligation.site_identity;
      site.pin.kind = obligation.kind;
      site.pin.input_digest = obligation.input_digest;
      const SourceRange surface_range = obligation_range(
          surface.graph.packages[package_index].loaded, obligation);
      if (!surface_range.is_valid()) {
        diagnostics.error(
            SourceRange::invalid(),
            "synthesis obligation has no source range for its persistent map");
        return stage;
      }
      site.surface_range = surface_range;
      site.pin.source_map.root_identity = obligation.root_identity;
      site.pin.source_map.root_relative_path = obligation.root_relative_path;
      site.pin.source_map.source_relative_path = obligation.source_relative_path;
      site.pin.source_map.surface_begin = surface_range.begin.offset;
      site.pin.source_map.surface_end = surface_range.end.offset;
      if (fresh || (options.revalidate && existing != nullptr &&
                    existing->kind == obligation.kind)) {
        if (!load_generated_expansion(
                options.compile.workspace.workspace_directory,
                existing->expansion_digest,
                site.expansion.source,
                diagnostics)) {
          return stage;
        }
        site.expansion.digest = existing->expansion_digest;
        site.pin.expansion_digest = existing->expansion_digest;
        site.pin.provider_identity = existing->provider_identity;
        site.pin.model_identity = existing->model_identity;
        site.pin.configuration_identity = existing->configuration_identity;
        site.accepted = true;
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
        site.requires_provider = true;
        if (!build_request(
                obligation, *record, site.request, diagnostics)) {
          return stage;
        }
      }
      sites.push_back(std::move(site));
    }
  }

  // Each attempt is one bounded ready wave. Only sites rejected by the ordinary
  // compiler enter the next wave, carrying their own exact correction history.
  // Accepted siblings never become visible to those requests or private checks.
  for (std::uint32_t attempt = 1;
       attempt <= options.maximum_proposal_attempts; ++attempt) {
    std::vector<std::size_t> pending_sites;
    for (std::size_t site_index = 0; site_index < sites.size(); ++site_index) {
      if (sites[site_index].requires_provider && !sites[site_index].accepted) {
        pending_sites.push_back(site_index);
      }
    }
    if (pending_sites.empty()) break;
    if (resolution_cancelled(options, diagnostics)) return stage;
    if (!prepare_provider(
            options.provider, provider_prepared, diagnostics)) {
      return stage;
    }

    WorkGraph graph;
    graph.tasks.resize(pending_sites.size());
    std::vector<ProviderWaveCall> calls(pending_sites.size());
    for (std::size_t task_index = 0; task_index < calls.size(); ++task_index) {
      calls[task_index].site_index = pending_sites[task_index];
    }
    ProviderWaveContext context{
        &options.provider,
        &sites,
        &calls,
        options.cancellation_state,
        options.cancellation_requested,
    };
    if (options.compile.timings != nullptr) {
      options.compile.timings->add_counter("synthesis provider ready waves", 1);
    }
    TimingScope provider_timing = options.compile.timings != nullptr
        ? options.compile.timings->scope("provider synthesis")
        : TimingScope{};
    const WorkGraphRunResult wave = options.compile.work_executor->run(
        graph,
        WorkGraphRunOptions{options.provider.maximum_parallel_calls},
        invoke_provider_task,
        &context);
    provider_timing.finish();
    if (options.compile.timings != nullptr) {
      std::uint64_t invoked_calls = 0;
      for (const ProviderWaveCall &call : calls) {
        if (call.invoked) ++invoked_calls;
      }
      options.compile.timings->add_counter(
          "synthesis provider calls", invoked_calls);
    }

    // Stable task order is stable package/obligation order. Check each response
    // only after all provider tasks have joined. Thus compiler state and the
    // single-threaded timing/source managers are never shared with workers.
    for (std::size_t task_index = 0; task_index < calls.size(); ++task_index) {
      ProviderWaveCall &call = calls[task_index];
      StageSite &site = sites[call.site_index];
      // Successful provider warnings/notes were visible in the former direct
      // sequential path. Replay every task-local diagnostic here, before the
      // stable failure decision, to preserve that behavior deterministically.
      publish_diagnostics(call.diagnostics, diagnostics);
      if (task_index >= wave.tasks.size() ||
          wave.tasks[task_index].state != WorkTaskState::Succeeded ||
          !call.ok) {
        if (!call.diagnostics.has_errors()) {
          diagnostics.error(
              site.surface_range,
              "synthesis provider task failed without a diagnostic");
        }
        return stage;
      }

      GeneratedExpansion candidate_expansion;
      candidate_expansion.source = std::move(call.response.source);
      candidate_expansion.digest = sha256(candidate_expansion.source);
      ResolutionPin candidate_pin = site.pin;
      candidate_pin.expansion_digest = candidate_expansion.digest;
      candidate_pin.provider_identity = options.provider.provider_identity;
      candidate_pin.model_identity = options.provider.model_identity;
      candidate_pin.configuration_identity =
          options.provider.configuration_identity;
      candidate_pin.source_map.expansion_bytes =
          static_cast<std::uint64_t>(candidate_expansion.source.size());

      // Lexical boundary checks and the ordinary stage compile share one
      // private sink. On rejection its generated-source rendering becomes
      // correction data for only this site's next stateless provider call.
      DiagnosticSink attempt_diagnostics;
      const std::string display_name =
          "<generated/" + site.obligation->site_identity + ">";
      bool proposal_ok = validate_generated_source_boundary(
          sources,
          display_name,
          candidate_expansion.source,
          attempt_diagnostics);
      if (proposal_ok) {
        const CompiledPackage &package =
            *surface.packages[site.package_index];
        proposal_ok = proposal_compiles(
            sources,
            root_package_directory,
            stage_compile_options,
            surface,
            package.identity,
            surface.graph.packages[site.package_index].loaded,
            *site.obligation,
            candidate_pin,
            candidate_expansion,
            attempt_diagnostics);
      }
      if (proposal_ok) {
        site.expansion = std::move(candidate_expansion);
        site.pin = std::move(candidate_pin);
        site.expansion_boundary_checked = true;
        site.accepted = true;
        ++result.synthesized_sites;
        if (site.regenerate) ++result.regenerated_sites;
        continue;
      }
      if (!attempt_diagnostics.has_errors()) {
        attempt_diagnostics.error(
            site.surface_range,
            "compiler rejected a synthesis proposal without an error");
      }
      const std::string rendered =
          render_diagnostics(sources, attempt_diagnostics);
      site.request.prior_rejections.push_back({
          attempt,
          std::move(candidate_expansion.source),
          rendered,
      });
      if (attempt == options.maximum_proposal_attempts) {
        publish_diagnostics(attempt_diagnostics, diagnostics);
        diagnostics.note(
            site.surface_range,
            "synthesis site exhausted " + std::to_string(attempt) +
                " compiler-checked proposal attempt(s)");
        return stage;
      }
    }
  }

  // Publish verified rows in their original semantic order. Reused bytes still
  // receive a current lexical boundary check; provider bytes already passed it
  // as part of their private ordinary-compiler acceptance.
  for (StageSite &site : sites) {
    if (!site.accepted) {
      diagnostics.error(
          site.surface_range,
          "synthesis proposal waves ended without an accepted expansion");
      return stage;
    }
    site.pin.source_map.expansion_bytes =
        static_cast<std::uint64_t>(site.expansion.source.size());

    // This check runs for reused bytes as well as new proposals. It prevents an
    // older or externally supplied store from smuggling provider work into the
    // next stage before the complete resolved-program check can run.
    const std::string display_name =
        "<generated/" + site.obligation->site_identity + ">";
    if (!site.expansion_boundary_checked &&
        !validate_generated_source_boundary(
              sources,
              display_name,
              site.expansion.source,
              diagnostics)) {
      return stage;
    }
    if (!add_expansion(
            expansions, std::move(site.expansion), diagnostics)) {
      return stage;
    }
    stage.manifest.pins.push_back(std::move(site.pin));
  }

  const ResolutionOverlayResult overlays = build_resolution_overlays(
      sources,
      surface_packages,
      stage.manifest,
      options.compile.target.facts.identity,
      options.compile.workspace.workspace_directory,
      ResolutionInputVerification::RequireCurrentInput,
      expansions,
      diagnostics);
  if (!overlays.ok) return stage;
  stage.overrides = overlays.sources;
  stage.ok = true;
  return stage;
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
  if (options.maximum_proposal_attempts == 0 ||
      options.maximum_proposal_attempts > 8) {
    diagnostics.error(
        SourceRange::invalid(),
        "synthesis proposal attempt count must be between 1 and 8");
    return result;
  }
  if (options.revalidate && options.regenerate) {
    diagnostics.error(
        SourceRange::invalid(),
        "resolution cannot revalidate and regenerate in one command");
    return result;
  }
  if (!options.regenerate &&
      !options.regeneration_site_identities.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "regeneration selectors require regeneration mode");
    return result;
  }
  if (resolution_cancelled(options, diagnostics)) return result;

  WorkspacePackageSelection selected_root;
  if (!identify_workspace_package(
          options.compile.workspace.workspace_directory,
          root_package_directory,
          selected_root,
          diagnostics)) {
    return result;
  }
  const ResolutionStoreKey store_key{
      options.compile.target.facts.identity,
      selected_root.identity,
  };

  const ResolutionManifestLoadResult loaded = load_resolution_manifest(
      options.compile.workspace.workspace_directory, store_key, diagnostics);
  if (loaded.state == ResolutionManifestLoadState::Invalid) return result;

  std::vector<GeneratedExpansion> expansions;
  ResolutionManifest manifest;
  manifest.target_identity = options.compile.target.facts.identity;
  manifest.root_package = selected_root.identity;
  std::vector<WorkspaceSourceOverride> interface_overrides;
  std::vector<std::string> regeneration_matches;
  bool provider_prepared = false;

  // Interface synthesis advances in dependency-ready rounds. Every package in
  // one round sees completed prerequisite package interfaces but none of its
  // own round's proposals. Merging a nonempty round removes at least one
  // provider site, and generated-source validation forbids adding another, so
  // the finite source graph guarantees termination without an iteration cap.
  CompileWorkspaceOptions interface_options = options.compile;
  interface_options.stage =
      CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  interface_options.lower_mir = false;
  interface_options.emit_llvm = false;
  CompileWorkspaceResult interface_surface = compile_workspace(
      sources,
      root_package_directory,
      interface_options,
      diagnostics);
  if (!interface_surface.ok) return result;
  while (true) {
    if (resolution_cancelled(options, diagnostics)) return result;
    ResolvedStage interface_stage = resolve_stage(
        sources,
        root_package_directory,
        interface_options,
        interface_surface,
        loaded,
        options,
        result,
        provider_prepared,
        regeneration_matches,
        expansions,
        diagnostics);
    if (!interface_stage.ok) return result;
    const bool made_progress = !interface_stage.manifest.pins.empty();
    if (!append_stage_pins(
            manifest, std::move(interface_stage.manifest), diagnostics)) {
      return result;
    }
    if (made_progress &&
        !apply_compiled_workspace_source_overrides(
            sources,
            interface_stage.overrides,
            WorkspaceSemanticChange::Interface,
            interface_options,
            interface_surface,
            diagnostics)) {
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

  // Stage 2 continues the final interface graph into bodies after all interface
  // edits are installed. Its obligations therefore include exact expected
  // expression types and visible locals together with the generated interface
  // dependencies, without reloading or reanalyzing declarations.
  if (resolution_cancelled(options, diagnostics)) return result;
  CompileWorkspaceOptions body_options = options.compile;
  body_options.stage = CompileWorkspaceStage::Complete;
  body_options.lower_mir = false;
  body_options.emit_llvm = false;
  body_options.workspace.source_overrides = interface_overrides;
  if (!continue_compiled_workspace_semantics(
          sources,
          root_package_directory,
          body_options,
          interface_surface,
          diagnostics)) {
    return result;
  }
  CompileWorkspaceResult body_surface = std::move(interface_surface);
  if (!body_surface.ok) return result;

  ResolvedStage body_stage = resolve_stage(
      sources,
      root_package_directory,
      body_options,
      body_surface,
      loaded,
      options,
      result,
      provider_prepared,
      regeneration_matches,
      expansions,
      diagnostics);
  if (!body_stage.ok ||
      !append_stage_pins(
          manifest, std::move(body_stage.manifest), diagnostics)) {
    return result;
  }

  // With no sites and no prior manifest there is no transaction to perform.
  // An existing manifest still proceeds so obsolete pins become an empty map.
  // The checked handwritten graph is nevertheless returned, and a native
  // request continues it through lowering without another front-end pass.
  if (manifest.pins.empty() &&
      loaded.state == ResolutionManifestLoadState::Missing) {
    ResolutionManifest empty_manifest;
    empty_manifest.target_identity = options.compile.target.facts.identity;
    body_surface.resolved_program_digest = hash_resolved_program(
        sources,
        body_surface.graph,
        options.compile.target,
        empty_manifest,
        options.compile.compiler_content_identity,
        options.compile.configuration);
    // A handwritten program has no source transaction to publish, but the
    // resolver still returns the checked semantic graph at the same boundary
    // as a committed generated program. A caller performing `resolve --build`
    // advances this result only after resolution has succeeded; keeping target
    // lowering outside the resolver makes backend failure independent of
    // generated-source selection in both cases.
    result.compiled_program = std::move(body_surface);
    result.ok = diagnostics.error_count() == initial_errors;
    return result;
  }

  const ResolvedAgentBoundary surface_boundary =
      capture_resolved_agent_boundary(body_surface);
  if (resolution_cancelled(options, diagnostics)) return result;
  if (!body_stage.overrides.empty()) {
    CompileWorkspaceOptions update_options = options.compile;
    update_options.stage =
        CompileWorkspaceStage::DiscoverInterfaceSynthesis;
    update_options.lower_mir = false;
    update_options.emit_llvm = false;
    if (!apply_compiled_workspace_source_overrides(
            sources,
            body_stage.overrides,
            WorkspaceSemanticChange::Body,
            update_options,
            body_surface,
            diagnostics) ||
        !continue_compiled_workspace_semantics(
            sources,
            root_package_directory,
            body_options,
            body_surface,
            diagnostics)) {
      return result;
    }
  }
  if (!validate_resolved_agent_boundaries(
          surface_boundary, body_surface, diagnostics)) {
    return result;
  }

  // Exact selector validation happens after all dependency stages have exposed
  // their body sites. A missing selector never publishes the candidate, even
  // if unrelated stale sites required provider work earlier in the attempt.
  for (const std::string &selected : options.regeneration_site_identities) {
    bool matched = false;
    for (const std::string &observed : regeneration_matches) {
      matched = matched || selected == observed;
    }
    if (!matched) {
      diagnostics.error(
          SourceRange::invalid(),
          "regeneration selector did not match a current synthesis site: '" +
              selected + "'");
      return result;
    }
  }

  manifest.resolved_program_digest = hash_resolved_program(
      sources,
      body_surface.graph,
      options.compile.target,
      manifest,
      options.compile.compiler_content_identity,
      options.compile.configuration);
  body_surface.resolved_program_digest = manifest.resolved_program_digest;
  // The manifest copy is bound before the store commit and native emission.
  // Both consumers therefore use the exact candidate checked above, never a
  // path that could be replaced by another process after this point.
  body_surface.resolution_manifest = manifest;

  // This is the final cancellation boundary. Once commit_resolution starts it
  // performs one crash-safe object-before-manifest transaction and must not be
  // interrupted by a cooperative flag halfway through its atomic publication.
  if (resolution_cancelled(options, diagnostics)) return result;
  if (!commit_resolution(
          options.compile.workspace.workspace_directory,
          store_key,
          manifest,
          expansions,
          diagnostics)) {
    return result;
  }
  result.manifest = std::move(manifest);
  // Target lowering deliberately remains a caller-owned continuation after
  // this commit. A MIR, LLVM, assembler, linker, or debug-symbol failure must
  // not discard generated Draft source that already passed the complete
  // grammar, semantic, denial, and resolved-program checks above.
  result.compiled_program = std::move(body_surface);
  result.committed = true;
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
