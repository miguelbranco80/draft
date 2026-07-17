// Arbitrary-precision integer and exact decimal rational conformance tests.
//
// Expected values are written as decimal strings so the test does not rely on
// host integer overflow behavior. Negative bitwise cases exercise the language's
// infinite two's-complement rule, which is the easiest part to accidentally
// implement as a fixed-width host operation.

#include "sema/big_integer.h"

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

} // namespace

int main() {
  TestState state;
  test_arithmetic(state);
  test_infinite_twos_complement(state);
  test_exact_rationals(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " big-integer test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all big-integer tests passed\n";
  return EXIT_SUCCESS;
}
