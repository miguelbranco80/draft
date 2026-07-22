// Canonical Draft type construction, frozen task suffixes, and natural layout.
//
// Builtins are inserted in one fixed order so dumps and early diagnostics are
// stable, although no persistent format may rely on their numeric IDs. Structural
// interning uses direct linear scans. This is intentionally simple and correct
// for the bootstrap's early scale; a measured need may later add a deterministic
// key table without changing type identity semantics. Procedure tasks freeze a
// private copy's existing rows and return only its exact append packet; the
// coordinator publishes that packet after validating its canonical prefix.

#include "sema/type.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

namespace draft {
namespace {

// Layout construction guarantees power-of-two alignment before round_up. This
// predicate exists to assert that internal contract and is intentionally absent
// from Release call sites after NDEBUG removes the assertion.
[[maybe_unused, nodiscard]] bool is_power_of_two(std::uint32_t value) {
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
  // category. Each row points directly at its native value counterpart; code
  // generation never needs to recover semantic facts from the type spelling.
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
    const TypeId little = add_scalar(
        base_name + "le", TypeKind::EndianScalar, bit_width, alignment);
    type_mut(little).element = *native;
    type_mut(little).scalar_byte_order = ScalarByteOrder::Little;
    const TypeId big = add_scalar(
        base_name + "be", TypeKind::EndianScalar, bit_width, alignment);
    type_mut(big).element = *native;
    type_mut(big).scalar_byte_order = ScalarByteOrder::Big;
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

  // Exact type values exist only while compiling. Their meta-type therefore
  // has no runtime layout. The three reflection result enums do have ordinary
  // u8 layout so a folded query may initialize a runtime binding without
  // emitting a reflection table or a runtime query.
  Type meta_type;
  meta_type.kind = TypeKind::MetaType;
  meta_type.name = "type";
  builtins_.meta_type = add(std::move(meta_type));
  add_builtin_alias("type", builtins_.meta_type);
  builtins_.type_kind_type = add_compiler_enum("Type_Kind", builtins_.u8_type, 24);
  builtins_.type_byte_order_type = add_compiler_enum(
      "Type_Byte_Order", builtins_.u8_type, 3);
  builtins_.calling_convention_type = add_compiler_enum(
      "Calling_Convention", builtins_.u8_type, 2);
  builtins_.target_architecture_type = add_compiler_enum(
      "Target_Architecture", builtins_.u8_type, 1);
  builtins_.target_operating_system_type = add_compiler_enum(
      "Target_Operating_System", builtins_.u8_type, 2);
  builtins_.target_abi_type = add_compiler_enum(
      "Target_ABI", builtins_.u8_type, 2);
  builtins_.target_byte_order_type = add_compiler_enum(
      "Target_Byte_Order", builtins_.u8_type, 2);
  builtins_.target_object_format_type = add_compiler_enum(
      "Target_Object_Format", builtins_.u8_type, 2);
}

TypeStore::TypeStore(
    AppendOnlyOverlayTag,
    const TypeStore &base,
    bool permits_prefix_patches)
    : pointer_bits_(base.pointer_bits_), base_(&base),
      base_size_(base.size()),
      permits_prefix_patches_(permits_prefix_patches),
      builtins_(base.builtins_) {
  assert(base.base_ == nullptr);
}

const BuiltinTypes &TypeStore::builtins() const {
  return builtins_;
}

const Type &TypeStore::type(TypeId id) const {
  assert(id.is_valid());
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < size());
  if (index < base_size_) {
    assert(base_ != nullptr);
    if (const TypeStorePatch *patch = find_prefix_patch(id)) {
      return patch->type;
    }
    return base_->type(id);
  }
  return types_[index - base_size_];
}

Type &TypeStore::type_mut(TypeId id) {
  assert(id.is_valid());
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < size());
  if (index < base_size_) {
    assert(base_ != nullptr);
    assert(permits_prefix_patches_);
    if (TypeStorePatch *patch = find_prefix_patch(id)) {
      return patch->type;
    }
    prefix_patches_.push_back({id, base_->type(id), base_->completion(id)});
    return prefix_patches_.back().type;
  }
  return types_[index - base_size_];
}

