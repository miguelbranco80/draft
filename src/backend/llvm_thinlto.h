// In-process ThinLTO preparation and whole-artifact native emission.
//
// This module is the only bootstrap layer which combines LLVM modules from
// different Draft semantic packages. Package workers first produce one
// summary-bearing bitcode buffer per package through the private constructed-
// module seam declared in llvm_module_emission.h. A later workspace task gives
// those immutable buffers to LLVM's ThinLTO driver, which performs one thin
// link, imports profitable definitions across package boundaries, and runs its
// independent native backends in parallel.
//
// The public values below deliberately contain no LLVM handle. Inputs borrow
// command-owned bitcode for one synchronous call; outputs own their native
// bytes and identify the canonical input module which produced them. No cache,
// temporary file, process launch, or physical checkout path participates. The
// caller remains responsible for package scheduling, artifact layout, linking,
// diagnostic publication, and stable output ordering.
//
// LLVM may omit a backend output for a module which becomes empty after
// importing and internalization. It may not invent an unknown module identity
// or return two objects for one Draft package. The adapter validates those
// invariants before publishing any bytes. Relevant specification:
// docs/specification/06-compiler.md, "Native lowering and summaries".

#pragma once

#include "backend/llvm_object_emitter.h"
#include "target/profile.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// Result of preparing one already-constructed package module for ThinLTO.
// bitcode owns summary-bearing binary LLVM IR suitable for exactly one later
// emit_llvm_thinlto_in_process call. failure is empty on success. The ordinary
// object phase timings are reused for target initialization, validation,
// verification, target-machine construction, and the O2 field, which here
// measures LLVM's ThinLTO pre-link pipeline rather than final whole-artifact
// optimization.
struct LlvmThinLtoBitcodeResult {
  bool ok = false;
  std::string bitcode;
  std::string failure;
  LlvmObjectEmissionPhaseTimings phase_timings;
};

// One immutable package module supplied to a complete ThinLTO operation.
// identifier is a logical, deterministic package identity and must be unique
// within inputs. bitcode must contain the matching module summary and remain
// alive until emit_llvm_thinlto_in_process returns.
struct LlvmThinLtoModuleInput {
  std::string identifier;
  std::string_view bitcode;
  // Hidden definitions named here are observable artifact roots even if no
  // other ThinLTO module references them. The span borrows caller-owned names
  // for this synchronous operation. Explicit C exports need not be listed;
  // their default visibility already preserves them.
  std::span<const std::string> preserved_symbols;
};

// One native backend result. module_index addresses the caller's input span;
// outputs are returned in increasing module_index order even though LLVM's
// backend tasks may complete in any order. bytes contains one object or one
// assembly source according to the operation options.
struct LlvmThinLtoModuleOutput {
  std::size_t module_index = 0;
  std::string bytes;
};

// Diagnostic wall durations for one complete ThinLTO call. Input loading and
// symbol resolution are sequential. thin_link_and_backends includes LLVM's
// combined-index construction, importing, O2 work, optional instrumentation,
// and parallel target emission. Output copying canonicalizes task buffers only
// after every LLVM backend has joined. Zero means measurement was disabled or
// the operation was not reached.
struct LlvmThinLtoPhaseTimings {
  std::uint64_t input_loading_nanoseconds = 0;
  std::uint64_t thin_link_and_backends_nanoseconds = 0;
  std::uint64_t output_copy_nanoseconds = 0;
};

// Complete value-only result. failure is empty on success and owns a stable
// adapter-prefixed diagnostic on failure. A successful outputs vector may be
// smaller than inputs only when LLVM proved an input module contributes no
// native bytes.
struct LlvmThinLtoResult {
  bool ok = false;
  std::vector<LlvmThinLtoModuleOutput> outputs;
  std::string failure;
  LlvmThinLtoPhaseTimings phase_timings;
};

// Runs one whole-artifact ThinLTO operation synchronously. options must select
// O2; O0 continues through the ordinary per-unit object emitter. A nonzero
// worker_count is the compiler-owned bound for LLVM's significant-memory
// backend tasks; zero selects LLVM's physical-core default, matching the
// compiler executor's zero-is-automatic convention. The call never enables
// LLVM's persistent ThinLTO cache.
[[nodiscard]] LlvmThinLtoResult emit_llvm_thinlto_in_process(
    const TargetProfile &target,
    std::span<const LlvmThinLtoModuleInput> inputs,
    LlvmObjectEmissionOptions options,
    std::size_t worker_count);

} // namespace draft
