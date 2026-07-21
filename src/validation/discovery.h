// Typed discovery for Draft test and benchmark procedures.
//
// Validation files are ordinary package source selected by the command. This
// module does not parse filenames into executable guesses: it starts from the
// checked package scope, keeps only declarations physically anchored in the
// selected validation file kind, and proves the exact core/testing.Test or
// core/benchmark.Benchmark pointer signature before publishing an entry.

#pragma once

#include "sema/analyzer.h"
#include "sema/body_checker.h"
#include "source/diagnostic.h"
#include "workspace/package.h"
#include "workspace/workspace.h"

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

enum class ValidationKind {
  None,
  Test,
  Benchmark,
};

// ValidationEntry contains only target-checked facts needed by the native
// harness. State layout comes from the selected core nominal rather than being
// repeated as a backend constant. package and procedure form the stable source
// order key and the eventual Draft linker name.
struct ValidationEntry {
  ValidationKind kind = ValidationKind::None;
  PackageIdentity package;
  std::string procedure;
  std::uint64_t state_size = 0;
  std::uint64_t state_alignment = 1;
  std::uint64_t failure_offset = 0;
  // The harness reports the stable scalar prefix through and including the
  // failures field. Private user pointers never cross the runner boundary.
  std::uint64_t report_size = 0;

  bool operator==(const ValidationEntry &) const = default;
};

[[nodiscard]] std::string_view validation_kind_name(ValidationKind kind);

// Discovers one package in declaration order. Invalid candidate signatures are
// diagnosed at their declaration and omitted; the enclosing compilation fails
// through the shared DiagnosticSink. selected_indices address procedure-owned
// HIR products in canonical selected-program order. Discovery needs only body
// presence and therefore never constructs a package HIR projection.
[[nodiscard]] std::vector<ValidationEntry> discover_validation_entries(
    ValidationKind kind,
    std::string_view core_root_identity,
    const PackageIdentity &identity,
    const LoadedPackage &loaded,
    const SemanticPackage &semantic,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    DiagnosticSink &diagnostics);

// Package graph discovery order is deterministic but not the language's
// validation order. This explicit bytewise identity/declaration sort gives all
// runners and evidence serializers one canonical order.
void sort_validation_entries(std::vector<ValidationEntry> &entries);

} // namespace draft