const TypeCompletion &TypeStore::completion(TypeId id) const {
  assert(id.is_valid());
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < size());
  if (index < base_size_) {
    assert(base_ != nullptr);
    if (const TypeStorePatch *patch = find_prefix_patch(id)) {
      return patch->completion;
    }
    return base_->completion(id);
  }
  return completion_[index - base_size_];
}

TypeFacetState TypeStore::facet_state(TypeId id, TypeFacet facet) const {
  const TypeCompletion &facets = completion(id);
  switch (facet) {
  case TypeFacet::Identity:
    return facets.identity;
  case TypeFacet::Members:
    return facets.members;
  case TypeFacet::MemberTypes:
    return facets.member_types;
  case TypeFacet::NaturalLayout:
    return facets.natural_layout;
  }
  return TypeFacetState::NotApplicable;
}

std::size_t TypeStore::size() const {
  return base_size_ + types_.size();
}

TypeStore TypeStore::fork_append_only() const {
  return TypeStore(AppendOnlyOverlayTag{}, *this, false);
}

TypeStore TypeStore::fork_with_prefix_patches() const {
  return TypeStore(AppendOnlyOverlayTag{}, *this, true);
}

TypeStoreAppend TypeStore::appended_since(std::size_t base_size) const {
  assert(base_ != nullptr);
  assert(base_size == base_size_);
  assert(types_.size() == completion_.size());
  TypeStoreAppend appended;
  appended.base_size = base_size;
  appended.types = types_;
  appended.completions = completion_;
  return appended;
}

std::vector<TypeStorePatch> TypeStore::prefix_patches_since(
    std::size_t base_size) const {
  assert(base_ != nullptr);
  assert(base_size == base_size_);
  return prefix_patches_;
}

void TypeStore::append_exact(TypeStoreAppend appended) {
  assert(base_ == nullptr);
  assert(size() == appended.base_size);
  assert(appended.types.size() == appended.completions.size());
  types_.insert(
      types_.end(),
      std::make_move_iterator(appended.types.begin()),
      std::make_move_iterator(appended.types.end()));
  completion_.insert(
      completion_.end(),
      std::make_move_iterator(appended.completions.begin()),
      std::make_move_iterator(appended.completions.end()));
}

void TypeStore::apply_patch_exact(TypeStorePatch patch) {
  assert(base_ == nullptr);
  assert(patch.id.is_valid());
  assert(static_cast<std::size_t>(patch.id.value) < size());
  types_[patch.id.value] = std::move(patch.type);
  completion_[patch.id.value] = patch.completion;
}

TypeStorePatch *TypeStore::find_prefix_patch(TypeId id) {
  for (TypeStorePatch &patch : prefix_patches_) {
    if (patch.id == id) return &patch;
  }
  return nullptr;
}

const TypeStorePatch *TypeStore::find_prefix_patch(TypeId id) const {
  for (const TypeStorePatch &patch : prefix_patches_) {
    if (patch.id == id) return &patch;
  }
  return nullptr;
}

TypeCompletion &TypeStore::completion_mut(TypeId id) {
  assert(id.is_valid());
  const std::size_t index = static_cast<std::size_t>(id.value);
  assert(index < size());
  if (index < base_size_) {
    // type_mut owns patch creation so the Type and TypeCompletion copies can
    // never diverge or acquire different first-mutation ordering.
    (void)type_mut(id);
    TypeStorePatch *patch = find_prefix_patch(id);
    assert(patch != nullptr);
    return patch->completion;
  }
  return completion_[index - base_size_];
}

std::optional<TypeId> TypeStore::find_builtin(std::string_view name) const {
  if (base_ != nullptr) return base_->find_builtin(name);
  for (const BuiltinName &builtin : builtin_names_) {
    if (builtin.name == name) {
      return builtin.type;
    }
  }
  return std::nullopt;
}

