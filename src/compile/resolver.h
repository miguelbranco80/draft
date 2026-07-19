// Transactional provider-driven resolution of a complete workspace graph.
//
// This is the orchestration counterpart to compiler.cpp's read-only offline
// path. It compiles surface obligations, reuses fresh pins, invokes a configured
// provider for missing/stale sites, recompiles the proposed resolved source,
// enforces agent boundaries, computes the coherent program identity, and then
// commits through the resolution store. No provider proposal is trusted before
// the ordinary compiler accepts the complete program.
//
// Declaration/member sites form an early opaque interface stage; statement,
// expression, and assembly sites run only after those interfaces are installed
// and bodies can be checked. Both stages share one atomic transaction and never
// expose one same-stage proposal while computing another obligation. Relevant
// specification: docs/specification/03-agent-synthesis.md sections 9-10 and docs/specification/06-compiler.md
// section 15.

#pragma once

#include "compile/compiler.h"
#include "elaborator/provider.h"
#include "elaborator/resolution.h"
#include "source/diagnostic.h"
#include "source/source.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace draft {

// Resolution cancellation is provider-neutral. The driver may source it from a
// signal, while an embedding may use an atomic flag or task cancellation token.
// The resolver polls only at transaction boundaries; provider adapters remain
// responsible for interrupting their own child work.
using ResolutionCancellationRequested = bool (*)(void *state);

// Options own the provider adapter values but borrow provider.state. Caller
// lowering flags do not control resolution; the resolver performs complete
// semantic candidate checks before committing source.
struct ResolveWorkspaceOptions {
  CompileWorkspaceOptions compile;
  SynthesisProvider provider;
  void *cancellation_state = nullptr;
  ResolutionCancellationRequested cancellation_requested = nullptr;
  // One proposal attempt is one successful provider response followed by the
  // ordinary parser and semantic checker. This budget is deliberately distinct
  // from adapter-level process retries. Values outside 1..8 are rejected before
  // compilation so a malformed embedding cannot create an unbounded loop.
  std::uint32_t maximum_proposal_attempts = 2;
  // Revalidation never invokes provider.synthesize. A stale site must retain an
  // existing expansion object, which is checked under its current obligation
  // and re-pinned only if the entire working program succeeds.
  bool revalidate = false;
  // Regeneration deliberately asks the provider to reconsider accepted source
  // even when its semantic obligation is still fresh. An empty selector vector
  // selects every synthesis site; otherwise each entry is one exact persistent
  // `site-...` identity and must match a current site. Ordinary stale or missing
  // sites elsewhere are still resolved normally.
  bool regenerate = false;
  std::vector<std::string> regeneration_site_identities;
  // External inputs are configured as one complete set, never patched by
  // individual rows. When false, an existing manifest's set is preserved so a
  // routine synthesis refresh cannot silently unlock a build. When true, this
  // vector replaces it; an explicitly empty vector therefore removes the lock.
  bool external_inputs_configured = false;
  std::vector<ExternalInputPin> external_inputs;
};

// Counts describe provider/reuse work performed by the attempt, including work
// later rejected by checking. committed distinguishes a no-site no-op from a
// written coherent manifest. On failure manifest is empty and the persistent
// store retains its previous visible state.
struct ResolveWorkspaceResult {
  bool ok = false;
  bool committed = false;
  std::size_t reused_sites = 0;
  std::size_t synthesized_sites = 0;
  std::size_t regenerated_sites = 0;
  ResolutionManifest manifest;
};

// Resolves one selected package graph without printing or lowering native code.
// The SourceManager owns both surface and resolved source for diagnostic
// rendering until the result is consumed. On every failure before the final
// store commit, .draft/resolution.json remains unchanged.
[[nodiscard]] ResolveWorkspaceResult resolve_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    ResolveWorkspaceOptions options,
    DiagnosticSink &diagnostics);

} // namespace draft
