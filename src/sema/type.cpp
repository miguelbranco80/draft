// Canonical Draft type construction and natural aggregate layout.
//
// Builtins are inserted in one fixed order so dumps and early diagnostics are
// stable, although no persistent format may rely on their numeric IDs. Structural
// interning uses direct linear scans. This is intentionally simple and correct
// for the bootstrap's early scale; a measured need may later add a deterministic
// key table without changing type identity semantics.

#include "sema/type.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace draft {
namespace {

[[nodiscard]] bool is_power_of_two(std::uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] std::optional<std::uint64_t> round_up(
    std::uint64_t value, std::uint32_t alignment) {
  assert(is_power_of_two(alignment));
  const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1);
  if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
    return std::nullopt;
  }
  return (value + mask) & ~mask;
}

} // namespace

bool TypeId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

TypeStore::TypeStore(std::uint32_t pointer_bits) : pointer_bits_(pointer_bits) {
  assert(pointer_bits_ == 32 || pointer_bits_ == 64);
  const std::uint32_t pointer_bytes = pointer_bits_ / 8;

  Type invalid;
  invalid.kind = TypeKind::Invalid;
  invalid.name = "<invalid>";
  builtins_.invalid = add(std::move(invalid));

  Type void_type;
  void_type.kind = TypeKind::Void;
  void_type.name = "<void>";
  void_type.layout = {true, 0, 1};
  builtins_.void_type = add(std::move(void_type));

  Type untyped_integer;
  untyped_integer.kind = TypeKind::UntypedInteger;
  untyped_integer.name = "<untyped integer>";
  builtins_.untyped_integer = add(std::move(untyped_integer));

  Type untyped_float;
  untyped_float.kind = TypeKind::UntypedFloat;
  untyped_float.name = "<untyped float>";
  builtins_.untyped_float = add(std::move(untyped_float));

  builtins_.bool_type = add_scalar("bool", TypeKind::Bool, 8, 1);
  (void)add_scalar("b8", TypeKind::BooleanStorage, 8, 1);
  (void)add_scalar("b16", TypeKind::BooleanStorage, 16, 2);
  (void)add_scalar("b32", TypeKind::BooleanStorage, 32, 4);
  (void)add_scalar("b64", TypeKind::BooleanStorage, 64, 8);

  (void)add_scalar("i8", TypeKind::SignedInteger, 8, 1);
  (void)add_scalar("i16", TypeKind::SignedInteger, 16, 2);
  (void)add_scalar("i32", TypeKind::SignedInteger, 32, 4);
  (void)add_scalar("i64", TypeKind::SignedInteger, 64, 8);
  (void)add_scalar("i128", TypeKind::SignedInteger, 128, 16);
  builtins_.u8_type = add_scalar("u8", TypeKind::UnsignedInteger, 8, 1);
  (void)add_scalar("u16", TypeKind::UnsignedInteger, 16, 2);
  (void)add_scalar("u32", TypeKind::UnsignedInteger, 32, 4);
  (void)add_scalar("u64", TypeKind::UnsignedInteger, 64, 8);
  (void)add_scalar("u128", TypeKind::UnsignedInteger, 128, 16);

  builtins_.int_type = add_scalar("int", TypeKind::SignedInteger, pointer_bits_, pointer_bytes);
  builtins_.uint_type = add_scalar("uint", TypeKind::UnsignedInteger, pointer_bits_, pointer_bytes);
  builtins_.isize_type = add_scalar("isize", TypeKind::SignedInteger, pointer_bits_, pointer_bytes);
  builtins_.usize_type = add_scalar("usize", TypeKind::UnsignedInteger, pointer_bits_, pointer_bytes);
  builtins_.uintptr_type = add_scalar("uintptr", TypeKind::UnsignedInteger, pointer_bits_, pointer_bytes);

  (void)add_scalar("f16", TypeKind::Float, 16, 2);
  (void)add_scalar("f32", TypeKind::Float, 32, 4);
  (void)add_scalar("f64", TypeKind::Float, 64, 8);

  // byte is a true alias, so both names intentionally map to one TypeId.
  add_builtin_alias("byte", builtins_.u8_type);
  builtins_.rune_type = add_scalar("rune", TypeKind::Rune, 32, 4);

  // Endian storage types have scalar size/alignment but a distinct operation
  // category. The name determines signedness/float interpretation during casts.
  constexpr const char *endian_bases[] = {
      "i16", "i32", "i64", "i128", "u16", "u32", "u64", "u128",
      "f16", "f32", "f64"};
  for (const char *base : endian_bases) {
    const std::string base_name(base);
    const std::optional<TypeId> native = find_builtin(base_name);
    assert(native.has_value());
    // Copy the small pieces of source type metadata before adding either new
    // type. add_scalar() can grow types_, which invalidates references into the
    // vector. Keeping scalar values here makes the lifetime boundary explicit.
    const std::uint32_t bit_width = type(*native).bit_width;
    const std::uint32_t alignment = type(*native).layout.alignment;
    (void)add_scalar(base_name + "le", TypeKind::EndianScalar, bit_width, alignment);
    (void)add_scalar(base_name + "be", TypeKind::EndianScalar, bit_width, alignment);
  }

  Type raw_pointer;
  raw_pointer.kind = TypeKind::RawPointer;
  raw_pointer.name = "rawptr";
  raw_pointer.layout = {true, pointer_bytes, pointer_bytes};
  raw_pointer.bit_width = pointer_bits_;
  builtins_.rawptr_type = add(std::move(raw_pointer));
  add_builtin_alias("rawptr", builtins_.rawptr_type);
  Type c_string;
  c_string.kind = TypeKind::CString;
  c_string.name = "cstring";
  c_string.layout = {true, pointer_bytes, pointer_bytes};
  c_string.bit_width = pointer_bits_;
  builtins_.cstring_type = add(std::move(c_string));
  add_builtin_alias("cstring", builtins_.cstring_type);
  Type string;
  string.kind = TypeKind::String;
  string.name = "string";
  string.layout = {true, static_cast<std::uint64_t>(pointer_bytes) * 2U, pointer_bytes};
  builtins_.string_type = add(std::move(string));
  add_builtin_alias("string", builtins_.string_type);
}

