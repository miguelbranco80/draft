// Materialized complete-source projection for inspection and editor tooling.
//
// This module belongs after provider-free compilation. Its input is one fully
// checked CompileWorkspaceResult plus the SourceManager that owns that result's
// exact final file bytes. It writes those bytes and their generated-to-surface
// maps into a new directory; it never reads a resolution manifest, invokes a
// provider, changes pins, or assigns language meaning.
//
// The output is deliberately a projection rather than another source of truth.
// Package roots receive index-based safe directory names and a length-framed
// root manifest records their semantic identities. Every source file receives a
// `.draft-map` sidecar, including an empty sidecar for handwritten or assembly
// source, so consumers never infer mapping state from file absence. Relevant
// specification: docs/specification/03-agent-synthesis.md section 10.

#pragma once

#include "compile/compiler.h"

#include <cstddef>
#include <filesystem>

namespace draft {

// Counts describe the committed projection. `source_files` includes selected
// Draft and assembly inputs; `mapped_expansions` counts generated intervals
// across their sidecars. On failure no output directory becomes visible.
struct ExpandedSourceProjectionResult {
  bool ok = false;
  std::filesystem::path output_directory;
  std::size_t source_files = 0;
  std::size_t mapped_expansions = 0;
};

// Writes one new output directory transactionally. `output_directory` must not
// already exist, which prevents stale files from an older graph being mistaken
// for current output. A sibling `.tmp` directory owns partial writes and is
// removed on every failure before the final rename.
[[nodiscard]] ExpandedSourceProjectionResult materialize_expanded_source(
    const SourceManager &sources,
    const CompileWorkspaceResult &compiled,
    const std::filesystem::path &output_directory,
    DiagnosticSink &diagnostics);

} // namespace draft
