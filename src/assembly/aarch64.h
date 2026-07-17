// Parsed straight-line AArch64 assembly for the initial Draft target profile.

#pragma once

#include "sema/analyzer.h"
#include "sema/hir.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/package.h"

#include <cstddef>
#include <string>
#include <vector>

namespace draft {

struct AssemblyRegion {
  SyntaxReference syntax;
  TypeId result_type;
  std::size_t input_count = 0;
  std::size_t output_count = 0;
  std::string instruction_text;
  std::string llvm_constraints;
};

struct AssemblyProgram {
  bool ok = false;
  std::vector<AssemblyRegion> regions;

  [[nodiscard]] const AssemblyRegion *find(SyntaxReference syntax) const;
};

// Parses and validates every HIR assembly site against the closed
// draft-aarch64-apple-v1 vocabulary. The first instruction set intentionally
// covers straight-line integer, floating-point, fixed-vector, typed-memory, and
// barrier operations; unsupported instructions are hard diagnostics, never
// unparsed strings passed to LLVM.
[[nodiscard]] AssemblyProgram analyze_aarch64_assembly(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetProfile &target,
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics);

} // namespace draft