const BuiltinTypes &TypeStore::builtins() const {
  return builtins_;
}

const Type &TypeStore::type(TypeId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < types_.size());
  return types_[id.value];
}

Type &TypeStore::type_mut(TypeId id) {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < types_.size());
  return types_[id.value];
}

std::size_t TypeStore::size() const {
  return types_.size();
}

std::optional<TypeId> TypeStore::find_builtin(std::string_view name) const {
  for (const BuiltinName &builtin : builtin_names_) {
    if (builtin.name == name) {
      return builtin.type;
    }
  }
  return std::nullopt;
}

TypeId TypeStore::add(Type type_value) {
  assert(types_.size() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const TypeId id{static_cast<std::uint32_t>(types_.size())};
  types_.push_back(std::move(type_value));
  return id;
}

void TypeStore::add_builtin_alias(std::string name, TypeId id) {
  builtin_names_.push_back({std::move(name), id});
}

TypeId TypeStore::add_scalar(
    std::string name, TypeKind kind, std::uint32_t bits, std::uint32_t alignment) {
  assert(bits % 8 == 0);
  Type scalar;
  scalar.kind = kind;
  scalar.name = name;
  scalar.layout = {true, bits / 8U, alignment};
  scalar.bit_width = bits;
  const TypeId id = add(std::move(scalar));
  add_builtin_alias(std::move(name), id);
  return id;
}

TypeId TypeStore::pointer(TypeId element) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Pointer && candidate.element == element) {
      return TypeId{index};
    }
  }
  const std::uint32_t bytes = pointer_bits_ / 8;
  Type result;
  result.kind = TypeKind::Pointer;
  result.layout = {true, bytes, bytes};
  result.element = element;
  return add(std::move(result));
}

TypeId TypeStore::multi_pointer(TypeId element) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::MultiPointer && candidate.element == element) {
      return TypeId{index};
    }
  }
  const std::uint32_t bytes = pointer_bits_ / 8;
  Type result;
  result.kind = TypeKind::MultiPointer;
  result.layout = {true, bytes, bytes};
  result.element = element;
  return add(std::move(result));
}

