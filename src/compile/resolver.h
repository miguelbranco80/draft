// Transactional provider-driven resolution of a complete workspace graph.
//
// This is the orchestration counterpart to compiler.cpp's read-only offline
// path. It compiles surface obligations, reuses fresh pins, invokes a configured
// provider for missing/stale sites, recompiles the proposed resolved source,
// enforces agent boundaries, computes the coherent program identity, and then
// commits through the resolution store. No provider proposal is trusted before
// the ordinary compiler accepts the complete program.
//
// The first scheduler handles sites whose surface graph can reach complete typed
// obligations in one pass. Declaration/member dependency staging remains an
// explicit later extension; the transaction, provider, overlay, and checking
// boundaries do not change. Relevant specification: 03-agent-synthesis.md
// sections 9-10 and 06-compiler.md section 15.

#pragma once

#include "compile/compiler.h"
#include "elaborator/provider.h"
#include "elaborator/resolution.h"
#include "source/diagnostic.h"
#include "source/source.h"

#include <cstddef>
#include <string>

namespace draft {

// Options own the provider adapter values but borrow provider.state. Native
// lowering flags are ignored because resolution validates source/semantics and
// commits before any separate requested artifact build.
struct ResolveWorkspaceOptions {
  CompileWorkspaceOptions compile;
  SynthesisProvider provider;
  // Revalidation never invokes provider.synthesize. A stale site must retain an
  // existing expansion object, which is checked under its current obligation
  // and re-pinned only if the entire working program succeeds.
  bool revalidate = false;
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
