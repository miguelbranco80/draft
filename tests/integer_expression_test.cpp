#include "sema/integer_expression.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (condition) return;
    ++failures;
    std::cerr << "integer_expression_test.cpp:" << line
              << ": expectation failed: " << expression << '\n';
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

draft::IntegerExpressionType unsigned_type(std::uint32_t bits) {
  return {
      draft::IntegerExpressionRepresentation::Unsigned,
      bits,
      "u" + std::to_string(bits),
  };
}

draft::IntegerExpressionType signed_type(std::uint32_t bits) {
  return {
      draft::IntegerExpressionRepresentation::Signed,
      bits,
      "i" + std::to_string(bits),
  };
}

void test_substitution_and_wrapping(TestState &state) {
  draft::IntegerExpression expression;
  const std::uint32_t parameter = draft::append_integer_parameter(
      expression, 17, unsigned_type(8));
  const std::uint32_t one = draft::append_integer_constant(
      expression, draft::BigInteger::from_u64(1));
  expression.root = draft::append_integer_binary(
      expression,
      draft::IntegerExpressionOperation::Add,
      parameter,
      one,
      unsigned_type(8));

  EXPECT(state, expression.is_valid());
  EXPECT(state, draft::integer_expression_has_parameters(expression));
  EXPECT(state, !draft::single_integer_parameter(expression).has_value());

  std::string error;
  const std::optional<draft::IntegerExpression> substituted =
      draft::substitute_integer_expression(
          expression,
          {{17, draft::BigInteger::from_u64(255), {}}},
          error);
  EXPECT(state, substituted.has_value());
  if (!substituted.has_value()) return;
  EXPECT(state, !draft::integer_expression_has_parameters(*substituted));
  const draft::IntegerExpressionResult result =
      draft::evaluate_integer_expression(*substituted);
  EXPECT(state, result.ok);
  EXPECT(state, result.value == draft::BigInteger::from_u64(0));
}

void test_composed_substitution_and_remapping(TestState &state) {
  draft::IntegerExpression outer;
  outer.root = draft::append_integer_parameter(
      outer, 10, unsigned_type(64));

  draft::IntegerExpression inner;
  const std::uint32_t parameter = draft::append_integer_parameter(
      inner, 20, unsigned_type(64));
  const std::uint32_t two = draft::append_integer_constant(
      inner, draft::BigInteger::from_u64(2));
  inner.root = draft::append_integer_binary(
      inner,
      draft::IntegerExpressionOperation::Multiply,
      parameter,
      two,
      unsigned_type(64));

  std::string error;
  const std::optional<draft::IntegerExpression> composed =
      draft::substitute_integer_expression(outer, {{10, {}, inner}}, error);
  EXPECT(state, composed.has_value());
  if (!composed.has_value()) return;
  EXPECT(state, draft::integer_expression_has_parameters(*composed));

  const std::optional<draft::IntegerExpression> remapped =
      draft::remap_integer_expression(*composed, {{20, 0}});
  EXPECT(state, remapped.has_value());
  if (!remapped.has_value()) return;
  EXPECT(state,
         draft::integer_expression_identity(*remapped).find("pu64_0") !=
             std::string::npos);

  const std::optional<draft::IntegerExpression> concrete =
      draft::substitute_integer_expression(
          *remapped,
          {{0, draft::BigInteger::from_u64(21), {}}},
          error);
  EXPECT(state, concrete.has_value());
  if (!concrete.has_value()) return;
  const draft::IntegerExpressionResult result =
      draft::evaluate_integer_expression(*concrete);
  EXPECT(state, result.ok);
  EXPECT(state, result.value == draft::BigInteger::from_u64(42));
}

