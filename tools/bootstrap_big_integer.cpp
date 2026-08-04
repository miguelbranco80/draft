// Production BigInteger oracle for the Draft-written integer substrate.
//
// This non-installed executable applies one deterministic operation matrix to
// src/sema/BigInteger and prints only mathematical results. The paired Draft
// executable receives the same literal spellings and operation order. Exact
// stdout, stderr, and exit status comparison therefore detects representation,
// sign, division, shift, infinite-two's-complement, conversion, and formatting
// disagreements without making either implementation consume the other's
// internal limb layout.
//
// The matrix is deliberately fixed rather than source-configurable: this is a
// phase migration oracle, not another public calculator CLI. BigInteger owns
// every allocation through ordinary C++ value lifetime and no compiler-global
// state is read or changed.

#include "sema/big_integer.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

// required_integer constructs fixed matrix state and converts an impossible
// literal failure into an explicit oracle-process failure. The returned value
// owns its limbs independently of the source spelling.
[[nodiscard]] draft::BigInteger required_integer(std::string_view spelling) {
  const std::optional<draft::BigInteger> parsed =
      draft::BigInteger::parse_literal(spelling);
  if (!parsed.has_value()) {
    std::cerr << "oracle literal did not parse: " << spelling << '\n';
    std::exit(EXIT_FAILURE);
  }
  return *parsed;
}

// emit_integer publishes one canonical label plus shortest decimal value row.
// std::cout owns all output buffering until main's final stream check.
void emit_integer(std::string_view label, const draft::BigInteger &value) {
  std::cout << label << ' ' << value.to_decimal() << '\n';
}

// emit_boolean deliberately uses 0/1, matching default C++ stream behavior and
// avoiding any language-specific boolean spelling in the paired Draft tool.
void emit_boolean(std::string_view label, bool value) {
  std::cout << label << ' ' << (value ? 1 : 0) << '\n';
}

// emit_signed publishes small metadata and the exact i64 minimum through the
// same locale-independent decimal stream configuration as the integer rows.
void emit_signed(std::string_view label, std::int64_t value) {
  std::cout << label << ' ' << value << '\n';
}

} // namespace

