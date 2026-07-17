// Runtime body checking and structured typed-HIR construction.

#include "sema/body_checker.h"

#include "sema/type_resolver.h"
#include "syntax/token.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool token_is_contextual_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
         kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
         kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber;
}

[[nodiscard]] bool node_is_type_syntax(NodeKind kind) {
  switch (kind) {
  case NodeKind::NamedType:
  case NodeKind::PointerType:
  case NodeKind::MultiPointerType:
  case NodeKind::SliceType:
  case NodeKind::ArrayType:
  case NodeKind::SimdType:
  case NodeKind::TupleType:
  case NodeKind::ProcedureType:
  case NodeKind::DistinctType:
  case NodeKind::StructType:
  case NodeKind::EnumType:
  case NodeKind::TaggedUnionType:
  case NodeKind::RawUnionType:
    return true;
  default:
    return false;
  }
}

struct SourceName {
  std::string text;
  SourceRange range;
};

// Runtime control depth distinguishes switch/loop break targets from loop-only
// continue targets.
struct ControlDepth {
  std::uint32_t breakable = 0;
  std::uint32_t loops = 0;
};

// BodyChecker is one sequential phase context. It owns only the HIR under
// construction; source, syntax, semantic tables, constants, and selections are
// caller-owned. SymbolTable may grow, so operations retain SymbolId values and
// copy source records instead of holding element references across declarations.
class BodyChecker {
public:
  BodyChecker(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      const ConditionalSelections &selections,
      SemanticPackage &semantic,
      const ConstantTable &constants,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), selections_(selections),
        semantic_(semantic), constants_(constants), diagnostics_(diagnostics) {}

