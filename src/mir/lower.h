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

// One procedure-owned lowering result. A symbolic template or compile-time-only
// procedure is a successful non-runtime answer with lowered false. A concrete
// runtime row owns exactly one verified MirProcedure. Keeping that distinction
// explicit lets the compiler form one MirProcedure product per emitted body
// without fabricating empty IR nodes for erased compile-time code.
struct MirProcedureLoweringResult {
  bool ok = false;
  bool lowered = false;
  MirProcedure procedure;
};

[[nodiscard]] MirProcedureLoweringResult lower_procedure_to_mir(
    const SemanticPackage &semantic,
    const HirProgram &hir,
    const HirProcedure &procedure,
    const AssemblyProgram *assembly,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics);

// Lowering reads one immutable semantic package and HIR program. Compiler-only
// storage addresses use MIR's addressed_type metadata rather than interning
// pointer rows into the semantic TypeStore. No source or syntax tree is
// consulted: all executable distinctions must already be in HIR.
[[nodiscard]] MirLoweringResult lower_package_to_mir(
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics);

// Disabled assertions are removed before lowering either operand. The source
// still passed parsing and type checking, but no runtime evaluation or optimizer
// assumption survives into MIR.
[[nodiscard]] MirLoweringResult lower_package_to_mir(
    const SemanticPackage &semantic,
    const HirProgram &hir,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics);

[[nodiscard]] MirLoweringResult lower_package_to_mir(
    const SemanticPackage &semantic,
    const HirProgram &hir,
    const AssemblyProgram &assembly,
    DiagnosticSink &diagnostics);

[[nodiscard]] MirLoweringResult lower_package_to_mir(
    const SemanticPackage &semantic,
    const HirProgram &hir,
    const AssemblyProgram &assembly,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics);

} // namespace draft