void test_integer_traps(TestState &state) {
  draft::IntegerExpression division;
  const std::uint32_t minimum = draft::append_integer_constant(
      division, draft::BigInteger::from_i64(-128), signed_type(8));
  const std::uint32_t negative_one = draft::append_integer_constant(
      division, draft::BigInteger::from_i64(-1), signed_type(8));
  division.root = draft::append_integer_binary(
      division,
      draft::IntegerExpressionOperation::Divide,
      minimum,
      negative_one,
      signed_type(8));
  const draft::IntegerExpressionResult division_result =
      draft::evaluate_integer_expression(division);
  EXPECT(state, !division_result.ok);
  EXPECT(state, division_result.error.find("signed minimum") != std::string::npos);

  draft::IntegerExpression shift;
  const std::uint32_t value = draft::append_integer_constant(
      shift, draft::BigInteger::from_u64(1), unsigned_type(8));
  const std::uint32_t count = draft::append_integer_constant(
      shift, draft::BigInteger::from_u64(8));
  shift.root = draft::append_integer_binary(
      shift,
      draft::IntegerExpressionOperation::ShiftLeft,
      value,
      count,
      unsigned_type(8));
  const draft::IntegerExpressionResult shift_result =
      draft::evaluate_integer_expression(shift);
  EXPECT(state, !shift_result.ok);
  EXPECT(state, shift_result.error.find("out-of-range shift") != std::string::npos);
}

void test_malformed_and_resource_limited_trees(TestState &state) {
  draft::IntegerExpression detached;
  static_cast<void>(draft::append_integer_constant(
      detached, draft::BigInteger::from_u64(99)));
  detached.root = draft::append_integer_constant(
      detached, draft::BigInteger::from_u64(1));
  EXPECT(state, !detached.is_valid());

  draft::IntegerExpression invalid_width;
  invalid_width.root = draft::append_integer_constant(
      invalid_width,
      draft::BigInteger::from_u64(1),
      {draft::IntegerExpressionRepresentation::Unsigned, 0, "u0"});
  EXPECT(state, !invalid_width.is_valid());

  draft::IntegerExpression enormous_shift;
  const std::uint32_t value = draft::append_integer_constant(
      enormous_shift, draft::BigInteger::from_u64(1));
  const std::uint32_t count = draft::append_integer_constant(
      enormous_shift, draft::BigInteger::from_u64(1'000'001));
  enormous_shift.root = draft::append_integer_binary(
      enormous_shift,
      draft::IntegerExpressionOperation::ShiftLeft,
      value,
      count,
      {});
  const draft::IntegerExpressionResult result =
      draft::evaluate_integer_expression(enormous_shift);
  EXPECT(state, !result.ok);
  EXPECT(state, result.error.find("out-of-range shift") != std::string::npos);
}

void test_explicit_integer_cast(TestState &state) {
  draft::IntegerExpression expression;
  const std::uint32_t value = draft::append_integer_constant(
      expression, draft::BigInteger::from_u64(256));
  expression.root = draft::append_integer_unary(
      expression,
      draft::IntegerExpressionOperation::Cast,
      value,
      unsigned_type(8));
  const draft::IntegerExpressionResult result =
      draft::evaluate_integer_expression(expression);
  EXPECT(state, result.ok);
  EXPECT(state, result.value == draft::BigInteger::from_u64(0));
}

