// Direct implementation of Draft target-natural aggregate layout.
//
// See type_layout.h for ownership and phase boundaries. All arithmetic is
// checked before addition or rounding. Traversal follows source member order,
// so output offsets are deterministic and already aligned with the symbol/type
// parallel arrays owned by semantic analysis.

#include "sema/type_layout.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>

namespace draft {
namespace {

[[nodiscard]] std::optional<std::uint64_t> round_up(
    std::uint64_t value, std::uint32_t alignment) {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1);
  if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
    return std::nullopt;
  }
  return (value + mask) & ~mask;
}

[[nodiscard]] NaturalAggregateLayout waiting_layout() {
  return {NaturalLayoutStatus::Waiting, {}, {}};
}

[[nodiscard]] NaturalAggregateLayout overflow_layout() {
  return {NaturalLayoutStatus::Overflow, {}, {}};
}

} // namespace

NaturalAggregateLayout compute_struct_natural_layout(
    const TypeStore &types, std::span<const TypeId> members) {
  NaturalAggregateLayout result;
  result.status = NaturalLayoutStatus::Complete;
  result.layout = {true, 0, 1};
  result.member_offsets.reserve(members.size());
  for (TypeId member_id : members) {
    const TypeLayout member = types.type(member_id).layout;
    if (!member.known) return waiting_layout();
    const std::optional<std::uint64_t> offset =
        round_up(result.layout.size, member.alignment);
    if (!offset.has_value() ||
        member.size > std::numeric_limits<std::uint64_t>::max() - *offset) {
      return overflow_layout();
    }
    result.member_offsets.push_back(*offset);
    result.layout.size = *offset + member.size;
    result.layout.alignment =
        std::max(result.layout.alignment, member.alignment);
  }
  const std::optional<std::uint64_t> size =
      round_up(result.layout.size, result.layout.alignment);
  if (!size.has_value()) return overflow_layout();
  result.layout.size = *size;
  return result;
}

NaturalAggregateLayout compute_raw_union_natural_layout(
    const TypeStore &types, std::span<const TypeId> members) {
  NaturalAggregateLayout result;
  result.status = NaturalLayoutStatus::Complete;
  result.layout = {true, 0, 1};
  result.member_offsets.assign(members.size(), 0);
  for (TypeId member_id : members) {
    const TypeLayout member = types.type(member_id).layout;
    if (!member.known) return waiting_layout();
    result.layout.size = std::max(result.layout.size, member.size);
    result.layout.alignment =
        std::max(result.layout.alignment, member.alignment);
  }
  const std::optional<std::uint64_t> size =
      round_up(result.layout.size, result.layout.alignment);
  if (!size.has_value()) return overflow_layout();
  result.layout.size = *size;
  return result;
}

NaturalAggregateLayout compute_tagged_union_natural_layout(
    const TypeStore &types,
    TypeId discriminator,
    std::span<const TypeId> alternatives) {
  const TypeLayout discriminator_layout = types.type(discriminator).layout;
  if (!discriminator_layout.known) return waiting_layout();

  std::uint64_t payload_size = 0;
  std::uint32_t payload_alignment = 1;
  for (TypeId alternative : alternatives) {
    const TypeLayout payload = types.type(alternative).layout;
    if (!payload.known) return waiting_layout();
    payload_size = std::max(payload_size, payload.size);
    payload_alignment = std::max(payload_alignment, payload.alignment);
  }

  const std::optional<std::uint64_t> rounded_payload =
      round_up(payload_size, payload_alignment);
  const std::optional<std::uint64_t> payload_offset =
      round_up(discriminator_layout.size, payload_alignment);
  if (!rounded_payload.has_value() || !payload_offset.has_value() ||
      *rounded_payload >
          std::numeric_limits<std::uint64_t>::max() - *payload_offset) {
    return overflow_layout();
  }

  const std::uint32_t alignment =
      std::max(discriminator_layout.alignment, payload_alignment);
  const std::optional<std::uint64_t> size =
      round_up(*payload_offset + *rounded_payload, alignment);
  if (!size.has_value()) return overflow_layout();

  NaturalAggregateLayout result;
  result.status = NaturalLayoutStatus::Complete;
  result.layout = {true, *size, alignment};
  result.member_offsets.assign(alternatives.size(), *payload_offset);
  return result;
}

} // namespace draft
