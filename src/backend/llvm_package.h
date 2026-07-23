// Value-only configuration and retained text for one package LLVM module.
//
// This header is the LLVM package boundary shared by compiler orchestration,
// direct in-process construction, artifact planning, and inspection clients. It
// intentionally exposes no LLVM handles and defines no alternate emitter. A
// package module is constructed exactly once through llvm_package_emitter; an
// explicit `emit-llvm` or qualification request may retain the printed,
// pre-optimization text in LlvmIrResult.
//
// Package identity and validation entries are immutable semantic inputs. Debug
// information, entry-wrapper selection, and text retention are operational
// artifact choices. Relevant specification: docs/specification/06-compiler.md
// "Native lowering and summaries".

#pragma once

#include "validation/discovery.h"
#include "workspace/workspace.h"

#include <string>
#include <vector>

namespace draft {

// LlvmIrOptions contains the package-level choices needed while constructing
// one direct LLVM module. Debug information changes derived inspection/native
// artifacts only. Runtime support is a separate embedded target object, so
// package-module configuration has no switch for embedding it.
struct LlvmIrOptions {
  // Supplies the stable semantic package prefix used for every non-native
  // symbol and the logical module/source name. Physical checkout paths never
  // enter those names.
  PackageIdentity package;
  // Opts into task-local DIBuilder metadata. It changes only derived debug
  // artifacts and is false for ordinary fast builds.
  bool emit_debug_information = false;
  // Executable roots own the hosted C main/wmain adapter; library roots and
  // non-root package units do not. A validation root selects its compiler-owned
  // harness through validation_kind and validation_entries below.
  bool emit_program_entry = false;
  // None selects the ordinary root main/wmain adapter. Test or Benchmark
  // selects the matching validation harness over the already checked entries.
  ValidationKind validation_kind = ValidationKind::None;
  // Entries are in canonical validation order and are borrowed semantically by
  // value for the synchronous module build. They are empty for ordinary roots.
  std::vector<ValidationEntry> validation_entries;
};

// LlvmIrResult is retained inspection text from the direct package builder.
// text is the verified pre-optimization module and is populated only for an
// explicit IR consumer or qualification oracle. Native-only builds leave the
// result empty instead of printing an intermediate representation.
struct LlvmIrResult {
  // True means direct construction and LLVM verification both completed
  // without adding a backend diagnostic.
  bool ok = false;
  // Owns LLVMPrintModuleToString output only when retention was requested.
  std::string text;
};

} // namespace draft