TypeId TypeStore::add(Type type_value) {
  assert(size() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const TypeId id{static_cast<std::uint32_t>(size())};
  TypeCompletion completion;
  switch (type_value.kind) {
  case TypeKind::Tuple:
  case TypeKind::Procedure:
    completion.members = TypeFacetState::Complete;
    completion.member_types = TypeFacetState::Complete;
    break;
  case TypeKind::Struct:
  case TypeKind::Enum:
  case TypeKind::Variant:
  case TypeKind::Union:
    // begin_nominal allocates an empty shell. Compiler-defined enums arrive
    // with their complete member packet already installed.
    if (!type_value.members.empty()) {
      completion.members = TypeFacetState::Complete;
      completion.member_types = TypeFacetState::Complete;
    } else {
      completion.members = TypeFacetState::Waiting;
      completion.member_types = TypeFacetState::Waiting;
    }
    break;
  default:
    break;
  }
  if (type_value.layout.known) {
    completion.natural_layout = TypeFacetState::Complete;
  } else {
    switch (type_value.kind) {
    case TypeKind::UntypedInteger:
    case TypeKind::UntypedFloat:
    case TypeKind::MetaType:
    case TypeKind::Invalid:
      completion.natural_layout = TypeFacetState::NotApplicable;
      break;
    default:
      completion.natural_layout = TypeFacetState::Waiting;
      break;
    }
  }
  types_.push_back(std::move(type_value));
  completion_.push_back(completion);
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

TypeId TypeStore::add_compiler_enum(
    std::string name, TypeId backing, std::size_t member_count) {
  Type result;
  result.kind = TypeKind::Enum;
  result.name = name;
  result.layout = type(backing).layout;
  result.element = backing;
  result.members.assign(member_count, backing);
  result.member_offsets.assign(member_count, 0);
  const TypeId id = add(std::move(result));
  add_builtin_alias(std::move(name), id);
  return id;
}

TypeId TypeStore::pointer(TypeId element) {
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Pointer && candidate.element == element &&
        !candidate.owner_evaluated_type_application) {
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
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::MultiPointer &&
        candidate.element == element &&
        !candidate.owner_evaluated_type_application) {
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
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Slice && candidate.element == element &&
        !candidate.owner_evaluated_type_application) {
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
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Array && candidate.element == element &&
        candidate.element_count == count &&
        !candidate.owner_evaluated_type_application) {
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
    TypeId element, IntegerExpression count) {
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Array && candidate.element == element &&
        candidate.element_count_expression == count &&
        !candidate.owner_evaluated_type_application) {
      return TypeId{index};
    }
  }
  Type result;
  result.kind = TypeKind::Array;
  result.element = element;
  result.element_count_expression = std::move(count);
  return add(std::move(result));
}

TypeId TypeStore::owner_evaluated_array(
    TypeId element, std::uint32_t deferred_index) {
  // Do not intern these rows by element alone: two source expressions may call
  // different private procedures yet currently have the same element type.
  // Their defining-package side-table entries are the only equality evidence.
  Type result;
  result.kind = TypeKind::Array;
  result.element = element;
  result.owner_evaluated_element_count = true;
  result.deferred_element_count_index = deferred_index;
  return add(std::move(result));
}

TypeId TypeStore::simd(
    TypeId element,
    std::uint64_t lanes,
    SourceRange declaration) {
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Simd && candidate.element == element &&
        candidate.element_count == lanes &&
        !candidate.owner_evaluated_type_application) {
      // Structural interning may first see a type through an imported graph,
      // which has no local source location. Preserve the first useful local use
      // so a target-profile rejection points at source instead of file zero.
      if (static_cast<std::size_t>(index) >= base_size_ &&
          !candidate.declaration.is_valid() && declaration.is_valid()) {
        type_mut(TypeId{index}).declaration = declaration;
      }
      return TypeId{index};
    }
  }

  Type result;
  result.kind = TypeKind::Simd;
  result.element = element;
  result.element_count = lanes;
  result.declaration = declaration;
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
    TypeId element, IntegerExpression lanes) {
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Simd && candidate.element == element &&
        candidate.element_count_expression == lanes &&
        !candidate.owner_evaluated_type_application) {
      return TypeId{index};
    }
  }
  Type result;
  result.kind = TypeKind::Simd;
  result.element = element;
  result.element_count_expression = std::move(lanes);
  return add(std::move(result));
}

