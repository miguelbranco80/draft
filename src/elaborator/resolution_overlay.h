// Construction of complete resolved source files from surface sites and pins.
//
// The surface compiler produces typed AgentObligation rows whose process-local
// syntax references identify exact `...` ranges. This module matches those rows
// to a validated manifest, loads the referenced content-addressed source, and
// constructs whole-file overrides. The ordinary workspace loader then lexes,
// parses, and semantically checks those bytes from scratch.
//
// No syntax tree is patched and no HIR is manufactured here. That invariant is
// what makes generated code obey the same grammar, typing, denials, and lowering
// rules as handwritten source. The resolver may call this operation once for
// an interface stage and again for a body stage; each call still consumes exact
// coordinates from its own semantic pass. See 03-agent-synthesis.md sections
// 9-10 and 06-compiler.md section 15.

#pragma once

#include "elaborator/obligation.h"
#include "elaborator/resolution.h"
#include "elaborator/resolution_store.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/workspace.h"

#include <filesystem>
#include <span>
#include <vector>

namespace draft {

// One borrowed surface package. All pointers must be non-null and remain valid
// for the duration of build_resolution_overlays. Rows normally point into one
// CompileWorkspaceResult, but this lower-level module does not depend on the
// compiler orchestrator.
struct ResolutionSurfacePackage {
  const PackageIdentity *identity = nullptr;
  const LoadedPackage *loaded = nullptr;
  const AgentObligationResult *obligations = nullptr;
};

// The result owns every replacement file and may be moved directly into
// WorkspaceLoadOptions. applied_sites counts successful surface replacements,
// not files or pins. On failure both the count and sources are empty.
struct ResolutionOverlayResult {
  bool ok = false;
  std::size_t applied_sites = 0;
  // Only files containing at least one selected synthesis site appear here.
  // Ordering follows package input order and then canonical package file order.
  std::vector<WorkspaceSourceOverride> sources;
};

// Ordinary compilation must prove that every selected expansion still matches
// its complete typed synthesis input. A validation-only graph is different:
// compile_workspace_with_resolution first authenticates the ordinary graph,
// including the current test and benchmark context, and then adds the selected
// command-only sources. Recomputing the ordinary site's input in that derived
// graph is neither necessary nor stable because validation compilation has a
// different context-enrichment mode. AuthenticatedManifest permits that one
// orchestrated reuse while retaining every structural and content check below.
enum class ResolutionInputVerification {
  RequireCurrentInput,
  AuthenticatedManifest,
};

// Requires one structurally matching pin for every surface synthesis obligation
// and no unassociated manifest pin. RequireCurrentInput additionally proves
// freshness against the obligation digest. target_identity is the selected
// TargetFacts identity. Generated objects are loaded and hash-verified through
// the store;
// an active resolver may supply not-yet-committed objects in staged_expansions.
// AuthenticatedManifest may only be selected after the caller has successfully
// checked this exact manifest against the ordinary graph. Failures produce no
// partial override result and never modify the workspace.
[[nodiscard]] ResolutionOverlayResult build_resolution_overlays(
    const SourceManager &surface_sources,
    std::span<const ResolutionSurfacePackage> packages,
    const ResolutionManifest &manifest,
    std::string_view target_identity,
    const std::filesystem::path &workspace_directory,
    ResolutionInputVerification input_verification,
    std::span<const GeneratedExpansion> staged_expansions,
    DiagnosticSink &diagnostics);

} // namespace draft
