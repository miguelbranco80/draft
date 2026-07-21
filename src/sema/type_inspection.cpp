// Direct implementation of Draft's compile-time structural type queries.
//
// Every operation below follows the canonical Type row and, for nominal member
// names and enum values, the source-ordered side tables owned by
// SemanticPackage. The deliberately explicit switches make the accepted input
// kinds and returned information auditable. No query creates a runtime type
// descriptor; callers fold the returned ConstantValue before MIR.

#include "sema/type_inspection.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace draft {
namespace {

constexpr std::array<std::string_view, 24> kTypeKindNames = {
    "void",
    "bool",
    "boolean_storage",
    "signed_integer",
    "unsigned_integer",
    "float",
    "rune",
    "endian_scalar",
    "raw_pointer",
    "c_string",
    "string",
    "pointer",
    "multi_pointer",
    "slice",
    "array",
    "tuple",
    "procedure",
    "simd",
    "struct",
    "enumeration",
    "tagged_union",
    "raw_union",
    "distinct",
    "type",
};

constexpr std::array<TypeKind, 24> kTypeKinds = {
    TypeKind::Void,
    TypeKind::Bool,
    TypeKind::BooleanStorage,
    TypeKind::SignedInteger,
    TypeKind::UnsignedInteger,
    TypeKind::Float,
    TypeKind::Rune,
    TypeKind::EndianScalar,
    TypeKind::RawPointer,
    TypeKind::CString,
    TypeKind::String,
    TypeKind::Pointer,
    TypeKind::MultiPointer,
    TypeKind::Slice,
    TypeKind::Array,
    TypeKind::Tuple,
    TypeKind::Procedure,
    TypeKind::Simd,
    TypeKind::Struct,
    TypeKind::Enum,
    TypeKind::TaggedUnion,
    TypeKind::RawUnion,
    TypeKind::Distinct,
    TypeKind::MetaType,
};

constexpr std::array<std::string_view, 3> kByteOrderNames = {
    "native", "little", "big"};

constexpr std::array<std::string_view, 2> kCallingConventionNames = {
    "draft", "c"};

// Target categorical values are nominal enums, not interchangeable labels.
// The source order below defines their stable member ordinals and the order
// observed by structural type inspection. New target profiles may append
// alternatives, but must not reorder an existing vocabulary.
constexpr std::array<std::string_view, 1> kTargetArchitectureNames = {
    "aarch64"};
constexpr std::array<std::string_view, 2> kTargetOperatingSystemNames = {
    "macos", "linux"};
constexpr std::array<std::string_view, 2> kTargetAbiNames = {
    "darwin_arm64", "aapcs64_gnu"};
constexpr std::array<std::string_view, 2> kTargetByteOrderNames = {
    "little", "big"};
constexpr std::array<std::string_view, 2> kTargetObjectFormatNames = {
    "macho", "elf"};

[[nodiscard]] TypeInspectionAttempt failure(std::string error) {
  TypeInspectionAttempt result;
  result.recognized = true;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] TypeInspectionAttempt waiting(TypeFacet facet) {
  TypeInspectionAttempt result;
  result.recognized = true;
  result.required_facet = facet;
  return result;
}

[[nodiscard]] TypeInspectionAttempt success(
    ConstantValue value, TypeId type) {
  TypeInspectionAttempt result;
  result.recognized = true;
  result.result = TypeInspectionResult{std::move(value), type};
  return result;
}

[[nodiscard]] TypeInspectionAttempt type_result(
    const SemanticPackage &package, TypeId type) {
  return success(
      ConstantValue::make_type(type.value),
      package.types.builtins().meta_type);
}

[[nodiscard]] TypeInspectionAttempt integer_result(
    const SemanticPackage &package, std::uint64_t value) {
  return success(
      ConstantValue::make_integer(BigInteger::from_u64(value)),
      package.types.builtins().usize_type);
}

[[nodiscard]] std::optional<std::uint64_t> type_kind_value(TypeKind kind) {
  switch (kind) {
  case TypeKind::Void: return 0;
  case TypeKind::Bool: return 1;
  case TypeKind::BooleanStorage: return 2;
  case TypeKind::SignedInteger: return 3;
  case TypeKind::UnsignedInteger: return 4;
  case TypeKind::Float: return 5;
  case TypeKind::Rune: return 6;
  case TypeKind::EndianScalar: return 7;
  case TypeKind::RawPointer: return 8;
  case TypeKind::CString: return 9;
  case TypeKind::String: return 10;
  case TypeKind::Pointer: return 11;
  case TypeKind::MultiPointer: return 12;
  case TypeKind::Slice: return 13;
  case TypeKind::Array: return 14;
  case TypeKind::Tuple: return 15;
  case TypeKind::Procedure: return 16;
  case TypeKind::Simd: return 17;
  case TypeKind::Struct: return 18;
  case TypeKind::Enum: return 19;
  case TypeKind::TaggedUnion: return 20;
  case TypeKind::RawUnion: return 21;
  case TypeKind::Distinct: return 22;
  case TypeKind::MetaType: return 23;
  case TypeKind::Invalid:
  case TypeKind::UntypedInteger:
  case TypeKind::UntypedFloat:
  case TypeKind::TypeParameter:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<SymbolId> type_owner(
    const SemanticPackage &package, TypeId type) {
  for (const OwnedSemanticScope &owned : package.owned_scopes_for_read()) {
    if (package.symbols.scope(owned.scope).kind == ScopeKind::Type &&
        package.symbols.symbol(owned.owner).type == type) {
      return owned.owner;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<SymbolId> named_members(
    const SemanticPackage &package, TypeId type) {
  std::vector<SymbolId> result;
  const std::optional<SymbolId> owner = type_owner(package, type);
  if (!owner.has_value()) return result;
  for (const AggregateMember &member : package.aggregate_members_for_read()) {
    if (member.owner == *owner) result.push_back(member.member);
  }
  return result;
}

[[nodiscard]] std::string canonical_type_name(
    const SemanticPackage &package,
    TypeId id,
    std::vector<TypeId> &active) {
  if (!id.is_valid()) return "<invalid>";
  if (std::find(active.begin(), active.end(), id) != active.end()) {
    const Type &recursive = package.types.type(id);
    return recursive.name.empty() ? "<recursive>" : recursive.name;
  }
  active.push_back(id);
  const Type &type = package.types.type(id);
  std::string result;
  if (!type.name.empty()) {
    result = type.name;
  } else if (type.kind == TypeKind::Pointer) {
    result = "^" + canonical_type_name(package, type.element, active);
  } else if (type.kind == TypeKind::MultiPointer) {
    result = "[^]" + canonical_type_name(package, type.element, active);
  } else if (type.kind == TypeKind::Slice) {
    result = "[]" + canonical_type_name(package, type.element, active);
  } else if (type.kind == TypeKind::Array) {
    result = "[" + std::to_string(type.element_count) + "]" +
        canonical_type_name(package, type.element, active);
  } else if (type.kind == TypeKind::Simd) {
    result = "#simd[" + std::to_string(type.element_count) + "]" +
        canonical_type_name(package, type.element, active);
  } else if (type.kind == TypeKind::Tuple) {
    result = "(";
    for (std::size_t index = 0; index < type.members.size(); ++index) {
      if (index != 0) result += ", ";
      result += canonical_type_name(package, type.members[index], active);
    }
    result += ")";
  } else if (type.kind == TypeKind::Procedure) {
    result = type.c_calling_convention ? "c proc(" : "proc(";
    const std::size_t parameter_count = type.members.empty()
        ? 0
        : type.members.size() - 1;
    for (std::size_t index = 0; index < parameter_count; ++index) {
      if (index != 0) result += ", ";
      result += canonical_type_name(package, type.members[index], active);
    }
    result += ")";
    if (!type.members.empty() &&
        type.members.back() != package.types.builtins().void_type) {
      result += " -> ";
      result += canonical_type_name(package, type.members.back(), active);
    }
  } else {
    result = std::string(type_kind_name(type.kind));
  }
  active.pop_back();
  return result;
}

[[nodiscard]] bool aggregate_kind(TypeKind kind) {
  return kind == TypeKind::Tuple || kind == TypeKind::Struct ||
      kind == TypeKind::Enum || kind == TypeKind::TaggedUnion ||
      kind == TypeKind::RawUnion;
}

[[nodiscard]] std::optional<std::uint64_t> required_index(
    std::optional<std::uint64_t> index,
    std::uint64_t count) {
  if (!index.has_value() || *index >= count) return std::nullopt;
  return *index;
}

[[nodiscard]] TypeInspectionAttempt enum_result(
    TypeId enum_type,
    std::uint64_t value) {
  return success(
      ConstantValue::make_integer(BigInteger::from_u64(value)), enum_type);
}

[[nodiscard]] std::optional<TypeInspectionAttempt> require_facet(
    const SemanticPackage &package,
    TypeId type,
    TypeFacet facet,
    std::string_view query) {
  const TypeFacetState state = package.types.facet_state(type, facet);
  if (state == TypeFacetState::Complete) return std::nullopt;
  if (state == TypeFacetState::Waiting) return waiting(facet);
  return failure(
      std::string(query) + " is not defined for a type without " +
      std::string(type_facet_name(facet)));
}

} // namespace

std::string_view type_facet_name(TypeFacet facet) {
  switch (facet) {
  case TypeFacet::Identity:
    return "type identity";
  case TypeFacet::Members:
    return "complete members";
  case TypeFacet::MemberTypes:
    return "complete member types";
  case TypeFacet::NaturalLayout:
    return "complete natural layout";
  }
  return "type facet";
}

bool is_type_inspection_query(std::string_view name) {
  constexpr std::array<std::string_view, 18> names = {
      "type_kind", "type_name", "type_bit_width", "type_byte_order",
      "type_element", "type_element_count", "type_member_count",
      "type_member_name", "type_member_type", "type_member_offset",
      "type_member_value", "type_underlying", "type_discriminator",
      "type_parameter_count", "type_parameter_type", "type_result",
      "type_calling_convention", "type_is_c_repr",
  };
  if (name == "type_requested_alignment") return true;
  return std::find(names.begin(), names.end(), name) != names.end();
}

TypeInspectionAttempt inspect_type(
    const SemanticPackage &package,
    std::string_view query,
    TypeId queried,
    std::optional<std::uint64_t> index) {
  if (!is_type_inspection_query(query)) return {};
  if (!queried.is_valid() || queried.value >= package.types.size()) {
    return failure(std::string(query) + " requires a valid type");
  }
  const BuiltinTypes &builtins = package.types.builtins();
  const Type &type = package.types.type(queried);

  if (query == "type_kind") {
    const std::optional<std::uint64_t> value = type_kind_value(type.kind);
    return value.has_value()
        ? enum_result(builtins.type_kind_type, *value)
        : failure("type_kind requires a concrete Draft type");
  }
  if (query == "type_name") {
    std::vector<TypeId> active;
    return success(
        ConstantValue::make_string(canonical_type_name(package, queried, active)),
        builtins.string_type);
  }
  if (query == "type_bit_width") {
    if (type.bit_width == 0) {
      return failure("type_bit_width requires a scalar type with a defined bit width");
    }
    return integer_result(package, type.bit_width);
  }
  if (query == "type_byte_order") {
    if (type.kind == TypeKind::EndianScalar) {
      return enum_result(
          builtins.type_byte_order_type,
          static_cast<std::uint64_t>(type.scalar_byte_order));
    }
    if (type.kind == TypeKind::Bool ||
        type.kind == TypeKind::BooleanStorage ||
        type.kind == TypeKind::SignedInteger ||
        type.kind == TypeKind::UnsignedInteger ||
        type.kind == TypeKind::Float || type.kind == TypeKind::Rune) {
      return enum_result(builtins.type_byte_order_type, 0);
    }
    return failure("type_byte_order requires a scalar storage type");
  }
  if (query == "type_element") {
    if (type.kind != TypeKind::Pointer && type.kind != TypeKind::MultiPointer &&
        type.kind != TypeKind::Slice && type.kind != TypeKind::Array &&
        type.kind != TypeKind::Simd) {
      return failure("type_element requires a pointer, slice, array, or SIMD type");
    }
    return type_result(package, type.element);
  }
  if (query == "type_element_count") {
    if (type.kind != TypeKind::Array && type.kind != TypeKind::Simd) {
      return failure("type_element_count requires an array or SIMD type");
    }
    if (type.element_count_expression.is_valid() ||
        type.owner_evaluated_element_count) {
      return failure("type_element_count requires a concrete element count");
    }
    return integer_result(package, type.element_count);
  }
  if (query == "type_member_count") {
    if (!aggregate_kind(type.kind)) {
      return failure("type_member_count requires a tuple or aggregate type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(package, queried, TypeFacet::Members, query)) {
      return *unavailable;
    }
    return integer_result(package, type.members.size());
  }
  if (query == "type_member_name") {
    if (!aggregate_kind(type.kind)) {
      return failure("type_member_name requires a tuple or aggregate type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(package, queried, TypeFacet::Members, query)) {
      return *unavailable;
    }
    const std::optional<std::uint64_t> member = required_index(
        index, static_cast<std::uint64_t>(type.members.size()));
    if (!member.has_value()) return failure("type_member_name index is out of bounds");
    if (type.kind == TypeKind::Tuple) {
      return success(
          ConstantValue::make_string(std::to_string(*member)),
          builtins.string_type);
    }
    if (queried == builtins.type_kind_type) {
      return success(
          ConstantValue::make_string(std::string(kTypeKindNames[*member])),
          builtins.string_type);
    }
    if (queried == builtins.type_byte_order_type) {
      return success(
          ConstantValue::make_string(std::string(kByteOrderNames[*member])),
          builtins.string_type);
    }
    if (queried == builtins.calling_convention_type) {
      return success(
          ConstantValue::make_string(std::string(kCallingConventionNames[*member])),
          builtins.string_type);
    }
    if (queried == builtins.target_architecture_type) {
      return success(
          ConstantValue::make_string(
              std::string(kTargetArchitectureNames[*member])),
          builtins.string_type);
    }
    if (queried == builtins.target_operating_system_type) {
      return success(
          ConstantValue::make_string(
              std::string(kTargetOperatingSystemNames[*member])),
          builtins.string_type);
    }
    if (queried == builtins.target_abi_type) {
      return success(
          ConstantValue::make_string(std::string(kTargetAbiNames[*member])),
          builtins.string_type);
    }
    if (queried == builtins.target_byte_order_type) {
      return success(
          ConstantValue::make_string(
              std::string(kTargetByteOrderNames[*member])),
          builtins.string_type);
    }
    if (queried == builtins.target_object_format_type) {
      return success(
          ConstantValue::make_string(
              std::string(kTargetObjectFormatNames[*member])),
          builtins.string_type);
    }
    const std::vector<SymbolId> members = named_members(package, queried);
    if (*member >= members.size()) {
      return failure("type_member_name has no source member metadata");
    }
    return success(
        ConstantValue::make_string(package.symbols.symbol(members[*member]).name),
        builtins.string_type);
  }
  if (query == "type_member_type") {
    if (!aggregate_kind(type.kind)) {
      return failure("type_member_type requires a tuple or aggregate type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(package, queried, TypeFacet::MemberTypes, query)) {
      return *unavailable;
    }
    const std::optional<std::uint64_t> member = required_index(
        index, static_cast<std::uint64_t>(type.members.size()));
    return member.has_value()
        ? type_result(package, type.members[*member])
        : failure("type_member_type index is out of bounds");
  }
  if (query == "type_member_offset") {
    if (type.kind == TypeKind::Enum) {
      return failure("type_member_offset is not defined for enum members");
    }
    if (!aggregate_kind(type.kind)) {
      return failure("type_member_offset requires a tuple or aggregate type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(
                package, queried, TypeFacet::NaturalLayout, query)) {
      return *unavailable;
    }
    const std::optional<std::uint64_t> member = required_index(
        index, static_cast<std::uint64_t>(type.member_offsets.size()));
    return member.has_value()
        ? integer_result(package, type.member_offsets[*member])
        : failure("type_member_offset index is out of bounds");
  }
  if (query == "type_member_value") {
    if (type.kind != TypeKind::Enum) {
      return failure("type_member_value requires an enum type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(package, queried, TypeFacet::Members, query)) {
      return *unavailable;
    }
    const std::optional<std::uint64_t> member = required_index(
        index, static_cast<std::uint64_t>(type.members.size()));
    if (!member.has_value()) return failure("type_member_value index is out of bounds");
    if (queried == builtins.type_kind_type ||
        queried == builtins.type_byte_order_type ||
        queried == builtins.calling_convention_type ||
        queried == builtins.target_architecture_type ||
        queried == builtins.target_operating_system_type ||
        queried == builtins.target_abi_type ||
        queried == builtins.target_byte_order_type ||
        queried == builtins.target_object_format_type) {
      return success(
          ConstantValue::make_integer(BigInteger::from_u64(*member)),
          type.element);
    }
    const std::vector<SymbolId> members = named_members(package, queried);
    if (*member >= members.size()) {
      return failure("type_member_value has no source member metadata");
    }
    for (const EnumMemberValue &value : package.enum_member_values_for_read()) {
      if (value.member == members[*member]) {
        return success(ConstantValue::make_integer(value.value), type.element);
      }
    }
    return failure("type_member_value could not find the enum value");
  }
  if (query == "type_underlying") {
    if (type.kind != TypeKind::Distinct && type.kind != TypeKind::Enum &&
        type.kind != TypeKind::EndianScalar) {
      return failure("type_underlying requires a distinct, enum, or endian scalar type");
    }
    if (type.kind == TypeKind::Enum) {
      if (const std::optional<TypeInspectionAttempt> unavailable =
              require_facet(
                  package, queried, TypeFacet::MemberTypes, query)) {
        return *unavailable;
      }
    }
    return type_result(package, type.element);
  }
  if (query == "type_discriminator") {
    if (type.kind != TypeKind::TaggedUnion) {
      return failure("type_discriminator requires a tagged union type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(
                package, queried, TypeFacet::MemberTypes, query)) {
      return *unavailable;
    }
    return type_result(package, type.element);
  }
  if (query == "type_parameter_count") {
    if (type.kind != TypeKind::Procedure) {
      return failure("type_parameter_count requires a procedure type");
    }
    return integer_result(package, type.members.empty() ? 0 : type.members.size() - 1);
  }
  if (query == "type_parameter_type") {
    if (type.kind != TypeKind::Procedure) {
      return failure("type_parameter_type requires a procedure type");
    }
    const std::uint64_t count = type.members.empty() ? 0 : type.members.size() - 1;
    const std::optional<std::uint64_t> parameter = required_index(index, count);
    return parameter.has_value()
        ? type_result(package, type.members[*parameter])
        : failure("type_parameter_type index is out of bounds");
  }
  if (query == "type_result") {
    if (type.kind != TypeKind::Procedure || type.members.empty()) {
      return failure("type_result requires a procedure type");
    }
    return type_result(package, type.members.back());
  }
  if (query == "type_calling_convention") {
    if (type.kind != TypeKind::Procedure) {
      return failure("type_calling_convention requires a procedure type");
    }
    return enum_result(
        builtins.calling_convention_type,
        type.c_calling_convention ? 1 : 0);
  }
  if (query == "type_is_c_repr") {
    if (type.kind != TypeKind::Struct && type.kind != TypeKind::Enum &&
        type.kind != TypeKind::RawUnion) {
      return failure("type_is_c_repr requires a struct, enum, or raw union type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(package, queried, TypeFacet::Members, query)) {
      return *unavailable;
    }
    return success(ConstantValue::make_bool(type.c_representation), builtins.bool_type);
  }
  if (query == "type_requested_alignment") {
    if (type.kind != TypeKind::Struct && type.kind != TypeKind::RawUnion) {
      return failure("type_requested_alignment requires a struct or raw union type");
    }
    if (const std::optional<TypeInspectionAttempt> unavailable =
            require_facet(package, queried, TypeFacet::Members, query)) {
      return *unavailable;
    }
    return integer_result(package, type.requested_alignment);
  }
  return {};
}

std::optional<std::uint64_t> compiler_enum_member_value(
    const SemanticPackage &package,
    TypeId enum_type,
    std::string_view name) {
  const BuiltinTypes &builtins = package.types.builtins();
  const auto find = [name](const auto &names) -> std::optional<std::uint64_t> {
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (names[index] == name) return static_cast<std::uint64_t>(index);
    }
    return std::nullopt;
  };
  if (enum_type == builtins.type_kind_type) return find(kTypeKindNames);
  if (enum_type == builtins.type_byte_order_type) return find(kByteOrderNames);
  if (enum_type == builtins.calling_convention_type) {
    return find(kCallingConventionNames);
  }
  if (enum_type == builtins.target_architecture_type) {
    return find(kTargetArchitectureNames);
  }
  if (enum_type == builtins.target_operating_system_type) {
    return find(kTargetOperatingSystemNames);
  }
  if (enum_type == builtins.target_abi_type) return find(kTargetAbiNames);
  if (enum_type == builtins.target_byte_order_type) {
    return find(kTargetByteOrderNames);
  }
  if (enum_type == builtins.target_object_format_type) {
    return find(kTargetObjectFormatNames);
  }
  return std::nullopt;
}

std::optional<std::string_view> compiler_enum_member_name(
    const SemanticPackage &package,
    TypeId enum_type,
    std::uint64_t value) {
  const BuiltinTypes &builtins = package.types.builtins();
  if (enum_type == builtins.type_kind_type && value < kTypeKindNames.size()) {
    return kTypeKindNames[value];
  }
  if (enum_type == builtins.type_byte_order_type &&
      value < kByteOrderNames.size()) {
    return kByteOrderNames[value];
  }
  if (enum_type == builtins.calling_convention_type &&
      value < kCallingConventionNames.size()) {
    return kCallingConventionNames[value];
  }
  if (enum_type == builtins.target_architecture_type &&
      value < kTargetArchitectureNames.size()) {
    return kTargetArchitectureNames[value];
  }
  if (enum_type == builtins.target_operating_system_type &&
      value < kTargetOperatingSystemNames.size()) {
    return kTargetOperatingSystemNames[value];
  }
  if (enum_type == builtins.target_abi_type &&
      value < kTargetAbiNames.size()) {
    return kTargetAbiNames[value];
  }
  if (enum_type == builtins.target_byte_order_type &&
      value < kTargetByteOrderNames.size()) {
    return kTargetByteOrderNames[value];
  }
  if (enum_type == builtins.target_object_format_type &&
      value < kTargetObjectFormatNames.size()) {
    return kTargetObjectFormatNames[value];
  }
  return std::nullopt;
}

std::optional<TypeKind> inspected_type_kind(std::string_view name) {
  for (std::size_t index = 0; index < kTypeKindNames.size(); ++index) {
    if (kTypeKindNames[index] == name) return kTypeKinds[index];
  }
  return std::nullopt;
}

std::span<const TypeKind> inspectable_type_kinds() {
  return kTypeKinds;
}

} // namespace draft
