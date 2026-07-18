#include "sema/integer_expression.h"

#include <cstddef>
#include <utility>

namespace draft {
namespace {

constexpr std::uint32_t kInvalidIndex =
    std::numeric_limits<std::uint32_t>::max();

// Symbolic expressions can contain untyped constant subexpressions. Match the
// general constant evaluator's generous ceiling so a source-authored shift or
// malformed interface cannot request an effectively unbounded allocation.
constexpr std::uint64_t kMaximumIntegerExpressionBits = 1'000'000;

[[nodiscard]] bool valid_type(IntegerExpressionType type) {
  if (type.representation == IntegerExpressionRepresentation::Untyped) {
    return type.bit_width == 0;
  }
  return type.bit_width != 0 &&
      type.bit_width <= kMaximumIntegerExpressionBits;
}

[[nodiscard]] bool typed(IntegerExpressionType type) {
  return type.representation != IntegerExpressionRepresentation::Untyped;
}

[[nodiscard]] BigInteger modulus(std::uint32_t bits) {
  return BigInteger::from_u64(1).shifted_left(bits);
}

// Reduces a mathematical integer to the node's concrete two's-complement
// representation. BigInteger division truncates toward zero, so a negative
// remainder is moved into the canonical unsigned residue before signed
// interpretation.
[[nodiscard]] BigInteger wrap_integer(
    const BigInteger &value, IntegerExpressionType type) {
  if (!typed(type) || type.bit_width == 0) return value;
  const BigInteger width_modulus = modulus(type.bit_width);
  BigInteger quotient;
  BigInteger remainder;
  if (!value.divide(width_modulus, quotient, remainder)) return value;
  if (remainder.is_negative()) {
    remainder = remainder.added(width_modulus);
  }
  if (type.representation == IntegerExpressionRepresentation::Signed) {
    const BigInteger sign_bit = modulus(type.bit_width - 1);
    if (remainder.compare(sign_bit) >= 0) {
      remainder = remainder.subtracted(width_modulus);
    }
  }
  return remainder;
}

[[nodiscard]] bool valid_node_reference(
    const IntegerExpression &expression,
    std::uint32_t index,
    std::size_t before) {
  return index != kInvalidIndex && index < before &&
      index < expression.nodes.size();
}

[[nodiscard]] bool unary(IntegerExpressionOperation operation) {
  return operation == IntegerExpressionOperation::Positive ||
      operation == IntegerExpressionOperation::Negate ||
      operation == IntegerExpressionOperation::BitwiseNot ||
      operation == IntegerExpressionOperation::Cast;
}

[[nodiscard]] bool binary(IntegerExpressionOperation operation) {
  return operation == IntegerExpressionOperation::Add ||
      operation == IntegerExpressionOperation::Subtract ||
      operation == IntegerExpressionOperation::Multiply ||
      operation == IntegerExpressionOperation::Divide ||
      operation == IntegerExpressionOperation::Remainder ||
      operation == IntegerExpressionOperation::ShiftLeft ||
      operation == IntegerExpressionOperation::ShiftRight ||
      operation == IntegerExpressionOperation::BitwiseAnd ||
      operation == IntegerExpressionOperation::BitwiseOr ||
      operation == IntegerExpressionOperation::BitwiseXor;
}

[[nodiscard]] std::string operation_name(IntegerExpressionOperation operation) {
  switch (operation) {
  case IntegerExpressionOperation::Constant: return "c";
  case IntegerExpressionOperation::Parameter: return "p";
  case IntegerExpressionOperation::Positive: return "pos";
  case IntegerExpressionOperation::Negate: return "neg";
  case IntegerExpressionOperation::BitwiseNot: return "not";
  case IntegerExpressionOperation::Add: return "add";
  case IntegerExpressionOperation::Subtract: return "sub";
  case IntegerExpressionOperation::Multiply: return "mul";
  case IntegerExpressionOperation::Divide: return "div";
  case IntegerExpressionOperation::Remainder: return "rem";
  case IntegerExpressionOperation::ShiftLeft: return "shl";
  case IntegerExpressionOperation::ShiftRight: return "shr";
  case IntegerExpressionOperation::BitwiseAnd: return "and";
  case IntegerExpressionOperation::BitwiseOr: return "or";
  case IntegerExpressionOperation::BitwiseXor: return "xor";
  case IntegerExpressionOperation::Cast: return "cast";
  }
  return "invalid";
}

[[nodiscard]] std::string type_identity(IntegerExpressionType type) {
  if (type.representation == IntegerExpressionRepresentation::Untyped) {
    return "u";
  }
  return std::string(
             type.representation == IntegerExpressionRepresentation::Signed
                 ? "i"
                 : "u") +
      std::to_string(type.bit_width);
}

[[nodiscard]] std::uint32_t import_expression(
    IntegerExpression &destination,
    const IntegerExpression &source) {
  if (!source.is_valid()) return kInvalidIndex;
  const std::uint32_t base =
      static_cast<std::uint32_t>(destination.nodes.size());
  for (const IntegerExpressionNode &source_node : source.nodes) {
    IntegerExpressionNode node = source_node;
    if (node.left != kInvalidIndex) node.left += base;
    if (node.right != kInvalidIndex) node.right += base;
    destination.nodes.push_back(std::move(node));
  }
  return base + source.root;
}

} // namespace

bool IntegerExpression::is_valid() const {
  // Builders append operands before their user, so the root is always the
  // final row. Requiring that form (and reachability below) gives interfaces
  // one canonical tree instead of permitting irrelevant detached rows to
  // affect hashes or evaluation.
  if (nodes.empty() || root != nodes.size() - 1) return false;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const IntegerExpressionNode &node = nodes[index];
    if (!valid_type(node.type)) return false;
    if (node.operation == IntegerExpressionOperation::Constant) {
      if (node.parameter != kInvalidIndex || node.left != kInvalidIndex ||
          node.right != kInvalidIndex) {
        return false;
      }
      continue;
    }
    if (node.operation == IntegerExpressionOperation::Parameter) {
      if (node.parameter == kInvalidIndex || node.left != kInvalidIndex ||
          node.right != kInvalidIndex) {
        return false;
      }
      continue;
    }
    if (unary(node.operation)) {
      if (!valid_node_reference(*this, node.left, index) ||
          node.parameter != kInvalidIndex || node.right != kInvalidIndex) {
        return false;
      }
      continue;
    }
    if (binary(node.operation)) {
      if (!valid_node_reference(*this, node.left, index) ||
          !valid_node_reference(*this, node.right, index) ||
          node.parameter != kInvalidIndex) {
        return false;
      }
      continue;
    }
    return false;
  }

