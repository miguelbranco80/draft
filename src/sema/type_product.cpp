// Type-facet task evaluation and deterministic publication.
//
// See type_product.h for the phase boundary and ownership contract. The code is
// intentionally direct: select the nominal representation, invoke the pure
// layout operation, apply the source-requested alignment, then publish one
// packet. No callback, query framework, or hidden recursive completion path is
// involved.

#include "sema/type_product.h"

#include "sema/type_layout.h"

#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace draft {
namespace {

// Rounds the final aggregate size to its requested array-stride alignment.
// requested is already known to be a positive power of two because attribute
// typing established that invariant before MemberTypes could complete.
[[nodiscard]] std::optional<std::uint64_t> round_up(
    std::uint64_t value, std::uint32_t requested) {
  assert(requested != 0 && (requested & (requested - 1)) == 0);
  const std::uint64_t mask = static_cast<std::uint64_t>(requested - 1);
  if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
    return std::nullopt;
  }
  return (value + mask) & ~mask;
}

[[nodiscard]] NaturalLayoutProductAttempt blocked_member_types(TypeId nominal) {
  NaturalLayoutProductAttempt result;
  result.status = TypeProductStatus::Blocked;
  result.dependencies.push_back({nominal, TypeFacet::MemberTypes});
  return result;
}

[[nodiscard]] NaturalLayoutProductAttempt blocked_natural_layouts(
    std::vector<TypeId> dependencies) {
  NaturalLayoutProductAttempt result;
  result.status = TypeProductStatus::Blocked;
  result.dependencies.reserve(dependencies.size());
  for (TypeId dependency : dependencies) {
    result.dependencies.push_back(
        {dependency, TypeFacet::NaturalLayout});
  }
  return result;
}

} // namespace

NaturalLayoutProductAttempt evaluate_natural_layout_product(
    const TypeStore &types,
    TypeId nominal,
    DiagnosticSink &diagnostics) {
  NaturalLayoutProductAttempt result;
  if (!nominal.is_valid() || nominal.value >= types.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "natural-layout product names an out-of-range type");
    return result;
  }

  const Type &type = types.type(nominal);
  if (type.kind != TypeKind::Struct && type.kind != TypeKind::Enum &&
      type.kind != TypeKind::TaggedUnion && type.kind != TypeKind::RawUnion) {
    diagnostics.error(
        type.declaration,
        "natural-layout product requires a nominal aggregate type");
    return result;
  }
  if (types.facet_state(nominal, TypeFacet::MemberTypes) ==
      TypeFacetState::Waiting) {
    return blocked_member_types(nominal);
  }
  if (types.facet_state(nominal, TypeFacet::MemberTypes) !=
      TypeFacetState::Complete) {
    diagnostics.error(
        type.declaration,
        "nominal aggregate has no member-type facet to lay out");
    return result;
  }

  NaturalAggregateLayout natural;
  if (type.kind == TypeKind::Struct) {
    natural = compute_struct_natural_layout(types, type.members);
  } else if (type.kind == TypeKind::RawUnion) {
    natural = compute_raw_union_natural_layout(types, type.members);
  } else if (type.kind == TypeKind::TaggedUnion) {
    if (!type.element.is_valid()) {
      diagnostics.error(
          type.declaration,
          "tagged union has no completed discriminator type");
      return result;
    }
    natural = compute_tagged_union_natural_layout(
        types, type.element, type.members);
  } else {
    if (!type.element.is_valid()) {
      diagnostics.error(
          type.declaration,
          "enum has no completed backing type");
      return result;
    }
    const TypeLayout backing = types.type(type.element).layout;
    if (!backing.known) {
      return blocked_natural_layouts({type.element});
    }
    natural.status = NaturalLayoutStatus::Complete;
    natural.layout = backing;
    natural.member_offsets.assign(type.members.size(), 0);
  }

  if (natural.status == NaturalLayoutStatus::Waiting) {
    return blocked_natural_layouts(std::move(natural.dependencies));
  }
  if (natural.status == NaturalLayoutStatus::Overflow) {
    diagnostics.error(type.declaration, "natural aggregate size overflows u64");
    return result;
  }

  TypeLayout layout = natural.layout;
  if (type.requested_alignment != 0) {
    if (type.requested_alignment < layout.alignment) {
      diagnostics.error(
          type.declaration,
          "'@align' cannot reduce the type's natural alignment");
      return result;
    }
    const std::optional<std::uint64_t> size =
        round_up(layout.size, type.requested_alignment);
    if (!size.has_value()) {
      diagnostics.error(
          type.declaration, "aligned aggregate size overflows u64");
      return result;
    }
    layout.size = *size;
    layout.alignment = type.requested_alignment;
  }

  result.status = TypeProductStatus::Complete;
  result.layout = layout;
  result.member_offsets = std::move(natural.member_offsets);
  return result;
}

bool publish_natural_layout_product(
    SemanticPackage &package,
    SymbolId owner,
    TypeId nominal,
    NaturalLayoutProductAttempt attempt,
    DiagnosticSink &diagnostics) {
  if (attempt.status != TypeProductStatus::Complete ||
      !attempt.layout.known) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish an incomplete natural-layout product");
    return false;
  }
  if (!owner.is_valid() || owner.value >= package.symbols.symbol_count() ||
      package.symbols.symbol(owner).type != nominal) {
    diagnostics.error(
        SourceRange::invalid(),
        "natural-layout product owner does not name its nominal type");
    return false;
  }
  if (package.types.facet_state(nominal, TypeFacet::NaturalLayout) !=
      TypeFacetState::Waiting) {
    diagnostics.error(
        package.types.type(nominal).declaration,
        "natural-layout facet was published more than once");
    return false;
  }

  std::size_t member_count = 0;
  for (const AggregateMember &member : package.aggregate_members) {
    if (member.owner == owner) ++member_count;
  }
  if (member_count != attempt.member_offsets.size()) {
    diagnostics.error(
        package.types.type(nominal).declaration,
        "natural-layout offsets do not match the nominal member set");
    return false;
  }

  std::size_t offset_index = 0;
  for (AggregateMember &member : package.aggregate_members) {
    if (member.owner != owner) continue;
    member.offset = attempt.member_offsets[offset_index];
    ++offset_index;
  }
  package.types.publish_nominal_natural_layout(
      nominal,
      attempt.layout,
      std::move(attempt.member_offsets));
  return true;
}

} // namespace draft
