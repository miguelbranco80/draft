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
// rules as handwritten source. This first overlay pass requires the surface
// semantic pass to have produced all obligations; staged declaration/member
// dependency elaboration is a later scheduler built on the same replacement
// representation. See 03-agent-synthesis.md sections 9-10 and 06-compiler.md
// section 15.

#pragma once

#include "elaborator/obligation.h"
#include "elaborator/resolution.h"
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

// Requires one fresh matching pin for every surface synthesis obligation and no
// unassociated manifest pin. target_identity is the selected TargetFacts
// identity. Generated objects are loaded and hash-verified through the store.
// Failures produce no partial override result and never modify the workspace.
[[nodiscard]] ResolutionOverlayResult build_resolution_overlays(
    const SourceManager &surface_sources,
    std::span<const ResolutionSurfacePackage> packages,
    const ResolutionManifest &manifest,
    std::string_view target_identity,
    const std::filesystem::path &workspace_directory,
    DiagnosticSink &diagnostics);

} // namespace draft