  std::vector<bool> reachable(nodes.size(), false);
  reachable[root] = true;
  for (std::size_t offset = nodes.size(); offset != 0; --offset) {
    const std::size_t index = offset - 1;
    if (!reachable[index]) continue;
    const IntegerExpressionNode &node = nodes[index];
    if (unary(node.operation)) {
      reachable[node.left] = true;
    } else if (binary(node.operation)) {
      reachable[node.left] = true;
      reachable[node.right] = true;
    }
  }
  for (bool used : reachable) {
    if (!used) return false;
  }
  return true;
}

std::uint32_t append_integer_constant(
    IntegerExpression &expression,
    BigInteger value,
    IntegerExpressionType type) {
  IntegerExpressionNode node;
  node.operation = IntegerExpressionOperation::Constant;
  node.type = type;
  node.constant = std::move(value);
  expression.nodes.push_back(std::move(node));
  return static_cast<std::uint32_t>(expression.nodes.size() - 1);
}

std::uint32_t append_integer_parameter(
    IntegerExpression &expression,
    std::uint32_t parameter,
    IntegerExpressionType type) {
  IntegerExpressionNode node;
  node.operation = IntegerExpressionOperation::Parameter;
  node.type = type;
  node.parameter = parameter;
  expression.nodes.push_back(std::move(node));
  return static_cast<std::uint32_t>(expression.nodes.size() - 1);
}