TypeId TypeStore::slice(TypeId element) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Slice && candidate.element == element) {
      return TypeId{index};
    }
  }
  const std::uint32_t bytes = pointer_bits_ / 8;
  Type result;
  result.kind = TypeKind::Slice;
  result.layout = {true, static_cast<std::uint64_t>(bytes) * 2U, bytes};
  result.element = element;
  return add(std::move(result));
}

TypeId TypeStore::array(TypeId element, std::uint64_t count) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Array && candidate.element == element &&
        candidate.element_count == count) {
      return TypeId{index};
    }
  }

  Type result;
  result.kind = TypeKind::Array;
  result.element = element;
  result.element_count = count;
  const TypeLayout element_layout = type(element).layout;
  if (element_layout.known && count != 0 &&
      element_layout.size <= std::numeric_limits<std::uint64_t>::max() / count) {
    result.layout = {true, element_layout.size * count, element_layout.alignment};
  }
  return add(std::move(result));
}

TypeId TypeStore::parametric_array(
    TypeId element, std::uint32_t value_parameter) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Array && candidate.element == element &&
        candidate.element_count_parameter == value_parameter) {
      return TypeId{index};
    }
  }
  Type result;
  result.kind = TypeKind::Array;
  result.element = element;
  result.element_count_parameter = value_parameter;
  return add(std::move(result));
}

TypeId TypeStore::simd(TypeId element, std::uint64_t lanes) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Simd && candidate.element == element &&
        candidate.element_count == lanes) {
      return TypeId{index};
    }
  }

  Type result;
  result.kind = TypeKind::Simd;
  result.element = element;
  result.element_count = lanes;
  const TypeLayout element_layout = type(element).layout;
  if (element_layout.known && lanes != 0 &&
      element_layout.size <= std::numeric_limits<std::uint64_t>::max() / lanes) {
    const std::uint64_t size = element_layout.size * lanes;
    // Target validation later rejects unsupported lane/type combinations. The
    // canonical semantic size is still known. Alignment is the next power of
    // two covering the value, capped at the initial target's 16-byte vector
    // alignment, and therefore always satisfies the layout invariant.
    std::uint32_t alignment = 1;
    while (alignment < 16 && static_cast<std::uint64_t>(alignment) < size) {
      alignment *= 2;
    }
    result.layout = {true, size, alignment};
  }
  return add(std::move(result));
}

TypeId TypeStore::parametric_simd(
    TypeId element, std::uint32_t value_parameter) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Simd && candidate.element == element &&
        candidate.element_count_parameter == value_parameter) {
      return TypeId{index};
    }
  }
  Type result;
  result.kind = TypeKind::Simd;
  result.element = element;
  result.element_count_parameter = value_parameter;
  return add(std::move(result));
}

TypeLayout TypeStore::aggregate_layout(const std::vector<TypeId> &members) const {
  TypeLayout result{true, 0, 1};
  for (TypeId member : members) {
    const TypeLayout layout = type(member).layout;
    if (!layout.known) {
      return {};
    }
    const std::optional<std::uint64_t> offset = round_up(result.size, layout.alignment);
    if (!offset.has_value() ||
        layout.size > std::numeric_limits<std::uint64_t>::max() - *offset) {
      return {};
    }
    result.size = *offset + layout.size;
    result.alignment = std::max(result.alignment, layout.alignment);
  }
  const std::optional<std::uint64_t> final_size = round_up(result.size, result.alignment);
  if (!final_size.has_value()) {
    return {};
  }
  result.size = *final_size;
  return result;
}

TypeId TypeStore::tuple(const std::vector<TypeId> &members) {
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Tuple && candidate.members == members) {
      return TypeId{index};
    }
  }
  Type result;
  result.kind = TypeKind::Tuple;
  result.layout = aggregate_layout(members);
  result.members = members;
  if (result.layout.known) {
    std::uint64_t next_offset = 0;
    result.member_offsets.reserve(members.size());
    for (TypeId member : members) {
      const TypeLayout member_layout = type(member).layout;
      const std::optional<std::uint64_t> offset =
          round_up(next_offset, member_layout.alignment);
      assert(offset.has_value());
      result.member_offsets.push_back(*offset);
      next_offset = *offset + member_layout.size;
    }
  }
  return add(std::move(result));
}

