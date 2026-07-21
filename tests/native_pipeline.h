// Procedure-product helpers for direct MIR and LLVM subsystem tests.
//
// Production compilation schedules each HIR product, MIR procedure, package
// static-data unit, and machine-function unit through the semantic graph. A
// unit test often starts below that coordinator and still needs to exercise all
// products from one in-memory package. These helpers perform only that visible
// source-order loop. They never concatenate HIR arenas, build a MirProgram, or
// manufacture a package LLVM module. The combined LLVM text is an observation
// buffer for substring assertions; every appended fragment remains a complete,
// independently emitted module and is also validated by focused object tests.

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

// Test observation of the split LLVM units. text is deliberately not submitted
// to LLVM as one module: it merely lets existing assertions search across the
// static-data and function fragments. Source correlations are appended in
// stable procedure ordinal order, matching artifact publication.
struct EmittedLlvmProducts {
  bool ok = false;
  std::string text;
  std::vector<SourceCorrelationEntry> source_correlations;
};

// Emits one package static-data unit and one unit for each supplied MIR product.
// The procedure symbol list is frozen before emission because every unit must
// declare the same canonical package function set. Diagnostics and success are
// accumulated without hiding which production emitter produced a failure.
[[nodiscard]] inline EmittedLlvmProducts emit_llvm_products(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const Aarch64CAbiTable &abi,
    const ConstantTable &global_initializers,
    std::span<const MirProcedure> procedures,
    DiagnosticSink &diagnostics) {
  EmittedLlvmProducts result;
  result.ok = true;
  std::vector<SymbolId> symbols;
  symbols.reserve(procedures.size());
  for (const MirProcedure &procedure : procedures) {
    symbols.push_back(procedure.symbol);
  }

  LlvmIrResult static_data = emit_llvm_package_static_data(
      target,
      sources,
      options,
      semantic,
      abi,
      global_initializers,
      symbols,
      diagnostics);
  result.ok = result.ok && static_data.ok;
  result.text += static_data.text;
  result.source_correlations.insert(
      result.source_correlations.end(),
      std::make_move_iterator(static_data.source_correlations.begin()),
      std::make_move_iterator(static_data.source_correlations.end()));

  for (std::size_t index = 0; index < procedures.size(); ++index) {
    LlvmIrResult function = emit_llvm_machine_function(
        target,
        sources,
        options,
        semantic,
        abi,
        symbols,
        index,
        procedures[index],
        diagnostics);
    result.ok = result.ok && function.ok;
    result.text.push_back('\n');
    result.text += function.text;
    result.source_correlations.insert(
        result.source_correlations.end(),
        std::make_move_iterator(function.source_correlations.begin()),
        std::make_move_iterator(function.source_correlations.end()));
  }
  return result;
}

} // namespace draft::test_support