std::uint32_t append_integer_unary(
    IntegerExpression &expression,
    IntegerExpressionOperation operation,
    std::uint32_t operand,
    IntegerExpressionType type) {
  IntegerExpressionNode node;
  node.operation = operation;
  node.type = type;
  node.left = operand;
  expression.nodes.push_back(std::move(node));
  return static_cast<std::uint32_t>(expression.nodes.size() - 1);
}

std::uint32_t append_integer_binary(
    IntegerExpression &expression,
    IntegerExpressionOperation operation,
    std::uint32_t left,
    std::uint32_t right,
    IntegerExpressionType type) {
  IntegerExpressionNode node;
  node.operation = operation;
  node.type = type;
  node.left = left;
  node.right = right;
  expression.nodes.push_back(std::move(node));
  return static_cast<std::uint32_t>(expression.nodes.size() - 1);
}

bool integer_expression_has_parameters(const IntegerExpression &expression) {
  for (const IntegerExpressionNode &node : expression.nodes) {
    if (node.operation == IntegerExpressionOperation::Parameter) return true;
  }
  return false;
}

std::optional<std::uint32_t> single_integer_parameter(
    const IntegerExpression &expression) {
  if (!expression.is_valid() || expression.nodes.size() != 1) {
    return std::nullopt;
  }
  const IntegerExpressionNode &root = expression.nodes[expression.root];
  return root.operation == IntegerExpressionOperation::Parameter
      ? std::optional<std::uint32_t>(root.parameter)
      : std::nullopt;
}

IntegerExpressionResult evaluate_integer_expression(
    const IntegerExpression &expression) {
  if (!expression.is_valid()) {
    return {false, {}, "dependent integer expression is malformed"};
  }
  std::vector<BigInteger> values;
  values.reserve(expression.nodes.size());
  for (const IntegerExpressionNode &node : expression.nodes) {
    if (node.operation == IntegerExpressionOperation::Parameter) {
      return {false, {}, "dependent integer expression is still symbolic"};
    }
    if (node.operation == IntegerExpressionOperation::Constant) {
      values.push_back(wrap_integer(node.constant, node.type));
      continue;
    }
    if (unary(node.operation)) {
      const BigInteger &operand = values[node.left];
      BigInteger result = operand;
      if (node.operation == IntegerExpressionOperation::Negate) {
        result = operand.negated();
      } else if (node.operation == IntegerExpressionOperation::BitwiseNot) {
        result = operand.bitwise_not();
      }
      values.push_back(wrap_integer(result, node.type));
      continue;
    }

    const BigInteger &left = values[node.left];
    const BigInteger &right = values[node.right];
    BigInteger result;
    switch (node.operation) {
    case IntegerExpressionOperation::Add:
      result = left.added(right);
      break;
    case IntegerExpressionOperation::Subtract:
      result = left.subtracted(right);
      break;
    case IntegerExpressionOperation::Multiply:
      result = left.multiplied(right);
      break;
    case IntegerExpressionOperation::Divide:
    case IntegerExpressionOperation::Remainder: {
      BigInteger quotient;
      BigInteger remainder;
      if (!left.divide(right, quotient, remainder)) {
        return {false, {}, "dependent integer expression divides by zero"};
      }
      if (node.operation == IntegerExpressionOperation::Divide &&
          node.type.representation == IntegerExpressionRepresentation::Signed &&
          typed(node.type)) {
        const BigInteger minimum = modulus(node.type.bit_width - 1).negated();
        if (left == minimum && right == BigInteger::from_i64(-1)) {
          return {
              false,
              {},
              "dependent integer expression divides signed minimum by -1",
          };
        }
      }
      result = node.operation == IntegerExpressionOperation::Divide
          ? std::move(quotient)
          : std::move(remainder);
      break;
    }
    case IntegerExpressionOperation::ShiftLeft:
    case IntegerExpressionOperation::ShiftRight: {
      if (right.is_negative()) {
        return {false, {}, "dependent integer expression has a negative shift"};
      }
      const std::optional<std::uint64_t> count = right.to_u64();
      if (!count.has_value() ||
          *count > kMaximumIntegerExpressionBits ||
          *count > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max()) ||
          (typed(node.type) && *count >= node.type.bit_width)) {
        return {
            false,
            {},
            "dependent integer expression has an out-of-range shift",
        };
      }
      result = node.operation == IntegerExpressionOperation::ShiftLeft
          ? left.shifted_left(static_cast<std::size_t>(*count))
          : left.shifted_right(static_cast<std::size_t>(*count));
      break;
    }
    case IntegerExpressionOperation::BitwiseAnd:
      result = left.bitwise_and(right);
      break;
    case IntegerExpressionOperation::BitwiseOr:
      result = left.bitwise_or(right);
      break;
    case IntegerExpressionOperation::BitwiseXor:
      result = left.bitwise_xor(right);
      break;
    default:
      return {false, {}, "dependent integer expression has an invalid operation"};
    }
    values.push_back(wrap_integer(result, node.type));
  }
  return {true, values[expression.root], {}};
}

