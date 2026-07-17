// Semantic validation for Draft C imports, exports, and link providers.

#pragma once

#include "sema/analyzer.h"
#include "sema/hir.h"
#include "source/diagnostic.h"

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

[[nodiscard]] NativeInteropResult validate_native_interop(
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics);

} // namespace draft
