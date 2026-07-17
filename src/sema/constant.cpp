// Deterministic scalar constant evaluation and declaration-level `when` discovery.

#include "sema/constant.h"

#include "syntax/literal.h"

#include "syntax/token.h"

#include <algorithm>
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

enum class EvalStatus {
  Ready,
  Pending,
  Error,
};

// EvalResult carries readiness separately from ConstantKind. The Target object
// is a ready non-scalar intermediate, while Unavailable is reserved for default
// construction and never masquerades as a successful result.
struct EvalResult {
  EvalStatus status = EvalStatus::Pending;
  ConstantValue value;
};

enum class BindingState {
  Unvisited,
  Evaluating,
  Ready,
  Pending,
  Error,
};

[[nodiscard]] bool token_is_contextual_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
         kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
         kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber;
}

[[nodiscard]] EvalResult ready(ConstantValue value) {
  return {EvalStatus::Ready, std::move(value)};
}

[[nodiscard]] EvalResult pending() {
  return {};
}

[[nodiscard]] EvalResult error_result() {
  return {EvalStatus::Error, {}};
}

// ConstantEvaluator is a phase-local view over immutable syntax and mutable
// symbol types. Its per-symbol arrays cover the symbol table size at round start;
// constant evaluation never appends symbols, so indices and references remain
// stable during the traversal.
class ConstantEvaluator {
public:
  ConstantEvaluator(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      SemanticPackage &semantic,
      const TargetFacts &target,
      bool diagnose_unready,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), semantic_(semantic), target_(target),
        diagnose_unready_(diagnose_unready), diagnostics_(diagnostics),
        states_(semantic.symbols.symbol_count(), BindingState::Unvisited),
        values_(semantic.symbols.symbol_count()) {}

