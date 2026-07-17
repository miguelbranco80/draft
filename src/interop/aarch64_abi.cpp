// AArch64 Darwin C ABI classification implementation.

#include "interop/aarch64_abi.h"

#include <cstddef>
#include <optional>

namespace draft {
namespace {

struct HomogeneousFloatInfo {
  std::uint32_t bits = 0;
  std::uint32_t count = 0;
};

[[nodiscard]] bool direct_scalar(const TypeStore &types, TypeId id) {
  const Type &type = types.type(id);
  switch (type.kind) {
  case TypeKind::SignedInteger:
  case TypeKind::UnsignedInteger:
  case TypeKind::Float:
  case TypeKind::Rune:
  case TypeKind::BooleanStorage:
  case TypeKind::EndianScalar:
  case TypeKind::RawPointer:
  case TypeKind::CString:
  case TypeKind::Pointer:
  case TypeKind::MultiPointer:
    return true;
  case TypeKind::Procedure:
    return type.c_calling_convention;
  case TypeKind::Enum:
    return type.c_representation && type.element.is_valid() &&
        direct_scalar(types, type.element);
  default:
    return false;
  }
}

// Member legality is recursive because C-represented aggregates may contain
// arrays and other C-represented aggregates by value. Pointer pointees are not
// traversed: C permits pointers to opaque and non-C Draft types.
[[nodiscard]] bool aggregate_member_legal(const TypeStore &types, TypeId id) {
  if (direct_scalar(types, id)) {
    return true;
  }
  const Type &type = types.type(id);
  if (type.kind == TypeKind::Array) {
    return aggregate_member_legal(types, type.element);
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::RawUnion) ||
      !type.c_representation || !type.layout.known) {
    return false;
  }
  for (TypeId member : type.members) {
    if (!aggregate_member_legal(types, member)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<HomogeneousFloatInfo> homogeneous_float(
    const TypeStore &types, TypeId id) {
  const Type &type = types.type(id);
  if (type.kind == TypeKind::Float &&
      (type.bit_width == 16 || type.bit_width == 32 || type.bit_width == 64)) {
    return HomogeneousFloatInfo{type.bit_width, 1};
  }
  if (type.kind == TypeKind::Array) {
    const std::optional<HomogeneousFloatInfo> element =
        homogeneous_float(types, type.element);
    if (!element.has_value() || type.element_count == 0 ||
        type.element_count > 4 ||
        element->count > 4 / type.element_count) {
      return std::nullopt;
    }
    return HomogeneousFloatInfo{
        element->bits,
        static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(element->count) * type.element_count),
    };
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::RawUnion) ||
      !type.c_representation || type.members.empty()) {
    return std::nullopt;
  }

  std::optional<HomogeneousFloatInfo> result;
  for (TypeId member : type.members) {
    const std::optional<HomogeneousFloatInfo> candidate =
        homogeneous_float(types, member);
    if (!candidate.has_value()) {
      return std::nullopt;
    }
    if (!result.has_value()) {
      result = candidate;
      continue;
    }
    if (candidate->bits != result->bits) {
      return std::nullopt;
    }
    if (type.kind == TypeKind::RawUnion) {
      // Every union view must describe the same homogeneous register shape.
      if (candidate->count != result->count) {
        return std::nullopt;
      }
    } else {
      if (candidate->count > 4 - result->count) {
        return std::nullopt;
      }
      result->count += candidate->count;
    }
  }
  if (!result.has_value() || result->count == 0 || result->count > 4) {
    return std::nullopt;
  }

  // Extra tail padding, including explicit over-alignment, disqualifies an
  // otherwise homogeneous aggregate in Darwin Clang's arm64 ABI lowering.
  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(result->bits / 8U) * result->count;
  if (!type.layout.known || type.layout.size != expected_size) {
    return std::nullopt;
  }
  return result;
}

} // namespace

Aarch64CAbiType classify_aarch64_darwin_c_type(
    const TypeStore &types, TypeId type_id) {
  const Type &type = types.type(type_id);
  Aarch64CAbiType result;
  result.size = type.layout.size;
  result.alignment = type.layout.alignment;

  if (direct_scalar(types, type_id)) {
    result.classification = Aarch64CAbiClass::Direct;
    return result;
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::RawUnion) ||
      !type.c_representation || !type.layout.known || type.layout.size == 0 ||
      !aggregate_member_legal(types, type_id)) {
    return result;
  }

  if (const std::optional<HomogeneousFloatInfo> homogeneous =
          homogeneous_float(types, type_id)) {
    result.classification = Aarch64CAbiClass::HomogeneousFloatAggregate;
    result.homogeneous_element_bits = homogeneous->bits;
    result.homogeneous_element_count = homogeneous->count;
    return result;
  }

  if (type.layout.size <= 16) {
    result.classification = Aarch64CAbiClass::SmallAggregate;
    if (type.layout.size <= 8) {
      result.argument_integer_bits = 64;
      result.argument_integer_count = 1;
      result.result_integer_bits =
          static_cast<std::uint32_t>(type.layout.size * 8U);
      result.result_integer_count = 1;
    } else if (type.layout.alignment >= 16 && type.layout.size == 16) {
      result.argument_integer_bits = 128;
      result.argument_integer_count = 1;
      result.result_integer_bits = 128;
      result.result_integer_count = 1;
    } else {
      result.argument_integer_bits = 64;
      result.argument_integer_count = 2;
      result.result_integer_bits = 64;
      result.result_integer_count = 2;
    }
    return result;
  }

  result.classification = Aarch64CAbiClass::Indirect;
  return result;
}

} // namespace draft
