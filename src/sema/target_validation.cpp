// Validation of target-dependent semantic types. See target_validation.h for
// phase ownership, dependency, and specification boundaries.

#include "sema/target_validation.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace draft {
namespace {

[[nodiscard]] bool supports_simd_shape(
    const TargetFacts &target,
    const Type &element,
    std::uint64_t lanes) {
  // A linear scan is deliberate: the initial profile owns only nineteen rows,
  // and preserving the plain canonical vector avoids a second search index or
  // comparator whose ordering could drift from persistent target hashing.
  // Only canonical scalar builtins can be profile entries. Aliases already use
  // the same TypeId and therefore arrive with the builtin name. Distinct and
  // endian types retain different kinds/names and cannot accidentally inherit
  // a register representation from their storage.
  for (const TargetSimdShape &shape : target.simd_shapes) {
    if (shape.element == element.name && shape.lanes == lanes) return true;
  }
  return false;
}

} // namespace

bool validate_target_types(
    const TypeStore &types,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  for (std::size_t index = 0; index < types.size(); ++index) {
    const Type &type = types.type(TypeId{static_cast<std::uint32_t>(index)});
    if (type.kind != TypeKind::Simd) continue;

    // A symbolic value parameter is checked when monomorphization creates the
    // concrete row. The element may likewise still be a constrained type
    // parameter in the generic declaration; its concrete substitution is the
    // representation-bearing type that the profile must name.
    if (type.element_count_expression.is_valid() ||
        type.owner_evaluated_element_count ||
        !type.element.is_valid() ||
        types.type(type.element).kind == TypeKind::TypeParameter) {
      continue;
    }

    const Type &element = types.type(type.element);
    if (supports_simd_shape(target, element, type.element_count)) continue;

    const std::string element_name = element.name.empty()
        ? std::string(type_kind_name(element.kind))
        : element.name;
    diagnostics.error(
        type.declaration,
        "SIMD shape 'simd[" + std::to_string(type.element_count) + "]" +
            element_name + "' is not supported by target '" +
            target.identity + "'");
  }
  return diagnostics.error_count() == initial_errors;
}

} // namespace draft
