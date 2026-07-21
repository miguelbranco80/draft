// Stable native object work derived from a completely lowered package graph.
//
// This module is the boundary between compiler-owned package products and an
// object emitter. It converts every complete package LLVM module and selected
// package-assembly input into one explicit task. Task IDs, stems, and diagnostic
// names are fixed before execution; emitters may run tasks concurrently but
// must place products back into corresponding task-indexed result slots.
//
// Input byte views borrow CompileWorkspaceResult storage for the synchronous
// native build and retain nothing afterward. The plan owns only small metadata
// and an edgeless WorkGraph: after semantic closure and target lowering, every
// package module contains external declarations for dependency symbols and can
// be emitted independently. Linking is a later ordered publication phase.
//
// This module depends on compile products, target profiles, and the base work
// graph. It deliberately knows nothing about LLVM APIs, subprocesses, object
// formats, link arguments, diagnostics rendering, or filesystem publication.
// Relevant specification: docs/specification/04-native-interop.md section 12
// and docs/specification/06-compiler.md "Native lowering and summaries".

#pragma once

#include "base/work_graph.h"
#include "compile/compiler.h"
#include "target/profile.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// Identifies which already selected input route produces one object product.
// The value affects execution and output naming inside one command but is not a
// persistent semantic identity or content-hash field.
enum class NativeObjectTaskKind {
  PackageLlvmModule,
  PackageAssembly,
};

// One task names one independently emittable native input. package_index is in
// CompileWorkspaceResult::packages. input_index addresses the package's
// assembly-source vector and is zero for its LLVM module. output_stem is
// collision-free within one native build directory and contains no physical
// source path. source_extension is ".ll" for an LLVM unit
// or the exact selected assembly extension for a package source. producer is
// the exact semantic product copied from PackageArtifactLayout; this preserves
// graph-to-object identity without making workers inspect compiler side tables.
struct NativeObjectTask {
  NativeObjectTaskKind kind = NativeObjectTaskKind::PackageLlvmModule;
  std::size_t package_index = 0;
  std::size_t input_index = 0;
  SemanticProductId producer;
  std::string display_name;
  std::string output_stem;
  std::string source_extension;
  std::string_view input_bytes;
};

// The task and graph vectors have exactly matching index domains. WorkTaskId is
// therefore also the NativeObjectTask ID. Native emission starts only after all
// packages reach target lowering, so every graph row is intentionally empty;
// the explicit graph still gives the shared scheduler one honest ready set and
// stable result domain rather than hiding parallelism inside an emitter loop.
struct NativeObjectPlan {
  WorkGraph graph;
  std::vector<NativeObjectTask> tasks;
};

// Copies each package's already published ArtifactLayout in ascending package
// order. It validates every selected package module and assembly rule before
// publishing a partial plan. On failure plan is empty and reason names the
// first package/input in stable order.
[[nodiscard]] bool prepare_native_object_plan(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    NativeObjectPlan &plan,
    std::string &reason);

} // namespace draft
