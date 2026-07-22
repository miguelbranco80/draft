// Target C ABI classification for semantic Draft types.
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

enum class CAbiClass {
  Illegal,
  Direct,
  HomogeneousFloatAggregate,
  SmallAggregate,
  Indirect,
};

struct CAbiType {
  CAbiClass classification = CAbiClass::Illegal;
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

  bool operator==(const CAbiType &) const = default;
};

// CAbiTable is the immutable target-specific ABI facet for one package's
// source-semantic TypeId prefix. rows are indexed directly by TypeId and include
// Illegal as an ordinary completed result for types which cannot cross a C
// boundary. target_identity prevents a table classified for one profile from
// being consumed by another profile with coincidentally similar aggregate
// rules. Workspace compilation publishes rows through TypeAbiClassification
// products; classify_c_types constructs the same table for direct
// subsystem tests.
struct CAbiTable {
  std::string target_identity;
  std::vector<CAbiType> rows;

  [[nodiscard]] const CAbiType *find(TypeId type) const;
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

// Classifies a direct C parameter/result source type for the selected ABI.
// Arrays are legal recursively as aggregate members but not as direct
// parameters or results. An Illegal result also covers an unsupported ABI, so
// semantic validation fails closed before LLVM. The architecture-specific
// classifier remains private to this module; consumers receive only this one
// target-owned product and cannot accidentally choose a different ABI rule.
[[nodiscard]] CAbiType classify_c_type(
    const TypeStore &types, TypeId type, const TargetFacts &target);

// Builds the complete table directly and deterministically. The workspace
// compiler does not use this aggregate path: it schedules the same pure
// per-TypeId operation as explicit products. Direct interop/backend unit tests
// use it when they intentionally bypass workspace orchestration.
[[nodiscard]] CAbiTable classify_c_types(
    const TypeStore &types,
    const TargetFacts &target);

} // namespace draft
