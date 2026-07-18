// Exact-rational to IEEE binary conversion and decoding.

#include "sema/ieee_float.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace draft {
namespace {

[[nodiscard]] std::uint32_t total_bits(IeeeBinaryFormat format) {
  return 1U + format.exponent_bits + format.fraction_bits;
}

[[nodiscard]] bool valid_format(IeeeBinaryFormat format) {
  const std::uint32_t total = total_bits(format);
  return format.exponent_bits > 0 && format.fraction_bits > 0 && total <= 64;
}

[[nodiscard]] std::uint64_t low_mask(std::uint32_t bit_count) {
  if (bit_count == 0) return 0;
  if (bit_count >= 64) return std::numeric_limits<std::uint64_t>::max();
  return (std::uint64_t{1} << bit_count) - 1U;
}

[[nodiscard]] std::uint64_t sign_mask(IeeeBinaryFormat format) {
  return std::uint64_t{1} << (total_bits(format) - 1U);
}

[[nodiscard]] std::uint64_t maximum_exponent_field(
    IeeeBinaryFormat format) {
  return low_mask(format.exponent_bits);
}

[[nodiscard]] std::optional<std::uint64_t> rounded_scaled_quotient(
    const BigInteger &numerator,
    const BigInteger &denominator,
    std::int64_t binary_shift) {
  BigInteger scaled_numerator = numerator;
  BigInteger scaled_denominator = denominator;
  if (binary_shift >= 0) {
    scaled_numerator = scaled_numerator.shifted_left(
        static_cast<std::size_t>(binary_shift));
  } else {
    scaled_denominator = scaled_denominator.shifted_left(
        static_cast<std::size_t>(-binary_shift));
  }
  BigInteger quotient;
  BigInteger remainder;
  if (!scaled_numerator.divide(scaled_denominator, quotient, remainder)) {
    return std::nullopt;
  }
  const BigInteger twice_remainder = remainder.shifted_left(1);
  const int halfway = twice_remainder.compare(scaled_denominator);
  const bool quotient_is_odd =
      !quotient.bitwise_and(BigInteger::from_u64(1)).is_zero();
  if (halfway > 0 || (halfway == 0 && quotient_is_odd)) {
    quotient = quotient.added(BigInteger::from_u64(1));
  }
  return quotient.to_u64();
}

} // namespace

std::optional<IeeeBinaryFormat> ieee_format_for_width(
    std::uint32_t bit_width) {
  if (bit_width == 16) return IeeeBinaryFormat{5, 10};
  if (bit_width == 32) return IeeeBinaryFormat{8, 23};
  if (bit_width == 64) return IeeeBinaryFormat{11, 52};
  return std::nullopt;
}

std::uint64_t ieee_zero_bits(IeeeBinaryFormat format, bool negative) {
  return negative ? sign_mask(format) : 0;
}

std::uint64_t ieee_infinity_bits(IeeeBinaryFormat format, bool negative) {
  const std::uint64_t sign = negative ? sign_mask(format) : 0;
  return sign |
      (maximum_exponent_field(format) << format.fraction_bits);
}

std::uint64_t ieee_nan_bits(IeeeBinaryFormat format) {
  // One quiet canonical NaN makes constant evaluation deterministic without
  // promising source-visible payload propagation.
  return (maximum_exponent_field(format) << format.fraction_bits) |
      (std::uint64_t{1} << (format.fraction_bits - 1U));
}