  // Evaluates each pending declaration conditional in source/site order. A
  // newly selected false branch may reveal a nested `else when` only in the next
  // rebuilt semantic round, exactly matching staged declaration visibility.
  [[nodiscard]] CompileTimeRoundResult run(ConditionalSelections &selections) {
    CompileTimeRoundResult result;
    for (const SemanticSite &site : semantic_.sites) {
      if ((site.kind != SemanticSiteKind::ConditionalDeclaration &&
           site.kind != SemanticSiteKind::ConditionalMember &&
           site.kind != SemanticSiteKind::ConditionalStatement) ||
          selections.find(site.syntax) != nullptr) {
        continue;
      }
      const SyntaxTree *tree = find_tree(site.syntax.file);
      if (tree == nullptr || !site.syntax.node.is_valid()) {
        ++result.unresolved_conditionals;
        continue;
      }
      const SyntaxNode &when = tree->node(site.syntax.node);
      if (when.children.empty()) {
        ++result.unresolved_conditionals;
        continue;
      }

      // Package declarations live in one package scope, but imports are
      // intentionally file-local. A package-level `when` condition therefore
      // starts lookup in its source file scope; member/statement conditions
      // already carry a lexical scope whose parent chain begins at that file.
      const ScopeId condition_scope =
          site.kind == SemanticSiteKind::ConditionalDeclaration &&
              site.scope == semantic_.package_scope
          ? file_scope(site.syntax.file)
          : site.scope;
      const EvalResult condition = evaluate_expression(
          *tree, when.children.front(), condition_scope, diagnose_unready_);
      if (condition.status == EvalStatus::Ready &&
          condition.value.kind == ConstantKind::Bool) {
        selections.entries.push_back({site.syntax, condition.value.boolean});
        ++result.new_selections;
        continue;
      }
      ++result.unresolved_conditionals;
      if (diagnose_unready_ && condition.status == EvalStatus::Ready) {
        diagnostics_.error(
            tree->node(when.children.front()).range,
            "compile-time 'when' condition must have type bool");
      } else if (diagnose_unready_ && condition.status == EvalStatus::Pending) {
        diagnostics_.error(
            tree->node(when.children.front()).range,
            "compile-time 'when' condition is not a ready constant");
      }
    }

    // A final semantic round also validates every data constant even when no
    // `when` depends on it. Discovery rounds avoid this work and its diagnostics
    // because branch selection can still reveal prerequisite declarations.
    if (diagnose_unready_) {
      const std::size_t symbol_count = semantic_.symbols.symbol_count();
      for (std::size_t index = 0; index < symbol_count; ++index) {
        const SymbolId id{static_cast<std::uint32_t>(index)};
        const Symbol &symbol = semantic_.symbols.symbol(id);
        if (symbol.kind != SymbolKind::Constant &&
            symbol.kind != SymbolKind::UnresolvedDeclaration) {
          continue;
        }
        const EvalResult value = evaluate_binding(id, true);
        if (value.status == EvalStatus::Pending) {
          diagnostics_.error(
              symbol.name_range,
              "constant '" + symbol.name + "' is not compile-time evaluable");
        }
      }
    }

    // Export ready constants in SymbolId order rather than dependency traversal
    // order. This is the deterministic order used by semantic dumps and hashing.
    for (std::uint32_t index = 0; index < states_.size(); ++index) {
      if (states_[index] == BindingState::Ready) {
        result.constants.bindings.push_back({SymbolId{index}, values_[index]});
      }
    }
    return result;
  }

private:
  // Locates the immutable tree named by a syntax reference. Package file order
  // is already canonical, so the direct scan is predictable and sufficient.
  [[nodiscard]] const SyntaxTree *find_tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) {
        return &*entry.syntax;
      }
    }
    return nullptr;
  }

  // Returns the scope containing this file's imports; malformed in-memory test
  // packages recover to the package scope.
  [[nodiscard]] ScopeId file_scope(FileId file) const {
    for (const FileSemanticScope &entry : semantic_.files) {
      if (entry.file == file) {
        return entry.scope;
      }
    }
    return semantic_.package_scope;
  }

  // Converts a semantic failure into Pending during discovery rounds and a
  // source diagnostic during the final no-progress round.
  [[nodiscard]] EvalResult fail(
      SourceRange range, std::string message, bool required) {
    if (!required) {
      return pending();
    }
    diagnostics_.error(range, std::move(message));
    return error_result();
  }

  // Extracts the last contextual name in a node. MemberExpression spans include
  // the base expression, so the final name is the selected field/method.
  [[nodiscard]] std::optional<std::string> final_name(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    std::optional<std::string> result;
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      const Token &token = tree.token(index);
      if (token_is_contextual_name(token.kind)) {
        result = std::string(sources_.text(token.range));
      }
    }
    return result;
  }

  // Resolves the special namespace operation `alias.public_name`. Package
  // aliases are not runtime or compile-time values themselves; the member node
  // is resolved directly into the consumer-local proxy installed from the
  // dependency interface.
  [[nodiscard]] std::optional<SymbolId> imported_member(
      const SyntaxTree &tree, const SyntaxNode &node, ScopeId scope) const {
    if (node.kind != NodeKind::MemberExpression || node.children.empty()) {
      return std::nullopt;
    }
    const SyntaxNode &base = tree.node(node.children.front());
    if (base.kind != NodeKind::NameExpression) {
      return std::nullopt;
    }
    const std::optional<std::string> alias = final_name(tree, base);
    const std::optional<std::string> member = final_name(tree, node);
    if (!alias.has_value() || !member.has_value()) {
      return std::nullopt;
    }
    const std::optional<SymbolId> import = semantic_.symbols.lookup(scope, *alias);
    if (!import.has_value() ||
        semantic_.symbols.symbol(*import).kind != SymbolKind::Import) {
      return std::nullopt;
    }
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner == *import &&
          semantic_.symbols.scope(owned.scope).kind == ScopeKind::ImportedPackage) {
        return semantic_.symbols.lookup_direct(owned.scope, *member);
      }
    }
    return std::nullopt;
  }

  // Integer and decimal-float token validation has already happened in the
  // lexer. These conversions construct the mathematical compile-time value and
  // never pass through a host floating or fixed-width integer representation.
  [[nodiscard]] std::optional<BigInteger> integer_literal(
      std::string_view spelling) const {
    return BigInteger::parse_literal(spelling);
  }

  // Decodes the ordinary escapes needed by compile-time strings and preserves
  // raw backtick bytes exactly. Unicode and byte escape decoding will share the
  // lexer's validated escape contract in the complete literal-value module.
  [[nodiscard]] std::optional<std::string> string_literal(
      std::string_view spelling, TokenKind kind) const {
    return decode_string_literal(spelling, kind);
  }

  // Finds the binary operator token between the two immediate child spans. The
  // parser does not copy operators into nodes, but its nonoverlapping token spans
  // make this lookup unambiguous.
  [[nodiscard]] TokenKind binary_operator(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    if (node.children.size() != 2) return TokenKind::Invalid;
    const SyntaxNode &left = tree.node(node.children[0]);
    const SyntaxNode &right = tree.node(node.children[1]);
    for (std::uint32_t index = left.token_end; index < right.token_begin; ++index) {
      const TokenKind kind = tree.token(index).kind;
      switch (kind) {
      case TokenKind::Plus:
      case TokenKind::Minus:
      case TokenKind::Star:
      case TokenKind::Slash:
      case TokenKind::Percent:
      case TokenKind::ShiftLeft:
      case TokenKind::ShiftRight:
      case TokenKind::Ampersand:
      case TokenKind::Pipe:
      case TokenKind::Caret:
      case TokenKind::EqualEqual:
      case TokenKind::BangEqual:
      case TokenKind::Less:
      case TokenKind::LessEqual:
      case TokenKind::Greater:
      case TokenKind::GreaterEqual:
      case TokenKind::LogicalAnd:
      case TokenKind::LogicalOr:
        return kind;
      default:
        break;
      }
    }
    return TokenKind::Invalid;
  }

  static constexpr std::size_t kMaximumConstantBits = 1000000;

  [[nodiscard]] EvalResult bounded_integer(
      BigInteger value, SourceRange range, bool required) {
    if (value.bit_count() > kMaximumConstantBits) {
      return fail(range, "compile-time integer resource limit exceeded", required);
    }
    return ready(ConstantValue::make_integer(std::move(value)));
  }

  [[nodiscard]] EvalResult bounded_float(
      ExactRational value, SourceRange range, bool required) {
    if (value.numerator().bit_count() > kMaximumConstantBits ||
        value.denominator().bit_count() > kMaximumConstantBits) {
      return fail(range, "compile-time rational resource limit exceeded", required);
    }
    return ready(ConstantValue::make_float(std::move(value)));
  }

  // Applies arbitrary-precision integer arithmetic. Right shift and bitwise
  // operations delegate to BigInteger's infinite two's-complement domain.
  [[nodiscard]] EvalResult evaluate_integer_binary(
      TokenKind operation,
      const BigInteger &left,
      const BigInteger &right,
      SourceRange range,
      bool required) {
    const int order = left.compare(right);
    switch (operation) {
    case TokenKind::EqualEqual: return ready(ConstantValue::make_bool(order == 0));
    case TokenKind::BangEqual: return ready(ConstantValue::make_bool(order != 0));
    case TokenKind::Less: return ready(ConstantValue::make_bool(order < 0));
    case TokenKind::LessEqual: return ready(ConstantValue::make_bool(order <= 0));
    case TokenKind::Greater: return ready(ConstantValue::make_bool(order > 0));
    case TokenKind::GreaterEqual: return ready(ConstantValue::make_bool(order >= 0));
    default:
      break;
    }

    if (operation == TokenKind::Plus) {
      return bounded_integer(left.added(right), range, required);
    }
    if (operation == TokenKind::Minus) {
      return bounded_integer(left.subtracted(right), range, required);
    }
    if (operation == TokenKind::Star) {
      return bounded_integer(left.multiplied(right), range, required);
    }
    if (operation == TokenKind::Slash || operation == TokenKind::Percent) {
      BigInteger quotient;
      BigInteger remainder;
      if (!left.divide(right, quotient, remainder)) {
        return fail(range, "division by zero in compile-time expression", required);
      }
      return bounded_integer(
          operation == TokenKind::Slash ? std::move(quotient) : std::move(remainder),
          range,
          required);
    }
    if (operation == TokenKind::Ampersand) {
      return bounded_integer(left.bitwise_and(right), range, required);
    }
    if (operation == TokenKind::Pipe) {
      return bounded_integer(left.bitwise_or(right), range, required);
    }
    if (operation == TokenKind::Caret) {
      return bounded_integer(left.bitwise_xor(right), range, required);
    }
    if (operation == TokenKind::ShiftLeft || operation == TokenKind::ShiftRight) {
      if (right.is_negative()) {
        return fail(range, "compile-time shift count is negative", required);
      }
      const std::optional<std::uint64_t> count = right.to_u64();
      if (!count.has_value() || *count > kMaximumConstantBits) {
        return fail(range, "compile-time shift resource limit exceeded", required);
      }
      const std::size_t host_count = static_cast<std::size_t>(*count);
      return bounded_integer(
          operation == TokenKind::ShiftLeft
              ? left.shifted_left(host_count)
              : left.shifted_right(host_count),
          range,
          required);
    }
    return fail(range, "operator is not defined for integer constants", required);
  }

  [[nodiscard]] EvalResult evaluate_float_binary(
      TokenKind operation,
      const ExactRational &left,
      const ExactRational &right,
      SourceRange range,
      bool required) {
    const int order = left.compare(right);
    switch (operation) {
    case TokenKind::EqualEqual: return ready(ConstantValue::make_bool(order == 0));
    case TokenKind::BangEqual: return ready(ConstantValue::make_bool(order != 0));
    case TokenKind::Less: return ready(ConstantValue::make_bool(order < 0));
    case TokenKind::LessEqual: return ready(ConstantValue::make_bool(order <= 0));
    case TokenKind::Greater: return ready(ConstantValue::make_bool(order > 0));
    case TokenKind::GreaterEqual: return ready(ConstantValue::make_bool(order >= 0));
    case TokenKind::Plus:
      return bounded_float(left.added(right), range, required);
    case TokenKind::Minus:
      return bounded_float(left.subtracted(right), range, required);
    case TokenKind::Star:
      return bounded_float(left.multiplied(right), range, required);
    case TokenKind::Slash: {
      ExactRational quotient;
      if (!left.divide(right, quotient)) {
        return fail(range, "division by zero in compile-time expression", required);
      }
      return bounded_float(std::move(quotient), range, required);
    }
    default:
      return fail(range, "operator is not valid for untyped floating constants", required);
    }
  }

  // Evaluates equality for nonnumeric scalar compile-time values. Categorical
  // target labels compare only with matching categorical labels.
  [[nodiscard]] EvalResult evaluate_scalar_equality(
      TokenKind operation,
      const ConstantValue &left,
      const ConstantValue &right,
      SourceRange range,
      bool required) {
    if (operation != TokenKind::EqualEqual && operation != TokenKind::BangEqual) {
      return fail(range, "operator is not defined for these compile-time values", required);
    }
    bool equal = false;
    if (left.kind == ConstantKind::Bool && right.kind == ConstantKind::Bool) {
      equal = left.boolean == right.boolean;
    } else if (left.kind == ConstantKind::EnumLabel &&
               right.kind == ConstantKind::EnumLabel) {
      equal = left.text == right.text;
    } else {
      return fail(range, "comparison uses incompatible compile-time values", required);
    }
    if (operation == TokenKind::BangEqual) equal = !equal;
    return ready(ConstantValue::make_bool(equal));
  }

  // Evaluates a binary node with source-defined short circuiting. Right operands
  // of `&&` and `||` are not requested when the left value decides the result.
  [[nodiscard]] EvalResult evaluate_binary(
      const SyntaxTree &tree,
      const SyntaxNode &node,
      ScopeId scope,
      bool required) {
    if (node.children.size() != 2) return pending();
    const TokenKind operation = binary_operator(tree, node);
    const EvalResult left = evaluate_expression(tree, node.children[0], scope, required);
    if (left.status != EvalStatus::Ready) return left;
    if (operation == TokenKind::LogicalAnd || operation == TokenKind::LogicalOr) {
      if (left.value.kind != ConstantKind::Bool) {
        return fail(node.range, "logical operator requires bool operands", required);
      }
      if (operation == TokenKind::LogicalAnd && !left.value.boolean) {
        return ready(ConstantValue::make_bool(false));
      }
      if (operation == TokenKind::LogicalOr && left.value.boolean) {
        return ready(ConstantValue::make_bool(true));
      }
      const EvalResult right =
          evaluate_expression(tree, node.children[1], scope, required);
      if (right.status != EvalStatus::Ready) return right;
      if (right.value.kind != ConstantKind::Bool) {
        return fail(node.range, "logical operator requires bool operands", required);
      }
      return right;
    }

    const EvalResult right = evaluate_expression(tree, node.children[1], scope, required);
    if (right.status != EvalStatus::Ready) return right;
    if (left.value.kind == ConstantKind::Integer &&
        right.value.kind == ConstantKind::Integer) {
      return evaluate_integer_binary(
          operation, left.value.integer, right.value.integer, node.range, required);
    }
    if ((left.value.kind == ConstantKind::Integer ||
         left.value.kind == ConstantKind::Float) &&
        (right.value.kind == ConstantKind::Integer ||
         right.value.kind == ConstantKind::Float)) {
      const ExactRational left_float = left.value.kind == ConstantKind::Float
          ? left.value.floating
          : ExactRational(left.value.integer);
      const ExactRational right_float = right.value.kind == ConstantKind::Float
          ? right.value.floating
          : ExactRational(right.value.integer);
      return evaluate_float_binary(
          operation, left_float, right_float, node.range, required);
    }
    return evaluate_scalar_equality(operation, left.value, right.value, node.range, required);
  }

  // Maps the built-in target object's stable fields to scalar compile-time
  // values. Unknown fields are diagnosed only when the expression is required.
  [[nodiscard]] EvalResult evaluate_target_member(
      std::string_view member, SourceRange range, bool required) {
    if (member == "identity") return ready(ConstantValue::make_string(target_.identity));
    if (member == "arch") return ready(ConstantValue::make_enum_label(target_.arch));
    if (member == "os") return ready(ConstantValue::make_enum_label(target_.os));
    if (member == "abi") return ready(ConstantValue::make_enum_label(target_.abi));
    if (member == "byte_order") {
      return ready(ConstantValue::make_enum_label(target_.byte_order));
    }
    if (member == "object_format") {
      return ready(ConstantValue::make_enum_label(target_.object_format));
    }
    if (member == "file_tag") return ready(ConstantValue::make_string(target_.file_tag));
    if (member == "pointer_bits" || member == "page_size") {
      const std::uint64_t source = member == "pointer_bits"
          ? target_.pointer_bits
          : target_.page_size;
      return ready(ConstantValue::make_integer(BigInteger::from_u64(source)));
    }
    return fail(range, "unknown target field '" + std::string(member) + "'", required);
  }

  // Handles the one target method in Draft 1 directly from its parsed call. The
  // known-feature table distinguishes false from an invalid feature spelling.
  [[nodiscard]] EvalResult evaluate_target_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      bool required) {
    if (call.children.size() != 2) return pending();
    const SyntaxNode &callee = tree.node(call.children.front());
    if (callee.kind != NodeKind::MemberExpression || callee.children.empty()) {
      return pending();
    }
    const EvalResult base =
        evaluate_expression(tree, callee.children.front(), scope, required);
    const std::optional<std::string> method = final_name(tree, callee);
    if (base.status != EvalStatus::Ready || base.value.kind != ConstantKind::Target ||
        !method.has_value() || *method != "has_feature") {
      return pending();
    }
    const EvalResult argument =
        evaluate_expression(tree, call.children[1], scope, required);
    if (argument.status != EvalStatus::Ready) return argument;
    if (argument.value.kind != ConstantKind::String) {
      return fail(call.range, "target.has_feature requires a compile-time string", required);
    }
    const bool known = std::binary_search(
        target_.known_features.begin(), target_.known_features.end(), argument.value.text);
    if (!known) {
      return fail(
          tree.node(call.children[1]).range,
          "unrecognized target feature '" + argument.value.text + "'",
          required);
    }
    const bool enabled = std::binary_search(
        target_.features.begin(), target_.features.end(), argument.value.text);
    return ready(ConstantValue::make_bool(enabled));
  }

  // Resolves one package constant lazily. The Evaluating state detects cycles;
  // successfully computed scalar types are written back to the symbol for later
  // expected-type checking.
  [[nodiscard]] EvalResult evaluate_binding(SymbolId id, bool required) {
    if (static_cast<std::size_t>(id.value) >= states_.size()) return pending();
    BindingState &state = states_[id.value];
    if (state == BindingState::Ready) return ready(values_[id.value]);
    if (state == BindingState::Pending) return pending();
    if (state == BindingState::Error) return error_result();
    const Symbol initial = semantic_.symbols.symbol(id);
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy != id) {
        continue;
      }
      if (!imported.has_constant) {
        state = BindingState::Pending;
        return fail(
            initial.name_range,
            "imported declaration '" + imported.public_name + "' is not a constant",
            required);
      }
      state = BindingState::Ready;
      values_[id.value] = imported.constant;
      return ready(imported.constant);
    }
    if (state == BindingState::Evaluating) {
      return fail(
          initial.name_range,
          "cycle while evaluating compile-time constant '" + initial.name + "'",
          required);
    }
    if (initial.kind != SymbolKind::Constant &&
        initial.kind != SymbolKind::UnresolvedDeclaration) {
      state = BindingState::Pending;
      return pending();
    }
    state = BindingState::Evaluating;

    const SyntaxTree *tree = find_tree(initial.syntax.file);
    if (tree == nullptr || !initial.syntax.node.is_valid()) {
      state = BindingState::Pending;
      return pending();
    }
    const SyntaxNode &declaration = tree->node(initial.syntax.node);
    if (declaration.children.size() < 2) {
      state = BindingState::Pending;
      return pending();
    }
    const NodeId expression = declaration.children.back();
    const EvalResult result = evaluate_expression(
        *tree, expression, file_scope(tree->file()), required);
    if (result.status == EvalStatus::Ready) {
      states_[id.value] = BindingState::Ready;
      values_[id.value] = result.value;
      TypeId type;
      if (result.value.kind == ConstantKind::Bool) {
        type = semantic_.types.builtins().bool_type;
      } else if (result.value.kind == ConstantKind::Integer) {
        type = semantic_.types.builtins().untyped_integer;
      } else if (result.value.kind == ConstantKind::Float) {
        type = semantic_.types.builtins().untyped_float;
      } else if (result.value.kind == ConstantKind::String) {
        type = semantic_.types.builtins().string_type;
      }
      if (type.is_valid()) semantic_.symbols.symbol_mut(id).type = type;
      return result;
    }
    states_[id.value] = result.status == EvalStatus::Error
        ? BindingState::Error
        : BindingState::Pending;
    return result;
  }

  // Evaluates the scalar expression subset in strict source evaluation order.
  // Unsupported forms return Pending; the final fixed-point caller supplies the
  // generic "not a ready constant" diagnostic rather than guessing semantics.
  [[nodiscard]] EvalResult evaluate_expression(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      bool required) {
    const SyntaxNode &node = tree.node(expression_id);
    switch (node.kind) {
    case NodeKind::LiteralExpression: {
      if (node.token_begin >= node.token_end) return pending();
      const Token &token = tree.token(node.token_begin);
      if (token.kind == TokenKind::KeywordTrue) {
        return ready(ConstantValue::make_bool(true));
      }
      if (token.kind == TokenKind::KeywordFalse) {
        return ready(ConstantValue::make_bool(false));
      }
      if (token.kind == TokenKind::IntegerLiteral) {
        const std::optional<BigInteger> value =
            integer_literal(sources_.text(token.range));
        if (!value.has_value()) {
          return fail(token.range, "invalid integer literal", required);
        }
        return ready(ConstantValue::make_integer(*value));
      }
      if (token.kind == TokenKind::FloatLiteral) {
        const std::optional<ExactRational> value =
            ExactRational::parse_decimal(sources_.text(token.range));
        if (!value.has_value()) {
          return fail(token.range, "invalid or excessive decimal floating literal", required);
        }
        return ready(ConstantValue::make_float(*value));
      }
      if (token.kind == TokenKind::StringLiteral ||
          token.kind == TokenKind::RawStringLiteral) {
        const std::optional<std::string> value =
            string_literal(sources_.text(token.range), token.kind);
        if (!value.has_value()) {
          return fail(token.range, "unsupported string escape in constant", required);
        }
        return ready(ConstantValue::make_string(*value));
      }
      return pending();
    }

    case NodeKind::NameExpression: {
      const std::optional<std::string> name = final_name(tree, node);
      if (!name.has_value()) return pending();
      if (*name == "target") return ready(ConstantValue::make_target());
      const std::optional<SymbolId> symbol = semantic_.symbols.lookup(scope, *name);
      if (!symbol.has_value()) return pending();
      return evaluate_binding(*symbol, required);
    }

    case NodeKind::ContextualAlternativeExpression: {
      const std::optional<std::string> name = final_name(tree, node);
      if (!name.has_value()) return pending();
      return ready(ConstantValue::make_enum_label(*name));
    }

    case NodeKind::MemberExpression: {
      if (node.children.empty()) return pending();
      if (const std::optional<SymbolId> imported = imported_member(tree, node, scope)) {
        return evaluate_binding(*imported, required);
      }
      const EvalResult base = evaluate_expression(tree, node.children.front(), scope, required);
      if (base.status != EvalStatus::Ready) return base;
      const std::optional<std::string> member = final_name(tree, node);
      if (base.value.kind == ConstantKind::Target && member.has_value()) {
        return evaluate_target_member(*member, node.range, required);
      }
      return pending();
    }

    case NodeKind::UnaryExpression: {
      if (node.children.empty()) return pending();
      const EvalResult operand =
          evaluate_expression(tree, node.children.front(), scope, required);
      if (operand.status != EvalStatus::Ready) return operand;
      const TokenKind operation = tree.token(node.token_begin).kind;
      if (operation == TokenKind::Bang && operand.value.kind == ConstantKind::Bool) {
        return ready(ConstantValue::make_bool(!operand.value.boolean));
      }
      if (operand.value.kind != ConstantKind::Integer) {
        if (operand.value.kind == ConstantKind::Float && operation == TokenKind::Plus) {
          return operand;
        }
        if (operand.value.kind == ConstantKind::Float && operation == TokenKind::Minus) {
          return bounded_float(operand.value.floating.negated(), node.range, required);
        }
        return fail(node.range, "unary operator uses an incompatible constant", required);
      }
      if (operation == TokenKind::Plus) return operand;
      if (operation == TokenKind::Minus) {
        return bounded_integer(operand.value.integer.negated(), node.range, required);
      }
      if (operation == TokenKind::Tilde) {
        return bounded_integer(operand.value.integer.bitwise_not(), node.range, required);
      }
      return pending();
    }

    case NodeKind::BinaryExpression:
      return evaluate_binary(tree, node, scope, required);

    case NodeKind::GroupExpression:
      if (!node.children.empty()) {
        return evaluate_expression(tree, node.children.front(), scope, required);
      }
      return pending();

    case NodeKind::ConditionalExpression: {
      if (node.children.size() != 3) return pending();
      const EvalResult condition =
          evaluate_expression(tree, node.children[1], scope, required);
      if (condition.status != EvalStatus::Ready) return condition;
      if (condition.value.kind != ConstantKind::Bool) {
        return fail(node.range, "constant conditional requires a bool condition", required);
      }
      return evaluate_expression(
          tree,
          condition.value.boolean ? node.children[0] : node.children[2],
          scope,
          required);
    }

    case NodeKind::CallExpression:
      return evaluate_target_call(tree, node, scope, required);

    case NodeKind::DenyExpression:
      if (!node.children.empty()) {
        return evaluate_expression(tree, node.children.back(), scope, required);
      }
      return pending();

    default:
      return pending();
    }
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  SemanticPackage &semantic_;
  const TargetFacts &target_;
  bool diagnose_unready_ = false;
  DiagnosticSink &diagnostics_;
  std::vector<BindingState> states_;
  std::vector<ConstantValue> values_;
};

} // namespace

