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
#include <utility>

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

// Collects the exact incomplete natural-layout inputs in first-use order. An
// aggregate may mention the same incomplete type more than once; one graph edge
// is sufficient and gives cycle diagnostics a minimal dependency set.
[[nodiscard]] std::vector<TypeId> missing_layout_dependencies(
    const TypeStore &types, std::span<const TypeId> inputs) {
  std::vector<TypeId> result;
  for (TypeId input : inputs) {
    if (types.type(input).layout.known ||
        std::find(result.begin(), result.end(), input) != result.end()) {
      continue;
    }
    result.push_back(input);
  }
  return result;
}

[[nodiscard]] NaturalAggregateLayout waiting_layout(
    std::vector<TypeId> dependencies) {
  NaturalAggregateLayout result;
  result.status = NaturalLayoutStatus::Waiting;
  result.dependencies = std::move(dependencies);
  return result;
}

[[nodiscard]] NaturalAggregateLayout overflow_layout() {
  NaturalAggregateLayout result;
  result.status = NaturalLayoutStatus::Overflow;
  return result;
}

} // namespace

NaturalAggregateLayout compute_struct_natural_layout(
    const TypeStore &types,
    std::span<const TypeId> members,
    std::span<const FieldLayout> field_layouts) {
  assert(field_layouts.empty() || field_layouts.size() == members.size());
  std::vector<TypeId> dependencies =
      missing_layout_dependencies(types, members);
  if (!dependencies.empty()) return waiting_layout(std::move(dependencies));

  NaturalAggregateLayout result;
  result.status = NaturalLayoutStatus::Complete;
  result.layout = {true, 0, 1};
  result.member_offsets.reserve(members.size());
  result.member_bit_offsets.reserve(members.size());
  std::uint64_t next_bit = 0;
  for (std::size_t index = 0; index < members.size(); ++index) {
    const TypeId member_id = members[index];
    const TypeLayout member = types.type(member_id).layout;
    assert(member.known);
    const FieldLayout field = field_layouts.empty()
        ? FieldLayout{}
        : field_layouts[index];
    if (field.kind == FieldLayoutKind::BitField) {
      if (field.bit_width == 0 ||
          next_bit > std::numeric_limits<std::uint64_t>::max() -
              field.bit_width) {
        return overflow_layout();
      }
      result.member_offsets.push_back(next_bit / 8U);
      result.member_bit_offsets.push_back(next_bit);
      next_bit += field.bit_width;
      result.layout.size = next_bit / 8U + (next_bit % 8U != 0 ? 1U : 0U);
      continue;
    }

    // An ordinary field cannot begin in the unused high bits of the previous
    // byte. Close a preceding bit-field run first, then apply this field's own
    // natural or packed byte-alignment rule.
    const std::uint64_t next_byte =
        next_bit / 8U + (next_bit % 8U != 0 ? 1U : 0U);
    const bool packed = field.kind == FieldLayoutKind::Packed;
    const std::uint32_t effective_alignment =
        packed ? 1U : member.alignment;
    const std::optional<std::uint64_t> offset =
        round_up(next_byte, effective_alignment);
    if (!offset.has_value() ||
        member.size > std::numeric_limits<std::uint64_t>::max() - *offset) {
      return overflow_layout();
    }
    result.member_offsets.push_back(*offset);
    if (*offset > std::numeric_limits<std::uint64_t>::max() / 8U) {
      return overflow_layout();
    }
    result.member_bit_offsets.push_back(*offset * 8U);
    result.layout.size = *offset + member.size;
    if (result.layout.size > std::numeric_limits<std::uint64_t>::max() / 8U) {
      return overflow_layout();
    }
    next_bit = result.layout.size * 8U;
    result.layout.alignment =
        std::max(result.layout.alignment, effective_alignment);
  }
  const std::optional<std::uint64_t> size =
      round_up(result.layout.size, result.layout.alignment);
  if (!size.has_value()) return overflow_layout();
  result.layout.size = *size;
  return result;
}

NaturalAggregateLayout compute_union_natural_layout(
    const TypeStore &types, std::span<const TypeId> members) {
  std::vector<TypeId> dependencies =
      missing_layout_dependencies(types, members);
  if (!dependencies.empty()) return waiting_layout(std::move(dependencies));

  NaturalAggregateLayout result;
  result.status = NaturalLayoutStatus::Complete;
  result.layout = {true, 0, 1};
  result.member_offsets.assign(members.size(), 0);
  result.member_bit_offsets.assign(members.size(), 0);
  for (TypeId member_id : members) {
    const TypeLayout member = types.type(member_id).layout;
    assert(member.known);
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

NaturalAggregateLayout compute_variant_natural_layout(
    const TypeStore &types,
    TypeId discriminator,
    std::span<const TypeId> alternatives) {
  std::vector<TypeId> dependencies;
  if (!types.type(discriminator).layout.known) {
    dependencies.push_back(discriminator);
  }
  std::vector<TypeId> alternative_dependencies =
      missing_layout_dependencies(types, alternatives);
  for (TypeId dependency : alternative_dependencies) {
    if (std::find(dependencies.begin(), dependencies.end(), dependency) ==
        dependencies.end()) {
      dependencies.push_back(dependency);
    }
  }
  if (!dependencies.empty()) return waiting_layout(std::move(dependencies));

  const TypeLayout discriminator_layout = types.type(discriminator).layout;
  assert(discriminator_layout.known);

  std::uint64_t payload_size = 0;
  std::uint32_t payload_alignment = 1;
  for (TypeId alternative : alternatives) {
    const TypeLayout payload = types.type(alternative).layout;
    assert(payload.known);
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
  if (*payload_offset > std::numeric_limits<std::uint64_t>::max() / 8U) {
    return overflow_layout();
  }
  result.member_bit_offsets.assign(
      alternatives.size(), *payload_offset * 8U);
  return result;
}

} // namespace draft
