// Deterministic textual LLVM IR emission from verified Draft MIR.

#pragma once

#include "backend/source_correlation.h"
#include "interop/aarch64_abi.h"
#include "mir/mir.h"
#include "sema/analyzer.h"
#include "sema/constant.h"
#include "source/diagnostic.h"
#include "target/profile.h"
#include "validation/discovery.h"
#include "workspace/workspace.h"

#include <span>
#include <string>
#include <vector>

namespace draft {

struct LlvmIrOptions {
  PackageIdentity package;
  // One module in every final artifact owns the compiler runtime definitions.
  // Executables additionally own the hosted C `main`; libraries do not.
  bool emit_runtime_support = false;
  bool emit_program_entry = false;
  // A validation executable has a compiler-owned entry point. The entries are
  // already signature- and layout-checked; LLVM emission only materializes
  // isolated state and calls their stable package symbols in canonical order.
  ValidationKind validation_kind = ValidationKind::None;
  std::vector<ValidationEntry> validation_entries;
};

struct LlvmIrResult {
  bool ok = false;
  std::string text;
  // These rows are collected at the same point as the LLVM debug markers, so
  // the sidecar and instruction metadata cannot silently drift apart.
  std::vector<SourceCorrelationEntry> source_correlations;
};

// The emitter produces opaque-pointer LLVM IR and never invokes a toolchain.
// Object emission and linking are separate host adapters that obey the selected
// target profile. The textual form is intentionally testable without LLVM
// headers or libraries on the bootstrap compiler's build machine.
[[nodiscard]] LlvmIrResult emit_llvm_ir(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const Aarch64CAbiTable &abi,
    const ConstantTable &global_initializers,
    const MirProgram &mir,
    DiagnosticSink &diagnostics);

// Emits the one complete LLVM module which owns package-level storage and
// process support but no Draft procedure definition. package_procedures is the
// complete canonical runtime procedure set for this package. It lets the
// static module declare procedure-valued constants and construct the hosted
// entry point without borrowing a package-wide MirProgram. Every listed symbol
// must name a checked, concrete procedure in semantic.
[[nodiscard]] LlvmIrResult emit_llvm_package_static_data(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const Aarch64CAbiTable &abi,
    const ConstantTable &global_initializers,
    std::span<const SymbolId> package_procedures,
    DiagnosticSink &diagnostics);

// Emits one independently compilable LLVM module for one verified MIR
// procedure. The module defines only procedure and uses external declarations
// for package globals and every other concrete procedure. procedure_ordinal is
// the stable package-local MIR product order recorded in source-correlation
// rows and private constant names; it must not depend on worker scheduling.
[[nodiscard]] LlvmIrResult emit_llvm_machine_function(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const Aarch64CAbiTable &abi,
    std::span<const SymbolId> package_procedures,
    std::size_t procedure_ordinal,
    const MirProcedure &procedure,
    DiagnosticSink &diagnostics);

} // namespace draft
