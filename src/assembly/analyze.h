// Target-dispatched parsed-assembly analysis.
//
// This module is the narrow compiler seam between target-independent HIR and
// an implemented, versioned parsed-assembly grammar. It owns no syntax, HIR,
// or assembly rows: inputs are borrowed for one synchronous analysis and the
// returned AssemblyProgram owns the validated source-keyed regions consumed by
// MIR lowering. A target which emits native code but has no parsed grammar is
// handled explicitly here, so it cannot fall through to another architecture's
// register and instruction rules.
//
// Relevant specification: docs/specification/04-native-interop.md section 11.

#pragma once

#include "assembly/aarch64.h"

namespace draft {

// Analyzes every assembly site in one procedure-owned HIR product. The target
// profile must already be internally valid. Unsupported parsed assembly emits
// one exact source diagnostic per site; a product with no assembly remains a
// successful empty product on every native target.
[[nodiscard]] AssemblyProgram analyze_target_assembly(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetProfile &target,
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics);

} // namespace draft