TypeId TypeStore::procedure(
    const std::vector<TypeId> &parameters, TypeId result_type, bool c_calling_convention) {
  std::vector<TypeId> signature = parameters;
  signature.push_back(result_type);
  for (std::uint32_t index = 0; index < types_.size(); ++index) {
    const Type &candidate = types_[index];
    if (candidate.kind == TypeKind::Procedure && candidate.members == signature &&
        candidate.c_calling_convention == c_calling_convention) {
      return TypeId{index};
    }
  }
  const std::uint32_t bytes = pointer_bits_ / 8;
  Type result;
  result.kind = TypeKind::Procedure;
  result.layout = {true, bytes, bytes};
  result.members = std::move(signature);
  result.c_calling_convention = c_calling_convention;
  return add(std::move(result));
}

TypeId TypeStore::distinct(std::string name, TypeId underlying, SourceRange declaration) {
  Type result;
  result.kind = TypeKind::Distinct;
  result.name = std::move(name);
  result.layout = type(underlying).layout;
  result.element = underlying;
  result.declaration = declaration;
  return add(std::move(result));
}

TypeId TypeStore::type_parameter(std::string name, SourceRange declaration) {
  Type result;
  result.kind = TypeKind::TypeParameter;
  result.name = std::move(name);
  result.declaration = declaration;
  return add(std::move(result));
}

TypeId TypeStore::begin_nominal(TypeKind kind, std::string name, SourceRange declaration) {
  assert(kind == TypeKind::Struct || kind == TypeKind::Enum ||
         kind == TypeKind::TaggedUnion || kind == TypeKind::RawUnion);
  Type result;
  result.kind = kind;
  result.name = std::move(name);
  result.declaration = declaration;
  return add(std::move(result));
}

void TypeStore::complete_nominal(
    TypeId id,
    TypeLayout layout,
    std::vector<TypeId> members,
    std::vector<std::uint64_t> member_offsets) {
  Type &nominal = type_mut(id);
  assert(nominal.kind == TypeKind::Struct || nominal.kind == TypeKind::Enum ||
         nominal.kind == TypeKind::TaggedUnion || nominal.kind == TypeKind::RawUnion);
  assert(!nominal.layout.known);
  nominal.layout = layout;
  nominal.members = std::move(members);
  nominal.member_offsets = std::move(member_offsets);
}

bool TypeStore::is_integer(TypeId id) const {
  const TypeKind kind = type(id).kind;
  return kind == TypeKind::SignedInteger || kind == TypeKind::UnsignedInteger;
}

bool TypeStore::is_float(TypeId id) const {
  return type(id).kind == TypeKind::Float;
}

bool TypeStore::is_number(TypeId id) const {
  return is_integer(id) || is_float(id);
}

std::string_view type_kind_name(TypeKind kind) {
  switch (kind) {
  case TypeKind::Invalid: return "invalid";
  case TypeKind::Void: return "void";
  case TypeKind::UntypedInteger: return "untyped integer";
  case TypeKind::UntypedFloat: return "untyped float";
  case TypeKind::Bool: return "bool";
  case TypeKind::BooleanStorage: return "boolean storage";
  case TypeKind::SignedInteger: return "signed integer";
  case TypeKind::UnsignedInteger: return "unsigned integer";
  case TypeKind::Float: return "float";
  case TypeKind::Rune: return "rune";
  case TypeKind::EndianScalar: return "endian scalar";
  case TypeKind::RawPointer: return "raw pointer";
  case TypeKind::CString: return "C string";
  case TypeKind::String: return "string";
  case TypeKind::Pointer: return "pointer";
  case TypeKind::MultiPointer: return "multi-pointer";
  case TypeKind::Slice: return "slice";
  case TypeKind::Array: return "array";
  case TypeKind::Tuple: return "tuple";
  case TypeKind::Procedure: return "procedure";
  case TypeKind::Simd: return "SIMD";
  case TypeKind::Struct: return "struct";
  case TypeKind::Enum: return "enum";
  case TypeKind::TaggedUnion: return "tagged union";
  case TypeKind::RawUnion: return "raw union";
  case TypeKind::Distinct: return "distinct";
  case TypeKind::TypeParameter: return "type parameter";
  }
  return "unknown type";
}

} // namespace draft