std::optional<std::uint64_t> round_ieee_bits(
    const ExactRational &value, IeeeBinaryFormat format) {
  if (!valid_format(format)) return std::nullopt;
  if (value.is_zero()) return ieee_zero_bits(format, false);
  const bool negative = value.numerator().is_negative();
  const BigInteger numerator = value.numerator().absolute();
  const BigInteger denominator = value.denominator();
  const std::size_t numerator_bits = numerator.bit_count();
  const std::size_t denominator_bits = denominator.bit_count();
  if (numerator_bits > static_cast<std::size_t>(
                           std::numeric_limits<std::int64_t>::max()) ||
      denominator_bits > static_cast<std::size_t>(
                             std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }

  std::int64_t exponent = static_cast<std::int64_t>(numerator_bits) -
      static_cast<std::int64_t>(denominator_bits);
  if (exponent >= 0) {
    if (numerator.compare(
            denominator.shifted_left(static_cast<std::size_t>(exponent))) < 0) {
      --exponent;
    }
  } else if (numerator.shifted_left(static_cast<std::size_t>(-exponent))
                 .compare(denominator) < 0) {
    --exponent;
  }

  const std::uint64_t bias =
      (std::uint64_t{1} << (format.exponent_bits - 1U)) - 1U;
  const std::int64_t minimum_exponent = 1 - static_cast<std::int64_t>(bias);
  const std::int64_t maximum_exponent = static_cast<std::int64_t>(bias);
  std::uint64_t exponent_field = 0;
  std::uint64_t fraction_field = 0;
  if (exponent >= minimum_exponent) {
    if (exponent > maximum_exponent) {
      return ieee_infinity_bits(format, negative);
    }
    const std::int64_t shift =
        static_cast<std::int64_t>(format.fraction_bits) - exponent;
    std::optional<std::uint64_t> significand =
        rounded_scaled_quotient(numerator, denominator, shift);
    if (!significand.has_value()) return std::nullopt;
    const std::uint64_t implicit =
        std::uint64_t{1} << format.fraction_bits;
    if (*significand == (implicit << 1U)) {
      ++exponent;
      *significand = implicit;
      if (exponent > maximum_exponent) {
        return ieee_infinity_bits(format, negative);
      }
    }
    exponent_field = static_cast<std::uint64_t>(exponent) + bias;
    fraction_field = *significand - implicit;
  } else {
    const std::int64_t shift =
        static_cast<std::int64_t>(format.fraction_bits) - minimum_exponent;
    const std::optional<std::uint64_t> significand =
        rounded_scaled_quotient(numerator, denominator, shift);
    if (!significand.has_value()) return std::nullopt;
    const std::uint64_t implicit =
        std::uint64_t{1} << format.fraction_bits;
    if (*significand >= implicit) {
      exponent_field = 1;
      fraction_field = 0;
    } else {
      fraction_field = *significand;
    }
  }
  const std::uint64_t sign = negative ? sign_mask(format) : 0;
  return sign | (exponent_field << format.fraction_bits) | fraction_field;
}

std::optional<DecodedIeeeValue> decode_ieee_bits(
    std::uint64_t bits, IeeeBinaryFormat format) {
  if (!valid_format(format)) return std::nullopt;
  const bool negative = (bits & sign_mask(format)) != 0;
  const std::uint64_t fraction = bits & low_mask(format.fraction_bits);
  const std::uint64_t exponent_field =
      (bits >> format.fraction_bits) & low_mask(format.exponent_bits);
  if (exponent_field == maximum_exponent_field(format)) {
    return DecodedIeeeValue{
        fraction == 0 ? IeeeValueKind::Infinity : IeeeValueKind::NaN,
        negative,
        {}};
  }
  if (exponent_field == 0 && fraction == 0) {
    return DecodedIeeeValue{IeeeValueKind::Finite, negative, {}};
  }

  const std::uint64_t bias =
      (std::uint64_t{1} << (format.exponent_bits - 1U)) - 1U;
  BigInteger significand = BigInteger::from_u64(fraction);
  std::int64_t exponent;
  if (exponent_field == 0) {
    exponent = 1 - static_cast<std::int64_t>(bias) -
        static_cast<std::int64_t>(format.fraction_bits);
  } else {
    significand = significand.added(
        BigInteger::from_u64(1).shifted_left(format.fraction_bits));
    exponent = static_cast<std::int64_t>(exponent_field) -
        static_cast<std::int64_t>(bias) -
        static_cast<std::int64_t>(format.fraction_bits);
  }
  BigInteger numerator = significand;
  BigInteger denominator = BigInteger::from_u64(1);
  if (exponent >= 0) {
    numerator = numerator.shifted_left(static_cast<std::size_t>(exponent));
  } else {
    denominator = denominator.shifted_left(static_cast<std::size_t>(-exponent));
  }
  if (negative) numerator = numerator.negated();
  const std::optional<ExactRational> finite =
      ExactRational::from_fraction(std::move(numerator), std::move(denominator));
  if (!finite.has_value()) return std::nullopt;
  return DecodedIeeeValue{IeeeValueKind::Finite, negative, *finite};
}

} // namespace draft