void test_unique_inverse_solver(TestState &state) {
  draft::IntegerExpression offset;
  const std::uint32_t parameter = draft::append_integer_parameter(
      offset, 17, unsigned_type(8));
  const std::uint32_t one = draft::append_integer_constant(
      offset, draft::BigInteger::from_u64(1), unsigned_type(8));
  offset.root = draft::append_integer_binary(
      offset,
      draft::IntegerExpressionOperation::Add,
      parameter,
      one,
      unsigned_type(8));
  const std::optional<draft::IntegerExpressionSolution> wrapped =
      draft::solve_unique_integer_expression(
          offset, draft::BigInteger::from_u64(0));
  EXPECT(state, wrapped.has_value());
  if (wrapped.has_value()) {
    EXPECT(state, wrapped->parameter == 17);
    EXPECT(state, wrapped->value == draft::BigInteger::from_u64(255));
  }

  draft::IntegerExpression reverse;
  const std::uint32_t ten = draft::append_integer_constant(
      reverse, draft::BigInteger::from_u64(10), unsigned_type(8));
  const std::uint32_t reverse_parameter = draft::append_integer_parameter(
      reverse, 23, unsigned_type(8));
  reverse.root = draft::append_integer_binary(
      reverse,
      draft::IntegerExpressionOperation::Subtract,
      ten,
      reverse_parameter,
      unsigned_type(8));
  const std::optional<draft::IntegerExpressionSolution> reversed =
      draft::solve_unique_integer_expression(
          reverse, draft::BigInteger::from_u64(7));
  EXPECT(state, reversed.has_value());
  if (reversed.has_value()) {
    EXPECT(state, reversed->parameter == 23);
    EXPECT(state, reversed->value == draft::BigInteger::from_u64(3));
  }

  draft::IntegerExpression widening_cast;
  const std::uint32_t narrow_parameter = draft::append_integer_parameter(
      widening_cast, 29, unsigned_type(8));
  widening_cast.root = draft::append_integer_unary(
      widening_cast,
      draft::IntegerExpressionOperation::Cast,
      narrow_parameter,
      unsigned_type(16));
  const std::optional<draft::IntegerExpressionSolution> widened =
      draft::solve_unique_integer_expression(
          widening_cast, draft::BigInteger::from_u64(255));
  EXPECT(state, widened.has_value());
  if (widened.has_value()) {
    EXPECT(state, widened->parameter == 29);
    EXPECT(state, widened->value == draft::BigInteger::from_u64(255));
  }

  draft::IntegerExpression narrowing_cast;
  const std::uint32_t wide_parameter = draft::append_integer_parameter(
      narrowing_cast, 30, unsigned_type(16));
  narrowing_cast.root = draft::append_integer_unary(
      narrowing_cast,
      draft::IntegerExpressionOperation::Cast,
      wide_parameter,
      unsigned_type(8));
  EXPECT(
      state,
      !draft::solve_unique_integer_expression(
           narrowing_cast, draft::BigInteger::from_u64(0)).has_value());

  draft::IntegerExpression xor_mask;
  const std::uint32_t masked_parameter = draft::append_integer_parameter(
      xor_mask, 30, unsigned_type(8));
  const std::uint32_t mask = draft::append_integer_constant(
      xor_mask, draft::BigInteger::from_u64(0xaa), unsigned_type(8));
  xor_mask.root = draft::append_integer_binary(
      xor_mask,
      draft::IntegerExpressionOperation::BitwiseXor,
      masked_parameter,
      mask,
      unsigned_type(8));
  const std::optional<draft::IntegerExpressionSolution> unmasked =
      draft::solve_unique_integer_expression(
          xor_mask, draft::BigInteger::from_u64(0));
  EXPECT(state, unmasked.has_value());
  if (unmasked.has_value()) {
    EXPECT(state, unmasked->value == draft::BigInteger::from_u64(0xaa));
  }

  draft::IntegerExpression non_unique;
  const std::uint32_t repeated = draft::append_integer_parameter(
      non_unique, 31, unsigned_type(8));
  const std::uint32_t repeated_again = draft::append_integer_parameter(
      non_unique, 31, unsigned_type(8));
  non_unique.root = draft::append_integer_binary(
      non_unique,
      draft::IntegerExpressionOperation::Add,
      repeated,
      repeated_again,
      unsigned_type(8));
  EXPECT(
      state,
      !draft::solve_unique_integer_expression(
           non_unique, draft::BigInteger::from_u64(8)).has_value());

  draft::IntegerExpression multiplied;
  const std::uint32_t multiplied_parameter =
      draft::append_integer_parameter(
          multiplied, 41, unsigned_type(8));
  const std::uint32_t two = draft::append_integer_constant(
      multiplied, draft::BigInteger::from_u64(2), unsigned_type(8));
  multiplied.root = draft::append_integer_binary(
      multiplied,
      draft::IntegerExpressionOperation::Multiply,
      multiplied_parameter,
      two,
      unsigned_type(8));
  EXPECT(
      state,
      !draft::solve_unique_integer_expression(
           multiplied, draft::BigInteger::from_u64(8)).has_value());
}

} // namespace

int main() {
  TestState state;
  test_substitution_and_wrapping(state);
  test_composed_substitution_and_remapping(state);
  test_integer_traps(state);
  test_malformed_and_resource_limited_trees(state);
  test_explicit_integer_cast(state);
  test_unique_inverse_solver(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " integer-expression expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all integer-expression tests passed\n";
  return EXIT_SUCCESS;
}
