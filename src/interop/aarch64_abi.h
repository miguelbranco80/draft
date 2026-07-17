// AArch64 Darwin C ABI classification for semantic Draft types.
//
// This module says how one already validated C source type crosses a native
// call boundary. It intentionally does not emit LLVM syntax: the same compact
// result drives declaration signatures, call-site packing, entry unpacking,
// and return handling. Keeping the decision here prevents those four sites
// from growing similar but subtly incompatible size heuristics.

#pragma once

#include "sema/type.h"

#include <cstdint>

namespace draft {

enum class Aarch64CAbiClass {
  Illegal,
  Direct,
  HomogeneousFloatAggregate,
  SmallAggregate,
  Indirect,
};

struct Aarch64CAbiType {
  Aarch64CAbiClass classification = Aarch64CAbiClass::Illegal;
  std::uint64_t size = 0;
  std::uint32_t alignment = 1;

  // HomogeneousFloatAggregate uses one f16/f32/f64 lane kind repeated one to
  // four times. SmallAggregate uses integer containers: arguments occupy one
  // or two 64-bit registers, while results at most eight bytes use exactly the
  // meaningful bit count (i8, i24, i40, and so on).
  std::uint32_t homogeneous_element_bits = 0;
  std::uint32_t homogeneous_element_count = 0;
  std::uint32_t argument_integer_bits = 0;
  std::uint32_t argument_integer_count = 0;
  std::uint32_t result_integer_bits = 0;
  std::uint32_t result_integer_count = 0;
};

// Classifies a direct C parameter/result source type for the one initial target
// profile. Arrays are legal recursively as aggregate members but not as direct
// parameters or results. An Illegal result means semantic validation must reject
// the containing native signature before LLVM emission.
[[nodiscard]] Aarch64CAbiType classify_aarch64_darwin_c_type(
    const TypeStore &types, TypeId type);

} // namespace draft
