// Deterministic textual LLVM IR emission from verified Draft MIR.

#pragma once

#include "mir/mir.h"
#include "sema/analyzer.h"
#include "sema/constant.h"
#include "source/diagnostic.h"
#include "target/profile.h"
#include "workspace/workspace.h"

#include <string>

namespace draft {

struct LlvmIrOptions {
  PackageIdentity package;
  bool emit_program_entry = false;
};

struct LlvmIrResult {
  bool ok = false;
  std::string text;
};

// The emitter produces opaque-pointer LLVM IR and never invokes a toolchain.
// Object emission and linking are separate adapters that must verify the pinned
// LLVM distribution. The textual form is intentionally testable without LLVM
// headers or libraries on the bootstrap compiler's build machine.
[[nodiscard]] LlvmIrResult emit_llvm_ir(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const ConstantTable &constants,
    const MirProgram &mir,
    DiagnosticSink &diagnostics);

} // namespace draft
