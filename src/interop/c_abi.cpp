// Target-dispatched C ABI aggregate classification implementation.

#include "interop/c_abi.h"

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

[[nodiscard]] CAbiType
classify_with_active_procedures(const TypeStore &types, TypeId type_id,
                                const TargetFacts &target,
                                std::vector<TypeId> &active_procedures);

[[nodiscard]] bool direct_scalar(const TypeStore &types, TypeId id,
                                 const TargetFacts &target,
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
    if (!type.c_calling_convention || type.members.empty())
      return false;
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
      if (classify_with_active_procedures(types, type.members[index], target,
                                          active_procedures)
              .classification == CAbiClass::Illegal) {
        active_procedures.pop_back();
        return false;
      }
    }
    const TypeId result = type.members.back();
    const bool legal_result = result == types.builtins().void_type ||
                              classify_with_active_procedures(
                                  types, result, target, active_procedures)
                                      .classification != CAbiClass::Illegal;
    active_procedures.pop_back();
    return legal_result;
  }
  case TypeKind::Enum:
    return type.c_representation && type.element.is_valid() &&
           direct_scalar(types, type.element, target, active_procedures);
  default:
    return false;
  }
}

// Member legality is recursive because C-represented aggregates may contain
// arrays and other C-represented aggregates by value. Pointer pointees are not
// traversed: C permits pointers to opaque and non-C Draft types.
[[nodiscard]] bool
aggregate_member_legal(const TypeStore &types, TypeId id,
                       const TargetFacts &target,
                       std::vector<TypeId> &active_procedures) {
  if (direct_scalar(types, id, target, active_procedures)) {
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
           aggregate_member_legal(types, type.element, target,
                                  active_procedures);
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
      !type.c_representation || !type.layout.known) {
    return false;
  }
  for (TypeId member : type.members) {
    if (!aggregate_member_legal(types, member, target, active_procedures)) {
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

[[nodiscard]] CAbiType classify_aarch64_with_active_procedures(
    const TypeStore &types, TypeId type_id, const TargetFacts &target,
    std::vector<TypeId> &active_procedures) {
  const Type &type = types.type(type_id);
  CAbiType result;
  result.size = type.layout.size;
  result.alignment = type.layout.alignment;

  if (direct_scalar(types, type_id, target, active_procedures)) {
    result.classification = CAbiClass::Direct;
    return result;
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
      !type.c_representation || !type.layout.known || type.layout.size == 0 ||
      !aggregate_member_legal(types, type_id, target, active_procedures)) {
    return result;
  }

  if (const std::optional<HomogeneousFloatInfo> homogeneous =
          homogeneous_float(types, type_id)) {
    result.classification = CAbiClass::HomogeneousFloatAggregate;
    result.homogeneous_element_bits = homogeneous->bits;
    result.homogeneous_element_count = homogeneous->count;
    return result;
  }

  if (type.layout.size <= 16) {
    result.classification = CAbiClass::SmallAggregate;
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

  result.classification = CAbiClass::Indirect;
  return result;
}

// SysV AMD64's complete class vocabulary includes x87 and vector continuation
// classes. Draft's legal C surface has neither long double nor C vector values,
// so recursively legal Draft aggregates can reach only INTEGER and SSE. The
// temporary occupied_bytes value records the last source byte represented in
// an SSE eightbyte; it lets the LLVM adapter choose half, float, or double as a
// bit-preserving XMM carrier without treating tail padding as a second class.
struct SysvEightbyteState {
  CAbiEightbyteClass classification = CAbiEightbyteClass::None;
  std::uint32_t occupied_bytes = 0;
};

[[nodiscard]] CAbiEightbyteClass merge_sysv_class(CAbiEightbyteClass left,
                                                  CAbiEightbyteClass right) {
  if (left == right)
    return left;
  if (left == CAbiEightbyteClass::None)
    return right;
  if (right == CAbiEightbyteClass::None)
    return left;
  if (left == CAbiEightbyteClass::Integer ||
      right == CAbiEightbyteClass::Integer) {
    return CAbiEightbyteClass::Integer;
  }
  return CAbiEightbyteClass::Sse;
}

[[nodiscard]] CAbiEightbyteClass sysv_scalar_class(const TypeStore &types,
                                                   TypeId type_id) {
  const Type &type = types.type(type_id);
  if (type.kind == TypeKind::Enum && type.element.is_valid()) {
    return sysv_scalar_class(types, type.element);
  }
  return type.kind == TypeKind::Float ? CAbiEightbyteClass::Sse
                                      : CAbiEightbyteClass::Integer;
}

// Recursively merges one member's occupied bytes into at most two eightbytes.
// base is the member's byte offset in the outermost aggregate. Natural C layout
// should already align every field; retaining the explicit check makes a future
// packed C form fail closed instead of silently receiving the wrong registers.
[[nodiscard]] bool
classify_sysv_member(const TypeStore &types, TypeId type_id, std::uint64_t base,
                     std::array<SysvEightbyteState, 2> &states) {
  const Type &type = types.type(type_id);
  if (!type.layout.known || type.layout.size == 0 ||
      type.layout.alignment == 0 || base % type.layout.alignment != 0 ||
      base > 16 || type.layout.size > 16 - base) {
    return false;
  }

  if (type.kind == TypeKind::Enum || type.kind == TypeKind::SignedInteger ||
      type.kind == TypeKind::UnsignedInteger || type.kind == TypeKind::Float ||
      type.kind == TypeKind::Rune || type.kind == TypeKind::BooleanStorage ||
      type.kind == TypeKind::EndianScalar ||
      type.kind == TypeKind::RawPointer || type.kind == TypeKind::CString ||
      type.kind == TypeKind::Pointer || type.kind == TypeKind::MultiPointer ||
      type.kind == TypeKind::Procedure) {
    const CAbiEightbyteClass classification = sysv_scalar_class(types, type_id);
    const std::uint64_t last = base + type.layout.size;
    for (std::uint64_t byte = base; byte < last; ++byte) {
      const std::size_t component = static_cast<std::size_t>(byte / 8U);
      states[component].classification =
          merge_sysv_class(states[component].classification, classification);
      const std::uint32_t occupied = static_cast<std::uint32_t>(byte % 8U + 1U);
      states[component].occupied_bytes =
          std::max(states[component].occupied_bytes, occupied);
    }
    return true;
  }

  if (type.kind == TypeKind::Array) {
    const Type &element = types.type(type.element);
    if (!element.layout.known || element.layout.size == 0)
      return false;
    for (std::uint64_t index = 0; index < type.element_count; ++index) {
      if (!classify_sysv_member(types, type.element,
                                base + index * element.layout.size, states)) {
        return false;
      }
    }
    return true;
  }

  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
      !type.c_representation ||
      type.member_offsets.size() != type.members.size()) {
    return false;
  }
  for (std::size_t index = 0; index < type.members.size(); ++index) {
    if (!classify_sysv_member(types, type.members[index],
                              base + type.member_offsets[index], states)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] CAbiType classify_sysv_amd64_with_active_procedures(
    const TypeStore &types, TypeId type_id, const TargetFacts &target,
    std::vector<TypeId> &active_procedures) {
  const Type &type = types.type(type_id);
  CAbiType result;
  result.size = type.layout.size;
  result.alignment = type.layout.alignment;

  if (direct_scalar(types, type_id, target, active_procedures)) {
    result.classification = CAbiClass::Direct;
    return result;
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
      !type.c_representation || !type.layout.known || type.layout.size == 0 ||
      !aggregate_member_legal(types, type_id, target, active_procedures)) {
    return result;
  }

  // The baseline Draft C surface has no SSEUP-producing vector member. Every
  // ordinary aggregate larger than two eightbytes is therefore MEMORY after
  // the psABI post-merge cleanup.
  if (type.layout.size > 16) {
    result.classification = CAbiClass::Indirect;
    return result;
  }

  std::array<SysvEightbyteState, 2> states{};
  if (!classify_sysv_member(types, type_id, 0, states)) {
    result.classification = CAbiClass::Indirect;
    return result;
  }
  std::size_t live_count = 0;
  for (std::size_t index = 0; index < states.size(); ++index) {
    if (states[index].classification != CAbiEightbyteClass::None) {
      live_count = index + 1;
    }
  }
  if (live_count == 0)
    return result;

  result.classification = CAbiClass::EightbyteAggregate;
  result.eightbyte_count = static_cast<std::uint32_t>(live_count);
  for (std::size_t index = 0; index < live_count; ++index) {
    if (states[index].classification == CAbiEightbyteClass::None) {
      // A live second component cannot follow a pure-padding first component
      // in Draft's nonempty naturally laid-out C aggregates.
      return CAbiType{};
    }
    result.eightbytes[index].classification = states[index].classification;
    // LLVM's public ABI carrier ends at the last occupied source byte, not at
    // the aggregate's tail padding. For example, Clang lowers
    // `struct { double; int; }` to `{ double, i32 }`, despite sizeof returning
    // 16. Interior padding remains represented because occupied_bytes is the
    // greatest live byte offset within the component rather than a byte count.
    result.eightbytes[index].bits = states[index].occupied_bytes * 8U;
  }
  return result;
}

// Microsoft x64 treats a naturally laid-out record as one integer scalar only
// when its complete size is exactly 1, 2, 4, or 8 bytes. Every other record is
// passed by reference; a result uses the ABI's hidden return pointer. Record
// contents do not create SSE classes: even an eight-byte pair of floats travels
// in the integer slot. These compact rules are intentionally separate from the
// SysV eightbyte walk despite sharing the x86-64 instruction architecture.
[[nodiscard]] CAbiType classify_win64_with_active_procedures(
    const TypeStore &types, TypeId type_id, const TargetFacts &target,
    std::vector<TypeId> &active_procedures) {
  const Type &type = types.type(type_id);
  CAbiType result;
  result.size = type.layout.size;
  result.alignment = type.layout.alignment;

  if (direct_scalar(types, type_id, target, active_procedures)) {
    // Microsoft x64 gives the Clang/GNU __int128 extension a split contract:
    // parameters are pointers to caller-owned 16-byte values, while results
    // use a <2 x i64> carrier. Treating it as an ordinary direct i128 happens
    // to verify as LLVM IR but disagrees with an independently compiled C
    // caller. Fixed-backing C enums recurse to the same physical scalar rule.
    Type scalar = type;
    while (scalar.kind == TypeKind::Enum && scalar.element.is_valid()) {
      scalar = types.type(scalar.element);
    }
    if ((scalar.kind == TypeKind::SignedInteger ||
         scalar.kind == TypeKind::UnsignedInteger ||
         scalar.kind == TypeKind::EndianScalar) &&
        scalar.bit_width == 128) {
      result.classification = CAbiClass::Win64WideInteger;
      return result;
    }
    result.classification = CAbiClass::Direct;
    return result;
  }
  if ((type.kind != TypeKind::Struct && type.kind != TypeKind::Union) ||
      !type.c_representation || !type.layout.known || type.layout.size == 0 ||
      !aggregate_member_legal(types, type_id, target, active_procedures)) {
    return result;
  }

  if (type.layout.size == 1 || type.layout.size == 2 ||
      type.layout.size == 4 || type.layout.size == 8) {
    result.classification = CAbiClass::SmallAggregate;
    result.argument_integer_bits =
        static_cast<std::uint32_t>(type.layout.size * 8U);
    result.argument_integer_count = 1;
    result.result_integer_bits = result.argument_integer_bits;
    result.result_integer_count = 1;
    return result;
  }

  result.classification = CAbiClass::Indirect;
  return result;
}

[[nodiscard]] CAbiType
classify_with_active_procedures(const TypeStore &types, TypeId type_id,
                                const TargetFacts &target,
                                std::vector<TypeId> &active_procedures) {
  if (target.arch == "aarch64" &&
      (target.abi == "darwin_arm64" || target.abi == "aapcs64_gnu")) {
    return classify_aarch64_with_active_procedures(types, type_id, target,
                                                   active_procedures);
  }
  if (target.arch == "x86_64" && target.abi == "sysv_amd64") {
    return classify_sysv_amd64_with_active_procedures(types, type_id, target,
                                                      active_procedures);
  }
  if (target.arch == "x86_64" && target.abi == "win64") {
    return classify_win64_with_active_procedures(types, type_id, target,
                                                 active_procedures);
  }
  return {};
}

} // namespace

CAbiType classify_c_type(const TypeStore &types, TypeId type_id,
                         const TargetFacts &target) {
  std::vector<TypeId> active_procedures;
  return classify_with_active_procedures(types, type_id, target,
                                         active_procedures);
}

const CAbiType *CAbiTable::find(TypeId type) const {
  if (!type.is_valid() || type.value >= rows.size())
    return nullptr;
  return &rows[type.value];
}

bool CAbiTable::complete_for(const TypeStore &types,
                             const TargetFacts &target) const {
  return valid_prefix_for(types, target) && rows.size() == types.size();
}

bool CAbiTable::valid_prefix_for(const TypeStore &types,
                                 const TargetFacts &target) const {
  return target_identity == target.identity && rows.size() <= types.size();
}

CAbiTable classify_c_types(const TypeStore &types, const TargetFacts &target) {
  CAbiTable result;
  result.target_identity = target.identity;
  result.rows.reserve(types.size());
  for (std::size_t index = 0; index < types.size(); ++index) {
    result.rows.push_back(classify_c_type(
        types, TypeId{static_cast<std::uint32_t>(index)}, target));
  }
  return result;
}

CAbiFunctionPlan plan_c_abi_function(const TypeStore &types,
                                     TypeId procedure_id, const CAbiTable &abi,
                                     const TargetFacts &target) {
  CAbiFunctionPlan result;
  if (!procedure_id.is_valid() || abi.target_identity != target.identity) {
    return result;
  }
  const Type &procedure = types.type(procedure_id);
  if (procedure.kind != TypeKind::Procedure ||
      !procedure.c_calling_convention || procedure.members.empty()) {
    return result;
  }

  const TypeId result_id = procedure.members.back();
  if (result_id != types.builtins().void_type) {
    const CAbiType *classified = abi.find(result_id);
    if (classified == nullptr ||
        classified->classification == CAbiClass::Illegal) {
      return result;
    }
    result.result = *classified;
  }

  const std::size_t parameter_count = procedure.members.size() - 1;
  result.parameters.resize(parameter_count);
  if (target.abi != "sysv_amd64") {
    for (std::size_t index = 0; index < parameter_count; ++index) {
      const CAbiType *classified = abi.find(procedure.members[index]);
      if (classified == nullptr ||
          classified->classification == CAbiClass::Illegal) {
        return CAbiFunctionPlan{};
      }
      if (classified->classification == CAbiClass::Indirect ||
          classified->classification == CAbiClass::Win64WideInteger) {
        result.parameters[index].mode = CAbiParameterMode::Indirect;
      }
    }
    result.ok = true;
    return result;
  }

  // The SysV AMD64 return buffer consumes RDI exactly like a hidden first
  // argument. Register assignment for later aggregates must therefore begin
  // with five rather than six available integer registers.
  std::uint32_t integer_registers =
      result.result.classification == CAbiClass::Indirect ? 5U : 6U;
  std::uint32_t sse_registers = 8;
  for (std::size_t index = 0; index < parameter_count; ++index) {
    const TypeId parameter_id = procedure.members[index];
    const CAbiType *classified = abi.find(parameter_id);
    if (classified == nullptr ||
        classified->classification == CAbiClass::Illegal) {
      return CAbiFunctionPlan{};
    }
    if (classified->classification == CAbiClass::Indirect) {
      result.parameters[index].mode = CAbiParameterMode::Indirect;
      continue;
    }

    std::uint32_t required_integer = 0;
    std::uint32_t required_sse = 0;
    if (classified->classification == CAbiClass::EightbyteAggregate) {
      for (std::size_t component = 0; component < classified->eightbyte_count;
           ++component) {
        if (classified->eightbytes[component].classification ==
            CAbiEightbyteClass::Integer) {
          ++required_integer;
        } else if (classified->eightbytes[component].classification ==
                   CAbiEightbyteClass::Sse) {
          ++required_sse;
        }
      }
    } else if (classified->classification == CAbiClass::Direct) {
      Type scalar = types.type(parameter_id);
      while (scalar.kind == TypeKind::Enum && scalar.element.is_valid()) {
        scalar = types.type(scalar.element);
      }
      if (scalar.kind == TypeKind::Float) {
        required_sse = 1;
      } else {
        required_integer = scalar.layout.size > 8 ? 2U : 1U;
      }
    }

    if (required_integer > integer_registers || required_sse > sse_registers) {
      if (classified->classification == CAbiClass::EightbyteAggregate) {
        // The psABI reverts every partial assignment and passes the complete
        // aggregate in memory. Scalar LLVM parameters already receive their
        // correct stack placement from the target calling convention.
        result.parameters[index].mode = CAbiParameterMode::Indirect;
      }
      continue;
    }
    integer_registers -= required_integer;
    sse_registers -= required_sse;
  }
  result.ok = true;
  return result;
}

} // namespace draft
