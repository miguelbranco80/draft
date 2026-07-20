// Pure target-natural layout computation for completed semantic member types.
//
// This module is the payload producer for the TypeNaturalLayout semantic
// product. Its inputs are an immutable TypeStore, an ordered member-type list,
// and, for tagged unions, the already selected discriminator type. Its output
// owns the exact aggregate layout and one byte offset per member. It does not
// declare symbols, resolve syntax, select `when`, evaluate constants, mutate a
// Type row, apply ABI call classification, or emit diagnostics.
//
// Waiting means at least one required input layout is not complete. Overflow
// means every input was complete but the specified size/rounding arithmetic
// exceeded u64. Keeping those states distinct lets the semantic work graph add
// dependencies for the former and terminalize the latter without retrying the
// declaration resolver.
//
// Relevant specification: docs/specification/02-types-memory-runtime.md,
// "Aggregate and layout types".

#pragma once

#include "sema/type.h"

#include <cstdint>
#include <span>
#include <vector>

namespace draft {

enum class NaturalLayoutStatus {
  Complete,
  Waiting,
  Overflow,
};

// NaturalAggregateLayout is task-owned until deterministic publication copies
// layout and member_offsets into the canonical Type row. member_offsets is
// populated only for Complete and has exactly the input member count.
struct NaturalAggregateLayout {
  NaturalLayoutStatus status = NaturalLayoutStatus::Waiting;
  TypeLayout layout;
  std::vector<std::uint64_t> member_offsets;
};

[[nodiscard]] NaturalAggregateLayout compute_struct_natural_layout(
    const TypeStore &types, std::span<const TypeId> members);

[[nodiscard]] NaturalAggregateLayout compute_raw_union_natural_layout(
    const TypeStore &types, std::span<const TypeId> members);

[[nodiscard]] NaturalAggregateLayout compute_tagged_union_natural_layout(
    const TypeStore &types,
    TypeId discriminator,
    std::span<const TypeId> alternatives);

} // namespace draft
