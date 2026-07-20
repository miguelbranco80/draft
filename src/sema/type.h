// Canonical semantic type storage and target-native layout facts.
//
// TypeStore assigns a stable TypeId to every built-in, structural, parametric,
// and nominal type in one semantic program. Structural constructors are interned:
// asking twice for `^u32` or `(u32, bool)` returns the same ID. Nominal types are
// created once at their declarations and remain distinct even when layout is
// identical. No syntax or LLVM type is stored here; source for an owner-evaluated
// generic count lives in SemanticPackage's side table. This table is Draft's
// type identity authority and will later be reproduced by the self-hosted compiler.
//
// Layout values are byte counts for the selected target profile. TypeStore also
// records the readiness of identity, members, member types, and natural layout
// separately. A nominal identity therefore exists before its member set closes,
// and a pointer to that identity can be complete while an inline use still waits
// for layout. The semantic work graph uses those facets as distinct product
// payloads; an unknown layout must never stand in for every earlier type fact.
//
// Relevant specification: docs/specification/02-types-memory-runtime.md, sections 5-6.

#pragma once

#include "sema/integer_expression.h"
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
  // MetaType is the compile-time-only type of exact Draft type values. It has
  // no layout and must be evaluated away before MIR. Keeping it in the
  // canonical type table lets ordinary expression checking represent
  // `type_of(value)`, type-valued constants, and equality without inventing a
  // parallel expression type system.
  MetaType,
};

// Endian scalar rows retain their byte order as semantic data. Keeping this
// fact out of the spelling avoids making the backend decode names such as
// `u32be`, and gives later target profiles one direct place to consult it.
enum class ScalarByteOrder {
  Native,
  Little,
  Big,
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

// TypeFacetState describes whether one facet has meaning and, when it does,
// whether its immutable value has been published. NotApplicable is a completed
// semantic answer rather than a scheduling wait: scalar types have no member
// set, and compile-time-only `type` values have no runtime natural layout.
// Waiting means another semantic product can still supply the facet. Errors are
// owned by the semantic product state and diagnostics, not encoded in this
// payload table.
enum class TypeFacetState {
  NotApplicable,
  Waiting,
  Complete,
};

// TypeCompletion is parallel to one TypeStore row. identity is Complete for
// every allocated TypeId because allocation itself establishes the structural
// or nominal identity. members closes the set of member names; member_types
// closes the declared type attached to every member. natural_layout is
// independent: pointer recursion can leave it Waiting while the preceding
// facets are Complete. The record is command-local and is never serialized.
struct TypeCompletion {
  TypeFacetState identity = TypeFacetState::Complete;
  TypeFacetState members = TypeFacetState::NotApplicable;
  TypeFacetState member_types = TypeFacetState::NotApplicable;
  TypeFacetState natural_layout = TypeFacetState::NotApplicable;
};

// Type is one canonical table row. Fields have meaning according to kind:
//
// - scalar kinds use bit_width and layout; EndianScalar additionally uses
//   element for its native counterpart and scalar_byte_order for storage order;
// - Pointer, MultiPointer, Slice, Array, Simd, and Distinct use element;
// - Array and Simd use element_count;
// - an Array or Simd whose count must run in its defining package sets
//   owner_evaluated_element_count and uses deferred_element_count_index only as
//   a package-round side-table key; neither package-local number is serialized;
// - a structural alias application whose value argument needs a full procedure
//   sets owner_evaluated_type_application. Its shape fields remain available to
//   symbolic checking, but its layout is unknown until the application recipe
//   is concrete;
// - Tuple and Procedure use members (procedure parameters followed by result);
// - nominal kinds use name, declaration, and an initially unknown layout.
//
// Procedure members always contain a final result TypeId; void procedures use
// the canonical void type there. c_calling_convention distinguishes physical
// procedure pointer identities because ordinary procedures carry Draft context.
// c_representation and requested_alignment are meaningful only on nominal
// aggregates. They remain on the type after layout so ABI validation and
// package-interface reconstruction do not infer source attributes from bytes.
struct Type {
  TypeKind kind = TypeKind::Invalid;
  std::string name;
  TypeLayout layout;
  std::uint32_t bit_width = 0;
  TypeId element;
  ScalarByteOrder scalar_byte_order = ScalarByteOrder::Native;
  std::uint64_t element_count = 0;
  // A dependent array/SIMD count retains the complete symbolic integer
  // expression. Parameter leaves contain local ValueParameter SymbolIds.
  // Concrete rows leave this invalid and use element_count above.
  IntegerExpression element_count_expression;
  // Some parameter-dependent constants cannot be reduced to the compact
  // IntegerExpression tree because they call ordinary compile-time procedures.
  // The defining package must evaluate those calls after concrete arguments
  // arrive. The index names SemanticPackage::deferred_element_counts and is
  // invalid on an imported interface marker until an owner request is formed.
  bool owner_evaluated_element_count = false;
  std::uint32_t deferred_element_count_index =
      std::numeric_limits<std::uint32_t>::max();
  bool owner_evaluated_type_application = false;
  std::uint32_t deferred_type_application_index =
      std::numeric_limits<std::uint32_t>::max();
  std::vector<TypeId> members;
  std::vector<std::uint64_t> member_offsets;
  bool c_calling_convention = false;
  bool c_representation = false;
  std::uint32_t requested_alignment = 0;
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
  TypeId meta_type;
  // Compiler-defined enums are ordinary scalar compile-time results. Their
  // source spellings and member ordinals are a stable Draft contract; they
  // deliberately do not expose the host C++ enum values used by the bootstrap
  // implementation. The five target enums keep categorical target fields
  // nominally distinct: sharing a ConstantKind or backing u8 does not make an
  // operating system comparable to an architecture or object format.
  TypeId type_kind_type;
  TypeId type_byte_order_type;
  TypeId calling_convention_type;
  TypeId target_architecture_type;
  TypeId target_operating_system_type;
  TypeId target_abi_type;
  TypeId target_byte_order_type;
  TypeId target_object_format_type;
};

class TypeStore {
public:
  // Draft 1's initial target has 64-bit pointers and natural integers. Keeping
  // the width explicit makes the table construction reusable when profiles grow.
  explicit TypeStore(std::uint32_t pointer_bits = 64);

