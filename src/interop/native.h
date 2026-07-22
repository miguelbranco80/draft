// Semantic validation for Draft C imports, exports, and link providers.

#pragma once

#include "interop/c_abi.h"
#include "sema/analyzer.h"
#include "sema/body_checker.h"
#include "source/diagnostic.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace draft {

struct NativeInteropResult {
  bool ok = false;
  // Unique logical providers in first declaration order. The build manifest
  // later maps these names to exact libraries/frameworks; the compiler never
  // guesses a host search path from a provider spelling.
  std::vector<std::string> providers;
};

// Validates native bindings against the exact currently selected procedure
// products. selected_indices address procedures in strictly increasing body-
// product order. Body presence is the only HIR fact needed here, so this phase
// never concatenates procedure-local arenas or rewrites their local IDs.
[[nodiscard]] NativeInteropResult validate_native_interop(
    const SemanticPackage &semantic,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const CAbiTable &abi,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Standalone subsystem form which treats every supplied procedure product as
// selected. Workspace compilation uses the explicit selection overload above.
[[nodiscard]] NativeInteropResult validate_native_interop(
    const SemanticPackage &semantic,
    std::span<const ProcedureBodyHirResult> procedures,
    const CAbiTable &abi,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

} // namespace draft