  [[nodiscard]] BodyCheckResult run() {
    BodyCheckResult result;
    const std::size_t initial_errors = diagnostics_.error_count();
    const std::vector<SymbolId> package_symbols =
        semantic_.symbols.scope(semantic_.package_scope).symbols;
    for (SymbolId id : package_symbols) {
      const Symbol symbol = semantic_.symbols.symbol(id);
      if (symbol.kind != SymbolKind::Procedure || !symbol.type.is_valid()) {
        continue;
      }
      if (check_procedure(id)) {
        ++result.checked_procedures;
      }
    }
    result.ok = diagnostics_.error_count() == initial_errors;
    result.program = std::move(hir_);
    return result;
  }

private:
  // Resolves a FileId to the immutable syntax tree used by all body references.
  [[nodiscard]] const SyntaxTree *find_tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) {
        return &*entry.syntax;
      }
    }
    return nullptr;
  }

  // Locates a scope already owned by a declaration during signature resolution.
  [[nodiscard]] std::optional<ScopeId> owned_scope(
      SymbolId owner, ScopeKind kind) const {
    for (const OwnedSemanticScope &entry : semantic_.owned_scopes) {
      if (entry.owner == owner && semantic_.symbols.scope(entry.scope).kind == kind) {
        return entry.scope;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] SourceName token_name(
      const SyntaxTree &tree, std::uint32_t index) const {
    const Token &token = tree.token(index);
    return {std::string(sources_.text(token.range)), token.range};
  }

  // Extracts names from a grammar-owned flat token span in source order.
  [[nodiscard]] std::vector<SourceName> names_in_span(
      const SyntaxTree &tree, std::uint32_t begin, std::uint32_t end) const {
    std::vector<SourceName> names;
    for (std::uint32_t index = begin; index < end; ++index) {
      if (token_is_contextual_name(tree.token(index).kind)) {
        names.push_back(token_name(tree, index));
      }
    }
    return names;
  }

  // Adds an invalid expression after a diagnostic so parent nodes retain exact
  // operand ordering without inventing a usable type.
  [[nodiscard]] HirExpressionId invalid_expression(SourceRange range) {
    HirExpression expression;
    expression.kind = HirExpressionKind::Invalid;
    expression.type = semantic_.types.builtins().invalid;
    expression.range = range;
    return hir_.add_expression(std::move(expression));
  }

  [[nodiscard]] bool is_invalid_type(TypeId type) const {
    return !type.is_valid() || semantic_.types.type(type).kind == TypeKind::Invalid;
  }

  [[nodiscard]] bool is_bool(TypeId type) const {
    return type == semantic_.types.builtins().bool_type;
  }

  [[nodiscard]] bool is_untyped_integer(TypeId type) const {
    return type == semantic_.types.builtins().untyped_integer;
  }

  [[nodiscard]] bool is_untyped_float(TypeId type) const {
    return type == semantic_.types.builtins().untyped_float;
  }

  [[nodiscard]] bool is_integer(TypeId type) const {
    if (is_invalid_type(type)) return false;
    return is_untyped_integer(type) || semantic_.types.is_integer(type);
  }

  [[nodiscard]] bool is_numeric(TypeId type) const {
    if (is_invalid_type(type)) return false;
    return is_untyped_integer(type) || is_untyped_float(type) ||
           semantic_.types.is_number(type);
  }

  // Applies Draft's initial expected-type rule: exact types match, and untyped
  // numeric constants may take a compatible concrete numeric type. Range checks
  // use the constant table/literal value in the completed numeric checker.
  [[nodiscard]] TypeId apply_expected_type(
      TypeId actual, TypeId expected, SourceRange range) {
    if (is_invalid_type(actual)) return semantic_.types.builtins().invalid;
    if (!expected.is_valid() || is_invalid_type(expected) || actual == expected) {
      return actual;
    }
    if ((is_untyped_integer(actual) && semantic_.types.is_integer(expected)) ||
        ((is_untyped_integer(actual) || is_untyped_float(actual)) &&
         semantic_.types.is_float(expected))) {
      return expected;
    }
    diagnostics_.error(
        range,
        "expression of type '" + std::string(type_kind_name(semantic_.types.type(actual).kind)) +
            "' does not match expected type '" +
            std::string(type_kind_name(semantic_.types.type(expected).kind)) + "'");
    return semantic_.types.builtins().invalid;
  }

  // Selects one common numeric operand type without implicit conversion between
  // concrete numeric types.
  [[nodiscard]] TypeId common_numeric_type(
      TypeId left, TypeId right, SourceRange range) {
    if (left == right && is_numeric(left)) return left;
    if ((is_untyped_integer(left) || is_untyped_float(left)) &&
        semantic_.types.is_number(right)) {
      return right;
    }
    if ((is_untyped_integer(right) || is_untyped_float(right)) &&
        semantic_.types.is_number(left)) {
      return left;
    }
    diagnostics_.error(range, "numeric operands require one common type");
    return semantic_.types.builtins().invalid;
  }

  // Finds the source operator between the immediate child spans.
  [[nodiscard]] TokenKind binary_operator(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    if (node.children.size() != 2) return TokenKind::Invalid;
    const SyntaxNode &left = tree.node(node.children[0]);
    const SyntaxNode &right = tree.node(node.children[1]);
    for (std::uint32_t index = left.token_end; index < right.token_begin; ++index) {
      const TokenKind kind = tree.token(index).kind;
      if (kind != TokenKind::Semicolon && kind != TokenKind::Comma) return kind;
    }
    return TokenKind::Invalid;
  }

  // Decodes integer literals in the same currently representable subset as the
  // constant evaluator. HIR stores the mathematical value, not source spelling.
  [[nodiscard]] std::optional<std::int64_t> integer_literal(
      std::string_view spelling) const {
    std::uint32_t base = 10;
    std::size_t index = 0;
    if (spelling.size() >= 2 && spelling[0] == '0') {
      if (spelling[1] == 'x' || spelling[1] == 'X') base = 16;
      if (spelling[1] == 'o' || spelling[1] == 'O') base = 8;
      if (spelling[1] == 'b' || spelling[1] == 'B') base = 2;
      if (base != 10) index = 2;
    }
    std::uint64_t value = 0;
    bool saw_digit = false;
    for (; index < spelling.size(); ++index) {
      const char character = spelling[index];
      if (character == '_') continue;
      std::uint32_t digit = 0;
      if (character >= '0' && character <= '9') {
        digit = static_cast<std::uint32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        digit = static_cast<std::uint32_t>(character - 'a') + 10U;
      } else if (character >= 'A' && character <= 'F') {
        digit = static_cast<std::uint32_t>(character - 'A') + 10U;
      } else {
        return std::nullopt;
      }
      if (digit >= base ||
          value > (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                   digit) /
              base) {
        return std::nullopt;
      }
      value = value * base + digit;
      saw_digit = true;
    }
    if (!saw_digit) return std::nullopt;
    return static_cast<std::int64_t>(value);
  }

  // Resolves a member symbol by the nominal base type's owned Type scope.
  [[nodiscard]] std::optional<SymbolId> find_member(
      TypeId base, std::string_view name) const {
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (semantic_.symbols.scope(owned.scope).kind != ScopeKind::Type) continue;
      const Symbol &owner = semantic_.symbols.symbol(owned.owner);
      if (owner.type == base) {
        return semantic_.symbols.lookup_direct(owned.scope, name);
      }
    }
    return std::nullopt;
  }

  // Returns the declaration symbol owning a nominal type's Type scope.
  [[nodiscard]] std::optional<SymbolId> type_owner(TypeId type) const {
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (semantic_.symbols.scope(owned.scope).kind == ScopeKind::Type &&
          semantic_.symbols.symbol(owned.owner).type == type) {
        return owned.owner;
      }
    }
    return std::nullopt;
  }

  // Type-checks one expression recursively and returns a HIR node even after an
  // error. expected is an invalid TypeId when the surrounding syntax supplies no
  // context.
  [[nodiscard]] HirExpressionId check_expression(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      TypeId expected = {}) {
    const SyntaxNode &node = tree.node(expression_id);
    switch (node.kind) {
    case NodeKind::LiteralExpression: {
      if (node.token_begin >= node.token_end) return invalid_expression(node.range);
      const Token &token = tree.token(node.token_begin);
      HirExpression expression;
      expression.kind = HirExpressionKind::Constant;
      expression.range = node.range;
      if (token.kind == TokenKind::KeywordTrue || token.kind == TokenKind::KeywordFalse) {
        expression.type = semantic_.types.builtins().bool_type;
        expression.constant = ConstantValue::make_bool(token.kind == TokenKind::KeywordTrue);
      } else if (token.kind == TokenKind::IntegerLiteral) {
        const std::optional<std::int64_t> value = integer_literal(sources_.text(token.range));
        if (!value.has_value()) {
          diagnostics_.error(token.range, "integer literal exceeds bootstrap HIR range");
          return invalid_expression(node.range);
        }
        expression.type = semantic_.types.builtins().untyped_integer;
        expression.constant = ConstantValue::make_integer(*value);
      } else if (token.kind == TokenKind::FloatLiteral) {
        expression.type = semantic_.types.builtins().untyped_float;
      } else if (token.kind == TokenKind::StringLiteral ||
                 token.kind == TokenKind::RawStringLiteral) {
        expression.type = semantic_.types.builtins().string_type;
        expression.constant.kind = ConstantKind::String;
        expression.constant.text = std::string(sources_.text(token.range));
      } else {
        diagnostics_.error(node.range, "literal is not yet valid in a runtime expression");
        return invalid_expression(node.range);
      }
      expression.type = apply_expected_type(expression.type, expected, node.range);
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::NameExpression: {
      const std::vector<SourceName> names =
          names_in_span(tree, node.token_begin, node.token_end);
      if (names.empty()) return invalid_expression(node.range);
      const std::optional<SymbolId> found =
          semantic_.symbols.lookup(scope, names.front().text);
      if (!found.has_value()) {
        diagnostics_.error(names.front().range, "unknown name '" + names.front().text + "'");
        return invalid_expression(node.range);
      }
      const Symbol symbol = semantic_.symbols.symbol(*found);
      if (!symbol.type.is_valid() || symbol.kind == SymbolKind::Type ||
          symbol.kind == SymbolKind::Import) {
        diagnostics_.error(names.front().range, "name does not denote a runtime value");
        return invalid_expression(node.range);
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Symbol;
      expression.range = node.range;
      expression.symbol = *found;
      expression.type = apply_expected_type(symbol.type, expected, node.range);
      expression.addressable = symbol.kind == SymbolKind::Variable ||
          symbol.kind == SymbolKind::Local || symbol.kind == SymbolKind::Parameter;
      if (const ConstantValue *constant = constants_.find(*found)) {
        expression.kind = HirExpressionKind::Constant;
        expression.constant = *constant;
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::GroupExpression:
      if (!node.children.empty()) {
        return check_expression(tree, node.children.front(), scope, expected);
      }
      return invalid_expression(node.range);

    case NodeKind::UnaryExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      const TokenKind operation = tree.token(node.token_begin).kind;
      const HirExpressionId operand_id =
          check_expression(tree, node.children.front(), scope);
      const HirExpression operand = hir_.expression(operand_id);
      TypeId result = operand.type;
      HirExpressionKind kind = HirExpressionKind::Unary;
      if (operation == TokenKind::Ampersand) {
        if (!operand.addressable) {
          diagnostics_.error(node.range, "address-of requires addressable storage");
          result = semantic_.types.builtins().invalid;
        } else {
          result = semantic_.types.pointer(operand.type);
          kind = HirExpressionKind::Address;
        }
      } else if (operation == TokenKind::Bang) {
        if (!is_bool(operand.type)) {
          diagnostics_.error(node.range, "logical not requires bool");
          result = semantic_.types.builtins().invalid;
        }
      } else if (operation == TokenKind::Tilde) {
        if (!is_integer(operand.type)) {
          diagnostics_.error(node.range, "bitwise not requires an integer");
          result = semantic_.types.builtins().invalid;
        }
      } else if (!is_numeric(operand.type)) {
        diagnostics_.error(node.range, "unary numeric operator requires a number");
        result = semantic_.types.builtins().invalid;
      }
      HirExpression expression;
      expression.kind = kind;
      expression.type = apply_expected_type(result, expected, node.range);
      expression.range = node.range;
      expression.operands.push_back(operand_id);
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::BinaryExpression: {
      if (node.children.size() != 2) return invalid_expression(node.range);
      const HirExpressionId left_id = check_expression(tree, node.children[0], scope);
      const HirExpressionId right_id = check_expression(tree, node.children[1], scope);
      const TypeId left = hir_.expression(left_id).type;
      const TypeId right = hir_.expression(right_id).type;
      const TokenKind operation = binary_operator(tree, node);
      TypeId result = semantic_.types.builtins().invalid;
      if (operation == TokenKind::LogicalAnd || operation == TokenKind::LogicalOr) {
        if (is_bool(left) && is_bool(right)) {
          result = semantic_.types.builtins().bool_type;
        } else {
          diagnostics_.error(node.range, "logical operators require bool operands");
        }
      } else if (operation == TokenKind::EqualEqual || operation == TokenKind::BangEqual ||
                 operation == TokenKind::Less || operation == TokenKind::LessEqual ||
                 operation == TokenKind::Greater || operation == TokenKind::GreaterEqual) {
        if ((is_numeric(left) && is_numeric(right) &&
             !is_invalid_type(common_numeric_type(left, right, node.range))) ||
            (left == right && is_bool(left))) {
          result = semantic_.types.builtins().bool_type;
        } else if (!is_numeric(left) || !is_numeric(right)) {
          diagnostics_.error(node.range, "comparison is not defined for operand types");
        }
      } else if (operation == TokenKind::ShiftLeft || operation == TokenKind::ShiftRight) {
        if (is_integer(left) && is_integer(right)) {
          result = left;
        } else {
          diagnostics_.error(node.range, "shift requires integer operands");
        }
      } else {
        result = common_numeric_type(left, right, node.range);
        if ((operation == TokenKind::Percent || operation == TokenKind::Ampersand ||
             operation == TokenKind::Pipe || operation == TokenKind::Caret) &&
            !is_integer(result)) {
          diagnostics_.error(node.range, "operator requires integer operands");
          result = semantic_.types.builtins().invalid;
        }
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Binary;
      expression.type = apply_expected_type(result, expected, node.range);
      expression.range = node.range;
      expression.operands = {left_id, right_id};
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::CallExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      const HirExpressionId callee = check_expression(tree, node.children.front(), scope);
      const TypeId callee_type = hir_.expression(callee).type;
      if (is_invalid_type(callee_type) ||
          semantic_.types.type(callee_type).kind != TypeKind::Procedure) {
        diagnostics_.error(node.range, "called expression does not have procedure type");
        return invalid_expression(node.range);
      }
      const Type signature = semantic_.types.type(callee_type);
      const std::size_t parameter_count = signature.members.empty()
          ? 0
          : signature.members.size() - 1;
      if (node.children.size() - 1 != parameter_count) {
        diagnostics_.error(node.range, "procedure call has the wrong number of arguments");
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Call;
      expression.range = node.range;
      expression.operands.push_back(callee);
      const std::size_t checked_count = std::min(parameter_count, node.children.size() - 1);
      for (std::size_t index = 0; index < checked_count; ++index) {
        expression.operands.push_back(check_expression(
            tree, node.children[index + 1], scope, signature.members[index]));
      }
      for (std::size_t index = checked_count + 1; index < node.children.size(); ++index) {
        expression.operands.push_back(check_expression(tree, node.children[index], scope));
      }
      TypeId result = signature.members.empty()
          ? semantic_.types.builtins().void_type
          : signature.members.back();
      expression.type = apply_expected_type(result, expected, node.range);
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::MemberExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      const HirExpressionId base_id = check_expression(tree, node.children.front(), scope);
      const HirExpression base = hir_.expression(base_id);
      const std::vector<SourceName> names =
          names_in_span(tree, node.token_begin, node.token_end);
      if (names.empty()) return invalid_expression(node.range);
      const std::optional<SymbolId> member = find_member(base.type, names.back().text);
      if (!member.has_value()) {
        diagnostics_.error(names.back().range, "type has no member named '" + names.back().text + "'");
        return invalid_expression(node.range);
      }
      const Symbol member_symbol = semantic_.symbols.symbol(*member);
      HirExpression expression;
      expression.kind = HirExpressionKind::Member;
      expression.range = node.range;
      expression.symbol = *member;
      expression.type = apply_expected_type(member_symbol.type, expected, node.range);
      expression.operands.push_back(base_id);
      expression.addressable = base.addressable && member_symbol.kind == SymbolKind::Field;
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::DereferenceExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      const HirExpressionId pointer_id = check_expression(tree, node.children.front(), scope);
      const Type pointer = semantic_.types.type(hir_.expression(pointer_id).type);
      if (pointer.kind != TypeKind::Pointer && pointer.kind != TypeKind::MultiPointer) {
        diagnostics_.error(node.range, "dereference requires a typed data pointer");
        return invalid_expression(node.range);
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Dereference;
      expression.range = node.range;
      expression.type = apply_expected_type(pointer.element, expected, node.range);
      expression.operands.push_back(pointer_id);
      expression.addressable = true;
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::BracketExpression: {
      if (node.children.size() != 2) {
        diagnostics_.error(node.range, "multi-index expressions are not yet supported");
        return invalid_expression(node.range);
      }
      const HirExpressionId base_id = check_expression(tree, node.children[0], scope);
      const Type base = semantic_.types.type(hir_.expression(base_id).type);
      if (base.kind != TypeKind::Array && base.kind != TypeKind::Slice &&
          base.kind != TypeKind::MultiPointer) {
        diagnostics_.error(node.range, "indexing requires an array, slice, or multi-pointer");
        return invalid_expression(node.range);
      }
      const HirExpressionId index_id = check_expression(tree, node.children[1], scope);
      if (!is_integer(hir_.expression(index_id).type)) {
        diagnostics_.error(tree.node(node.children[1]).range, "index must be an integer");
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Index;
      expression.range = node.range;
      expression.type = apply_expected_type(base.element, expected, node.range);
      expression.operands = {base_id, index_id};
      expression.addressable = true;
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::ConditionalExpression: {
      if (node.children.size() != 3) return invalid_expression(node.range);
      const HirExpressionId condition = check_expression(
          tree, node.children[1], scope, semantic_.types.builtins().bool_type);
      const HirExpressionId left = check_expression(tree, node.children[0], scope, expected);
      const TypeId left_type = hir_.expression(left).type;
      const HirExpressionId right = check_expression(
          tree,
          node.children[2],
          scope,
          expected.is_valid() ? expected : left_type);
      TypeId result = left_type;
      if (hir_.expression(right).type != left_type) {
        result = common_numeric_type(left_type, hir_.expression(right).type, node.range);
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Conditional;
      expression.range = node.range;
      expression.type = result;
      expression.operands = {condition, left, right};
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::ContextualAlternativeExpression: {
      if (!expected.is_valid() || is_invalid_type(expected)) {
        diagnostics_.error(node.range, "contextual alternative requires an expected enum or union type");
        return invalid_expression(node.range);
      }
      const TypeKind expected_kind = semantic_.types.type(expected).kind;
      if (expected_kind != TypeKind::Enum && expected_kind != TypeKind::TaggedUnion) {
        diagnostics_.error(node.range, "contextual alternative expected type is not an enum or tagged union");
        return invalid_expression(node.range);
      }
      const std::vector<SourceName> names =
          names_in_span(tree, node.token_begin, node.token_end);
      if (names.empty()) return invalid_expression(node.range);
      const std::optional<SymbolId> alternative = find_member(expected, names.back().text);
      if (!alternative.has_value()) {
        diagnostics_.error(names.back().range, "unknown alternative '" + names.back().text + "'");
        return invalid_expression(node.range);
      }
      const Symbol member = semantic_.symbols.symbol(*alternative);
      HirExpression expression;
      expression.kind = HirExpressionKind::Constant;
      expression.range = node.range;
      expression.type = expected;
      expression.symbol = *alternative;
      expression.constant = ConstantValue::make_enum_label(names.back().text);
      if (expected_kind == TypeKind::TaggedUnion) {
        const bool has_payload = member.type != semantic_.types.builtins().void_type;
        if (has_payload != !node.children.empty()) {
          diagnostics_.error(node.range, has_payload
              ? "tagged-union alternative requires a payload"
              : "payload-free alternative cannot carry a value");
        } else if (has_payload) {
          expression.kind = HirExpressionKind::Composite;
          expression.operands.push_back(check_expression(
              tree, node.children.front(), scope, member.type));
        }
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::DenyExpression:
      if (!node.children.empty()) {
        return check_expression(tree, node.children.back(), scope, expected);
      }
      return invalid_expression(node.range);

    case NodeKind::SynthesisExpression: {
      semantic_.sites.push_back(
          {SemanticSiteKind::SynthesisExpression,
           {tree.file(), expression_id},
           scope,
           current_procedure_});
      HirExpression expression;
      expression.kind = HirExpressionKind::Synthesis;
      expression.range = node.range;
      expression.type = expected;
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::AsmExpression: {
      TypeId result = semantic_.types.builtins().invalid;
      for (NodeId child : node.children) {
        if (node_is_type_syntax(tree.node(child).kind)) {
          result = resolve_type_syntax(
              sources_, loaded_, semantic_, selections_, tree, child, scope, diagnostics_);
        } else if (tree.node(child).kind == NodeKind::SynthesisAssembly) {
          semantic_.sites.push_back(
              {SemanticSiteKind::SynthesisAssembly,
               {tree.file(), child},
               scope,
               current_procedure_});
        }
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Assembly;
      expression.range = node.range;
      expression.type = apply_expected_type(result, expected, node.range);
      return hir_.add_expression(std::move(expression));
    }

    default:
      diagnostics_.error(node.range, "expression form is not yet implemented in body checking");
      return invalid_expression(node.range);
    }
  }

  // Finds the assignment token separating lvalue and rvalue syntax children.
  [[nodiscard]] std::optional<std::uint32_t> assignment_operator_index(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      switch (tree.token(index).kind) {
      case TokenKind::Equal:
      case TokenKind::PlusEqual:
      case TokenKind::MinusEqual:
      case TokenKind::StarEqual:
      case TokenKind::SlashEqual:
      case TokenKind::PercentEqual:
      case TokenKind::AmpersandEqual:
      case TokenKind::PipeEqual:
      case TokenKind::CaretEqual:
      case TokenKind::ShiftLeftEqual:
      case TokenKind::ShiftRightEqual:
        return index;
      default:
        break;
      }
    }
    return std::nullopt;
  }

  // Checks one local declaration and appends bindings to the current block scope.
  [[nodiscard]] HirStatementId check_local_declaration(
      const SyntaxTree &tree, NodeId statement_id, ScopeId scope) {
    const SyntaxNode &statement_node = tree.node(statement_id);
    if (statement_node.kind != NodeKind::Declaration &&
        statement_node.children.empty()) {
      return hir_.add_statement(
          {HirStatementKind::Invalid, statement_node.range, {}, {}, {}, {}});
    }
    const NodeId declaration_id = statement_node.kind == NodeKind::Declaration
        ? statement_id
        : statement_node.children.front();
    const SyntaxNode &declaration = tree.node(declaration_id);
    if (declaration.children.empty()) {
      return hir_.add_statement(
          {HirStatementKind::Invalid, declaration.range, {}, {}, {}, {}});
    }
    const SyntaxNode &pattern = tree.node(declaration.children.front());
    if (pattern.kind == NodeKind::TuplePattern) {
      diagnostics_.error(pattern.range, "tuple local destructuring is not yet implemented");
    }
    std::vector<SourceName> names;
    for (NodeId child : pattern.children) {
      const SyntaxNode &name_list = tree.node(child);
      if (name_list.kind == NodeKind::NameList) {
        names = names_in_span(tree, name_list.token_begin, name_list.token_end);
      }
    }

    TypeId declared_type;
    std::optional<NodeId> initializer;
    for (std::size_t index = 1; index < declaration.children.size(); ++index) {
      const NodeId child = declaration.children[index];
      if (node_is_type_syntax(tree.node(child).kind)) {
        declared_type = resolve_type_syntax(
            sources_, loaded_, semantic_, selections_, tree, child, scope, diagnostics_);
      } else if (tree.node(child).kind != NodeKind::ParametricParameterList) {
        initializer = child;
      }
    }

    HirStatement statement;
    statement.kind = HirStatementKind::LocalDeclaration;
    statement.range = declaration.range;
    if (initializer.has_value() &&
        tree.node(*initializer).kind != NodeKind::UninitializedExpression) {
      const HirExpressionId value = check_expression(
          tree, *initializer, scope, declared_type);
      statement.expressions.push_back(value);
      if (!declared_type.is_valid()) {
        declared_type = hir_.expression(value).type;
        if (is_untyped_integer(declared_type)) {
          declared_type = semantic_.types.builtins().int_type;
        } else if (is_untyped_float(declared_type)) {
          const std::optional<TypeId> f64 = semantic_.types.find_builtin("f64");
          declared_type = f64.value_or(semantic_.types.builtins().invalid);
        }
      }
    }
    if (!declared_type.is_valid()) {
      diagnostics_.error(declaration.range, "local declaration requires a type or initializer");
      declared_type = semantic_.types.builtins().invalid;
    }

    for (const SourceName &name : names) {
      if (name.text == "_") continue;
      Symbol symbol;
      symbol.name = name.text;
      symbol.kind = SymbolKind::Local;
      symbol.scope = scope;
      symbol.type = declared_type;
      symbol.syntax = {tree.file(), declaration_id};
      symbol.name_range = name.range;
      const SymbolId id = semantic_.symbols.declare(std::move(symbol), diagnostics_);
      if (id.is_valid()) statement.bindings.push_back(id);
    }
    return hir_.add_statement(std::move(statement));
  }

  // Checks a statement list into an already-created lexical block.
  void check_statement_list(
      const SyntaxTree &tree,
      const SyntaxNode &list,
      ScopeId scope,
      TypeId result_type,
      ControlDepth depth,
      HirBlock &block) {
    for (NodeId statement : list.children) {
      block.statements.push_back(
          check_statement(tree, statement, scope, result_type, depth));
    }
  }

  // Creates a lexical block scope and checks its source-order statements.
  [[nodiscard]] HirBlockId check_block(
      const SyntaxTree &tree,
      NodeId block_id,
      ScopeId parent,
      TypeId result_type,
      ControlDepth depth) {
    const SyntaxNode &source_block = tree.node(block_id);
    const ScopeId scope = semantic_.symbols.add_scope(
        ScopeKind::Block, parent, source_block.range);
    HirBlock block;
    block.scope = scope;
    block.range = source_block.range;
    if (!source_block.children.empty()) {
      const SyntaxNode &list = tree.node(source_block.children.front());
      check_statement_list(tree, list, scope, result_type, depth, block);
    }
    return hir_.add_block(std::move(block));
  }

  // Checks a selected `when` brace region without creating a lexical ScopeId.
  // The HIR block is an ordered container only; its scope intentionally equals
  // the surrounding scope so selected declarations remain visible afterward.
  [[nodiscard]] HirBlockId check_compile_time_block(
      const SyntaxTree &tree,
      NodeId block_id,
      ScopeId scope,
      TypeId result_type,
      ControlDepth depth) {
    const SyntaxNode &source_block = tree.node(block_id);
    HirBlock block;
    block.scope = scope;
    block.range = source_block.range;
    if (!source_block.children.empty()) {
      const SyntaxNode &list = tree.node(source_block.children.front());
      check_statement_list(tree, list, scope, result_type, depth, block);
    }
    return hir_.add_block(std::move(block));
  }

  // Checks the common structured statement forms. Complex iteration bindings,
  // switch exhaustiveness, and body-level compile-time `when` remain explicit
  // diagnostics until their dedicated data-flow passes are connected.
  [[nodiscard]] HirStatementId check_statement(
      const SyntaxTree &tree,
      NodeId statement_id,
      ScopeId scope,
      TypeId result_type,
      ControlDepth depth) {
    const SyntaxNode &node = tree.node(statement_id);
    HirStatement statement;
    statement.range = node.range;
    switch (node.kind) {
    case NodeKind::DeclarationStatement:
      return check_local_declaration(tree, statement_id, scope);

    case NodeKind::ExpressionStatement:
      statement.kind = HirStatementKind::Expression;
      for (NodeId child : node.children) {
        statement.expressions.push_back(check_expression(tree, child, scope));
      }
      break;

    case NodeKind::AssignmentStatement: {
      statement.kind = HirStatementKind::Assignment;
      const std::optional<std::uint32_t> operation =
          assignment_operator_index(tree, node);
      if (!operation.has_value()) {
        diagnostics_.error(node.range, "assignment has no operator");
        break;
      }
      std::size_t left_count = 0;
      for (NodeId child : node.children) {
        if (tree.node(child).token_end <= *operation) ++left_count;
      }
      const std::size_t right_count = node.children.size() - left_count;
      if (left_count != right_count) {
        diagnostics_.error(node.range, "assignment sides must have equal arity");
      }
      for (std::size_t index = 0; index < left_count; ++index) {
        const HirExpressionId left = check_expression(tree, node.children[index], scope);
        statement.expressions.push_back(left);
        if (!hir_.expression(left).addressable) {
          diagnostics_.error(tree.node(node.children[index]).range, "assignment target is not addressable");
        }
      }
      const std::size_t paired = std::min(left_count, right_count);
      for (std::size_t index = 0; index < right_count; ++index) {
        TypeId expected;
        if (index < paired) expected = hir_.expression(statement.expressions[index]).type;
        statement.expressions.push_back(check_expression(
            tree, node.children[left_count + index], scope, expected));
      }
      break;
    }

    case NodeKind::ReturnStatement:
      statement.kind = HirStatementKind::Return;
      if (node.children.empty()) {
        if (result_type != semantic_.types.builtins().void_type) {
          diagnostics_.error(node.range, "non-void procedure return requires a value");
        }
      } else {
        if (result_type == semantic_.types.builtins().void_type) {
          diagnostics_.error(node.range, "void procedure cannot return a value");
        }
        statement.expressions.push_back(
            check_expression(tree, node.children.front(), scope, result_type));
      }
      break;

    case NodeKind::Block:
      statement.kind = HirStatementKind::Block;
      statement.blocks.push_back(
          check_block(tree, statement_id, scope, result_type, depth));
      break;

    case NodeKind::IfStatement:
      statement.kind = HirStatementKind::If;
      if (!node.children.empty()) {
        statement.expressions.push_back(check_expression(
            tree,
            node.children.front(),
            scope,
            semantic_.types.builtins().bool_type));
      }
      for (std::size_t index = 1; index < node.children.size(); ++index) {
        const NodeId child = node.children[index];
        if (tree.node(child).kind == NodeKind::Block) {
          statement.blocks.push_back(
              check_block(tree, child, scope, result_type, depth));
        } else {
          HirBlock synthetic;
          synthetic.scope = semantic_.symbols.add_scope(
              ScopeKind::Block, scope, tree.node(child).range);
          synthetic.range = tree.node(child).range;
          synthetic.statements.push_back(
              check_statement(tree, child, synthetic.scope, result_type, depth));
          statement.blocks.push_back(hir_.add_block(std::move(synthetic)));
        }
      }
      break;

    case NodeKind::BreakStatement:
      statement.kind = HirStatementKind::Break;
      if (depth.breakable == 0) diagnostics_.error(node.range, "break is outside a loop or switch");
      break;

    case NodeKind::ContinueStatement:
      statement.kind = HirStatementKind::Continue;
      if (depth.loops == 0) diagnostics_.error(node.range, "continue is outside a loop");
      break;

    case NodeKind::DeferStatement:
      statement.kind = HirStatementKind::Defer;
      if (!node.children.empty()) {
        const HirExpressionId call = check_expression(tree, node.children.front(), scope);
        statement.expressions.push_back(call);
        if (hir_.expression(call).kind != HirExpressionKind::Call) {
          diagnostics_.error(node.range, "defer requires a procedure call");
        }
      }
      break;

    case NodeKind::ForStatement:
      statement.kind = HirStatementKind::For;
      if (node.children.empty()) break;
      if (tree.node(node.children.front()).kind == NodeKind::IterationHeader) {
        const NodeId header_id = node.children.front();
        const SyntaxNode &header = tree.node(header_id);
        if (header.children.empty()) break;
        const ScopeId loop_scope = semantic_.symbols.add_scope(
            ScopeKind::Block, scope, header.range);
        const HirExpressionId iterable =
            check_expression(tree, header.children.front(), scope);
        statement.expressions.push_back(iterable);
        const Type iterable_type = semantic_.types.type(hir_.expression(iterable).type);
        TypeId element_type = semantic_.types.builtins().invalid;
        if (iterable_type.kind == TypeKind::Array || iterable_type.kind == TypeKind::Slice) {
          element_type = iterable_type.element;
        } else {
          diagnostics_.error(header.range, "iteration requires an array or slice");
        }
        const SyntaxNode &iterable_syntax = tree.node(header.children.front());
        const std::vector<SourceName> names = names_in_span(
            tree, header.token_begin, iterable_syntax.token_begin);
        for (std::size_t index = 0; index < names.size() && index < 2; ++index) {
          if (names[index].text == "_") continue;
          Symbol binding;
          binding.name = names[index].text;
          binding.kind = SymbolKind::Local;
          binding.scope = loop_scope;
          binding.type = index == 0
              ? element_type
              : semantic_.types.builtins().usize_type;
          binding.syntax = {tree.file(), header_id};
          binding.name_range = names[index].range;
          const SymbolId binding_id =
              semantic_.symbols.declare(std::move(binding), diagnostics_);
          if (binding_id.is_valid()) statement.bindings.push_back(binding_id);
        }
        if (node.children.size() >= 2) {
          statement.blocks.push_back(check_block(
              tree,
              node.children.back(),
              loop_scope,
              result_type,
              {depth.breakable + 1, depth.loops + 1}));
        }
      } else if (tree.node(node.children.front()).kind == NodeKind::ForClause) {
        const SyntaxNode &clause = tree.node(node.children.front());
        const ScopeId loop_scope = semantic_.symbols.add_scope(
            ScopeKind::Block, scope, clause.range);
        std::vector<std::uint32_t> separators;
        for (std::uint32_t index = clause.token_begin; index < clause.token_end; ++index) {
          if (tree.token(index).kind == TokenKind::Semicolon) separators.push_back(index);
        }
        for (NodeId header_child : clause.children) {
          const SyntaxNode &child = tree.node(header_child);
          if (!separators.empty() && child.token_end <= separators.front()) {
            if (child.kind == NodeKind::Declaration) {
              statement.header_statements.push_back(
                  check_local_declaration(tree, header_child, loop_scope));
            } else {
              statement.header_statements.push_back(check_statement(
                  tree, header_child, loop_scope, result_type, depth));
            }
          } else if (separators.size() >= 2 && child.token_begin > separators.back()) {
            statement.header_statements.push_back(check_statement(
                tree, header_child, loop_scope, result_type, depth));
          } else {
            statement.expressions.push_back(check_expression(
                tree,
                header_child,
                loop_scope,
                semantic_.types.builtins().bool_type));
          }
        }
        if (node.children.size() >= 2) {
          statement.blocks.push_back(check_block(
              tree,
              node.children.back(),
              loop_scope,
              result_type,
              {depth.breakable + 1, depth.loops + 1}));
        }
      } else {
        // Infinite loops contain only a block; conditional loops contain a bool
        // expression followed by the block.
        for (NodeId child : node.children) {
          if (tree.node(child).kind == NodeKind::Block) {
            statement.blocks.push_back(
                check_block(
                    tree,
                    child,
                    scope,
                    result_type,
                    {depth.breakable + 1, depth.loops + 1}));
          } else if (tree.node(child).kind == NodeKind::ExpressionStatement &&
                     tree.node(child).children.size() == 1) {
            statement.expressions.push_back(check_expression(
                tree,
                tree.node(child).children.front(),
                scope,
                semantic_.types.builtins().bool_type));
          } else {
            statement.expressions.push_back(check_expression(
                tree,
                child,
                scope,
                semantic_.types.builtins().bool_type));
          }
        }
      }
      break;

    case NodeKind::DenyStatement:
      statement.kind = HirStatementKind::Block;
      if (!node.children.empty()) {
        statement.blocks.push_back(
            check_block(tree, node.children.back(), scope, result_type, depth));
      }
      break;

    case NodeKind::UncheckedStatement:
      statement.kind = HirStatementKind::Unchecked;
      if (!node.children.empty()) {
        statement.blocks.push_back(
            check_block(tree, node.children.back(), scope, result_type, depth));
      }
      break;

    case NodeKind::Judgment:
      statement.kind = HirStatementKind::Judgment;
      semantic_.sites.push_back(
          {SemanticSiteKind::Judgment, {tree.file(), statement_id}, scope, current_procedure_});
      break;

    case NodeKind::SynthesisStatement:
      statement.kind = HirStatementKind::Synthesis;
      semantic_.sites.push_back(
          {SemanticSiteKind::SynthesisStatement,
           {tree.file(), statement_id},
           scope,
           current_procedure_});
      break;

    case NodeKind::AsmStatement:
      statement.kind = HirStatementKind::Assembly;
      for (NodeId child : node.children) {
        if (tree.node(child).kind == NodeKind::SynthesisAssembly) {
          semantic_.sites.push_back(
              {SemanticSiteKind::SynthesisAssembly,
               {tree.file(), child},
               scope,
               current_procedure_});
        }
      }
      break;

    case NodeKind::WhenStatement:
      statement.kind = HirStatementKind::CompileTimeSelection;
      if (const ConditionalSelection *selection =
              selections_.find({tree.file(), statement_id})) {
        if (selection->select_true) {
          if (node.children.size() >= 2) {
            statement.blocks.push_back(check_compile_time_block(
                tree, node.children[1], scope, result_type, depth));
          }
        } else if (node.children.size() >= 3) {
          const NodeId alternative = node.children[2];
          if (tree.node(alternative).kind == NodeKind::WhenStatement) {
            HirBlock nested;
            nested.scope = scope;
            nested.range = tree.node(alternative).range;
            nested.statements.push_back(check_statement(
                tree, alternative, scope, result_type, depth));
            statement.blocks.push_back(hir_.add_block(std::move(nested)));
          } else {
            statement.blocks.push_back(check_compile_time_block(
                tree, alternative, scope, result_type, depth));
          }
        }
      } else {
        diagnostics_.error(node.range, "compile-time 'when' statement was not selected");
      }
      break;

    case NodeKind::SwitchStatement:
      statement.kind = HirStatementKind::Switch;
      if (node.children.empty()) break;
      {
        const HirExpressionId subject =
            check_expression(tree, node.children.front(), scope);
        statement.expressions.push_back(subject);
        const TypeId subject_type = hir_.expression(subject).type;
        bool has_default = false;
        std::vector<SymbolId> covered_alternatives;
        for (std::size_t case_index = 1; case_index < node.children.size(); ++case_index) {
          const SyntaxNode &case_node = tree.node(node.children[case_index]);
          if (case_node.kind != NodeKind::SwitchCase || case_node.children.empty()) continue;
          const NodeId list_id = case_node.children.back();
          if (case_node.children.size() == 1) has_default = true;
          for (std::size_t label_index = 0;
               label_index + 1 < case_node.children.size();
               ++label_index) {
            const HirExpressionId label = check_expression(
                tree, case_node.children[label_index], scope, subject_type);
            statement.expressions.push_back(label);
            if (hir_.expression(label).symbol.is_valid()) {
              const SymbolId alternative = hir_.expression(label).symbol;
              if (std::find(
                      covered_alternatives.begin(),
                      covered_alternatives.end(),
                      alternative) != covered_alternatives.end()) {
                diagnostics_.error(
                    tree.node(case_node.children[label_index]).range,
                    "duplicate switch alternative");
              } else {
                covered_alternatives.push_back(alternative);
              }
            }
          }
          const ScopeId case_scope = semantic_.symbols.add_scope(
              ScopeKind::Block, scope, case_node.range);
          HirBlock case_block;
          case_block.scope = case_scope;
          case_block.range = case_node.range;
          check_statement_list(
              tree,
              tree.node(list_id),
              case_scope,
              result_type,
              {depth.breakable + 1, depth.loops},
              case_block);
          statement.blocks.push_back(hir_.add_block(std::move(case_block)));
        }

        const TypeKind subject_kind = is_invalid_type(subject_type)
            ? TypeKind::Invalid
            : semantic_.types.type(subject_type).kind;
        if (!has_default &&
            (subject_kind == TypeKind::Enum || subject_kind == TypeKind::TaggedUnion)) {
          const std::optional<SymbolId> owner = type_owner(subject_type);
          if (owner.has_value()) {
            std::size_t alternative_count = 0;
            for (const AggregateMember &member : semantic_.aggregate_members) {
              if (member.owner == *owner) ++alternative_count;
            }
            if (covered_alternatives.size() != alternative_count) {
              diagnostics_.error(
                  node.range,
                  "switch over enum or tagged union is not exhaustive and has no default");
            }
          }
        }
      }
      break;

    default:
      statement.kind = HirStatementKind::Invalid;
      diagnostics_.error(node.range, "statement form is not yet implemented in body checking");
      break;
    }
    return hir_.add_statement(std::move(statement));
  }

  // Conservative return analysis recognizes direct returns and if/else chains
  // whose every branch definitely returns. Loops remain nonterminating-unknown.
  [[nodiscard]] bool definitely_returns(const SyntaxTree &tree, NodeId node_id) const {
    const SyntaxNode &node = tree.node(node_id);
    if (node.kind == NodeKind::ReturnStatement) return true;
    if (node.kind == NodeKind::Block && !node.children.empty()) {
      return definitely_returns(tree, node.children.front());
    }
    if (node.kind == NodeKind::StatementList) {
      return !node.children.empty() && definitely_returns(tree, node.children.back());
    }
    if (node.kind == NodeKind::IfStatement && node.children.size() == 3) {
      return definitely_returns(tree, node.children[1]) &&
             definitely_returns(tree, node.children[2]);
    }
    return false;
  }

  // Checks one source procedure definition. Signature members contain parameters
  // followed by the result, and the prebuilt Procedure scope contains parameters.
  [[nodiscard]] bool check_procedure(SymbolId id) {
    const Symbol procedure_symbol = semantic_.symbols.symbol(id);
    const SyntaxTree *tree = find_tree(procedure_symbol.syntax.file);
    if (tree == nullptr) return false;
    const SyntaxNode &declaration = tree->node(procedure_symbol.syntax.node);
    if (declaration.children.empty()) return false;
    const NodeId procedure_id = declaration.children.back();
    const SyntaxNode &procedure = tree->node(procedure_id);
    if (procedure.kind != NodeKind::Procedure) return false;
    std::optional<NodeId> body;
    for (NodeId child : procedure.children) {
      if (tree->node(child).kind == NodeKind::Block) body = child;
    }
    if (!body.has_value()) return false;
    const std::optional<ScopeId> parameter_scope = owned_scope(id, ScopeKind::Procedure);
    if (!parameter_scope.has_value()) {
      diagnostics_.error(procedure.range, "procedure parameter scope is missing");
      return false;
    }

    const Type signature = semantic_.types.type(procedure_symbol.type);
    const TypeId result_type = signature.members.empty()
        ? semantic_.types.builtins().void_type
        : signature.members.back();
    const std::size_t initial_errors = diagnostics_.error_count();
    current_procedure_ = id;
    const HirBlockId checked_body = check_block(
        *tree, *body, *parameter_scope, result_type, {});
    if (result_type != semantic_.types.builtins().void_type &&
        !definitely_returns(*tree, *body)) {
      diagnostics_.error(procedure.range, "not every path returns a value");
    }
    hir_.add_procedure(
        {id,
         procedure_symbol.type,
         checked_body,
         diagnostics_.error_count() == initial_errors});
    current_procedure_ = {};
    return true;
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const ConditionalSelections &selections_;
  SemanticPackage &semantic_;
  const ConstantTable &constants_;
  DiagnosticSink &diagnostics_;
  HirProgram hir_;
  SymbolId current_procedure_;
};

} // namespace

BodyCheckResult check_package_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    SemanticPackage &package,
    const ConstantTable &constants,
    DiagnosticSink &diagnostics) {
  BodyChecker checker(
      sources, loaded, selections, package, constants, diagnostics);
  return checker.run();
}

} // namespace draft