  [[nodiscard]] const BuiltinTypes &builtins() const;
  [[nodiscard]] const Type &type(TypeId id) const;
  [[nodiscard]] Type &type_mut(TypeId id);
  [[nodiscard]] const TypeCompletion &completion(TypeId id) const;
  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] std::optional<TypeId> find_builtin(std::string_view name) const;

  [[nodiscard]] TypeId pointer(TypeId element);
  [[nodiscard]] TypeId multi_pointer(TypeId element);
  [[nodiscard]] TypeId slice(TypeId element);
  [[nodiscard]] TypeId array(TypeId element, std::uint64_t count);
  [[nodiscard]] TypeId simd(
      TypeId element,
      std::uint64_t lanes,
      SourceRange declaration = SourceRange::invalid());
  [[nodiscard]] TypeId parametric_array(
      TypeId element, IntegerExpression count);
  [[nodiscard]] TypeId parametric_simd(
      TypeId element, IntegerExpression lanes);
  // Creates a deliberately non-interned symbolic row. Until the defining
  // package evaluates its side-table request, no element count or layout is
  // available. deferred_index is package-local and may be invalid for an
  // imported marker; callers must never treat it as interface identity.
  [[nodiscard]] TypeId owner_evaluated_array(
      TypeId element,
      std::uint32_t deferred_index = std::numeric_limits<std::uint32_t>::max());
  [[nodiscard]] TypeId owner_evaluated_simd(
      TypeId element,
      std::uint32_t deferred_index = std::numeric_limits<std::uint32_t>::max());
  // Clones an alias result into a deliberately non-interned placeholder. The
  // clone does not create identity—even if the alias targets a nominal type—
  // because its side-table application is evaluated away before any
  // runtime-bearing type identity is observed.
  [[nodiscard]] TypeId owner_evaluated_application(
      TypeId shape,
      std::uint32_t deferred_index = std::numeric_limits<std::uint32_t>::max());
  [[nodiscard]] TypeId tuple(const std::vector<TypeId> &members);
  [[nodiscard]] TypeId procedure(
      const std::vector<TypeId> &parameters, TypeId result, bool c_calling_convention);
  [[nodiscard]] TypeId distinct(std::string name, TypeId underlying, SourceRange declaration);
  [[nodiscard]] TypeId type_parameter(std::string name, SourceRange declaration);
  [[nodiscard]] TypeId begin_nominal(
      TypeKind kind, std::string name, SourceRange declaration);
  void complete_nominal(
      TypeId id,
      TypeLayout layout,
      std::vector<TypeId> members,
      std::vector<std::uint64_t> member_offsets = {},
      bool members_complete = true,
      bool member_types_complete = true);

  [[nodiscard]] bool is_integer(TypeId id) const;
  [[nodiscard]] bool is_float(TypeId id) const;
  [[nodiscard]] bool is_number(TypeId id) const;
  // Reports whether a value of this type would contain Draft's layout-less
  // compile-time `type` value, directly or through an aggregate, view, pointer,
  // distinct wrapper, or procedure signature. Recursive nominal graphs are
  // followed with cycle detection. TypeParameter also returns true because a
  // symbolic template must be specialized before this question can become a
  // runtime representation decision.
  [[nodiscard]] bool contains_compile_time_type(TypeId id) const;

private:
  [[nodiscard]] TypeId add(Type type);
  void add_builtin_alias(std::string name, TypeId id);
  [[nodiscard]] TypeId add_scalar(
      std::string name, TypeKind kind, std::uint32_t bits, std::uint32_t alignment);
  [[nodiscard]] TypeId add_compiler_enum(
      std::string name, TypeId backing, std::size_t member_count);
  [[nodiscard]] TypeLayout aggregate_layout(const std::vector<TypeId> &members) const;
  [[nodiscard]] bool contains_compile_time_type(
      TypeId id, std::vector<TypeId> &active) const;
  // Procedure signatures may intern tuples while a nominal member is still a
  // forward-declared shell. Completing that nominal must publish any newly
  // computable tuple layout before body checking or MIR observes the tuple.
  // A fixpoint handles tuples nested inside other pending tuples.
  void complete_pending_tuple_layouts();

  struct BuiltinName {
    std::string name;
    TypeId type;
  };

  std::uint32_t pointer_bits_ = 64;
  std::vector<Type> types_;
  // completion_ has exactly one row per types_ entry and shares its TypeId
  // index domain. Keeping the facts parallel avoids enlarging the hot Type row
  // while making phase readiness directly inspectable.
  std::vector<TypeCompletion> completion_;
  std::vector<BuiltinName> builtin_names_;
  BuiltinTypes builtins_;
};

[[nodiscard]] std::string_view type_kind_name(TypeKind kind);

} // namespace draft
