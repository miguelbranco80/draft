// Arbitrary-precision integer and exact decimal rational conformance tests.
//
// Expected values are written as decimal strings so the test does not rely on
// host integer overflow behavior. Negative bitwise cases exercise the language's
// infinite two's-complement rule, which is the easiest part to accidentally
// implement as a fixed-width host operation.

#include "sema/big_integer.h"
#include "sema/ieee_float.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "big_integer_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

draft::BigInteger integer(std::string_view spelling) {
  const std::optional<draft::BigInteger> parsed =
      draft::BigInteger::parse_literal(spelling);
  if (!parsed.has_value()) {
    std::cerr << "test integer did not parse: " << spelling << '\n';
    std::exit(EXIT_FAILURE);
  }
  return *parsed;
}

void test_arithmetic(TestState &state) {
  const draft::BigInteger power = integer("340282366920938463463374607431768211456");
  EXPECT(state, power.bit_count() == 129);
  EXPECT(state, power.added(draft::BigInteger::from_u64(7)).to_decimal() ==
                    "340282366920938463463374607431768211463");
  EXPECT(state, integer("0xffffffffffffffffffffffffffffffff").to_decimal() ==
                    "340282366920938463463374607431768211455");

  const draft::BigInteger left = integer("123456789012345678901234567890");
  const draft::BigInteger right = integer("98765432109876543210");
  EXPECT(state, left.multiplied(right).to_decimal() ==
                    "12193263113702179522496570642237463801111263526900");

  draft::BigInteger quotient;
  draft::BigInteger remainder;
  EXPECT(state, left.divide(right, quotient, remainder));
  EXPECT(state, quotient.to_decimal() == "1249999988");
  EXPECT(state, remainder.to_decimal() == "60185185207253086410");

  const draft::BigInteger negative = left.negated();
  EXPECT(state, negative.divide(right, quotient, remainder));
  EXPECT(state, quotient.to_decimal() == "-1249999988");
  EXPECT(state, remainder.to_decimal() == "-60185185207253086410");
}

void test_infinite_twos_complement(TestState &state) {
  const draft::BigInteger minus_three = draft::BigInteger::from_i64(-3);
  EXPECT(state, minus_three.shifted_right(1).to_decimal() == "-2");
  EXPECT(state, minus_three.shifted_right(200).to_decimal() == "-1");
  EXPECT(state, minus_three.bitwise_not().to_decimal() == "2");
  EXPECT(state, draft::BigInteger::from_i64(-1)
                    .bitwise_and(integer("340282366920938463463374607431768211455"))
                    .to_decimal() == "340282366920938463463374607431768211455");
  EXPECT(state, draft::BigInteger::from_i64(-8)
                    .bitwise_or(draft::BigInteger::from_i64(3))
                    .to_decimal() == "-5");
  EXPECT(state, draft::BigInteger::from_i64(-8)
                    .bitwise_xor(draft::BigInteger::from_i64(3))
                    .to_decimal() == "-5");
}

void test_exact_rationals(TestState &state) {
  const std::optional<draft::ExactRational> first =
      draft::ExactRational::parse_decimal("1.2500");
  const std::optional<draft::ExactRational> second =
      draft::ExactRational::parse_decimal("12.5e-1");
  const std::optional<draft::ExactRational> thousand =
      draft::ExactRational::parse_decimal("1e3");
  EXPECT(state, first.has_value());
  EXPECT(state, second.has_value());
  EXPECT(state, thousand.has_value());
  if (!first.has_value() || !second.has_value() || !thousand.has_value()) return;
  EXPECT(state, first->to_fraction() == "5/4");
  EXPECT(state, first->compare(*second) == 0);
  EXPECT(state, thousand->to_fraction() == "1000/1");
  EXPECT(state, first->multiplied(*thousand).to_fraction() == "1250/1");

  draft::ExactRational quotient;
  EXPECT(state, first->divide(*thousand, quotient));
  EXPECT(state, quotient.to_fraction() == "1/800");
}

void test_ieee_rounding(TestState &state) {
  const std::optional<draft::IeeeBinaryFormat> f32 =
      draft::ieee_format_for_width(32);
  const std::optional<draft::IeeeBinaryFormat> f64 =
      draft::ieee_format_for_width(64);
  const std::optional<draft::ExactRational> tenth =
      draft::ExactRational::parse_decimal("0.1");
  EXPECT(state, f32.has_value());
  EXPECT(state, f64.has_value());
  EXPECT(state, tenth.has_value());
  if (!f32.has_value() || !f64.has_value() || !tenth.has_value()) return;

  EXPECT(state, draft::round_ieee_bits(*tenth, *f32) == 1036831949U);
  EXPECT(state, draft::round_ieee_bits(*tenth, *f64) ==
                    4591870180066957722ULL);

  const std::optional<draft::ExactRational> halfway_even_down =
      draft::ExactRational::from_fraction(
          draft::BigInteger::from_u64(16777217),
          draft::BigInteger::from_u64(16777216));
  const std::optional<draft::ExactRational> halfway_even_up =
      draft::ExactRational::from_fraction(
          draft::BigInteger::from_u64(16777219),
          draft::BigInteger::from_u64(16777216));
  EXPECT(state, halfway_even_down.has_value());
  EXPECT(state, halfway_even_up.has_value());
  if (halfway_even_down.has_value() && halfway_even_up.has_value()) {
    EXPECT(state, draft::round_ieee_bits(*halfway_even_down, *f32) ==
                      0x3f800000U);
    EXPECT(state, draft::round_ieee_bits(*halfway_even_up, *f32) ==
                      0x3f800002U);
  }

  const std::optional<draft::ExactRational> huge =
      draft::ExactRational::parse_decimal("1e1000");
  EXPECT(state, huge.has_value());
  if (huge.has_value()) {
    EXPECT(state, draft::round_ieee_bits(*huge, *f32) == 0x7f800000U);
  }

  const std::optional<draft::DecodedIeeeValue> one =
      draft::decode_ieee_bits(0x3f800000U, *f32);
  const std::optional<draft::DecodedIeeeValue> negative_zero =
      draft::decode_ieee_bits(0x80000000U, *f32);
  const std::optional<draft::DecodedIeeeValue> infinity =
      draft::decode_ieee_bits(0x7f800000U, *f32);
  const std::optional<draft::DecodedIeeeValue> nan =
      draft::decode_ieee_bits(0x7fc00000U, *f32);
  EXPECT(state, one.has_value());
  EXPECT(state, negative_zero.has_value());
  EXPECT(state, infinity.has_value());
  EXPECT(state, nan.has_value());
  if (one.has_value()) EXPECT(state, one->finite.to_fraction() == "1/1");
  if (negative_zero.has_value()) {
    EXPECT(state, negative_zero->kind == draft::IeeeValueKind::Finite);
    EXPECT(state, negative_zero->negative);
    EXPECT(state, negative_zero->finite.is_zero());
  }
  if (infinity.has_value()) {
    EXPECT(state, infinity->kind == draft::IeeeValueKind::Infinity);
  }
  if (nan.has_value()) EXPECT(state, nan->kind == draft::IeeeValueKind::NaN);
}

} // namespace

int main() {
  TestState state;
  test_arithmetic(state);
  test_infinite_twos_complement(state);
  test_exact_rationals(state);
  test_ieee_rounding(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " big-integer test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all big-integer tests passed\n";
  return EXIT_SUCCESS;
}