std::optional<IntegerExpression> substitute_integer_expression(
    const IntegerExpression &expression,
    const std::vector<IntegerExpressionReplacement> &replacements,
    std::string &error) {
  if (!expression.is_valid()) {
    error = "dependent integer expression is malformed";
    return std::nullopt;
  }
  IntegerExpression result;
  std::vector<std::uint32_t> translated;
  translated.reserve(expression.nodes.size());
  for (const IntegerExpressionNode &source : expression.nodes) {
    if (source.operation == IntegerExpressionOperation::Parameter) {
      const IntegerExpressionReplacement *replacement = nullptr;
      for (const IntegerExpressionReplacement &candidate : replacements) {
        if (candidate.parameter == source.parameter) {
          replacement = &candidate;
          break;
        }
      }
      if (replacement != nullptr && replacement->value.has_value()) {
        translated.push_back(append_integer_constant(
            result, *replacement->value, source.type));
      } else if (replacement != nullptr && replacement->expression.is_valid()) {
        translated.push_back(import_expression(result, replacement->expression));
      } else {
        translated.push_back(append_integer_parameter(
            result, source.parameter, source.type));
      }
      continue;
    }
    if (source.operation == IntegerExpressionOperation::Constant) {
      translated.push_back(append_integer_constant(
          result, source.constant, source.type));
      continue;
    }
    if (unary(source.operation)) {
      translated.push_back(append_integer_unary(
          result,
          source.operation,
          translated[source.left],
          source.type));
      continue;
    }
    if (!binary(source.operation)) {
      error = "dependent integer expression has an invalid operation";
      return std::nullopt;
    }
    translated.push_back(append_integer_binary(
        result,
        source.operation,
        translated[source.left],
        translated[source.right],
        source.type));
  }
  result.root = translated[expression.root];
  return result;
}

std::optional<IntegerExpression> remap_integer_expression(
    const IntegerExpression &expression,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &mapping) {
  if (!expression.is_valid()) return std::nullopt;
  IntegerExpression result = expression;
  for (IntegerExpressionNode &node : result.nodes) {
    if (node.operation != IntegerExpressionOperation::Parameter) continue;
    bool found = false;
    for (const auto &[source, destination] : mapping) {
      if (node.parameter != source) continue;
      node.parameter = destination;
      found = true;
      break;
    }
    if (!found) return std::nullopt;
  }
  return result;
}

std::string integer_expression_identity(const IntegerExpression &expression) {
  if (!expression.is_valid()) return "invalid";
  std::vector<std::string> values;
  values.reserve(expression.nodes.size());
  for (const IntegerExpressionNode &node : expression.nodes) {
    const std::string type = type_identity(node.type);
    if (node.operation == IntegerExpressionOperation::Constant) {
      values.push_back("c" + type + "_" + node.constant.to_decimal());
    } else if (node.operation == IntegerExpressionOperation::Parameter) {
      values.push_back("p" + type + "_" + std::to_string(node.parameter));
    } else if (unary(node.operation)) {
      values.push_back(
          operation_name(node.operation) + type + "(" + values[node.left] + ")");
    } else {
      values.push_back(
          operation_name(node.operation) + type + "(" + values[node.left] +
          "," + values[node.right] + ")");
    }
  }
  return values[expression.root];
}

} // namespace draft
