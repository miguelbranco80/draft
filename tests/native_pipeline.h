// Procedure-product helpers for direct MIR and LLVM subsystem tests.
//
// Production compilation schedules each HIR product and MIR procedure through
// the semantic graph, then emits one complete LLVM module for their semantic
// package. A unit test often starts below that coordinator and still needs to
// exercise the same ownership boundary from one in-memory package. These
// helpers perform only that visible source-order lowering. They never
// concatenate HIR arenas or build a package-wide MirProgram.

#pragma once

#include "assembly/aarch64.h"
#include "backend/llvm_ir.h"
#include "mir/lower.h"
#include "sema/body_checker.h"

#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace draft::test_support {

// One direct test's procedure-owned MIR products in canonical body/product
// order. Each MirProcedure remains an independent value with no package-wide
// ID domain. lowered_procedures counts runtime products; symbolic templates and
// compile-time-only procedures are successful erased results and do not enter
// procedures.
struct LoweredProcedureProducts {
  bool ok = false;
  std::vector<MirProcedure> procedures;
  std::size_t lowered_procedures = 0;
};

// Lowers every procedure row in every supplied HIR product without changing
// arena ownership. assembly may be null when the fixture has no parsed assembly
// regions. Existing diagnostics are tolerated; ok reports only failures added
// by this lowering operation and failed procedure results.
[[nodiscard]] inline LoweredProcedureProducts lower_procedure_products(
    const SemanticPackage &semantic,
    std::span<const ProcedureBodyHirResult> products,
    const AssemblyProgram *assembly,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics) {
  LoweredProcedureProducts result;
  result.ok = true;
  const std::size_t initial_errors = diagnostics.error_count();
  for (const ProcedureBodyHirResult &product : products) {
    for (const HirProcedure &procedure : product.program.procedures()) {
      MirProcedureLoweringResult lowered = lower_procedure_to_mir(
          semantic,
          product.program,
          procedure,
          assembly,
          runtime_assertions,
          diagnostics);
      result.ok = result.ok && lowered.ok;
      if (!lowered.lowered) continue;
      ++result.lowered_procedures;
      result.procedures.push_back(std::move(lowered.procedure));
    }
  }
  result.ok = result.ok && diagnostics.error_count() == initial_errors &&
      result.lowered_procedures == result.procedures.size();
  return result;
}

[[nodiscard]] inline LoweredProcedureProducts lower_procedure_products(
    const SemanticPackage &semantic,
    std::span<const ProcedureBodyHirResult> products,
    DiagnosticSink &diagnostics,
    RuntimeAssertionMode runtime_assertions = RuntimeAssertionMode::On) {
  return lower_procedure_products(
      semantic, products, nullptr, runtime_assertions, diagnostics);
}

// Emits the same complete semantic-package module as production. The temporary
// pointer vector borrows the caller-owned procedure products only for the
// synchronous emitter call and preserves their canonical source order.
[[nodiscard]] inline LlvmIrResult emit_package_llvm_module(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const Aarch64CAbiTable &abi,
    const ConstantTable &global_initializers,
    std::span<const MirProcedure> procedures,
    DiagnosticSink &diagnostics) {
  std::vector<const MirProcedure *> procedure_pointers;
  procedure_pointers.reserve(procedures.size());
  for (const MirProcedure &procedure : procedures) {
    procedure_pointers.push_back(&procedure);
  }
  return emit_llvm_package_module(
      target,
      sources,
      options,
      semantic,
      abi,
      global_initializers,
      procedure_pointers,
      diagnostics);
}

} // namespace draft::test_support