TypeId TypeStore::owner_evaluated_simd(
    TypeId element, std::uint32_t deferred_index) {
  // SIMD uses the same owner-evaluation contract as arrays. Target shape
  // validation waits for the later concrete row and never guesses lane count.
  Type result;
  result.kind = TypeKind::Simd;
  result.element = element;
  result.owner_evaluated_element_count = true;
  result.deferred_element_count_index = deferred_index;
  return add(std::move(result));
}

TypeId TypeStore::owner_evaluated_application(
    TypeId shape, std::uint32_t deferred_index) {
  // Copy the semantic shape for template-only checking, but make physical use
  // fail closed. The ordinary structural constructor will produce the final
  // canonical row after the application arguments become exact.
  Type result = type(shape);
  result.layout = {};
  result.owner_evaluated_type_application = true;
  result.deferred_type_application_index = deferred_index;
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
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Tuple && candidate.members == members &&
        !candidate.owner_evaluated_type_application) {
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

void TypeStore::complete_pending_tuple_layouts() {
  bool made_progress = true;
  while (made_progress) {
    made_progress = false;
    for (std::size_t local_index = 0; local_index < types_.size();
         ++local_index) {
      Type &pending = types_[local_index];
      if (pending.kind != TypeKind::Tuple || pending.layout.known ||
          pending.owner_evaluated_type_application) {
        continue;
      }
      const TypeLayout layout = aggregate_layout(pending.members);
      if (!layout.known) continue;

      // Compute the semantic member byte offsets at the same moment as the
      // total layout. Publishing one without the other would let LLVM choose
      // field zero for a later nonzero-offset tuple operand.
      std::vector<std::uint64_t> offsets;
      offsets.reserve(pending.members.size());
      std::uint64_t next_offset = 0;
      for (TypeId member : pending.members) {
        const TypeLayout member_layout = type(member).layout;
        const std::optional<std::uint64_t> offset =
            round_up(next_offset, member_layout.alignment);
        assert(offset.has_value());
        offsets.push_back(*offset);
        next_offset = *offset + member_layout.size;
      }
      pending.layout = layout;
      pending.member_offsets = std::move(offsets);
      completion_[local_index].natural_layout = TypeFacetState::Complete;
      made_progress = true;
    }
  }
}

TypeId TypeStore::procedure(
    const std::vector<TypeId> &parameters, TypeId result_type, bool c_calling_convention) {
  std::vector<TypeId> signature = parameters;
  signature.push_back(result_type);
  for (std::uint32_t index = 0; index < size(); ++index) {
    const Type &candidate = type(TypeId{index});
    if (candidate.kind == TypeKind::Procedure && candidate.members == signature &&
        candidate.c_calling_convention == c_calling_convention &&
        !candidate.owner_evaluated_type_application) {
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

bool TypeStore::contains_compile_time_type(TypeId id) const {
  std::vector<TypeId> active;
  return contains_compile_time_type(id, active);
}

bool TypeStore::contains_compile_time_type(
    TypeId id, std::vector<TypeId> &active) const {
  if (!id.is_valid()) return false;
  if (std::find(active.begin(), active.end(), id) != active.end()) {
    // A direct recursive aggregate is rejected while layout is computed; the
    // cycles which remain here pass through pointer/view structure. Reaching an
    // active row therefore proves only recursion, not the presence of `type`.
    return false;
  }

  const Type &candidate = type(id);
  if (candidate.kind == TypeKind::MetaType ||
      candidate.kind == TypeKind::TypeParameter) {
    return true;
  }

  active.push_back(id);
  bool contains = false;
  switch (candidate.kind) {
  case TypeKind::Pointer:
  case TypeKind::MultiPointer:
  case TypeKind::Slice:
  case TypeKind::Array:
  case TypeKind::Simd:
  case TypeKind::Distinct:
    contains = candidate.element.is_valid() &&
        contains_compile_time_type(candidate.element, active);
    break;
  case TypeKind::Tuple:
  case TypeKind::Procedure:
  case TypeKind::Struct:
  case TypeKind::Variant:
  case TypeKind::Union:
    for (TypeId member : candidate.members) {
      if (contains_compile_time_type(member, active)) {
        contains = true;
        break;
      }
    }
    break;
  case TypeKind::Invalid:
  case TypeKind::Void:
  case TypeKind::UntypedInteger:
  case TypeKind::UntypedFloat:
  case TypeKind::Bool:
  case TypeKind::BooleanStorage:
  case TypeKind::SignedInteger:
  case TypeKind::UnsignedInteger:
  case TypeKind::Float:
  case TypeKind::Rune:
  case TypeKind::EndianScalar:
  case TypeKind::RawPointer:
  case TypeKind::CString:
  case TypeKind::String:
  case TypeKind::Enum:
  case TypeKind::MetaType:
  case TypeKind::TypeParameter:
    break;
  }
  active.pop_back();
  return contains;
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
         kind == TypeKind::Variant || kind == TypeKind::Union);
  Type result;
  result.kind = kind;
  result.name = std::move(name);
  result.declaration = declaration;
  return add(std::move(result));
}

void TypeStore::publish_nominal_members(TypeId id) {
  Type &nominal = type_mut(id);
  assert(nominal.kind == TypeKind::Struct || nominal.kind == TypeKind::Enum ||
         nominal.kind == TypeKind::Variant || nominal.kind == TypeKind::Union);
  TypeCompletion &facets = completion_mut(id);
  assert(facets.members == TypeFacetState::Waiting);
  facets.members = TypeFacetState::Complete;
}

void TypeStore::publish_nominal_member_types(
    TypeId id, std::vector<TypeId> members) {
  Type &nominal = type_mut(id);
  assert(nominal.kind == TypeKind::Struct || nominal.kind == TypeKind::Enum ||
         nominal.kind == TypeKind::Variant || nominal.kind == TypeKind::Union);
  TypeCompletion &facets = completion_mut(id);
  assert(facets.member_types == TypeFacetState::Waiting);
  nominal.members = std::move(members);
  facets.member_types = TypeFacetState::Complete;
}

void TypeStore::publish_nominal_natural_layout(
    TypeId id,
    TypeLayout layout,
    std::vector<std::uint64_t> member_offsets) {
  Type &nominal = type_mut(id);
  assert(nominal.kind == TypeKind::Struct || nominal.kind == TypeKind::Enum ||
         nominal.kind == TypeKind::Variant || nominal.kind == TypeKind::Union);
  assert(layout.known);
  TypeCompletion &facets = completion_mut(id);
  assert(facets.natural_layout == TypeFacetState::Waiting);
  assert(facets.member_types == TypeFacetState::Complete);
  nominal.layout = layout;
  nominal.member_offsets = std::move(member_offsets);
  facets.natural_layout = TypeFacetState::Complete;
  // A return tuple can be interned from a procedure signature before a struct
  // declared in another selected package file is laid out. Body checking later
  // reuses that exact TypeId, so repair the canonical row here rather than
  // asking each consumer to rediscover layout.
  complete_pending_tuple_layouts();
}

bool TypeStore::is_integer(TypeId id) const {
  const Type &value = type(id);
  if (value.kind == TypeKind::Distinct) return is_integer(value.element);
  const TypeKind kind = value.kind;
  return kind == TypeKind::SignedInteger || kind == TypeKind::UnsignedInteger;
}

bool TypeStore::is_float(TypeId id) const {
  const Type &value = type(id);
  if (value.kind == TypeKind::Distinct) return is_float(value.element);
  return value.kind == TypeKind::Float;
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
  case TypeKind::Variant: return "variant";
  case TypeKind::Union: return "union";
  case TypeKind::Distinct: return "distinct";
  case TypeKind::TypeParameter: return "type parameter";
  case TypeKind::MetaType: return "type";
  }
  return "unknown type";
}

} // namespace draft