ConstantValue ConstantValue::make_bool(bool value) {
  ConstantValue result;
  result.kind = ConstantKind::Bool;
  result.boolean = value;
  return result;
}

ConstantValue ConstantValue::make_integer(std::int64_t value) {
  return make_integer(BigInteger::from_i64(value));
}

ConstantValue ConstantValue::make_integer(BigInteger value) {
  ConstantValue result;
  result.kind = ConstantKind::Integer;
  result.integer = std::move(value);
  return result;
}

ConstantValue ConstantValue::make_float(ExactRational value) {
  ConstantValue result;
  result.kind = ConstantKind::Float;
  result.floating = std::move(value);
  return result;
}

ConstantValue ConstantValue::make_string(std::string value) {
  ConstantValue result;
  result.kind = ConstantKind::String;
  result.text = std::move(value);
  return result;
}

ConstantValue ConstantValue::make_enum_label(std::string value) {
  ConstantValue result;
  result.kind = ConstantKind::EnumLabel;
  result.text = std::move(value);
  return result;
}

ConstantValue ConstantValue::make_target() {
  ConstantValue result;
  result.kind = ConstantKind::Target;
  return result;
}

const ConstantValue *ConstantTable::find(SymbolId symbol) const {
  for (const ConstantBinding &binding : bindings) {
    if (binding.symbol == symbol) {
      return &binding.value;
    }
  }
  return nullptr;
}

CompileTimeRoundResult evaluate_compile_time_round(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    ConditionalSelections &selections,
    bool diagnose_unready,
    DiagnosticSink &diagnostics) {
  ConstantEvaluator evaluator(
      sources, loaded, package, target, diagnose_unready, diagnostics);
  return evaluator.run(selections);
}

} // namespace draft
