// Canonical symbolic integer expressions used by dependent type/value syntax.
//
// A Draft array length or parametric value argument is normally an ordinary
// compile-time expression. While a template is being checked, however, one or
// more value parameters are not concrete yet. This small post-order tree keeps
// that expression intact until specialization without retaining parser nodes,
// source-manager pointers, or package-local names.
//
// Every node records its integer representation. This is essential: concrete
// integer arithmetic wraps after each operation, whereas an untyped constant
// remains arbitrary precision until context converts it. Parameter numbers are
// deliberately plain uint32 values. In a live semantic graph they are SymbolId
// values; in a package interface they are declaration-local ordinals. Explicit
// remapping is therefore required at each interface boundary.

#pragma once

#include "sema/big_integer.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace draft {

enum class IntegerExpressionOperation {
  Constant,
  Parameter,
  Positive,
  Negate,
  BitwiseNot,
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  ShiftLeft,
  ShiftRight,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  Cast,
};

enum class IntegerExpressionRepresentation {
  Untyped,
  Signed,
  Unsigned,
};

struct IntegerExpressionType {
  IntegerExpressionRepresentation representation =
      IntegerExpressionRepresentation::Untyped;
  std::uint32_t bit_width = 0;
  // Concrete Draft integers with the same representation are still distinct:
  // on AArch64, u64, uint, usize, and uintptr all have 64 unsigned bits. Keep
  // the canonical semantic spelling so value-parameter checking never turns
  // representation equality into an implicit conversion. Untyped nodes leave
  // this empty.
  std::string identity;

  bool operator==(const IntegerExpressionType &) const = default;
};

struct IntegerExpressionNode {
  IntegerExpressionOperation operation = IntegerExpressionOperation::Constant;
  IntegerExpressionType type;
  BigInteger constant;
  std::uint32_t parameter = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t left = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t right = std::numeric_limits<std::uint32_t>::max();

  bool operator==(const IntegerExpressionNode &) const = default;
};

struct IntegerExpression {
  std::vector<IntegerExpressionNode> nodes;
  std::uint32_t root = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const IntegerExpression &) const = default;
};

struct IntegerExpressionReplacement {
  std::uint32_t parameter = std::numeric_limits<std::uint32_t>::max();
  std::optional<BigInteger> value;
  IntegerExpression expression;
};

struct IntegerExpressionResult {
  bool ok = false;
  BigInteger value;
  std::string error;
};

[[nodiscard]] std::uint32_t append_integer_constant(
    IntegerExpression &expression,
    BigInteger value,
    IntegerExpressionType type = {});

[[nodiscard]] std::uint32_t append_integer_parameter(
    IntegerExpression &expression,
    std::uint32_t parameter,
    IntegerExpressionType type);

[[nodiscard]] std::uint32_t append_integer_unary(
    IntegerExpression &expression,
    IntegerExpressionOperation operation,
    std::uint32_t operand,
    IntegerExpressionType type);

[[nodiscard]] std::uint32_t append_integer_binary(
    IntegerExpression &expression,
    IntegerExpressionOperation operation,
    std::uint32_t left,
    std::uint32_t right,
    IntegerExpressionType type);

[[nodiscard]] bool integer_expression_has_parameters(
    const IntegerExpression &expression);

// Returns a parameter only when the whole expression is exactly that leaf.
// This narrow query preserves the existing unambiguous inference rule while
// richer dependent-expression solving is handled separately.
[[nodiscard]] std::optional<std::uint32_t> single_integer_parameter(
    const IntegerExpression &expression);

[[nodiscard]] IntegerExpressionResult evaluate_integer_expression(
    const IntegerExpression &expression);

// Replaces parameter leaves with concrete values or other symbolic trees.
// Unmentioned parameters remain symbolic. The caller owns type-compatibility
// checks; this operation preserves every node's already-validated semantics.
[[nodiscard]] std::optional<IntegerExpression> substitute_integer_expression(
    const IntegerExpression &expression,
    const std::vector<IntegerExpressionReplacement> &replacements,
    std::string &error);

// Renumbers parameter leaves without changing expression structure. Every
// parameter must have a mapping; failure prevents process-local SymbolIds from
// leaking into a canonical package interface.
[[nodiscard]] std::optional<IntegerExpression> remap_integer_expression(
    const IntegerExpression &expression,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &mapping);

// Stable structural text used only for generated instance names, diagnostics,
// and hashing. It is not source syntax and is never parsed back into a tree.
[[nodiscard]] std::string integer_expression_identity(
    const IntegerExpression &expression);

} // namespace draft