int main() {
  // Construct the shared operands once. Every later operation returns an
  // independent value, so no result can mutate matrix input state.
  const draft::BigInteger zero = required_integer("0");
  const draft::BigInteger power =
      required_integer("340282366920938463463374607431768211456");
  const draft::BigInteger wide = required_integer(
      "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  const draft::BigInteger left =
      required_integer("123456789012345678901234567890");
  const draft::BigInteger right = required_integer("98765432109876543210");
  const draft::BigInteger negative_left = left.negated();
  const draft::BigInteger negative_right = right.negated();

  // Literal rows cover every supported radix, ignored separators, and the
  // closed invalid cases mirrored by the Draft parser.
  emit_integer("parse-zero", zero);
  emit_integer("parse-hex",
               required_integer("0xffffffffffffffffffffffffffffffff"));
  emit_integer("parse-binary", required_integer("0b1010_0101"));
  emit_integer("parse-octal", required_integer("0o755"));
  emit_integer("parse-underscores", required_integer("1_000_000"));
  emit_boolean("parse-invalid-empty",
               draft::BigInteger::parse_literal("").has_value());
  emit_boolean("parse-invalid-prefix",
               draft::BigInteger::parse_literal("0x").has_value());
  emit_boolean("parse-invalid-digit",
               draft::BigInteger::parse_literal("0b102").has_value());

  // Metadata, comparison, equality, and absolute-value rows expose the
  // canonical sign/magnitude interpretation without reading internal limbs.
  emit_signed("zero-sign", zero.sign());
  emit_signed("power-sign", power.sign());
  emit_signed("negative-sign", negative_left.sign());
  emit_signed("power-bits", static_cast<std::int64_t>(power.bit_count()));
  emit_signed("wide-bits", static_cast<std::int64_t>(wide.bit_count()));
  emit_signed("compare-left-right", left.compare(right));
  emit_signed("compare-negative-right", negative_left.compare(right));
  emit_signed("compare-equal", left.compare(left));
  emit_boolean("equal-left-copy",
               left == required_integer("123456789012345678901234567890"));
  emit_integer("absolute-negative", negative_left.absolute());

  // Signed arithmetic includes cancellation and a 512-bit product, preventing
  // native u128 from accidentally serving as the arbitrary-precision domain.
  emit_integer("add", left.added(right));
  emit_integer("subtract", left.subtracted(right));
  emit_integer("reverse-subtract", right.subtracted(left));
  emit_integer("cancel", left.added(negative_left));
  emit_integer("multiply", left.multiplied(right));
  emit_integer("wide-square", wide.multiplied(wide));

  draft::BigInteger quotient;
  draft::BigInteger remainder;
  // Division exercises every sign combination. Quotient truncation and the
  // dividend-signed remainder appear on independent rows.
  emit_boolean("divide-positive-ok", left.divide(right, quotient, remainder));
  emit_integer("divide-positive-quotient", quotient);
  emit_integer("divide-positive-remainder", remainder);
  emit_boolean("divide-negative-dividend-ok",
               negative_left.divide(right, quotient, remainder));
  emit_integer("divide-negative-dividend-quotient", quotient);
  emit_integer("divide-negative-dividend-remainder", remainder);
  emit_boolean("divide-negative-divisor-ok",
               left.divide(negative_right, quotient, remainder));
  emit_integer("divide-negative-divisor-quotient", quotient);
  emit_integer("divide-negative-divisor-remainder", remainder);
  emit_boolean("divide-two-negatives-ok",
               negative_left.divide(negative_right, quotient, remainder));
  emit_integer("divide-two-negatives-quotient", quotient);
  emit_integer("divide-two-negatives-remainder", remainder);
  emit_boolean("divide-zero-ok", left.divide(zero, quotient, remainder));

  // Shift rows cross several limbs and make negative right-shift rounding and
  // infinite sign extension directly observable.
  emit_integer("shift-left-129",
               draft::BigInteger::from_u64(1).shifted_left(129));
  emit_integer("shift-left-negative",
               draft::BigInteger::from_i64(-3).shifted_left(65));
  emit_integer("shift-right-positive", wide.shifted_right(129));
  emit_integer("shift-right-negative",
               draft::BigInteger::from_i64(-3).shifted_right(1));
  emit_integer("shift-right-negative-wide",
               draft::BigInteger::from_i64(-3).shifted_right(200));

  // Mixed-sign bitwise rows distinguish infinite two's-complement behavior
  // from any fixed native integer width.
  emit_integer(
      "bitwise-and-positive",
      power.subtracted(draft::BigInteger::from_u64(1)).bitwise_and(wide));
  emit_integer("bitwise-and-negative",
               draft::BigInteger::from_i64(-1).bitwise_and(
                   power.subtracted(draft::BigInteger::from_u64(1))));
  emit_integer("bitwise-or-negative",
               draft::BigInteger::from_i64(-8).bitwise_or(
                   draft::BigInteger::from_i64(3)));
  emit_integer("bitwise-xor-negative",
               draft::BigInteger::from_i64(-8).bitwise_xor(
                   draft::BigInteger::from_i64(3)));
  emit_integer("bitwise-not-positive", power.bitwise_not());
  emit_integer("bitwise-not-negative",
               draft::BigInteger::from_i64(-8).bitwise_not());

  // Conversion rows straddle each unsigned and signed host boundary. The
  // final value row proves that successful i64-min conversion is exact.
  const draft::BigInteger u64_max = required_integer("18446744073709551615");
  const draft::BigInteger u64_too_large =
      required_integer("18446744073709551616");
  const draft::BigInteger i64_max = required_integer("9223372036854775807");
  const draft::BigInteger i64_too_large =
      required_integer("9223372036854775808");
  const draft::BigInteger i64_min = i64_too_large.negated();
  const draft::BigInteger i64_too_small =
      required_integer("9223372036854775809").negated();

  emit_boolean("to-u64-max", u64_max.to_u64().has_value());
  emit_boolean("to-u64-too-large", u64_too_large.to_u64().has_value());
  emit_boolean("to-u64-negative",
               draft::BigInteger::from_i64(-1).to_u64().has_value());
  emit_boolean("to-i64-max", i64_max.to_i64().has_value());
  emit_boolean("to-i64-too-large", i64_too_large.to_i64().has_value());
  emit_boolean("to-i64-min", i64_min.to_i64().has_value());
  emit_boolean("to-i64-too-small", i64_too_small.to_i64().has_value());
  if (const std::optional<std::int64_t> converted = i64_min.to_i64()) {
    emit_signed("to-i64-min-value", *converted);
  } else {
    std::cerr << "oracle i64 minimum conversion failed\n";
    return EXIT_FAILURE;
  }

  if (!std::cout || !std::cerr)
    return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
