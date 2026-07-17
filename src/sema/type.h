// Canonical semantic type storage and target-native layout facts.
//
// TypeStore assigns a stable TypeId to every built-in, structural, parametric,
// and nominal type in one semantic program. Structural constructors are interned:
// asking twice for `^u32` or `(u32, bool)` returns the same ID. Nominal types are
// created once at their declarations and remain distinct even when layout is
// identical. No syntax or LLVM type is stored here; this table is Draft's type
// identity authority and will later be reproduced by the self-hosted compiler.
//
// Layout values are byte counts for the selected AArch64 macOS profile. Unknown
// layout is explicit and occurs while a nominal type is incomplete or when a
// requested aggregate overflows the target addressable size. The semantic layer
// must diagnose use that requires a still-unknown layout.
//
// Relevant specification: 02-types-memory-runtime.md, sections 5-6.

#pragma once

#include "source/source.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

struct TypeId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const TypeId &) const = default;
};

enum class TypeKind {
  Invalid,
  Void,
  UntypedInteger,
  UntypedFloat,
  Bool,
  BooleanStorage,
  SignedInteger,
  UnsignedInteger,
  Float,
  Rune,
  EndianScalar,
  RawPointer,
  CString,
  String,
  Pointer,
  MultiPointer,
  Slice,
  Array,
  Tuple,
  Procedure,
  Simd,
  Struct,
  Enum,
  TaggedUnion,
  RawUnion,
  Distinct,
  TypeParameter,
};

// TypeLayout uses natural Draft layout before call-boundary ABI decomposition.
// alignment is a positive power of two when known. A zero size remains valid for
// a payload-free union alternative, but complete runtime types in Draft 1 are
// otherwise rejected later when the specification forbids empty aggregates.
struct TypeLayout {
  bool known = false;
  std::uint64_t size = 0;
  std::uint32_t alignment = 1;

  bool operator==(const TypeLayout &) const = default;
};

// Type is one canonical table row. Fields have meaning according to kind:
//
// - scalar kinds use bit_width and layout;
// - Pointer, MultiPointer, Slice, Array, Simd, and Distinct use element;
// - Array and Simd use element_count;
// - Tuple and Procedure use members (procedure parameters followed by result);
// - nominal kinds use name, declaration, and an initially unknown layout.
//
// Procedure members always contain a final result TypeId; void procedures use
// the canonical void type there. c_calling_convention distinguishes physical
// procedure pointer identities because ordinary procedures carry Draft context.
struct Type {
  TypeKind kind = TypeKind::Invalid;
  std::string name;
  TypeLayout layout;
  std::uint32_t bit_width = 0;
  TypeId element;
  std::uint64_t element_count = 0;
  std::vector<TypeId> members;
  bool c_calling_convention = false;
  SourceRange declaration;
};

// BuiltinTypes stores IDs used frequently by semantic code. Other builtin names,
// including endian storage variants, are available through find_builtin.
struct BuiltinTypes {
  TypeId invalid;
  TypeId void_type;
  TypeId untyped_integer;
  TypeId untyped_float;
  TypeId bool_type;
  TypeId u8_type;
  TypeId int_type;
  TypeId uint_type;
  TypeId usize_type;
  TypeId isize_type;
  TypeId uintptr_type;
  TypeId string_type;
  TypeId cstring_type;
  TypeId rawptr_type;
  TypeId rune_type;
};

class TypeStore {
public:
  // Draft 1's initial target has 64-bit pointers and natural integers. Keeping
  // the width explicit makes the table construction reusable when profiles grow.
  explicit TypeStore(std::uint32_t pointer_bits = 64);

  [[nodiscard]] const BuiltinTypes &builtins() const;
  [[nodiscard]] const Type &type(TypeId id) const;
  [[nodiscard]] Type &type_mut(TypeId id);
  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] std::optional<TypeId> find_builtin(std::string_view name) const;

  [[nodiscard]] TypeId pointer(TypeId element);
  [[nodiscard]] TypeId multi_pointer(TypeId element);
  [[nodiscard]] TypeId slice(TypeId element);
  [[nodiscard]] TypeId array(TypeId element, std::uint64_t count);
  [[nodiscard]] TypeId tuple(const std::vector<TypeId> &members);
  [[nodiscard]] TypeId procedure(
      const std::vector<TypeId> &parameters, TypeId result, bool c_calling_convention);
  [[nodiscard]] TypeId distinct(std::string name, TypeId underlying, SourceRange declaration);
  [[nodiscard]] TypeId begin_nominal(
      TypeKind kind, std::string name, SourceRange declaration);
  void complete_nominal(TypeId id, TypeLayout layout, std::vector<TypeId> members);

  [[nodiscard]] bool is_integer(TypeId id) const;
  [[nodiscard]] bool is_float(TypeId id) const;
  [[nodiscard]] bool is_number(TypeId id) const;

private:
  [[nodiscard]] TypeId add(Type type);
  void add_builtin_alias(std::string name, TypeId id);
  [[nodiscard]] TypeId add_scalar(
      std::string name, TypeKind kind, std::uint32_t bits, std::uint32_t alignment);
  [[nodiscard]] TypeLayout aggregate_layout(const std::vector<TypeId> &members) const;

  struct BuiltinName {
    std::string name;
    TypeId type;
  };

  std::uint32_t pointer_bits_ = 64;
  std::vector<Type> types_;
  std::vector<BuiltinName> builtin_names_;
  BuiltinTypes builtins_;
};

[[nodiscard]] std::string_view type_kind_name(TypeKind kind);

} // namespace draft
