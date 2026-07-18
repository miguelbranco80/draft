// Deterministic IEEE binary floating-point constants.
//
// Draft keeps untyped decimal values as exact rationals, but a concrete f16,
// f32, or f64 operation must round exactly as the target operation would.  This
// adapter converts between those two domains without using host floating point.
// It also represents signed zero, infinities, and NaN, which ExactRational
// intentionally cannot express.

#pragma once

#include "sema/big_integer.h"

#include <cstdint>
#include <optional>

namespace draft {

struct IeeeBinaryFormat {
  std::uint32_t exponent_bits = 0;
  std::uint32_t fraction_bits = 0;

  bool operator==(const IeeeBinaryFormat &) const = default;
};

enum class IeeeValueKind {
  Finite,
  Infinity,
  NaN,
};

struct DecodedIeeeValue {
  IeeeValueKind kind = IeeeValueKind::Finite;
  bool negative = false;
  ExactRational finite;
};

[[nodiscard]] std::optional<IeeeBinaryFormat> ieee_format_for_width(
    std::uint32_t bit_width);

// Rounds one exact finite value using round-to-nearest, ties-to-even. Overflow
// produces the correctly signed infinity and underflow may produce signed zero.
[[nodiscard]] std::optional<std::uint64_t> round_ieee_bits(
    const ExactRational &value, IeeeBinaryFormat format);

[[nodiscard]] std::optional<DecodedIeeeValue> decode_ieee_bits(
    std::uint64_t bits, IeeeBinaryFormat format);

[[nodiscard]] std::uint64_t ieee_zero_bits(
    IeeeBinaryFormat format, bool negative);
[[nodiscard]] std::uint64_t ieee_infinity_bits(
    IeeeBinaryFormat format, bool negative);
[[nodiscard]] std::uint64_t ieee_nan_bits(IeeeBinaryFormat format);

} // namespace draft
