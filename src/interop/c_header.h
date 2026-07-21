// Deterministic C declarations for one package's explicit C exports.

#pragma once

#include "interop/aarch64_abi.h"
#include "sema/analyzer.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <cstddef>
#include <string>

namespace draft {

struct CHeaderOptions {
  // Empty selects a guard derived from the package short name. A caller may
  // provide a distribution-wide guard when composing installed headers.
  std::string include_guard;
};

struct CHeaderResult {
  bool ok = false;
  std::string text;
  std::size_t export_count = 0;
};

// Emits only explicit `export ... :: c proc` declarations. Every transitively
// required @repr(C) aggregate, enum, fixed array member, and C procedure-pointer
// typedef is emitted first, with size/alignment/offset assertions that make a
// mismatched C compiler fail at header compilation rather than at runtime.
[[nodiscard]] CHeaderResult emit_c_header(
    const SemanticPackage &semantic,
    const Aarch64CAbiTable &abi,
    const TargetProfile &target,
    const CHeaderOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
