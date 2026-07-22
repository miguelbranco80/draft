// Shared Darwin/GNU AArch64 C ABI aggregate classification implementation.

#include "interop/aarch64_abi.h"

#include "sema/constant.h"
#include "target/profile.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace draft {
namespace {

struct HomogeneousFloatInfo {
  std::uint32_t bits = 0;
  std::uint32_t count = 0;
};

[[nodiscard]] Aarch64CAbiType classify_with_active_procedures(
    const TypeStore &types,
    TypeId type_id,
    std::vector<TypeId> &active_procedures);

[[nodiscard]] bool direct_scalar(
    const TypeStore &types,
    TypeId id,
    std::vector<TypeId> &active_procedures) {
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
  case TypeKind::Procedure: {
    // A procedure value is a C function pointer only when its complete nested
    // signature is C-ABI legal. Checking just the calling-convention bit would
    // incorrectly admit callbacks such as `c proc(value: []u8)` as fields of a
    // C record, even though the slice cannot cross that callback boundary.
    if (!type.c_calling_convention || type.members.empty()) return false;
    // C records and callback signatures may refer to one another recursively:
    // `struct Node { void (*visit)(struct Node); }` is a finite C layout even
    // though its type graph has a cycle. Reaching a procedure already being
    // checked means the cycle itself has supplied no illegal leaf; accept that
    // edge provisionally while the outer walk validates the remaining fields.
    if (std::find(active_procedures.begin(), active_procedures.end(), id) !=
        active_procedures.end()) {
      return true;
    }
    active_procedures.push_back(id);
    for (std::size_t index = 0; index + 1 < type.members.size(); ++index) {
      if (classify_with_active_procedures(
              types, type.members[index], active_procedures)
              .classification == Aarch64CAbiClass::Illegal) {
        active_procedures.pop_back();
        return false;
      }
    }
    const TypeId result = type.members.back();
    const bool legal_result = result == types.builtins().void_type ||
        classify_with_active_procedures(types, result, active_procedures)
                .classification != Aarch64CAbiClass::Illegal;
    active_procedures.pop_back();
    return legal_result;
  }
  case TypeKind::Enum:
    return type.c_representation && type.element.is_valid() &&
        direct_scalar(types, type.element, active_procedures);
  default:
    return false;
  }
}

// Member legality is recursive because C-represented aggregates may contain
// arrays and other C-represented aggregates by value. Pointer pointees are not
// traversed: C permits pointers to opaque and non-C Draft types.
[[nodiscard]] bool aggregate_member_legal(
    const TypeStore &types,
    TypeId id,
    std::vector<TypeId> &active_procedures) {
  if (direct_scalar(types, id, active_procedures)) {
    return true;
  }
  const Type &type = types.type(id);
  if (type.kind == TypeKind::Array) {
    // C11 has no zero-length fixed array. Clang accepts one as an extension,
    // but using that extension here would make Draft C layout depend on host
    // flags and would let generated -std=c11 headers describe a non-C type.
    // A pointer to the Draft array remains legal through the ordinary opaque
    // pointer rule; only embedding the array by value is rejected.
    return type.element_count != 0 &&
        aggregate_member_legal(types, type.element, active_procedures);
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
      !type.c_representation || !type.layout.known) {
    return false;
  }
  for (TypeId member : type.members) {
    if (!aggregate_member_legal(types, member, active_procedures)) {
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
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
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
    if (type.kind == TypeKind::Union) {
      // A union overlays its members, so its homogeneous element count is
      // the largest alternative rather than the sum used by a struct.  The
      // alternatives need only share the same basic floating element type;
      // they do not need to contain the same number of elements.  For example,
      // Darwin arm64 passes `union { float scalar; float pair[2]; }` as two
      // floating lanes.  Requiring equal counts here would silently send that
      // source-compatible C union through integer registers instead.
      result->count = std::max(result->count, candidate->count);
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

[[nodiscard]] Aarch64CAbiType classify_with_active_procedures(
    const TypeStore &types,
    TypeId type_id,
    std::vector<TypeId> &active_procedures) {
  const Type &type = types.type(type_id);
  Aarch64CAbiType result;
  result.size = type.layout.size;
  result.alignment = type.layout.alignment;

  if (direct_scalar(types, type_id, active_procedures)) {
    result.classification = Aarch64CAbiClass::Direct;
    return result;
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
      !type.c_representation || !type.layout.known || type.layout.size == 0 ||
      !aggregate_member_legal(types, type_id, active_procedures)) {
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

} // namespace

Aarch64CAbiType classify_aarch64_c_type(
    const TypeStore &types,
    TypeId type_id,
    const TargetFacts &target) {
  // Both current targets use the fixed-arity AAPCS64 aggregate rules below.
  // Do not let a future ABI spelling inherit them merely because its machine
  // architecture is AArch64: adding a profile requires naming the complete ABI
  // here and adding its independent oracle tests.
  if (target.arch != "aarch64" ||
      (target.abi != "darwin_arm64" && target.abi != "aapcs64_gnu")) {
    return {};
  }
  std::vector<TypeId> active_procedures;
  return classify_with_active_procedures(types, type_id, active_procedures);
}

const Aarch64CAbiType *Aarch64CAbiTable::find(TypeId type) const {
  if (!type.is_valid() || type.value >= rows.size()) return nullptr;
  return &rows[type.value];
}

bool Aarch64CAbiTable::complete_for(
    const TypeStore &types,
    const TargetFacts &target) const {
  return valid_prefix_for(types, target) && rows.size() == types.size();
}

bool Aarch64CAbiTable::valid_prefix_for(
    const TypeStore &types,
    const TargetFacts &target) const {
  return target_identity == target.identity && rows.size() <= types.size();
}

Aarch64CAbiTable classify_aarch64_c_types(
    const TypeStore &types,
    const TargetFacts &target) {
  Aarch64CAbiTable result;
  result.target_identity = target.identity;
  result.rows.reserve(types.size());
  for (std::size_t index = 0; index < types.size(); ++index) {
    result.rows.push_back(classify_aarch64_c_type(
        types, TypeId{static_cast<std::uint32_t>(index)}, target));
  }
  return result;
}

} // namespace draft
