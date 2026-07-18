// Lower checked Draft HIR into explicit control-flow MIR.

#pragma once

#include "assembly/aarch64.h"
#include "compile/configuration.h"
#include "mir/mir.h"
#include "sema/analyzer.h"
#include "sema/hir.h"
#include "source/diagnostic.h"

#include <cstddef>

namespace draft {

struct MirLoweringResult {
  bool ok = false;
  MirProgram program;
  std::size_t lowered_procedures = 0;
};

// Lowering may intern pointer types used only by explicit MIR addresses, so the
// semantic package is mutable while HIR itself remains immutable. No source or
// syntax tree is consulted: all executable distinctions must already be in HIR.
[[nodiscard]] MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics);

// Disabled assertions are removed before lowering either operand. The source
// still passed parsing and type checking, but no runtime evaluation or optimizer
// assumption survives into MIR.
[[nodiscard]] MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics);

[[nodiscard]] MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    const AssemblyProgram &assembly,
    DiagnosticSink &diagnostics);

[[nodiscard]] MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    const AssemblyProgram &assembly,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics);

} // namespace draft
