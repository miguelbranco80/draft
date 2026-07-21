// Independently schedulable completion operations for semantic type facets.
//
// Declaration collection allocates nominal identity, and two independently
// scheduled type-resolution products later publish member names and declared
// member types. This module consumes those immutable earlier facets one product
// at a time. It does not inspect syntax, resolve names, select `when`, classify
// ABI arguments, or run a package retry.
//
// Natural-layout evaluation is read-only and task-owned. Publication is a
// separate coordinator operation that mutates exactly one canonical TypeStore
// row and its source-order AggregateMember offsets. This split is the seam used
// by the semantic work graph: workers may evaluate privately, while only the
// deterministic coordinator advances shared facet state.
//
// Relevant specification: docs/specification/02-types-memory-runtime.md,
// "Aggregate and layout types".

#pragma once

#include "sema/analyzer.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <vector>

namespace draft {

// TypeProductStatus is the terminal/wait vocabulary for one type-facet task.
// Blocked always carries explicit TypeFacetDependency rows. Error means the
// task already emitted a source-located diagnostic to its private sink.
enum class TypeProductStatus {
  Complete,
  Blocked,
  Error,
};

// NaturalLayoutProductAttempt owns one prospective natural-layout publication.
// Complete carries a known layout and one byte offset per nominal member.
// Blocked carries only exact prerequisites. No field aliases TypeStore memory,
// so the attempt remains valid while another task interns unrelated types.
struct NaturalLayoutProductAttempt {
  TypeProductStatus status = TypeProductStatus::Error;
  TypeLayout layout;
  std::vector<std::uint64_t> member_offsets;
  std::vector<TypeFacetDependency> dependencies;
};

// Evaluates the NaturalLayout facet for exactly nominal. MemberTypes must be
// complete. Inline member and tagged-discriminator waits become exact
// NaturalLayout dependencies; inline self-recursion therefore becomes a visible
// graph cycle, while a pointer member is already a complete pointer-sized input.
[[nodiscard]] NaturalLayoutProductAttempt evaluate_natural_layout_product(
    const TypeStore &types,
    TypeId nominal,
    DiagnosticSink &diagnostics);

// Publishes one successful attempt into the canonical semantic package. owner
// must be the symbol whose type is nominal, and its AggregateMember rows must
// still be in the same source order used by MemberTypes. Failure is an internal
// integration diagnostic; a blocked or erroneous attempt is never published.
[[nodiscard]] bool publish_natural_layout_product(
    SemanticPackage &package,
    SymbolId owner,
    TypeId nominal,
    NaturalLayoutProductAttempt attempt,
    DiagnosticSink &diagnostics);

} // namespace draft
