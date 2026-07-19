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

// The resolver owns candidate construction and semantic validation, but it
// deliberately does not own processes or native toolchains. An embedding
// driver supplies this narrow callback to execute an already typed validation
// graph before the manifest becomes visible. The callback may inspect only the
// immutable compilation and report diagnostics; it cannot alter pins. A pass
// returns the exact evidence object that the manifest will select.
struct ResolutionValidationEvidence {
  Sha256Digest key;
  Sha256Digest content_digest;
  bool recorded = false;
};

using ResolutionValidationRunFunction = bool (*)(
    void *state,
    const TargetProfile &target,
    ValidationKind kind,
    const CompileWorkspaceResult &compiled,
    ResolutionValidationEvidence &evidence,
    DiagnosticSink &diagnostics);

struct ResolutionValidationRunner {
  void *state = nullptr;
  ResolutionValidationRunFunction run = nullptr;
};

// Judgment execution is also an embedding concern. It runs only after the
// complete ordinary resolved graph and selected native validations pass. The
// callback returns the complete judgment-row set to publish (including any
// unchanged unselected rows it intentionally preserves); the resolver still
// owns the one final manifest transaction.
using ResolutionJudgmentRunFunction = bool (*)(
    void *state,
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    std::vector<ResolutionEvidencePin> &evidence,
    std::size_t &selected_judgments,
    DiagnosticSink &diagnostics);

struct ResolutionJudgmentRunner {
  void *state = nullptr;
  ResolutionJudgmentRunFunction run = nullptr;
};

// Resolution cancellation is provider-neutral. The driver may source it from a
// signal, while an embedding may use an atomic flag or task cancellation token.
// The resolver polls only at transaction boundaries; provider adapters and
// validation runners remain responsible for interrupting their own child work.
using ResolutionCancellationRequested = bool (*)(void *state);

// Options own the provider adapter values but borrow provider.state. Caller
// lowering flags do not control resolution; the resolver performs semantic
// candidate checks and requests native validation graphs explicitly.
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
  // External inputs are configured as one complete set, never patched by
  // individual rows. When false, an existing manifest's set is preserved so a
  // routine synthesis refresh cannot silently unlock a build. When true, this
  // vector replaces it; an explicitly empty vector therefore removes the lock.
  bool external_inputs_configured = false;
  std::vector<ExternalInputPin> external_inputs;
  // When the candidate selects typed test or benchmark procedures, this runner
  // is required and must accept them before the atomic pin-store commit. A
  // package with no selected validation remains provider/toolchain independent.
  ResolutionValidationRunner validation_runner;
  // Optional resolution profile for qualitative judgments. The callback sees
  // the final resolved program and may invoke a provider; ordinary resolution
  // leaves this null and never contacts a judge.
  ResolutionJudgmentRunner judgment_runner;
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
  std::size_t tested_procedures = 0;
  std::size_t benchmarked_procedures = 0;
  std::size_t judged_sites = 0;
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
