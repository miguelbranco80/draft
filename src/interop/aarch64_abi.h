// AArch64 C ABI classification for semantic Draft types.
//
// This module says how one already validated C source type crosses a native
// call boundary. It intentionally does not emit LLVM syntax: the same compact
// result drives declaration signatures, call-site packing, entry unpacking,
// and return handling. Keeping the decision here prevents those four sites
// from growing similar but subtly incompatible size heuristics.

#pragma once

#include "sema/type.h"

#include <cstdint>
#include <string>
#include <vector>

namespace draft {

struct TargetFacts;

enum class Aarch64CAbiClass {
  Illegal,
  Direct,
  HomogeneousFloatAggregate,
  SmallAggregate,
  Indirect,
};

struct Aarch64CAbiType {
  Aarch64CAbiClass classification = Aarch64CAbiClass::Illegal;
  std::uint64_t size = 0;
  std::uint32_t alignment = 1;

  // HomogeneousFloatAggregate uses one f16/f32/f64 lane kind repeated one to
  // four times. SmallAggregate uses integer containers: arguments occupy one
  // or two 64-bit registers, while results at most eight bytes use exactly the
  // meaningful bit count (i8, i24, i40, and so on).
  std::uint32_t homogeneous_element_bits = 0;
  std::uint32_t homogeneous_element_count = 0;
  std::uint32_t argument_integer_bits = 0;
  std::uint32_t argument_integer_count = 0;
  std::uint32_t result_integer_bits = 0;
  std::uint32_t result_integer_count = 0;

  bool operator==(const Aarch64CAbiType &) const = default;
};

// Aarch64CAbiTable is the immutable target-specific ABI facet for one package's
// source-semantic TypeId prefix. rows are indexed directly by TypeId and include
// Illegal as an ordinary completed result for types which cannot cross a C
// boundary. target_identity prevents a table classified for one profile from
// being consumed by another profile with coincidentally similar aggregate
// rules. Workspace compilation publishes rows through TypeAbiClassification
// products; classify_aarch64_c_types constructs the same table for direct
// subsystem tests.
struct Aarch64CAbiTable {
  std::string target_identity;
  std::vector<Aarch64CAbiType> rows;

  [[nodiscard]] const Aarch64CAbiType *find(TypeId type) const;
  // A semantic ABI table remains valid after MIR appends address-only pointer
  // types to the shared TypeStore. Those suffix rows cannot appear in a source
  // C signature and therefore have no TypeAbiClassification product. Consumers
  // which run after MIR use this prefix invariant and still require every ABI
  // query they make to resolve through find().
  [[nodiscard]] bool valid_prefix_for(
      const TypeStore &types,
      const TargetFacts &target) const;
  // Semantic validation runs before MIR and requires the stronger invariant:
  // every currently canonical source-semantic TypeId has a published row.
  [[nodiscard]] bool complete_for(
      const TypeStore &types,
      const TargetFacts &target) const;
};

// Classifies a direct C parameter/result source type for one supported AArch64
// ABI.  Darwin arm64 and GNU AAPCS64 share the aggregate register classes used
// here; target-specific scalar extension attributes and symbol spelling remain
// in their emission layers.  Arrays are legal recursively as aggregate members
// but not as direct parameters or results.  An Illegal result also covers a
// non-AArch64 or unknown ABI, so semantic validation fails closed before LLVM.
[[nodiscard]] Aarch64CAbiType classify_aarch64_c_type(
    const TypeStore &types, TypeId type, const TargetFacts &target);

// Builds the complete table directly and deterministically. The workspace
// compiler does not use this aggregate path: it schedules the same pure
// per-TypeId operation as explicit products. Direct interop/backend unit tests
// use it when they intentionally bypass workspace orchestration.
[[nodiscard]] Aarch64CAbiTable classify_aarch64_c_types(
    const TypeStore &types,
    const TargetFacts &target);

} // namespace draft
