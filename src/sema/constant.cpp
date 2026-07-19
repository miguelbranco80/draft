// Deterministic constant evaluation and declaration-level `when` discovery.

#include "sema/constant.h"

#include "sema/ieee_float.h"
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
  // The expression is otherwise a compile-time dependency, but interface
  // discovery reached source that resolution must replace before evaluation
  // can continue. This is distinct from an unknown ordinary constant: it
  // publishes an early synthesis obligation instead of a generic fixed-point
  // failure.
  BlockedBySynthesis,
  Error,
};

// EvalResult carries readiness separately from ConstantKind. The Target object
// is a ready non-scalar intermediate, while Unavailable is reserved for default
// construction and never masquerades as a successful result.
struct EvalResult {
  EvalStatus status = EvalStatus::Pending;
  ConstantValue value;
  TypeId type;
};

enum class BindingState {
  Unvisited,
  Evaluating,
  Ready,
  Pending,
  BlockedBySynthesis,
  Error,
};

// Procedure evaluation uses an explicit control signal instead of exceptions
// or host recursion for statement flow.  Failed carries the expression
// evaluator's Ready/Pending/Error distinction so an exploratory semantic round
// can still postpone a call whose dependency is not ready.
enum class ExecutionSignal {
  Normal,
  Return,
  Break,
  Continue,
  Failed,
};

struct ExecutionResult {
  ExecutionSignal signal = ExecutionSignal::Normal;
  EvalStatus failure = EvalStatus::Ready;
  ConstantValue value;
  TypeId type;
};

struct LocalBinding {
  std::string name;
  ConstantValue value;
  TypeId type;
};

// A deferred call owns the values which were produced at the defer statement.
// Keeping the prepared bindings, rather than the call syntax, is important:
// later assignments in the surrounding procedure must not change a saved
// argument, and argument expressions with procedure calls must execute only
// once.  The callee body is looked up again by its stable SymbolId when the
// lexical scope exits.
struct PreparedProcedureCall {
  SymbolId procedure;
  std::vector<LocalBinding> bindings;
  std::vector<ConstantTypeBinding> type_bindings;
  TypeId result_type;
  SourceRange range;
};

// A procedure call is a lexical boundary.  Its block scopes may see parameters
// and their own ancestors, but never the caller's locals.  Keeping frames and
// scopes as small vectors makes shadowing order explicit and deterministic.
struct LocalFrame {
  std::vector<std::vector<LocalBinding>> scopes;
  std::vector<ConstantTypeBinding> type_bindings;
  std::vector<std::vector<PreparedProcedureCall>> defer_scopes;
  TypeId result_type;
};

struct LocalTarget {
  std::string root;
  std::vector<std::size_t> path;
  ConstantValue value;
  TypeId type;
};

struct LocalTargetResult {
  EvalStatus status = EvalStatus::Pending;
  std::optional<LocalTarget> target;
};

[[nodiscard]] bool token_is_contextual_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
         kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
         kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
         kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory;
}

[[nodiscard]] EvalResult ready(ConstantValue value, TypeId type = {}) {
  return {EvalStatus::Ready, std::move(value), type};
}

[[nodiscard]] EvalResult pending() {
  return {};
}

[[nodiscard]] EvalResult blocked_by_synthesis() {
  return {EvalStatus::BlockedBySynthesis, {}, {}};
}

[[nodiscard]] EvalResult error_result() {
  return {EvalStatus::Error, {}, {}};
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
      CompileTimeSynthesisMode synthesis_mode,
      bool diagnose_unready,
      DiagnosticSink &diagnostics,
      const ConstantTable *local_constants = nullptr,
      const std::vector<ConstantTypeBinding> *local_types = nullptr)
      : sources_(sources), loaded_(loaded), semantic_(semantic), target_(target),
        synthesis_mode_(synthesis_mode), diagnose_unready_(diagnose_unready),
        diagnostics_(diagnostics),
        local_constants_(local_constants), local_types_(local_types),
        states_(semantic.symbols.symbol_count(), BindingState::Unvisited),
        values_(semantic.symbols.symbol_count()) {}

  // Evaluates each pending declaration conditional in source/site order. A
  // newly selected false branch may reveal a nested `else when` only in the next
  // rebuilt semantic round, exactly matching staged declaration visibility.
  [[nodiscard]] CompileTimeRoundResult run(ConditionalSelections &selections) {
    CompileTimeRoundResult result;
    // Discovery may append a direct synthesis-expression site while evaluating
    // one condition. Snapshot the original count and copy each row so vector
    // growth cannot invalidate the active site reference; appended synthesis
    // rows are outputs, not new conditionals for this interpreter pass.
    const std::size_t original_site_count = semantic_.sites.size();
    for (std::size_t site_index = 0;
         site_index < original_site_count;
         ++site_index) {
      const SemanticSite site = semantic_.sites[site_index];
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
          *tree,
          when.children.front(),
          condition_scope,
          diagnose_unready_,
          semantic_.types.builtins().bool_type);
      if (condition.status == EvalStatus::Ready &&
          condition.value.kind == ConstantKind::Bool) {
        selections.entries.push_back({site.syntax, condition.value.boolean});
        ++result.new_selections;
        continue;
      }
      if (condition.status == EvalStatus::BlockedBySynthesis) {
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

    result.compile_time_procedures = compile_time_procedures_;

    // Export ready constants in SymbolId order rather than dependency traversal
    // order. This is the deterministic order used by semantic dumps and hashing.
    for (std::uint32_t index = 0; index < states_.size(); ++index) {
      if (states_[index] == BindingState::Ready) {
        result.constants.bindings.push_back({SymbolId{index}, values_[index]});
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<EvaluatedConstant> evaluate_required_expression(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId expected = {}) {
    const EvalResult result =
        evaluate_expression(tree, expression, scope, true, expected);
    if (result.status == EvalStatus::Pending) {
      diagnostics_.error(
          tree.node(expression).range,
          "expression is not compile-time evaluable");
      return std::nullopt;
    }
    if (result.status == EvalStatus::BlockedBySynthesis) {
      return std::nullopt;
    }
    if (result.status != EvalStatus::Ready) return std::nullopt;
    return EvaluatedConstant{result.value, result.type};
  }

  [[nodiscard]] CompileTimeExpressionDiscoveryResult
  discover_required_expression(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId expected = {}) {
    CompileTimeExpressionDiscoveryResult discovered;
    const EvalResult result =
        evaluate_expression(tree, expression, scope, false, expected);
    discovered.blocked_by_synthesis =
        result.status == EvalStatus::BlockedBySynthesis;
    discovered.compile_time_procedures = compile_time_procedures_;
    if (result.status == EvalStatus::Ready) {
      discovered.value = EvaluatedConstant{result.value, result.type};
    }
    return discovered;
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

  // A source synthesis expression reached outside a procedure body is already
  // at its semantic program point, so the evaluator can publish it directly.
  // Procedure-body sites take the ordinary BodyChecker path instead: that pass
  // supplies lexical locals, branch refinements, and the enclosing declaration
  // skeleton which an execution-only interpreter must not try to recreate.
  [[nodiscard]] EvalResult unresolved_synthesis_expression(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId expected,
      bool required) {
    if (synthesis_mode_ == CompileTimeSynthesisMode::Reject) {
      return fail(
          tree.node(expression).range,
          "unresolved synthesis is unavailable during compile-time evaluation",
          required);
    }
    if (!active_procedures_.empty()) {
      for (SymbolId procedure : active_procedures_) {
        remember_compile_time_procedure(procedure);
      }
      return blocked_by_synthesis();
    }

    const SyntaxReference syntax{tree.file(), expression};
    for (SemanticSite &site : semantic_.sites) {
      if (site.kind != SemanticSiteKind::SynthesisExpression ||
          site.syntax != syntax) {
        continue;
      }
      if (!site.expected_type.is_valid() && expected.is_valid()) {
        site.expected_type = expected;
      } else if (site.expected_type.is_valid() && expected.is_valid() &&
                 site.expected_type != expected) {
        return fail(
            tree.node(expression).range,
            "compile-time synthesis site has inconsistent expected types",
            required);
      }
      return blocked_by_synthesis();
    }

    SemanticSite site;
    site.kind = SemanticSiteKind::SynthesisExpression;
    site.syntax = syntax;
    site.scope = scope;
    site.expected_type = expected;
    // Member-level compile-time conditions are anchored to their owning type.
    // Package/file scopes intentionally retain the invalid package anchor.
    ScopeId candidate = scope;
    while (candidate.is_valid()) {
      for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
        if (owned.scope == candidate &&
            semantic_.symbols.scope(candidate).kind == ScopeKind::Type) {
          site.anchor = owned.owner;
          break;
        }
      }
      if (site.anchor.is_valid() || candidate == semantic_.package_scope) break;
      candidate = semantic_.symbols.scope(candidate).parent;
    }
    semantic_.sites.push_back(std::move(site));
    return blocked_by_synthesis();
  }

  // The body checker later applies this set in declaration order. The
  // evaluator records first reachability only to avoid duplicate work when two
  // constants call the same helper or recursion revisits it.
  void remember_compile_time_procedure(SymbolId procedure) {
    if (std::find(
            compile_time_procedures_.begin(),
            compile_time_procedures_.end(),
            procedure) == compile_time_procedures_.end()) {
      compile_time_procedures_.push_back(procedure);
    }
  }

  [[nodiscard]] const ConstantValue *local_value(std::string_view name) const {
    if (local_frames_.empty()) return nullptr;
    const LocalFrame &frame = local_frames_.back();
    for (std::size_t remaining = frame.scopes.size(); remaining > 0; --remaining) {
      const std::vector<LocalBinding> &scope = frame.scopes[remaining - 1];
      for (std::size_t index = scope.size(); index > 0; --index) {
        if (scope[index - 1].name == name) return &scope[index - 1].value;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const LocalBinding *local_binding(
      std::string_view name) const {
    if (local_frames_.empty()) return nullptr;
    const LocalFrame &frame = local_frames_.back();
    for (std::size_t remaining = frame.scopes.size(); remaining > 0; --remaining) {
      const std::vector<LocalBinding> &scope = frame.scopes[remaining - 1];
      for (std::size_t index = scope.size(); index > 0; --index) {
        if (scope[index - 1].name == name) return &scope[index - 1];
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool assign_local(
      std::string_view name, ConstantValue value) {
    if (local_frames_.empty()) return false;
    LocalFrame &frame = local_frames_.back();
    for (std::size_t remaining = frame.scopes.size(); remaining > 0; --remaining) {
      std::vector<LocalBinding> &scope = frame.scopes[remaining - 1];
      for (std::size_t index = scope.size(); index > 0; --index) {
        if (scope[index - 1].name == name) {
          scope[index - 1].value = std::move(value);
          return true;
        }
      }
    }
    return false;
  }

  void declare_local(std::string name, ConstantValue value, TypeId type) {
    if (local_frames_.empty()) return;
    LocalFrame &frame = local_frames_.back();
    if (frame.scopes.empty()) frame.scopes.emplace_back();
    frame.scopes.back().push_back(
        {std::move(name), std::move(value), type});
  }

  [[nodiscard]] std::vector<std::string> names_in_node(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    std::vector<std::string> result;
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      const Token &token = tree.token(index);
      if (token_is_contextual_name(token.kind)) {
        result.emplace_back(sources_.text(token.range));
      }
    }
    return result;
  }

  [[nodiscard]] bool consume_execution_step(
      SourceRange range, bool required) {
    if (execution_steps_remaining_ != 0) {
      --execution_steps_remaining_;
      return true;
    }
    if (required && !execution_limit_reported_) {
      diagnostics_.error(range, "compile-time procedure step limit exceeded");
      execution_limit_reported_ = true;
    }
    return false;
  }

  [[nodiscard]] ExecutionResult failed_execution(EvalStatus status) const {
    ExecutionResult result;
    result.signal = ExecutionSignal::Failed;
    result.failure = status;
    return result;
  }

  [[nodiscard]] ExecutionResult failed_execution(
      const EvalResult &result) const {
    return failed_execution(result.status);
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

  // Contextual alternatives use the first name after '.', because a payload
  // expression may contain additional names of its own (`.value(local)`).
  [[nodiscard]] std::optional<std::string> first_name(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      const Token &token = tree.token(index);
      if (token_is_contextual_name(token.kind)) {
        return std::string(sources_.text(token.range));
      }
    }
    return std::nullopt;
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

  // Keeps a consumer-local SymbolId for immediate lowering and also records an
  // imported procedure's canonical origin. Local identities are canonicalized
  // later by the interface builder, which is the first phase that owns the
  // current package's workspace identity.
  [[nodiscard]] ConstantValue procedure_value(SymbolId symbol_id) const {
    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy == symbol_id) {
        return ConstantValue::make_procedure(
            symbol_id.value,
            imported.public_name,
            imported.root_identity,
            imported.root_relative_path);
      }
    }
    return ConstantValue::make_procedure(symbol_id.value, symbol.name);
  }

  [[nodiscard]] std::optional<SymbolId> procedure_symbol(
      const ConstantValue &value) const {
    if (value.kind != ConstantKind::Procedure) return std::nullopt;
    if (value.symbol_index != std::numeric_limits<std::uint32_t>::max() &&
        value.symbol_index < semantic_.symbols.symbol_count()) {
      const SymbolId symbol{value.symbol_index};
      if (semantic_.symbols.symbol(symbol).kind == SymbolKind::Procedure) {
        return symbol;
      }
    }
    if (!value.root_identity.empty()) {
      for (const ImportedSymbol &imported : semantic_.imported_symbols) {
        if (imported.root_identity == value.root_identity &&
            imported.root_relative_path == value.root_relative_path &&
            imported.public_name == value.text &&
            semantic_.symbols.symbol(imported.proxy).kind == SymbolKind::Procedure) {
          return imported.proxy;
        }
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

  [[nodiscard]] std::optional<std::uint32_t> rune_literal(
      std::string_view spelling) const {
    return decode_rune_literal(spelling);
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
  static constexpr std::size_t kMaximumExecutionSteps = 1000000;
  // A package-constant dependency chain does not necessarily execute a Draft
  // procedure and therefore does not consume the procedure recursion budget
  // below. Bound the evaluator's own mutually recursive binding/expression
  // walk independently before an acyclic source graph can exhaust the host
  // stack. Parser expression nesting is a third, separate resource.
  static constexpr std::size_t kMaximumBindingDependencyDepth = 256;
  // Procedure execution is deliberately recursive because that keeps source
  // control flow obvious.  Stop well before the host C++ stack becomes the
  // accidental resource limit; a Draft program gets a normal diagnostic
  // instead of a compiler crash.  This limit can be raised after the evaluator
  // moves to an explicit call stack.
  static constexpr std::size_t kMaximumProcedureCallDepth = 64;

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

  [[nodiscard]] Type runtime_type(TypeId type_id) const {
    Type result = semantic_.types.type(type_id);
    while (result.kind == TypeKind::Distinct) {
      result = semantic_.types.type(result.element);
    }
    return result;
  }

  [[nodiscard]] bool concrete_numeric(TypeId type_id) const {
    if (!type_id.is_valid()) return false;
    const TypeKind kind = runtime_type(type_id).kind;
    return kind == TypeKind::SignedInteger ||
        kind == TypeKind::UnsignedInteger || kind == TypeKind::Float;
  }

  [[nodiscard]] bool untyped_integer(TypeId type_id) const {
    return type_id.is_valid() &&
        semantic_.types.type(type_id).kind == TypeKind::UntypedInteger;
  }

  [[nodiscard]] bool untyped_float(TypeId type_id) const {
    return type_id.is_valid() &&
        semantic_.types.type(type_id).kind == TypeKind::UntypedFloat;
  }

  [[nodiscard]] bool integer_operand(TypeId type_id) const {
    if (untyped_integer(type_id)) return true;
    if (!type_id.is_valid()) return false;
    const TypeKind kind = runtime_type(type_id).kind;
    return kind == TypeKind::SignedInteger ||
        kind == TypeKind::UnsignedInteger;
  }

  // Index and slice-bound expressions have a deliberately narrower rule than
  // ordinary integer operands. An untyped integer constant receives the
  // surrounding `usize` context, while an already typed value must be exactly
  // `usize`; Draft does not silently convert i64, u64, or a distinct wrapper at
  // this boundary. Constant declarations and `when` conditions do not later
  // pass through the runtime body checker, so the evaluator must enforce the
  // same source-language rule itself.
  [[nodiscard]] bool usize_index_operand(TypeId type_id) const {
    return untyped_integer(type_id) ||
        type_id == semantic_.types.builtins().usize_type;
  }

  [[nodiscard]] bool numeric_operand(TypeId type_id) const {
    return untyped_integer(type_id) || untyped_float(type_id) ||
        concrete_numeric(type_id);
  }

  [[nodiscard]] bool common_numeric_operands(
      TypeId left, TypeId right) const {
    if (!numeric_operand(left) || !numeric_operand(right)) return false;
    const bool left_concrete = concrete_numeric(left);
    const bool right_concrete = concrete_numeric(right);
    if (left_concrete && right_concrete) return left == right;
    if (untyped_float(left) && right_concrete) {
      return runtime_type(right).kind == TypeKind::Float;
    }
    if (untyped_float(right) && left_concrete) {
      return runtime_type(left).kind == TypeKind::Float;
    }
    return true;
  }

  [[nodiscard]] bool equality_scalar(TypeId type_id) const {
    if (!type_id.is_valid()) return false;
    const TypeKind kind = runtime_type(type_id).kind;
    return kind == TypeKind::Bool || kind == TypeKind::BooleanStorage ||
        kind == TypeKind::EndianScalar || kind == TypeKind::Enum ||
        kind == TypeKind::Rune || kind == TypeKind::RawPointer ||
        kind == TypeKind::CString || kind == TypeKind::Pointer ||
        kind == TypeKind::MultiPointer || kind == TypeKind::Procedure;
  }

  [[nodiscard]] bool nil_context_type(TypeId type_id) const {
    if (!type_id.is_valid()) return false;
    const TypeKind kind = runtime_type(type_id).kind;
    return kind == TypeKind::RawPointer || kind == TypeKind::CString ||
        kind == TypeKind::Pointer || kind == TypeKind::MultiPointer ||
        kind == TypeKind::Procedure;
  }

  // `when` conditions and data constants do not subsequently pass through the
  // runtime body checker. Validate the same closed operator/type table here so
  // compile-time execution cannot acquire extra operators merely because two
  // values share an internal ConstantKind representation.
  [[nodiscard]] std::optional<EvalResult> invalid_binary_types(
      TokenKind operation,
      TypeId left,
      TypeId right,
      SourceRange range,
      bool required) {
    if (!left.is_valid() || !right.is_valid()) return std::nullopt;
    const TypeKind left_kind = semantic_.types.type(left).kind;
    const TypeKind right_kind = semantic_.types.type(right).kind;
    if (left_kind == TypeKind::Invalid || right_kind == TypeKind::Invalid) {
      return std::nullopt;
    }

    const bool equality = operation == TokenKind::EqualEqual ||
        operation == TokenKind::BangEqual;
    const bool ordering = operation == TokenKind::Less ||
        operation == TokenKind::LessEqual ||
        operation == TokenKind::Greater ||
        operation == TokenKind::GreaterEqual;
    const bool shift = operation == TokenKind::ShiftLeft ||
        operation == TokenKind::ShiftRight;
    const bool integer_only = operation == TokenKind::Percent ||
        operation == TokenKind::Ampersand || operation == TokenKind::Pipe ||
        operation == TokenKind::Caret;
    const bool arithmetic = operation == TokenKind::Plus ||
        operation == TokenKind::Minus || operation == TokenKind::Star ||
        operation == TokenKind::Slash;

    bool valid = false;
    if (shift) {
      // Shift counts intentionally need not share the left operand's concrete
      // integer type.
      valid = integer_operand(left) && integer_operand(right);
    } else if (integer_only) {
      valid = integer_operand(left) && integer_operand(right) &&
          common_numeric_operands(left, right);
    } else if (arithmetic) {
      valid = common_numeric_operands(left, right);
    } else if (equality || ordering) {
      valid = common_numeric_operands(left, right);
      if (!valid && left == right) {
        const TypeKind runtime_kind = runtime_type(left).kind;
        valid = (equality && equality_scalar(left)) ||
            (runtime_kind == TypeKind::Rune && ordering);
      }
    } else {
      // Logical operators are checked before the right operand is evaluated.
      return std::nullopt;
    }

    if (valid) return std::nullopt;
    return fail(
        range,
        "compile-time operator is not defined for operand types",
        required);
  }

  [[nodiscard]] std::optional<EvalResult> invalid_unary_type(
      TokenKind operation,
      TypeId operand,
      SourceRange range,
      bool required) {
    if (!operand.is_valid() ||
        semantic_.types.type(operand).kind == TypeKind::Invalid) {
      return std::nullopt;
    }
    bool valid = false;
    if (operation == TokenKind::Plus || operation == TokenKind::Minus) {
      valid = numeric_operand(operand);
    } else if (operation == TokenKind::Tilde) {
      valid = integer_operand(operand);
    } else if (operation == TokenKind::Bang) {
      valid = runtime_type(operand).kind == TypeKind::Bool;
    }
    if (valid) return std::nullopt;
    return fail(
        range,
        "compile-time unary operator is not defined for operand type",
        required);
  }

  [[nodiscard]] bool is_nil_literal(
      const SyntaxTree &tree, NodeId expression_id) const {
    const SyntaxNode &expression = tree.node(expression_id);
    return expression.kind == NodeKind::LiteralExpression &&
        expression.token_begin < expression.token_end &&
        tree.token(expression.token_begin).kind == TokenKind::KeywordNil;
  }

  [[nodiscard]] bool needs_value_context(
      const SyntaxTree &tree, NodeId expression_id) const {
    if (is_nil_literal(tree, expression_id)) return true;
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind == NodeKind::ContextualAlternativeExpression) {
      return true;
    }
    if ((expression.kind == NodeKind::GroupExpression ||
         expression.kind == NodeKind::DenyExpression) &&
        !expression.children.empty()) {
      return needs_value_context(tree, expression.children.back());
    }
    if (expression.kind == NodeKind::ConditionalExpression &&
        expression.children.size() == 3) {
      return needs_value_context(tree, expression.children[0]) &&
          needs_value_context(tree, expression.children[2]);
    }
    return false;
  }

  // Finds the declared type of a non-executed expression when that type is
  // enough to contextualize the selected branch of a constant conditional.
  // This deliberately recognizes only shapes whose type is available without
  // evaluating their value. In particular, it never enters a dead procedure
  // call or performs dead arithmetic merely to discover a type.
  [[nodiscard]] TypeId declared_value_type_hint(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind == NodeKind::NameExpression) {
      const std::optional<std::string> name = final_name(tree, expression);
      if (!name.has_value()) return {};
      if (const LocalBinding *local = local_binding(*name)) {
        return local->type;
      }
      const std::optional<SymbolId> found =
          semantic_.symbols.lookup(scope, *name);
      if (!found.has_value()) return {};
      const Symbol &symbol = semantic_.symbols.symbol(*found);
      TypeId type = substitute_local_type(symbol.type);
      if ((!type.is_valid() ||
           semantic_.types.type(type).kind == TypeKind::Invalid) &&
          (symbol.kind == SymbolKind::Constant ||
           symbol.kind == SymbolKind::UnresolvedDeclaration)) {
        // Constant lookup is side-effect free and guarded against cycles by
        // evaluate_binding. This permits forward constant names to expose an
        // already inferable type without executing an arbitrary dead branch.
        const EvalResult value = evaluate_binding(*found, false);
        if (value.status == EvalStatus::Ready) {
          type = substitute_local_type(value.type);
        }
      }
      return type.is_valid() &&
              semantic_.types.type(type).kind != TypeKind::Invalid
          ? type
          : TypeId{};
    }
    if (expression.kind == NodeKind::MemberExpression) {
      if (const std::optional<SymbolId> imported =
              imported_member(tree, expression, scope)) {
        return substitute_local_type(
            semantic_.symbols.symbol(*imported).type);
      }
      return {};
    }
    if ((expression.kind == NodeKind::GroupExpression ||
         expression.kind == NodeKind::DenyExpression) &&
        !expression.children.empty()) {
      return declared_value_type_hint(
          tree, expression.children.back(), scope);
    }
    if (expression.kind == NodeKind::ConditionalExpression &&
        expression.children.size() == 3) {
      const TypeId left = declared_value_type_hint(
          tree, expression.children.front(), scope);
      const TypeId right = declared_value_type_hint(
          tree, expression.children.back(), scope);
      if (left.is_valid() && right.is_valid() && left != right) return {};
      return left.is_valid() ? left : right;
    }
    if (expression.kind != NodeKind::CallExpression ||
        expression.children.empty()) {
      return {};
    }

    const SyntaxNode &callee = tree.node(expression.children.front());
    if (callee.kind == NodeKind::BracketExpression &&
        callee.children.size() == 2) {
      const SyntaxNode &base = tree.node(callee.children.front());
      const std::optional<std::string> name = final_name(tree, base);
      if (name.has_value() && *name == "cast") {
        return type_value(tree, callee.children.back(), scope).value_or(
            TypeId{});
      }
    }

    NodeId base_callee = expression.children.front();
    if (callee.kind == NodeKind::BracketExpression &&
        !callee.children.empty()) {
      base_callee = callee.children.front();
    }
    const SyntaxNode &base = tree.node(base_callee);
    std::optional<SymbolId> procedure;
    if (base.kind == NodeKind::NameExpression) {
      const std::optional<std::string> name = final_name(tree, base);
      if (name.has_value()) procedure = semantic_.symbols.lookup(scope, *name);
    } else if (base.kind == NodeKind::MemberExpression) {
      procedure = imported_member(tree, base, scope);
    }
    if (!procedure.has_value()) return {};
    const TypeId signature_id =
        semantic_.symbols.symbol(*procedure).type;
    if (!signature_id.is_valid() ||
        semantic_.types.type(signature_id).kind == TypeKind::Invalid) {
      return {};
    }
    const Type signature = semantic_.types.type(signature_id);
    if (signature.kind != TypeKind::Procedure || signature.members.empty()) {
      return {};
    }
    return substitute_local_type(signature.members.back());
  }

  [[nodiscard]] bool accepts_context_hint(
      const SyntaxTree &tree,
      NodeId contextual_expression,
      TypeId hinted_type) const {
    if (!hinted_type.is_valid() ||
        semantic_.types.type(hinted_type).kind == TypeKind::Invalid) {
      return false;
    }
    const SyntaxNode &expression = tree.node(contextual_expression);
    if ((expression.kind == NodeKind::GroupExpression ||
         expression.kind == NodeKind::DenyExpression) &&
        !expression.children.empty()) {
      return accepts_context_hint(
          tree, expression.children.back(), hinted_type);
    }
    if (expression.kind == NodeKind::ConditionalExpression &&
        expression.children.size() == 3) {
      return accepts_context_hint(tree, expression.children[0], hinted_type) &&
          accepts_context_hint(tree, expression.children[2], hinted_type);
    }
    if (is_nil_literal(tree, contextual_expression)) {
      return nil_context_type(hinted_type);
    }
    const TypeKind kind = runtime_type(hinted_type).kind;
    return kind == TypeKind::Enum || kind == TypeKind::TaggedUnion;
  }

  // Finds a concrete numeric type without evaluating the expression.  This is
  // intentionally a small syntactic query, not a second type checker.  Its job
  // is to discover context supplied by the opposite operand before source-order
  // evaluation begins.  For example, in `(large + 1.0) == F32_Zero`, the name
  // on the right makes every operation in the left operand an f32 operation.
  // Looking at its declared type is safe; evaluating the right operand early
  // would violate the language's observable evaluation order.
  [[nodiscard]] TypeId numeric_type_hint(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind == NodeKind::NameExpression) {
      const std::optional<std::string> name = final_name(tree, expression);
      if (!name.has_value()) return {};
      if (const LocalBinding *local = local_binding(*name)) {
        return concrete_numeric(local->type) ? local->type : TypeId{};
      }
      const std::optional<SymbolId> found =
          semantic_.symbols.lookup(scope, *name);
      if (!found.has_value()) return {};
      const Symbol &symbol = semantic_.symbols.symbol(*found);
      TypeId type = substitute_local_type(symbol.type);
      if (!concrete_numeric(type) &&
          (symbol.kind == SymbolKind::Constant ||
           symbol.kind == SymbolKind::UnresolvedDeclaration)) {
        // Asking for another declaration's value is safe here: constants have
        // no observable evaluation effects.  It also makes forward references
        // supply the same context as already-evaluated declarations.  Cycles
        // remain guarded by evaluate_binding's Evaluating state.
        const EvalResult value = evaluate_binding(*found, false);
        if (value.status == EvalStatus::Ready) {
          type = substitute_local_type(value.type);
        }
      }
      return concrete_numeric(type) ? type : TypeId{};
    }
    if (expression.kind == NodeKind::MemberExpression) {
      if (const std::optional<SymbolId> imported =
              imported_member(tree, expression, scope)) {
        const TypeId type = substitute_local_type(
            semantic_.symbols.symbol(*imported).type);
        return concrete_numeric(type) ? type : TypeId{};
      }
      return {};
    }
    if ((expression.kind == NodeKind::GroupExpression ||
         expression.kind == NodeKind::UnaryExpression ||
         expression.kind == NodeKind::DenyExpression) &&
        !expression.children.empty()) {
      return numeric_type_hint(
          tree, expression.children.back(), scope);
    }
    if (expression.kind == NodeKind::ConditionalExpression &&
        expression.children.size() == 3) {
      const TypeId left = numeric_type_hint(
          tree, expression.children.front(), scope);
      const TypeId right = numeric_type_hint(
          tree, expression.children.back(), scope);
      if (left.is_valid() && right.is_valid() && left != right) return {};
      return left.is_valid() ? left : right;
    }
    if (expression.kind == NodeKind::BinaryExpression &&
        expression.children.size() == 2) {
      const TokenKind operation = binary_operator(tree, expression);
      if (operation == TokenKind::EqualEqual ||
          operation == TokenKind::BangEqual || operation == TokenKind::Less ||
          operation == TokenKind::LessEqual ||
          operation == TokenKind::Greater ||
          operation == TokenKind::GreaterEqual ||
          operation == TokenKind::LogicalAnd ||
          operation == TokenKind::LogicalOr) {
        return {};
      }
      const TypeId left = numeric_type_hint(
          tree, expression.children.front(), scope);
      const TypeId right = numeric_type_hint(
          tree, expression.children.back(), scope);
      if (left.is_valid() && right.is_valid() && left != right) return {};
      return left.is_valid() ? left : right;
    }
    if (expression.kind != NodeKind::CallExpression ||
        expression.children.empty()) {
      return {};
    }

    const SyntaxNode &callee = tree.node(expression.children.front());
    if (callee.kind == NodeKind::BracketExpression &&
        callee.children.size() == 2) {
      const SyntaxNode &base = tree.node(callee.children.front());
      const std::optional<std::string> name = final_name(tree, base);
      if (name.has_value() && *name == "cast") {
        const std::optional<TypeId> target =
            type_value(tree, callee.children.back(), scope);
        return target.has_value() && concrete_numeric(*target)
            ? *target
            : TypeId{};
      }
    }

    NodeId procedure_name = expression.children.front();
    if (callee.kind == NodeKind::BracketExpression &&
        !callee.children.empty()) {
      procedure_name = callee.children.front();
    }
    const SyntaxNode &name_node = tree.node(procedure_name);
    if (name_node.kind != NodeKind::NameExpression) return {};
    const std::optional<std::string> name = final_name(tree, name_node);
    const std::optional<SymbolId> found = name.has_value()
        ? semantic_.symbols.lookup(scope, *name)
        : std::nullopt;
    if (!found.has_value()) return {};
    const Type procedure = semantic_.types.type(
        semantic_.symbols.symbol(*found).type);
    if (procedure.kind != TypeKind::Procedure || procedure.members.empty()) {
      return {};
    }
    const TypeId result = substitute_local_type(procedure.members.back());
    return concrete_numeric(result) ? result : TypeId{};
  }

  [[nodiscard]] std::optional<IeeeBinaryFormat> float_format(
      TypeId type_id) const {
    Type type = runtime_type(type_id);
    if (type.kind == TypeKind::EndianScalar && type.element.is_valid()) {
      type = runtime_type(type.element);
    }
    return type.kind == TypeKind::Float
        ? ieee_format_for_width(type.bit_width)
        : std::nullopt;
  }

  [[nodiscard]] std::optional<ConstantValue> float_from_bits(
      std::uint64_t bits, std::uint32_t bit_width) const {
    const std::optional<IeeeBinaryFormat> format =
        ieee_format_for_width(bit_width);
    if (!format.has_value()) return std::nullopt;
    const std::optional<DecodedIeeeValue> decoded =
        decode_ieee_bits(bits, *format);
    if (!decoded.has_value()) return std::nullopt;
    return ConstantValue::make_ieee_float(
        bit_width,
        bits,
        decoded->kind == IeeeValueKind::Finite
            ? decoded->finite
            : ExactRational{});
  }

  [[nodiscard]] EvalResult convert_float_to_type(
      const ConstantValue &value,
      TypeId type_id,
      SourceRange range,
      bool required) {
    const std::optional<IeeeBinaryFormat> target_format =
        float_format(type_id);
    if (!target_format.has_value()) {
      return fail(range, "floating type has no supported IEEE format", required);
    }
    const std::uint32_t target_width =
        1U + target_format->exponent_bits + target_format->fraction_bits;

    std::uint64_t bits = 0;
    if (value.kind == ConstantKind::Integer) {
      const std::optional<std::uint64_t> rounded = round_ieee_bits(
          ExactRational(value.integer), *target_format);
      if (!rounded.has_value()) {
        return fail(range, "compile-time float conversion exceeded resources", required);
      }
      bits = *rounded;
    } else if (value.kind == ConstantKind::Float &&
               value.float_bit_width == 0) {
      const std::optional<std::uint64_t> rounded =
          round_ieee_bits(value.floating, *target_format);
      if (!rounded.has_value()) {
        return fail(range, "compile-time float conversion exceeded resources", required);
      }
      bits = *rounded;
    } else if (value.kind == ConstantKind::Float) {
      const std::optional<IeeeBinaryFormat> source_format =
          ieee_format_for_width(value.float_bit_width);
      const std::optional<DecodedIeeeValue> source = source_format.has_value()
          ? decode_ieee_bits(value.float_bits, *source_format)
          : std::nullopt;
      if (!source.has_value()) {
        return fail(range, "compile-time float has an invalid encoding", required);
      }
      if (source->kind == IeeeValueKind::NaN) {
        bits = ieee_nan_bits(*target_format);
      } else if (source->kind == IeeeValueKind::Infinity) {
        bits = ieee_infinity_bits(*target_format, source->negative);
      } else if (source->finite.is_zero() && source->negative) {
        bits = ieee_zero_bits(*target_format, true);
      } else {
        const std::optional<std::uint64_t> rounded =
            round_ieee_bits(source->finite, *target_format);
        if (!rounded.has_value()) {
          return fail(
              range, "compile-time float conversion exceeded resources", required);
        }
        bits = *rounded;
      }
    } else {
      return fail(
          range, "compile-time value is not numeric", required);
    }

    const std::optional<ConstantValue> result =
        float_from_bits(bits, target_width);
    if (!result.has_value()) {
      return fail(range, "compile-time float encoding failed", required);
    }
    return ready(*result, type_id);
  }

  [[nodiscard]] std::uint32_t integer_width(TypeId type_id) const {
    const Type type = runtime_type(type_id);
    if (type.kind == TypeKind::Enum) {
      return static_cast<std::uint32_t>(type.layout.size * 8U);
    }
    return type.bit_width;
  }

  [[nodiscard]] bool integer_representable(
      const BigInteger &value, TypeId type_id) const {
    Type type = runtime_type(type_id);
    if (type.kind == TypeKind::EndianScalar && type.element.is_valid()) {
      type = runtime_type(type.element);
    }
    const std::uint32_t width = integer_width(type_id);
    if (width == 0) return false;
    if (type.kind == TypeKind::UnsignedInteger ||
        type.kind == TypeKind::BooleanStorage) {
      return !value.is_negative() && value.bit_count() <= width;
    }
    if (type.kind != TypeKind::SignedInteger && type.kind != TypeKind::Rune &&
        type.kind != TypeKind::Enum) {
      return false;
    }
    const BigInteger sign =
        BigInteger::from_u64(1).shifted_left(width - 1U);
    return value.compare(sign.negated()) >= 0 &&
        value.compare(sign.subtracted(BigInteger::from_u64(1))) <= 0;
  }

  [[nodiscard]] BigInteger wrapped_integer(
      const BigInteger &value, TypeId type_id) const {
    const std::uint32_t width = integer_width(type_id);
    if (width == 0) return value;
    const BigInteger modulus =
        BigInteger::from_u64(1).shifted_left(width);
    BigInteger quotient;
    BigInteger remainder;
    if (!value.divide(modulus, quotient, remainder)) return value;
    if (remainder.is_negative()) remainder = remainder.added(modulus);
    Type type = runtime_type(type_id);
    if (type.kind == TypeKind::EndianScalar && type.element.is_valid()) {
      type = runtime_type(type.element);
    }
    const bool signed_value = type.kind == TypeKind::SignedInteger ||
        type.kind == TypeKind::Rune || type.kind == TypeKind::Enum;
    if (signed_value) {
      const BigInteger sign =
          BigInteger::from_u64(1).shifted_left(width - 1U);
      if (remainder.compare(sign) >= 0) {
        remainder = remainder.subtracted(modulus);
      }
    }
    return remainder;
  }

  [[nodiscard]] bool valid_rune(const BigInteger &value) const {
    const BigInteger zero = BigInteger::from_u64(0);
    const BigInteger surrogate_begin = BigInteger::from_u64(0xd800);
    const BigInteger surrogate_end = BigInteger::from_u64(0xdfff);
    const BigInteger maximum = BigInteger::from_u64(0x10ffff);
    return value.compare(zero) >= 0 && value.compare(maximum) <= 0 &&
        (value.compare(surrogate_begin) < 0 ||
         value.compare(surrogate_end) > 0);
  }

  [[nodiscard]] bool valid_enum_value(
      TypeId type_id, const BigInteger &value) const {
    const std::optional<SymbolId> owner = aggregate_owner(type_id);
    if (!owner.has_value()) return false;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner != *owner) continue;
      for (const EnumMemberValue &enumerator : semantic_.enum_member_values) {
        if (enumerator.member == member.member &&
            enumerator.value.compare(value) == 0) {
          return true;
        }
      }
    }
    return false;
  }

  [[nodiscard]] EvalResult convert_to_type(
      ConstantValue value,
      TypeId type_id,
      bool wrap_integer,
      SourceRange range,
      bool required) {
    if (!type_id.is_valid()) return ready(std::move(value));
    const Type type = runtime_type(type_id);
    const TypeKind endian_value_kind =
        type.kind == TypeKind::EndianScalar && type.element.is_valid()
        ? runtime_type(type.element).kind
        : TypeKind::Invalid;
    if (value.kind == ConstantKind::Integer &&
        (type.kind == TypeKind::SignedInteger ||
         type.kind == TypeKind::UnsignedInteger || type.kind == TypeKind::Rune ||
         type.kind == TypeKind::BooleanStorage ||
         endian_value_kind == TypeKind::SignedInteger ||
         endian_value_kind == TypeKind::UnsignedInteger ||
         type.kind == TypeKind::Enum)) {
      if (!wrap_integer && !integer_representable(value.integer, type_id)) {
        return fail(
            range,
            "compile-time integer is not representable in its required type",
            required);
      }
      if (wrap_integer) value.integer = wrapped_integer(value.integer, type_id);
      return ready(std::move(value), type_id);
    }
    if (value.kind == ConstantKind::Integer &&
        (type.kind == TypeKind::Float ||
         endian_value_kind == TypeKind::Float)) {
      return convert_float_to_type(value, type_id, range, required);
    }
    if (value.kind == ConstantKind::Float &&
        (type.kind == TypeKind::Float ||
         endian_value_kind == TypeKind::Float)) {
      return convert_float_to_type(value, type_id, range, required);
    }
    if (value.kind == ConstantKind::Bool && type.kind == TypeKind::Bool) {
      return ready(std::move(value), type_id);
    }
    if (value.kind == ConstantKind::String && type.kind == TypeKind::String) {
      return ready(std::move(value), type_id);
    }
    if (value.kind == ConstantKind::Nil &&
        (type.kind == TypeKind::RawPointer || type.kind == TypeKind::CString ||
         type.kind == TypeKind::Pointer || type.kind == TypeKind::MultiPointer ||
         type.kind == TypeKind::Procedure)) {
      return ready(std::move(value), type_id);
    }
    if (value.kind == ConstantKind::Procedure &&
        type.kind == TypeKind::Procedure) {
      return ready(std::move(value), type_id);
    }
    if (value.kind == ConstantKind::Aggregate &&
        (type.kind == TypeKind::Array || type.kind == TypeKind::Tuple ||
         type.kind == TypeKind::Struct || type.kind == TypeKind::Simd)) {
      const std::size_t expected_count =
          type.kind == TypeKind::Array || type.kind == TypeKind::Simd
          ? static_cast<std::size_t>(type.element_count)
          : type.members.size();
      if (value.elements.size() != expected_count) {
        return fail(
            range,
            "compile-time aggregate has the wrong element count",
            required);
      }
      for (std::size_t index = 0; index < value.elements.size(); ++index) {
        const TypeId element_type =
            type.kind == TypeKind::Array || type.kind == TypeKind::Simd
            ? type.element
            : type.members[index];
        const EvalResult converted = convert_to_type(
            value.elements[index], element_type, false, range, required);
        if (converted.status != EvalStatus::Ready) return converted;
        value.elements[index] = converted.value;
      }
      return ready(std::move(value), type_id);
    }
    if (value.kind == ConstantKind::Aggregate &&
        (type.kind == TypeKind::RawUnion ||
         type.kind == TypeKind::TaggedUnion)) {
      if (value.variant_index >= type.members.size()) {
        return fail(
            range,
            "compile-time union has an invalid selected member",
            required);
      }
      const TypeId payload_type = type.members[value.variant_index];
      const bool has_payload =
          semantic_.types.type(payload_type).kind != TypeKind::Void;
      // A zero-initialized raw union means all storage bytes are zero.  It does
      // not semantically initialize the first member, which may itself be a
      // non-void type, so it carries no payload value.
      if (type.kind == TypeKind::RawUnion && value.elements.empty()) {
        return ready(std::move(value), type_id);
      }
      if (has_payload != (value.elements.size() == 1)) {
        return fail(
            range,
            "compile-time union payload does not match its selected member",
            required);
      }
      if (has_payload) {
        const EvalResult converted = convert_to_type(
            value.elements.front(), payload_type, false, range, required);
        if (converted.status != EvalStatus::Ready) return converted;
        value.elements.front() = converted.value;
      }
      return ready(std::move(value), type_id);
    }
    if (value.kind == ConstantKind::EnumLabel && type.kind == TypeKind::Enum) {
      const std::optional<BigInteger> member =
          enum_member_value(type_id, value.text);
      if (!member.has_value() || !value.elements.empty()) {
        return fail(
            range,
            "compile-time enum initializer names no matching member",
            required);
      }
      return ready(ConstantValue::make_integer(*member), type_id);
    }
    if (value.kind == ConstantKind::EnumLabel &&
        type.kind == TypeKind::TaggedUnion) {
      const std::optional<std::size_t> member =
          aggregate_member_index(type_id, value.text);
      if (!member.has_value() || *member >= type.members.size()) {
        return fail(
            range,
            "compile-time union initializer names no matching alternative",
            required);
      }
      const TypeId payload_type = type.members[*member];
      const bool has_payload =
          semantic_.types.type(payload_type).kind != TypeKind::Void;
      if (has_payload != (value.elements.size() == 1)) {
        return fail(
            range,
            has_payload
                ? "compile-time union alternative requires a payload"
                : "compile-time union alternative does not accept a payload",
            required);
      }
      std::vector<ConstantValue> payload;
      if (has_payload) {
        const EvalResult converted = convert_to_type(
            value.elements.front(), payload_type, false, range, required);
        if (converted.status != EvalStatus::Ready) return converted;
        payload.push_back(converted.value);
      }
      return ready(
          ConstantValue::make_aggregate(std::move(payload), *member), type_id);
    }
    return fail(
        range,
        "compile-time value is incompatible with its required type",
        required);
  }

  [[nodiscard]] TypeId default_value_type(const ConstantValue &value) const {
    switch (value.kind) {
    case ConstantKind::Bool: return semantic_.types.builtins().bool_type;
    case ConstantKind::Integer: return semantic_.types.builtins().int_type;
    case ConstantKind::Float:
      return semantic_.types.find_builtin("f64").value_or(
          semantic_.types.builtins().invalid);
    case ConstantKind::String: return semantic_.types.builtins().string_type;
    case ConstantKind::Procedure: return {};
    default: return {};
    }
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

  [[nodiscard]] EvalResult evaluate_typed_float_binary(
      TokenKind operation,
      const ConstantValue &left_value,
      const ConstantValue &right_value,
      TypeId result_type,
      SourceRange range,
      bool required) {
    const std::optional<IeeeBinaryFormat> format = float_format(result_type);
    if (!format.has_value()) {
      return fail(range, "typed float has no supported IEEE format", required);
    }
    const std::uint32_t bit_width =
        1U + format->exponent_bits + format->fraction_bits;
    const EvalResult converted_left = convert_float_to_type(
        left_value, result_type, range, required);
    if (converted_left.status != EvalStatus::Ready) return converted_left;
    const EvalResult converted_right = convert_float_to_type(
        right_value, result_type, range, required);
    if (converted_right.status != EvalStatus::Ready) return converted_right;
    const std::optional<DecodedIeeeValue> left = decode_ieee_bits(
        converted_left.value.float_bits, *format);
    const std::optional<DecodedIeeeValue> decoded_right = decode_ieee_bits(
        converted_right.value.float_bits, *format);
    if (!left.has_value() || !decoded_right.has_value()) {
      return fail(range, "typed float has an invalid encoding", required);
    }

    const bool comparison = operation == TokenKind::EqualEqual ||
        operation == TokenKind::BangEqual || operation == TokenKind::Less ||
        operation == TokenKind::LessEqual || operation == TokenKind::Greater ||
        operation == TokenKind::GreaterEqual;
    if (comparison) {
      if (left->kind == IeeeValueKind::NaN ||
          decoded_right->kind == IeeeValueKind::NaN) {
        return ready(
            ConstantValue::make_bool(operation == TokenKind::BangEqual),
            semantic_.types.builtins().bool_type);
      }
      int order = 0;
      if (left->kind == IeeeValueKind::Infinity &&
          decoded_right->kind == IeeeValueKind::Infinity) {
        order = left->negative == decoded_right->negative
            ? 0
            : (left->negative ? -1 : 1);
      } else if (left->kind == IeeeValueKind::Infinity) {
        order = left->negative ? -1 : 1;
      } else if (decoded_right->kind == IeeeValueKind::Infinity) {
        order = decoded_right->negative ? 1 : -1;
      } else {
        order = left->finite.compare(decoded_right->finite);
      }
      bool result = false;
      if (operation == TokenKind::EqualEqual) result = order == 0;
      if (operation == TokenKind::BangEqual) result = order != 0;
      if (operation == TokenKind::Less) result = order < 0;
      if (operation == TokenKind::LessEqual) result = order <= 0;
      if (operation == TokenKind::Greater) result = order > 0;
      if (operation == TokenKind::GreaterEqual) result = order >= 0;
      return ready(
          ConstantValue::make_bool(result),
          semantic_.types.builtins().bool_type);
    }

    DecodedIeeeValue right = *decoded_right;
    if (operation == TokenKind::Minus) {
      right.negative = !right.negative;
      if (right.kind == IeeeValueKind::Finite) {
        right.finite = right.finite.negated();
      }
    }
    const auto special = [&](std::uint64_t bits) -> EvalResult {
      const std::optional<ConstantValue> value =
          float_from_bits(bits, bit_width);
      return value.has_value()
          ? ready(*value, result_type)
          : fail(range, "typed float encoding failed", required);
    };
    const auto finite = [&](ExactRational value, bool negative_zero) -> EvalResult {
      if (value.numerator().bit_count() > kMaximumConstantBits ||
          value.denominator().bit_count() > kMaximumConstantBits) {
        return fail(
            range, "compile-time rational resource limit exceeded", required);
      }
      const std::optional<std::uint64_t> bits =
          round_ieee_bits(value, *format);
      if (!bits.has_value()) {
        return fail(
            range, "compile-time float rounding exceeded resources", required);
      }
      return special(value.is_zero() && negative_zero
          ? ieee_zero_bits(*format, true)
          : *bits);
    };
    if (left->kind == IeeeValueKind::NaN || right.kind == IeeeValueKind::NaN) {
      return special(ieee_nan_bits(*format));
    }

    if (operation == TokenKind::Plus || operation == TokenKind::Minus) {
      if (left->kind == IeeeValueKind::Infinity &&
          right.kind == IeeeValueKind::Infinity &&
          left->negative != right.negative) {
        return special(ieee_nan_bits(*format));
      }
      if (left->kind == IeeeValueKind::Infinity) {
        return special(ieee_infinity_bits(*format, left->negative));
      }
      if (right.kind == IeeeValueKind::Infinity) {
        return special(ieee_infinity_bits(*format, right.negative));
      }
      const ExactRational result = left->finite.added(right.finite);
      const bool negative_zero = left->finite.is_zero() &&
          right.finite.is_zero() && left->negative && right.negative;
      return finite(result, negative_zero);
    }

    const bool negative = left->negative != right.negative;
    const bool left_zero = left->kind == IeeeValueKind::Finite &&
        left->finite.is_zero();
    const bool right_zero = right.kind == IeeeValueKind::Finite &&
        right.finite.is_zero();
    if (operation == TokenKind::Star) {
      if ((left->kind == IeeeValueKind::Infinity && right_zero) ||
          (right.kind == IeeeValueKind::Infinity && left_zero)) {
        return special(ieee_nan_bits(*format));
      }
      if (left->kind == IeeeValueKind::Infinity ||
          right.kind == IeeeValueKind::Infinity) {
        return special(ieee_infinity_bits(*format, negative));
      }
      return finite(left->finite.multiplied(right.finite), negative);
    }
    if (operation == TokenKind::Slash) {
      if ((left->kind == IeeeValueKind::Infinity &&
           right.kind == IeeeValueKind::Infinity) ||
          (left_zero && right_zero)) {
        return special(ieee_nan_bits(*format));
      }
      if (left->kind == IeeeValueKind::Infinity) {
        return special(ieee_infinity_bits(*format, negative));
      }
      if (right.kind == IeeeValueKind::Infinity) {
        return special(ieee_zero_bits(*format, negative));
      }
      if (right_zero) {
        return special(ieee_infinity_bits(*format, negative));
      }
      ExactRational quotient;
      if (!left->finite.divide(right.finite, quotient)) {
        return fail(range, "typed float division failed", required);
      }
      return finite(std::move(quotient), negative);
    }
    return fail(range, "operator is not valid for typed floats", required);
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
    } else if (left.kind == ConstantKind::String &&
               right.kind == ConstantKind::String) {
      equal = left.text == right.text;
    } else if (left.kind == ConstantKind::EnumLabel &&
               right.kind == ConstantKind::EnumLabel) {
      equal = left.text == right.text;
    } else if (left.kind == ConstantKind::Procedure &&
               right.kind == ConstantKind::Procedure) {
      if (!left.root_identity.empty() || !right.root_identity.empty()) {
        equal = left.root_identity == right.root_identity &&
            left.root_relative_path == right.root_relative_path &&
            left.text == right.text;
      } else {
        equal = left.symbol_index == right.symbol_index;
      }
    } else if ((left.kind == ConstantKind::Procedure &&
                right.kind == ConstantKind::Nil) ||
               (left.kind == ConstantKind::Nil &&
                right.kind == ConstantKind::Procedure)) {
      equal = false;
    } else if (left.kind == ConstantKind::Nil &&
               right.kind == ConstantKind::Nil) {
      equal = true;
    } else {
      return fail(range, "comparison uses incompatible compile-time values", required);
    }
    if (operation == TokenKind::BangEqual) equal = !equal;
    return ready(ConstantValue::make_bool(equal));
  }

  [[nodiscard]] EvalResult evaluate_binary_values(
      TokenKind operation,
      const ConstantValue &left,
      const ConstantValue &right,
      SourceRange range,
      bool required) {
    if (left.kind == ConstantKind::Integer &&
        right.kind == ConstantKind::Integer) {
      return evaluate_integer_binary(
          operation, left.integer, right.integer, range, required);
    }
    if ((left.kind == ConstantKind::Integer || left.kind == ConstantKind::Float) &&
        (right.kind == ConstantKind::Integer || right.kind == ConstantKind::Float)) {
      const ExactRational left_float = left.kind == ConstantKind::Float
          ? left.floating
          : ExactRational(left.integer);
      const ExactRational right_float = right.kind == ConstantKind::Float
          ? right.floating
          : ExactRational(right.integer);
      return evaluate_float_binary(
          operation, left_float, right_float, range, required);
    }
    return evaluate_scalar_equality(operation, left, right, range, required);
  }

  // Evaluates a binary node with source-defined short circuiting. Right operands
  // of `&&` and `||` are not requested when the left value decides the result.
  [[nodiscard]] EvalResult evaluate_binary(
      const SyntaxTree &tree,
      const SyntaxNode &node,
      ScopeId scope,
      bool required,
      TypeId expected) {
    if (node.children.size() != 2) return pending();
    const TokenKind operation = binary_operator(tree, node);
    const TypeId left_hint = numeric_type_hint(
        tree, node.children.front(), scope);
    const TypeId right_hint = numeric_type_hint(
        tree, node.children.back(), scope);
    TypeId numeric_context = concrete_numeric(expected) ? expected : TypeId{};
    if (!numeric_context.is_valid() &&
        (!left_hint.is_valid() || !right_hint.is_valid() ||
         left_hint == right_hint)) {
      numeric_context = left_hint.is_valid() ? left_hint : right_hint;
    }
    EvalResult left = evaluate_expression(
        tree, node.children[0], scope, required, numeric_context);
    if (left.status != EvalStatus::Ready) return left;
    if (operation == TokenKind::LogicalAnd || operation == TokenKind::LogicalOr) {
      if (left.value.kind != ConstantKind::Bool || !left.type.is_valid() ||
          runtime_type(left.type).kind != TypeKind::Bool) {
        return fail(node.range, "logical operator requires bool operands", required);
      }
      if (operation == TokenKind::LogicalAnd && !left.value.boolean) {
        return ready(
            ConstantValue::make_bool(false),
            left.type);
      }
      if (operation == TokenKind::LogicalOr && left.value.boolean) {
        return ready(
            ConstantValue::make_bool(true),
            left.type);
      }
      const EvalResult right =
          evaluate_expression(tree, node.children[1], scope, required);
      if (right.status != EvalStatus::Ready) return right;
      if (right.value.kind != ConstantKind::Bool || right.type != left.type) {
        return fail(node.range, "logical operator requires bool operands", required);
      }
      return ready(right.value, left.type);
    }

    TypeId right_context = numeric_context;
    if (!right_context.is_valid() && concrete_numeric(left.type) &&
        !right_hint.is_valid()) {
      right_context = left.type;
    }
    EvalResult right = evaluate_expression(
        tree,
        node.children[1],
        scope,
        required,
        operation == TokenKind::ShiftLeft ||
                operation == TokenKind::ShiftRight
            ? TypeId{}
            : right_context);
    if (right.status != EvalStatus::Ready) return right;
    // Nil has no standalone type. Exactly one typed pointer/procedure operand
    // can supply its context; two bare nil literals cannot invent one.
    if (left.value.kind == ConstantKind::Nil && !left.type.is_valid() &&
        nil_context_type(right.type)) {
      left.type = right.type;
    }
    if (right.value.kind == ConstantKind::Nil && !right.type.is_valid() &&
        nil_context_type(left.type)) {
      right.type = left.type;
    }
    if ((left.value.kind == ConstantKind::Nil && !left.type.is_valid()) ||
        (right.value.kind == ConstantKind::Nil && !right.type.is_valid())) {
      return fail(
          node.range,
          "compile-time nil comparison requires a pointer or procedure type",
          required);
    }
    if (std::optional<EvalResult> invalid = invalid_binary_types(
            operation, left.type, right.type, node.range, required)) {
      return std::move(*invalid);
    }
    const bool comparison = operation == TokenKind::EqualEqual ||
        operation == TokenKind::BangEqual || operation == TokenKind::Less ||
        operation == TokenKind::LessEqual || operation == TokenKind::Greater ||
        operation == TokenKind::GreaterEqual;
    const TypeId result_type = numeric_context.is_valid()
        ? numeric_context
        : (concrete_numeric(left.type)
               ? left.type
               : (concrete_numeric(right.type) ? right.type : TypeId{}));

    if (result_type.is_valid() &&
        runtime_type(result_type).kind == TypeKind::Float &&
        (left.value.kind == ConstantKind::Integer ||
         left.value.kind == ConstantKind::Float) &&
        (right.value.kind == ConstantKind::Integer ||
         right.value.kind == ConstantKind::Float)) {
      return evaluate_typed_float_binary(
          operation,
          left.value,
          right.value,
          result_type,
          node.range,
          required);
    }

    if (result_type.is_valid() &&
        (operation == TokenKind::ShiftLeft ||
         operation == TokenKind::ShiftRight) &&
        right.value.kind == ConstantKind::Integer) {
      const std::optional<std::uint64_t> count = right.value.integer.to_u64();
      if (!count.has_value() || *count >= integer_width(result_type)) {
        return fail(node.range, "typed compile-time shift count traps", required);
      }
    }
    if (result_type.is_valid() && operation == TokenKind::Slash &&
        left.value.kind == ConstantKind::Integer &&
        right.value.kind == ConstantKind::Integer &&
        runtime_type(result_type).kind == TypeKind::SignedInteger) {
      const BigInteger minimum = BigInteger::from_u64(1)
          .shifted_left(integer_width(result_type) - 1U)
          .negated();
      if (left.value.integer == minimum &&
          right.value.integer == BigInteger::from_i64(-1)) {
        return fail(
            node.range,
            "typed compile-time signed division overflow traps",
            required);
      }
    }

    EvalResult result = evaluate_binary_values(
        operation, left.value, right.value, node.range, required);
    if (result.status != EvalStatus::Ready) return result;
    if (comparison) {
      result.type = semantic_.types.builtins().bool_type;
      return result;
    }
    if (result_type.is_valid()) {
      return convert_to_type(
          result.value, result_type, true, node.range, required);
    }
    return result;
  }

  // Maps the built-in target object's stable fields to scalar compile-time
  // values. Unknown fields are diagnosed only when the expression is required.
  [[nodiscard]] EvalResult evaluate_target_member(
      std::string_view member, SourceRange range, bool required) {
    if (member == "identity") {
      return ready(
          ConstantValue::make_string(target_.identity),
          semantic_.types.builtins().string_type);
    }
    if (member == "arch") return ready(ConstantValue::make_enum_label(target_.arch));
    if (member == "os") return ready(ConstantValue::make_enum_label(target_.os));
    if (member == "abi") return ready(ConstantValue::make_enum_label(target_.abi));
    if (member == "byte_order") {
      return ready(ConstantValue::make_enum_label(target_.byte_order));
    }
    if (member == "object_format") {
      return ready(ConstantValue::make_enum_label(target_.object_format));
    }
    if (member == "file_tag") {
      return ready(
          ConstantValue::make_string(target_.file_tag),
          semantic_.types.builtins().string_type);
    }
    if (member == "pointer_bits" || member == "page_size") {
      const std::uint64_t source = member == "pointer_bits"
          ? target_.pointer_bits
          : target_.page_size;
      return ready(
          ConstantValue::make_integer(BigInteger::from_u64(source)),
          semantic_.types.builtins().usize_type);
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
    return ready(
        ConstantValue::make_bool(enabled),
        semantic_.types.builtins().bool_type);
  }

  [[nodiscard]] std::optional<TypeId> type_value(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) {
    const SyntaxNode &expression = tree.node(expression_id);
    if (is_type_syntax(expression.kind)) {
      if (expression.kind == NodeKind::NamedType) {
        return named_type_from_syntax(tree, expression_id, scope);
      }
      if (expression.kind == NodeKind::PointerType &&
          !expression.children.empty()) {
        const std::optional<TypeId> element =
            type_value(tree, expression.children.back(), scope);
        return element.has_value()
            ? std::optional<TypeId>(semantic_.types.pointer(*element))
            : std::nullopt;
      }
      if (expression.kind == NodeKind::MultiPointerType &&
          !expression.children.empty()) {
        const std::optional<TypeId> element =
            type_value(tree, expression.children.back(), scope);
        return element.has_value()
            ? std::optional<TypeId>(semantic_.types.multi_pointer(*element))
            : std::nullopt;
      }
      if (expression.kind == NodeKind::SliceType &&
          !expression.children.empty()) {
        const std::optional<TypeId> element =
            type_value(tree, expression.children.back(), scope);
        return element.has_value()
            ? std::optional<TypeId>(semantic_.types.slice(*element))
            : std::nullopt;
      }
      if (expression.kind == NodeKind::ArrayType &&
          expression.children.size() == 2) {
        const EvalResult count = evaluate_expression(
            tree, expression.children.front(), scope, true);
        const std::optional<TypeId> element =
            type_value(tree, expression.children.back(), scope);
        if (count.status != EvalStatus::Ready ||
            count.value.kind != ConstantKind::Integer || !element.has_value()) {
          return std::nullopt;
        }
        const std::optional<std::uint64_t> length = count.value.integer.to_u64();
        return length.has_value()
            ? std::optional<TypeId>(semantic_.types.array(*element, *length))
            : std::nullopt;
      }
      if (expression.kind == NodeKind::TupleType) {
        std::vector<TypeId> members;
        for (NodeId child : expression.children) {
          const std::optional<TypeId> member = type_value(tree, child, scope);
          if (!member.has_value()) return std::nullopt;
          members.push_back(*member);
        }
        return semantic_.types.tuple(members);
      }
      if (expression.kind == NodeKind::DistinctType &&
          !expression.children.empty()) {
        return type_value(tree, expression.children.back(), scope);
      }
      return std::nullopt;
    }
    if (const std::optional<SymbolId> imported =
            imported_member(tree, expression, scope)) {
      const Symbol &symbol = semantic_.symbols.symbol(*imported);
      if (symbol.kind == SymbolKind::Type ||
          symbol.kind == SymbolKind::TypeParameter) {
        return substitute_local_type(symbol.type);
      }
      return std::nullopt;
    }
    if (expression.kind != NodeKind::NameExpression) return std::nullopt;
    const std::optional<std::string> name = final_name(tree, expression);
    if (!name.has_value()) return std::nullopt;
    if (const std::optional<TypeId> builtin =
            semantic_.types.find_builtin(*name)) {
      return *builtin;
    }
    const std::optional<SymbolId> found =
        semantic_.symbols.lookup(scope, *name);
    if (!found.has_value()) return std::nullopt;
    const Symbol &symbol = semantic_.symbols.symbol(*found);
    if (symbol.kind != SymbolKind::Type &&
        symbol.kind != SymbolKind::TypeParameter) {
      return std::nullopt;
    }
    return substitute_local_type(symbol.type);
  }

  [[nodiscard]] TypeId substitute_type_bindings(
      TypeId source,
      const std::vector<ConstantTypeBinding> &bindings) const {
    for (const ConstantTypeBinding &binding : bindings) {
      if (binding.parameter == source) return binding.replacement;
    }
    if (local_types_ == nullptr) return source;
    for (const ConstantTypeBinding &binding : *local_types_) {
      if (binding.parameter == source) return binding.replacement;
    }
    return source;
  }

  [[nodiscard]] TypeId substitute_local_type(TypeId source) const {
    if (!local_frames_.empty()) {
      return substitute_type_bindings(
          source, local_frames_.back().type_bindings);
    }
    if (local_types_ != nullptr) {
      for (const ConstantTypeBinding &binding : *local_types_) {
        if (binding.parameter == source) return binding.replacement;
      }
    }
    return source;
  }

  [[nodiscard]] EvalResult evaluate_layout_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      bool required) {
    if (call.children.size() != 2) return pending();
    const SyntaxNode &callee = tree.node(call.children.front());
    if (callee.kind != NodeKind::NameExpression) return pending();
    const std::optional<std::string> name = final_name(tree, callee);
    if (!name.has_value() || (*name != "size_of" && *name != "align_of")) {
      return pending();
    }
    const std::optional<TypeId> queried =
        type_value(tree, call.children[1], scope);
    if (!queried.has_value()) {
      return fail(
          tree.node(call.children[1]).range,
          *name + " requires one type argument",
          required);
    }
    const TypeLayout layout = semantic_.types.type(*queried).layout;
    if (!layout.known) {
      return fail(
          tree.node(call.children[1]).range,
          *name + " requires a type with complete layout",
          required);
    }
    const std::uint64_t value = *name == "size_of"
        ? layout.size
        : layout.alignment;
    // Layout queries are concrete usize values, not merely integer-shaped
    // constants. Retaining that type is required when the query itself feeds a
    // second layout boundary such as `[size_of(Header)]u8`.
    return ready(
        ConstantValue::make_integer(BigInteger::from_u64(value)),
        semantic_.types.builtins().usize_type);
  }

  [[nodiscard]] bool is_type_syntax(NodeKind kind) const {
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

  [[nodiscard]] std::optional<TypeId> named_type_from_syntax(
      const SyntaxTree &tree, NodeId type_node, ScopeId scope) const {
    const SyntaxNode &node = tree.node(type_node);
    if (node.kind == NodeKind::DistinctType && !node.children.empty()) {
      return named_type_from_syntax(tree, node.children.back(), scope);
    }
    if (node.kind != NodeKind::NamedType) return std::nullopt;
    const std::optional<std::string> name = final_name(tree, node);
    if (!name.has_value()) return std::nullopt;
    if (const std::optional<TypeId> builtin = semantic_.types.find_builtin(*name)) {
      return *builtin;
    }
    const std::optional<SymbolId> symbol = semantic_.symbols.lookup(scope, *name);
    if (!symbol.has_value()) return std::nullopt;
    const Symbol &found = semantic_.symbols.symbol(*symbol);
    if (found.kind != SymbolKind::Type && found.kind != SymbolKind::TypeParameter) {
      return std::nullopt;
    }
    return substitute_local_type(found.type);
  }

  [[nodiscard]] std::optional<SymbolId> aggregate_owner(TypeId type_id) const {
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (semantic_.symbols.scope(owned.scope).kind == ScopeKind::Type &&
          semantic_.symbols.symbol(owned.owner).type == type_id) {
        return owned.owner;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> aggregate_member_index(
      TypeId type_id, std::string_view name) const {
    const std::optional<SymbolId> owner = aggregate_owner(type_id);
    if (!owner.has_value()) return std::nullopt;
    std::size_t index = 0;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner != *owner) continue;
      if (semantic_.symbols.symbol(member.member).name == name) return index;
      ++index;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<BigInteger> enum_member_value(
      TypeId type_id, std::string_view name) const {
    const std::optional<SymbolId> owner = aggregate_owner(type_id);
    if (!owner.has_value()) return std::nullopt;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner != *owner ||
          semantic_.symbols.symbol(member.member).name != name) {
        continue;
      }
      for (const EnumMemberValue &value : semantic_.enum_member_values) {
        if (value.member == member.member) return value.value;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ConstantValue> zero_value(TypeId type_id) const {
    Type type = semantic_.types.type(type_id);
    while (type.kind == TypeKind::Distinct) {
      type = semantic_.types.type(type.element);
    }
    switch (type.kind) {
    case TypeKind::Bool:
      return ConstantValue::make_bool(false);
    case TypeKind::BooleanStorage:
    case TypeKind::SignedInteger:
    case TypeKind::UnsignedInteger:
    case TypeKind::Rune:
    case TypeKind::Enum:
      return ConstantValue::make_integer(0);
    case TypeKind::EndianScalar:
      if (!type.element.is_valid() ||
          runtime_type(type.element).kind != TypeKind::Float) {
        return ConstantValue::make_integer(0);
      }
      [[fallthrough]];
    case TypeKind::Float: {
      const std::optional<IeeeBinaryFormat> format = float_format(type_id);
      if (!format.has_value()) return std::nullopt;
      const std::uint32_t width =
          1U + format->exponent_bits + format->fraction_bits;
      return float_from_bits(ieee_zero_bits(*format, false), width);
    }
    case TypeKind::RawPointer:
    case TypeKind::CString:
    case TypeKind::Pointer:
    case TypeKind::MultiPointer:
    case TypeKind::Procedure:
      return ConstantValue::make_nil();
    case TypeKind::String:
      return ConstantValue::make_string({});
    case TypeKind::Array:
    case TypeKind::Simd: {
      const std::optional<ConstantValue> element = zero_value(type.element);
      if (!element.has_value()) return std::nullopt;
      std::vector<ConstantValue> elements(
          static_cast<std::size_t>(type.element_count), *element);
      return ConstantValue::make_aggregate(std::move(elements));
    }
    case TypeKind::Tuple:
    case TypeKind::Struct: {
      std::vector<ConstantValue> elements;
      elements.reserve(type.members.size());
      for (TypeId member : type.members) {
        const std::optional<ConstantValue> value = zero_value(member);
        if (!value.has_value()) return std::nullopt;
        elements.push_back(*value);
      }
      return ConstantValue::make_aggregate(std::move(elements));
    }
    case TypeKind::RawUnion:
      return ConstantValue::make_aggregate({}, 0);
    case TypeKind::TaggedUnion: {
      std::vector<ConstantValue> payload;
      if (!type.members.empty() &&
          semantic_.types.type(type.members.front()).kind != TypeKind::Void) {
        const std::optional<ConstantValue> value = zero_value(type.members.front());
        if (!value.has_value()) return std::nullopt;
        payload.push_back(*value);
      }
      return ConstantValue::make_aggregate(std::move(payload), 0);
    }
    default:
      return std::nullopt;
    }
  }

  [[nodiscard]] ConstantValue uninitialized_value(TypeId type_id) const {
    Type type = semantic_.types.type(type_id);
    while (type.kind == TypeKind::Distinct) {
      type = semantic_.types.type(type.element);
    }
    if (type.kind == TypeKind::Array || type.kind == TypeKind::Simd) {
      std::vector<ConstantValue> elements;
      elements.reserve(static_cast<std::size_t>(type.element_count));
      for (std::uint64_t index = 0; index < type.element_count; ++index) {
        elements.push_back(uninitialized_value(type.element));
      }
      return ConstantValue::make_aggregate(std::move(elements));
    }
    if (type.kind == TypeKind::Tuple || type.kind == TypeKind::Struct) {
      std::vector<ConstantValue> elements;
      elements.reserve(type.members.size());
      for (TypeId member : type.members) {
        elements.push_back(uninitialized_value(member));
      }
      return ConstantValue::make_aggregate(std::move(elements));
    }
    // A union does not have independently addressable simultaneous fields. Its
    // active alternative remains unknown until a complete union value is stored.
    return ConstantValue{};
  }

  [[nodiscard]] bool contains_uninitialized(
      const ConstantValue &value) const {
    if (value.kind == ConstantKind::Unavailable) return true;
    for (const ConstantValue &element : value.elements) {
      if (contains_uninitialized(element)) return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<ConstantValue> zero_value_from_syntax(
      const SyntaxTree &tree, NodeId type_node, ScopeId scope) {
    const SyntaxNode &node = tree.node(type_node);
    if (node.kind == NodeKind::PointerType ||
        node.kind == NodeKind::MultiPointerType ||
        node.kind == NodeKind::ProcedureType) {
      return ConstantValue::make_nil();
    }
    const std::optional<TypeId> resolved = type_value(tree, type_node, scope);
    return resolved.has_value() ? zero_value(*resolved) : std::nullopt;
  }

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

  [[nodiscard]] TokenKind binary_for_assignment(TokenKind operation) const {
    switch (operation) {
    case TokenKind::PlusEqual: return TokenKind::Plus;
    case TokenKind::MinusEqual: return TokenKind::Minus;
    case TokenKind::StarEqual: return TokenKind::Star;
    case TokenKind::SlashEqual: return TokenKind::Slash;
    case TokenKind::PercentEqual: return TokenKind::Percent;
    case TokenKind::AmpersandEqual: return TokenKind::Ampersand;
    case TokenKind::PipeEqual: return TokenKind::Pipe;
    case TokenKind::CaretEqual: return TokenKind::Caret;
    case TokenKind::ShiftLeftEqual: return TokenKind::ShiftLeft;
    case TokenKind::ShiftRightEqual: return TokenKind::ShiftRight;
    default: return TokenKind::Invalid;
    }
  }

  [[nodiscard]] std::optional<ScopeId> procedure_scope(SymbolId owner) const {
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner == owner &&
          semantic_.symbols.scope(owned.scope).kind == ScopeKind::Procedure) {
        return owned.scope;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<NodeId> procedure_payload(
      const SyntaxTree &tree, const Symbol &symbol) const {
    if (!symbol.syntax.node.is_valid()) return std::nullopt;
    const SyntaxNode &declaration = tree.node(symbol.syntax.node);
    if (declaration.kind == NodeKind::Procedure) return symbol.syntax.node;
    for (NodeId child : declaration.children) {
      if (tree.node(child).kind == NodeKind::Procedure) return child;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<std::string> procedure_parameter_names(
      const SyntaxTree &tree, const SyntaxNode &procedure) const {
    std::vector<std::string> result;
    for (NodeId child : procedure.children) {
      const SyntaxNode &candidate = tree.node(child);
      if (candidate.kind != NodeKind::ParameterList) continue;
      for (NodeId parameter_id : candidate.children) {
        const SyntaxNode &parameter = tree.node(parameter_id);
        if (parameter.children.empty()) continue;
        const SyntaxNode &name_list = tree.node(parameter.children.front());
        const std::vector<std::string> names = names_in_node(tree, name_list);
        result.insert(result.end(), names.begin(), names.end());
      }
      break;
    }
    return result;
  }

  [[nodiscard]] TypeId procedure_parameter_type(
      ScopeId scope, std::string_view name) const {
    const std::optional<SymbolId> symbol =
        semantic_.symbols.lookup_direct(scope, name);
    if (!symbol.has_value()) return semantic_.types.builtins().invalid;
    const Symbol &parameter = semantic_.symbols.symbol(*symbol);
    return parameter.kind == SymbolKind::Parameter
        ? substitute_local_type(parameter.type)
        : semantic_.types.builtins().invalid;
  }

  [[nodiscard]] std::vector<ParametricParameterRecord> parametric_parameters(
      SymbolId owner) const {
    std::vector<ParametricParameterRecord> result;
    for (const ParametricParameterRecord &parameter :
         semantic_.parametric_parameters) {
      if (parameter.owner == owner) result.push_back(parameter);
    }
    return result;
  }

  [[nodiscard]] std::optional<NodeId> procedure_body(
      const SyntaxTree &tree, const SyntaxNode &procedure) const {
    for (NodeId child : procedure.children) {
      if (tree.node(child).kind == NodeKind::Block) return child;
    }
    return std::nullopt;
  }

  [[nodiscard]] EvalResult evaluate_intrinsic_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      bool required) {
    if (call.children.empty()) return pending();
    const SyntaxNode &callee = tree.node(call.children.front());

    // `cast[T](value)` is parsed as a call whose callee is a bracket
    // expression.  Handle it before the direct-name intrinsics below.  Integer
    // casts wrap modulo the destination width; float-to-integer casts truncate
    // toward zero and trap when the result is not representable, matching the
    // ordinary body checker without using host floating point.
    if (callee.kind == NodeKind::BracketExpression &&
        !callee.children.empty()) {
      const SyntaxNode &base = tree.node(callee.children.front());
      const std::optional<std::string> base_name =
          base.kind == NodeKind::NameExpression
              ? final_name(tree, base)
              : std::nullopt;
      if (base_name.has_value() && *base_name == "cast") {
        if (callee.children.size() != 2 || call.children.size() != 2) {
          return fail(
              call.range,
              "cast[T] requires one type and one compile-time value",
              required);
        }
        const std::optional<TypeId> target =
            type_value(tree, callee.children[1], scope);
        if (!target.has_value()) {
          return fail(
              tree.node(callee.children[1]).range,
              "compile-time cast target is not a type",
              required);
        }
        const EvalResult argument =
            evaluate_expression(tree, call.children[1], scope, required);
        if (argument.status != EvalStatus::Ready) return argument;
        const Type target_type = runtime_type(*target);

        if (target_type.kind == TypeKind::Bool &&
            argument.value.kind == ConstantKind::Integer) {
          return ready(
              ConstantValue::make_bool(!argument.value.integer.is_zero()),
              *target);
        }
        if (target_type.kind == TypeKind::BooleanStorage &&
            argument.value.kind == ConstantKind::Bool) {
          return ready(
              ConstantValue::make_integer(argument.value.boolean ? 1 : 0),
              *target);
        }

        const TypeKind endian_value_kind =
            target_type.kind == TypeKind::EndianScalar &&
                target_type.element.is_valid()
            ? runtime_type(target_type.element).kind
            : TypeKind::Invalid;
        const bool integer_target =
            target_type.kind == TypeKind::SignedInteger ||
            target_type.kind == TypeKind::UnsignedInteger ||
            target_type.kind == TypeKind::Rune ||
            target_type.kind == TypeKind::Enum ||
            target_type.kind == TypeKind::BooleanStorage ||
            endian_value_kind == TypeKind::SignedInteger ||
            endian_value_kind == TypeKind::UnsignedInteger;
        if (integer_target &&
            (argument.value.kind == ConstantKind::Integer ||
             argument.value.kind == ConstantKind::Float)) {
          BigInteger integer;
          if (argument.value.kind == ConstantKind::Integer) {
            integer = wrapped_integer(argument.value.integer, *target);
          } else {
            ExactRational source = argument.value.floating;
            if (argument.value.float_bit_width != 0) {
              const std::optional<IeeeBinaryFormat> source_format =
                  ieee_format_for_width(argument.value.float_bit_width);
              const std::optional<DecodedIeeeValue> decoded =
                  source_format.has_value()
                  ? decode_ieee_bits(argument.value.float_bits, *source_format)
                  : std::nullopt;
              if (!decoded.has_value() ||
                  decoded->kind != IeeeValueKind::Finite) {
                return fail(
                    call.range,
                    "compile-time non-finite float-to-integer cast traps",
                    required);
              }
              source = decoded->finite;
            }
            BigInteger remainder;
            if (!source.numerator().divide(
                    source.denominator(), integer, remainder) ||
                !integer_representable(integer, *target)) {
              return fail(
                  call.range,
                  "compile-time float-to-integer cast is out of range",
                  required);
            }
          }
          if (target_type.kind == TypeKind::Rune && !valid_rune(integer)) {
            return fail(
                call.range,
                "compile-time cast does not produce a Unicode scalar",
                required);
          }
          if (target_type.kind == TypeKind::Enum &&
              !valid_enum_value(*target, integer)) {
            return fail(
                call.range,
                "compile-time cast does not name an enum member",
                required);
          }
          return ready(ConstantValue::make_integer(std::move(integer)), *target);
        }
        if ((target_type.kind == TypeKind::Float ||
             endian_value_kind == TypeKind::Float) &&
            (argument.value.kind == ConstantKind::Integer ||
             argument.value.kind == ConstantKind::Float)) {
          return convert_float_to_type(
              argument.value, *target, call.range, required);
        }

        return convert_to_type(
            argument.value, *target, true, call.range, required);
      }
    }
    if (callee.kind != NodeKind::NameExpression) return pending();
    const std::optional<std::string> name = final_name(tree, callee);
    if (!name.has_value()) return pending();

    if (*name == "len") {
      if (call.children.size() != 2) {
        return fail(call.range, "len requires one compile-time value", required);
      }
      const EvalResult argument =
          evaluate_expression(tree, call.children[1], scope, required);
      if (argument.status != EvalStatus::Ready) return argument;
      if (argument.value.kind == ConstantKind::String) {
        return ready(
            ConstantValue::make_integer(
                BigInteger::from_u64(argument.value.text.size())),
            semantic_.types.builtins().usize_type);
      }
      if (argument.value.kind == ConstantKind::Aggregate &&
          argument.type.is_valid() &&
          semantic_.types.type(argument.type).kind == TypeKind::Array) {
        return ready(
            ConstantValue::make_integer(BigInteger::from_u64(
                semantic_.types.type(argument.type).element_count)),
            semantic_.types.builtins().usize_type);
      }
      return fail(
          call.range,
          "compile-time len requires a string or array value",
          required);
    }

    if (*name == "static_assert") {
      if (call.children.size() < 2 || call.children.size() > 3) {
        return fail(
            call.range,
            "static_assert requires a bool and optional string",
            required);
      }
      const EvalResult condition =
          evaluate_expression(tree, call.children[1], scope, required);
      if (condition.status != EvalStatus::Ready) return condition;
      if (condition.value.kind != ConstantKind::Bool) {
        return fail(call.range, "static_assert condition must be bool", required);
      }
      std::string message = "static assertion failed";
      if (call.children.size() == 3) {
        const EvalResult supplied =
            evaluate_expression(tree, call.children[2], scope, required);
        if (supplied.status != EvalStatus::Ready) return supplied;
        if (supplied.value.kind != ConstantKind::String) {
          return fail(
              tree.node(call.children[2]).range,
              "static_assert message must be a string",
              required);
        }
        message += ": " + supplied.value.text;
      }
      if (!condition.value.boolean) return fail(call.range, message, required);
      // The value is ignored by statement execution.  Returning true avoids
      // inventing a source-level void constant solely for the interpreter.
      return ready(ConstantValue::make_bool(true));
    }

    if (*name == "assert") {
      return fail(
          call.range,
          "runtime assert is unavailable during compile-time evaluation",
          required);
    }
    return pending();
  }

  // Resolves a source call and evaluates all of its inputs without entering
  // the callee.  Ordinary calls invoke the result immediately; defer stores it
  // until lexical exit.  This shared preparation is what gives both paths the
  // same conversion rules and the same left-to-right argument evaluation.
  [[nodiscard]] EvalResult prepare_procedure_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      bool required,
      PreparedProcedureCall &prepared) {
    if (call.children.empty()) return pending();
    const SyntaxNode &callee = tree.node(call.children.front());
    NodeId base_callee = call.children.front();
    std::vector<NodeId> compile_time_arguments;
    if (callee.kind == NodeKind::BracketExpression && !callee.children.empty()) {
      base_callee = callee.children.front();
      compile_time_arguments.insert(
          compile_time_arguments.end(),
          callee.children.begin() + 1,
          callee.children.end());
    }

    // The callee is an ordinary expression and is evaluated before every
    // argument.  This admits grouped and selected procedure values, procedure
    // fields in constant aggregates, and imported procedure identities instead
    // of giving compile-time calls a narrower grammar than runtime calls.
    const EvalResult evaluated_callee = evaluate_expression(
        tree, base_callee, scope, required);
    if (evaluated_callee.status != EvalStatus::Ready) {
      return evaluated_callee;
    }
    const std::optional<SymbolId> found =
        procedure_symbol(evaluated_callee.value);
    if (!found.has_value()) {
      return fail(
          call.range,
          "compile-time call target is not a concrete procedure identity",
          required);
    }
    const Symbol symbol = semantic_.symbols.symbol(*found);
    if (symbol.flags.foreign) {
      return fail(
          call.range,
          "foreign calls are unavailable during compile-time evaluation",
          required);
    }

    const SyntaxTree *procedure_tree = find_tree(symbol.syntax.file);
    if (procedure_tree == nullptr) return pending();
    const std::optional<NodeId> payload = procedure_payload(*procedure_tree, symbol);
    if (!payload.has_value()) {
      return fail(
          call.range,
          "compile-time call requires a procedure body",
          required);
    }
    const SyntaxNode &procedure = procedure_tree->node(*payload);
    const std::optional<NodeId> body = procedure_body(*procedure_tree, procedure);
    if (!body.has_value()) {
      return fail(
          call.range,
          "compile-time call requires a procedure body",
          required);
    }
    const std::vector<std::string> parameters =
        procedure_parameter_names(*procedure_tree, procedure);
    if (call.children.size() - 1 != parameters.size()) {
      return fail(
          call.range,
          "compile-time procedure argument count does not match its parameters",
          required);
    }
    const std::vector<ParametricParameterRecord> compile_parameters =
        parametric_parameters(*found);
    if (compile_time_arguments.size() != compile_parameters.size()) {
      return fail(
          call.range,
          "compile-time procedure argument count does not match its parametric parameters",
          required);
    }
    const ScopeId body_scope =
        procedure_scope(*found).value_or(file_scope(symbol.syntax.file));

    // Evaluate every input in the caller before installing the callee frame.
    // This preserves source evaluation order and prevents accidental dynamic
    // scoping when a caller local shares a callee parameter name.
    prepared = {};
    prepared.procedure = *found;
    prepared.range = call.range;
    for (std::size_t index = 0; index < compile_parameters.size(); ++index) {
      const ParametricParameterRecord &parameter = compile_parameters[index];
      const Symbol &parameter_symbol =
          semantic_.symbols.symbol(parameter.parameter);
      if (parameter_symbol.kind == SymbolKind::TypeParameter) {
        const std::optional<TypeId> supplied = type_value(
            tree, compile_time_arguments[index], scope);
        if (!supplied.has_value()) {
          return fail(
              tree.node(compile_time_arguments[index]).range,
              "compile-time type parameter requires a type argument",
              required);
        }
        prepared.type_bindings.push_back(
            {parameter_symbol.type, *supplied});
        continue;
      }
      const EvalResult supplied = evaluate_expression(
          tree, compile_time_arguments[index], scope, required);
      if (supplied.status != EvalStatus::Ready) return supplied;
      const EvalResult converted = convert_to_type(
          supplied.value,
          parameter_symbol.type,
          false,
          tree.node(compile_time_arguments[index]).range,
          required);
      if (converted.status != EvalStatus::Ready) return converted;
      prepared.bindings.push_back(
          {parameter_symbol.name, converted.value, parameter_symbol.type});
    }

    std::vector<TypeId> parameter_types;
    parameter_types.reserve(parameters.size());
    for (const std::string &parameter_name : parameters) {
      TypeId parameter_type = procedure_parameter_type(
          body_scope, parameter_name);
      for (const ConstantTypeBinding &binding : prepared.type_bindings) {
        if (binding.parameter == parameter_type) {
          parameter_type = binding.replacement;
          break;
        }
      }
      parameter_types.push_back(parameter_type);
    }

    std::vector<EvalResult> supplied_arguments;
    supplied_arguments.reserve(parameters.size());
    for (std::size_t index = 1; index < call.children.size(); ++index) {
      const TypeId expected = index - 1U < parameter_types.size()
          ? parameter_types[index - 1U]
          : TypeId{};
      const EvalResult argument = evaluate_expression(
          tree, call.children[index], scope, required, expected);
      if (argument.status != EvalStatus::Ready) return argument;
      supplied_arguments.push_back(argument);
    }

    for (std::size_t index = 0; index < supplied_arguments.size(); ++index) {
      const TypeId parameter_type = parameter_types[index];
      const EvalResult &argument = supplied_arguments[index];
      if (argument.value.kind == ConstantKind::Procedure &&
          parameter_type.is_valid() && argument.type.is_valid() &&
          argument.type != parameter_type) {
        return fail(
            tree.node(call.children[index + 1]).range,
            "compile-time procedure argument has a different procedure type",
            required);
      }
      const EvalResult converted = parameter_type.is_valid() &&
              parameter_type != semantic_.types.builtins().invalid
          ? convert_to_type(
                argument.value,
                parameter_type,
                false,
                tree.node(call.children[index + 1]).range,
                required)
          : argument;
      if (converted.status != EvalStatus::Ready) {
        return converted;
      }
      prepared.bindings.push_back(
          {parameters[index], converted.value, parameter_type});
    }
    const Type &procedure_type = semantic_.types.type(symbol.type);
    prepared.result_type = procedure_type.members.empty()
        ? semantic_.types.builtins().invalid
        : substitute_type_bindings(
              procedure_type.members.back(), prepared.type_bindings);
    return ready(ConstantValue::make_bool(true));
  }

  // Executes a call whose callee and arguments have already been saved.  The
  // caller frame remains below the new frame only so nested evaluator work can
  // return to it; name lookup always consults the top frame and therefore
  // cannot accidentally become dynamically scoped.
  [[nodiscard]] EvalResult invoke_prepared_procedure_call(
      PreparedProcedureCall prepared,
      bool required,
      bool allow_void_result) {
    const Symbol symbol = semantic_.symbols.symbol(prepared.procedure);
    if (procedure_call_depth_ >= kMaximumProcedureCallDepth) {
      return fail(
          prepared.range,
          "compile-time procedure recursion limit exceeded",
          required);
    }
    if (!consume_execution_step(prepared.range, required)) {
      return error_result();
    }

    const SyntaxTree *procedure_tree = find_tree(symbol.syntax.file);
    if (procedure_tree == nullptr) return pending();
    const std::optional<NodeId> payload =
        procedure_payload(*procedure_tree, symbol);
    if (!payload.has_value()) {
      return fail(
          prepared.range,
          "compile-time call requires a procedure body",
          required);
    }
    const SyntaxNode &procedure = procedure_tree->node(*payload);
    const std::optional<NodeId> body =
        procedure_body(*procedure_tree, procedure);
    if (!body.has_value()) {
      return fail(
          prepared.range,
          "compile-time call requires a procedure body",
          required);
    }
    const ScopeId body_scope = procedure_scope(prepared.procedure).value_or(
        file_scope(symbol.syntax.file));

    LocalFrame frame;
    frame.scopes.push_back(std::move(prepared.bindings));
    frame.type_bindings = std::move(prepared.type_bindings);
    frame.result_type = prepared.result_type;
    local_frames_.push_back(std::move(frame));
    active_procedures_.push_back(prepared.procedure);
    ++procedure_call_depth_;
    const ExecutionResult execution = execute_block(
        *procedure_tree, *body, body_scope, required);
    const TypeId result_type = local_frames_.back().result_type;
    --procedure_call_depth_;
    active_procedures_.pop_back();
    local_frames_.pop_back();
    const bool returns_void =
        result_type == semantic_.types.builtins().invalid ||
        (result_type.is_valid() &&
         semantic_.types.type(result_type).kind == TypeKind::Void);

    if (execution.signal == ExecutionSignal::Return) {
      if (execution.value.kind == ConstantKind::Unavailable) {
        if (returns_void) {
          return allow_void_result
              ? ready(ConstantValue::make_bool(true))
              : fail(
                    prepared.range,
                    "void compile-time procedure call does not produce a value",
                    required);
        }
        return fail(
            procedure.range,
            "compile-time procedure returned no value",
            required);
      }
      if (execution.type == result_type) {
        return ready(execution.value, result_type);
      }
      if (execution.value.kind == ConstantKind::Procedure) {
        return fail(
            procedure.range,
            "compile-time procedure returned a different procedure type",
            required);
      }
      return convert_to_type(
          execution.value,
          result_type,
          false,
          procedure.range,
          required);
    }
    if (execution.signal == ExecutionSignal::Failed) {
      return {execution.failure, {}, {}};
    }
    if (execution.signal == ExecutionSignal::Break ||
        execution.signal == ExecutionSignal::Continue) {
      return fail(
          procedure.range,
          "compile-time procedure has control flow escaping its body",
          required);
    }
    if (returns_void) {
      // Statement evaluation needs a Ready value even though Draft has no
      // source-level void constant.  The boolean is an interpreter sentinel
      // and is never exposed as the procedure's result type.
      return allow_void_result
          ? ready(ConstantValue::make_bool(true))
          : fail(
                prepared.range,
                "void compile-time procedure call does not produce a value",
                required);
    }
    return fail(
        procedure.range,
        "compile-time procedure completed without returning a value",
        required);
  }

  [[nodiscard]] EvalResult evaluate_procedure_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      bool required,
      bool allow_void_result) {
    PreparedProcedureCall prepared;
    const EvalResult preparation = prepare_procedure_call(
        tree, call, scope, required, prepared);
    if (preparation.status != EvalStatus::Ready) return preparation;
    return invoke_prepared_procedure_call(
        std::move(prepared), required, allow_void_result);
  }

  [[nodiscard]] EvalResult evaluate_call_expression(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      bool required,
      bool allow_void_result) {
    const EvalResult layout =
        evaluate_layout_call(tree, call, scope, required);
    if (layout.status != EvalStatus::Pending) return layout;
    const EvalResult target_call =
        evaluate_target_call(tree, call, scope, required);
    if (target_call.status != EvalStatus::Pending) return target_call;
    const EvalResult intrinsic =
        evaluate_intrinsic_call(tree, call, scope, required);
    if (intrinsic.status != EvalStatus::Pending) return intrinsic;
    return evaluate_procedure_call(
        tree, call, scope, required, allow_void_result);
  }

  [[nodiscard]] ExecutionResult execute_declaration(
      const SyntaxTree &tree,
      const SyntaxNode &declaration,
      ScopeId scope,
      bool required) {
    if (declaration.children.empty()) {
      return failed_execution(fail(
          declaration.range, "malformed compile-time local declaration", required));
    }
    const SyntaxNode &pattern = tree.node(declaration.children.front());
    if (pattern.children.empty()) {
      return failed_execution(fail(
          declaration.range, "compile-time local requires a name", required));
    }
    const std::vector<std::string> names =
        names_in_node(tree, tree.node(pattern.children.front()));
    if (names.empty()) {
      return failed_execution(fail(
          declaration.range,
          "compile-time local requires at least one binding name",
          required));
    }
    const bool destructures_tuple = pattern.kind == NodeKind::TuplePattern;

    std::optional<NodeId> declared_type;
    std::optional<NodeId> initializer;
    for (std::size_t index = 1; index < declaration.children.size(); ++index) {
      const NodeId child = declaration.children[index];
      const NodeKind kind = tree.node(child).kind;
      if (is_type_syntax(kind)) {
        declared_type = child;
      } else if (kind != NodeKind::ParametricParameterList) {
        initializer = child;
      }
    }

    ConstantValue value;
    TypeId local_type;
    if (declared_type.has_value()) {
      local_type = type_value(tree, *declared_type, scope).value_or(
          semantic_.types.builtins().invalid);
    }
    const bool explicitly_uninitialized = initializer.has_value() &&
        tree.node(*initializer).kind == NodeKind::UninitializedExpression;
    if (explicitly_uninitialized) {
      if (!declared_type.has_value() ||
          local_type == semantic_.types.builtins().invalid) {
        return failed_execution(fail(
            tree.node(*initializer).range,
            "uninitialized compile-time local requires an explicit type",
            required));
      }
      value = uninitialized_value(local_type);
    } else if (initializer.has_value()) {
      const EvalResult evaluated =
          evaluate_expression(
              tree,
              *initializer,
              scope,
              required,
              declared_type.has_value() ? local_type : TypeId{});
      if (evaluated.status != EvalStatus::Ready) {
        return failed_execution(evaluated);
      }
      if (declared_type.has_value() && local_type.is_valid() &&
          local_type != semantic_.types.builtins().invalid) {
        if (evaluated.value.kind == ConstantKind::Procedure &&
            evaluated.type.is_valid() && evaluated.type != local_type) {
          return failed_execution(fail(
              tree.node(*initializer).range,
              "compile-time local has a different procedure type",
              required));
        }
        const EvalResult converted = convert_to_type(
            evaluated.value,
            local_type,
            false,
            tree.node(*initializer).range,
            required);
        if (converted.status != EvalStatus::Ready) {
          return failed_execution(converted);
        }
        value = converted.value;
      } else {
        value = evaluated.value;
        local_type = evaluated.type.is_valid()
            ? evaluated.type
            : default_value_type(evaluated.value);
      }
    } else if (declared_type.has_value()) {
      const std::optional<ConstantValue> zero =
          zero_value_from_syntax(tree, *declared_type, scope);
      if (!zero.has_value()) {
        return failed_execution(fail(
            tree.node(*declared_type).range,
            "compile-time local type has no supported zero value",
            required));
      }
      value = *zero;
    } else {
      return failed_execution(fail(
          declaration.range,
          "compile-time local requires a type or initializer",
          required));
    }

    if (destructures_tuple) {
      if (!local_type.is_valid() ||
          semantic_.types.type(local_type).kind != TypeKind::Tuple) {
        return failed_execution(fail(
            pattern.range,
            "compile-time tuple pattern requires a tuple value",
            required));
      }
      const Type tuple = semantic_.types.type(local_type);
      if (tuple.members.size() != names.size()) {
        return failed_execution(fail(
            pattern.range,
            "compile-time tuple pattern has the wrong arity",
            required));
      }
      if (!explicitly_uninitialized &&
          (value.kind != ConstantKind::Aggregate ||
           value.elements.size() != names.size())) {
        return failed_execution(fail(
            pattern.range,
            "compile-time tuple pattern value has the wrong shape",
            required));
      }
      for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == "_") continue;
        declare_local(
            names[index],
            explicitly_uninitialized
                ? uninitialized_value(tuple.members[index])
                : value.elements[index],
            tuple.members[index]);
      }
      return {};
    }

    for (const std::string &name : names) {
      if (name != "_") declare_local(name, value, local_type);
    }
    return {};
  }

  [[nodiscard]] LocalTargetResult local_target(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      bool required) {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind == NodeKind::NameExpression) {
      const std::optional<std::string> name = final_name(tree, expression);
      const LocalBinding *binding =
          name.has_value() ? local_binding(*name) : nullptr;
      if (binding == nullptr) return {};
      return {
          EvalStatus::Ready,
          LocalTarget{*name, {}, binding->value, binding->type}};
    }
    if ((expression.kind != NodeKind::BracketExpression &&
         expression.kind != NodeKind::MemberExpression) ||
        expression.children.empty()) {
      return {};
    }

    LocalTargetResult base = local_target(
        tree, expression.children.front(), scope, required);
    if (base.status != EvalStatus::Ready || !base.target.has_value()) return base;
    if (base.target->value.kind != ConstantKind::Aggregate ||
        !base.target->type.is_valid()) {
      const EvalResult failure = fail(
          expression.range,
          "compile-time assignment target is not aggregate storage",
          required);
      return {failure.status, std::nullopt};
    }

    const Type aggregate = semantic_.types.type(base.target->type);
    std::optional<std::size_t> selected;
    TypeId selected_type;
    if (expression.kind == NodeKind::BracketExpression) {
      if (expression.children.size() != 2 ||
          (aggregate.kind != TypeKind::Array &&
           aggregate.kind != TypeKind::Simd)) {
        const EvalResult failure = fail(
            expression.range,
            "compile-time indexed assignment requires an array local",
            required);
        return {failure.status, std::nullopt};
      }
      const EvalResult index = evaluate_expression(
          tree, expression.children[1], scope, required);
      if (index.status != EvalStatus::Ready) {
        return {index.status, std::nullopt};
      }
      const std::optional<std::uint64_t> value =
          index.value.kind == ConstantKind::Integer
          ? index.value.integer.to_u64()
          : std::nullopt;
      if (!value.has_value() || *value >= base.target->value.elements.size()) {
        const EvalResult failure = fail(
            expression.range,
            "compile-time indexed assignment is out of bounds",
            required);
        return {failure.status, std::nullopt};
      }
      selected = static_cast<std::size_t>(*value);
      selected_type = aggregate.element;
    } else if (aggregate.kind == TypeKind::Struct) {
      const std::optional<std::string> member = final_name(tree, expression);
      if (member.has_value()) {
        selected = aggregate_member_index(base.target->type, *member);
      }
      if (selected.has_value() && *selected < aggregate.members.size()) {
        selected_type = aggregate.members[*selected];
      }
    } else if (aggregate.kind == TypeKind::Tuple) {
      const SyntaxNode &base_node = tree.node(expression.children.front());
      for (std::uint32_t token_index = base_node.token_end;
           token_index < expression.token_end;
           ++token_index) {
        const Token &token = tree.token(token_index);
        if (token.kind != TokenKind::IntegerLiteral) continue;
        const std::optional<BigInteger> parsed =
            integer_literal(sources_.text(token.range));
        const std::optional<std::uint64_t> value =
            parsed.has_value() ? parsed->to_u64() : std::nullopt;
        if (value.has_value()) selected = static_cast<std::size_t>(*value);
        break;
      }
      if (selected.has_value() && *selected < aggregate.members.size()) {
        selected_type = aggregate.members[*selected];
      }
    }
    if (!selected.has_value() ||
        *selected >= base.target->value.elements.size() ||
        !selected_type.is_valid()) {
      const EvalResult failure = fail(
          expression.range,
          "compile-time assignment names no aggregate member",
          required);
      return {failure.status, std::nullopt};
    }
    base.target->path.push_back(*selected);
    // Copy the selected value before replacing its owning aggregate.  Assigning
    // directly from an element of `value` would invalidate the source while
    // ConstantValue's vector assignment destroys the old aggregate storage.
    ConstantValue selected_value = base.target->value.elements[*selected];
    base.target->value = std::move(selected_value);
    base.target->type = selected_type;
    return base;
  }

  [[nodiscard]] bool store_local_target(
      const LocalTarget &target, ConstantValue value) {
    const LocalBinding *root_binding = local_binding(target.root);
    if (root_binding == nullptr) return false;
    ConstantValue root = root_binding->value;
    ConstantValue *slot = &root;
    for (std::size_t index : target.path) {
      if (slot->kind != ConstantKind::Aggregate ||
          index >= slot->elements.size()) {
        return false;
      }
      slot = &slot->elements[index];
    }
    *slot = std::move(value);
    return assign_local(target.root, std::move(root));
  }

  [[nodiscard]] ExecutionResult execute_assignment(
      const SyntaxTree &tree,
      const SyntaxNode &assignment,
      ScopeId scope,
      bool required) {
    const std::optional<std::uint32_t> operator_index =
        assignment_operator_index(tree, assignment);
    if (!operator_index.has_value()) {
      return failed_execution(fail(
          assignment.range,
          "compile-time assignment has no operator",
          required));
    }
    std::size_t left_count = 0;
    for (NodeId child : assignment.children) {
      if (tree.node(child).token_end <= *operator_index) ++left_count;
    }
    const std::size_t right_count = assignment.children.size() - left_count;

    // A parenthesized assignment pattern consumes one tuple value. Resolve all
    // destination paths before evaluating that value, then convert and stage
    // every selected member before performing any write. This matches runtime
    // assignment ordering and permits swaps such as `(left, right) =
    // (right, left)` during compile-time procedure execution.
    if (left_count == 1 && right_count == 1 &&
        tree.token(*operator_index).kind == TokenKind::Equal &&
        tree.node(assignment.children.front()).kind ==
            NodeKind::TupleExpression) {
      const SyntaxNode &pattern = tree.node(assignment.children.front());
      std::vector<std::optional<LocalTarget>> targets;
      targets.reserve(pattern.children.size());
      for (NodeId target_id : pattern.children) {
        const SyntaxNode &target_expression = tree.node(target_id);
        const std::optional<std::string> name =
            target_expression.kind == NodeKind::NameExpression
            ? final_name(tree, target_expression)
            : std::nullopt;
        if (name.has_value() && *name == "_") {
          targets.push_back(std::nullopt);
          continue;
        }
        const LocalTargetResult resolved = local_target(
            tree, target_id, scope, required);
        if (resolved.status != EvalStatus::Ready ||
            !resolved.target.has_value()) {
          if (resolved.status == EvalStatus::Error) {
            return failed_execution(EvalStatus::Error);
          }
          return failed_execution(fail(
              target_expression.range,
              "compile-time tuple assignment cannot write runtime or unknown storage",
              required));
        }
        targets.push_back(*resolved.target);
      }

      const NodeId value_id = assignment.children.back();
      const EvalResult value = evaluate_expression(
          tree, value_id, scope, required);
      if (value.status != EvalStatus::Ready) return failed_execution(value);
      if (!value.type.is_valid() ||
          semantic_.types.type(value.type).kind != TypeKind::Tuple ||
          value.value.kind != ConstantKind::Aggregate ||
          value.value.elements.size() != pattern.children.size()) {
        return failed_execution(fail(
            tree.node(value_id).range,
            "compile-time tuple assignment requires a tuple of matching arity",
            required));
      }

      std::vector<std::optional<ConstantValue>> stored_values;
      stored_values.reserve(targets.size());
      for (std::size_t index = 0; index < targets.size(); ++index) {
        if (!targets[index].has_value()) {
          stored_values.push_back(std::nullopt);
          continue;
        }
        const LocalTarget &target = *targets[index];
        const TypeId member_type =
            semantic_.types.type(value.type).members[index];
        ConstantValue stored = value.value.elements[index];
        if (target.type.is_valid() &&
            target.type != semantic_.types.builtins().invalid &&
            member_type != target.type) {
          const EvalResult converted = convert_to_type(
              stored,
              target.type,
              false,
              tree.node(value_id).range,
              required);
          if (converted.status != EvalStatus::Ready) {
            return failed_execution(converted);
          }
          stored = converted.value;
        }
        stored_values.push_back(std::move(stored));
      }
      for (std::size_t index = 0; index < targets.size(); ++index) {
        if (!targets[index].has_value()) continue;
        if (!stored_values[index].has_value() ||
            !store_local_target(
                *targets[index], std::move(*stored_values[index]))) {
          return failed_execution(fail(
              tree.node(pattern.children[index]).range,
              "compile-time tuple assignment target became unavailable",
              required));
        }
      }
      return {};
    }

    if (left_count == 0 || left_count != right_count) {
      return failed_execution(fail(
          assignment.range,
          "compile-time assignment sides must have equal nonzero arity",
          required));
    }

    // Resolve every lvalue before evaluating any right-hand side.  The stored
    // snapshots are also the old values used by compound assignment; actual
    // writes happen only after every RHS has been evaluated and converted.
    std::vector<std::optional<LocalTarget>> targets;
    targets.reserve(left_count);
    for (std::size_t index = 0; index < left_count; ++index) {
      const NodeId target_id = assignment.children[index];
      const SyntaxNode &target_expression = tree.node(target_id);
      const std::optional<std::string> name =
          target_expression.kind == NodeKind::NameExpression
          ? final_name(tree, target_expression)
          : std::nullopt;
      if (name.has_value() && *name == "_") {
        targets.push_back(std::nullopt);
        continue;
      }
      const LocalTargetResult resolved = local_target(
          tree, target_id, scope, required);
      if (resolved.status != EvalStatus::Ready || !resolved.target.has_value()) {
        if (resolved.status == EvalStatus::Error) {
          return failed_execution(EvalStatus::Error);
        }
        return failed_execution(fail(
            target_expression.range,
            "compile-time assignment cannot write runtime or unknown storage",
            required));
      }
      targets.push_back(*resolved.target);
    }

    const TokenKind operation = tree.token(*operator_index).kind;
    if (operation != TokenKind::Equal) {
      for (const std::optional<LocalTarget> &target : targets) {
        if (!target.has_value()) {
          return failed_execution(fail(
              assignment.range,
              "discard target is invalid in compound assignment",
              required));
        }
      }
    }

    std::vector<std::optional<ConstantValue>> stored_values;
    stored_values.reserve(right_count);
    for (std::size_t index = 0; index < right_count; ++index) {
      const NodeId right_id = assignment.children[left_count + index];
      const TypeId expected = targets[index].has_value()
          ? targets[index]->type
          : TypeId{};
      const EvalResult right = evaluate_expression(
          tree, right_id, scope, required, expected);
      if (right.status != EvalStatus::Ready) return failed_execution(right);
      if (!targets[index].has_value()) {
        stored_values.push_back(std::nullopt);
        continue;
      }

      const LocalTarget &target = *targets[index];
      ConstantValue stored = right.value;
      if (operation != TokenKind::Equal) {
        if (target.value.kind == ConstantKind::Unavailable) {
          return failed_execution(fail(
              tree.node(assignment.children[index]).range,
              "compound assignment reads an uninitialized local",
              required));
        }
        const TokenKind binary = binary_for_assignment(operation);
        if (binary == TokenKind::Invalid) {
          return failed_execution(fail(
              assignment.range,
              "invalid compile-time compound assignment",
              required));
        }
        if (std::optional<EvalResult> invalid = invalid_binary_types(
                binary,
                target.type,
                right.type,
                assignment.range,
                required)) {
          return failed_execution(std::move(*invalid));
        }
        const EvalResult result =
            runtime_type(target.type).kind == TypeKind::Float
            ? evaluate_typed_float_binary(
                  binary,
                  target.value,
                  right.value,
                  target.type,
                  assignment.range,
                  required)
            : evaluate_binary_values(
                  binary,
                  target.value,
                  right.value,
                  assignment.range,
                  required);
        if (result.status != EvalStatus::Ready) {
          return failed_execution(result);
        }
        const EvalResult converted = convert_to_type(
            result.value,
            target.type,
            true,
            assignment.range,
            required);
        if (converted.status != EvalStatus::Ready) {
          return failed_execution(converted);
        }
        stored = converted.value;
      } else if (target.type.is_valid() &&
                 target.type != semantic_.types.builtins().invalid &&
                 right.type != target.type) {
        if (right.value.kind == ConstantKind::Procedure) {
          return failed_execution(fail(
              assignment.range,
              "compile-time assignment has a different procedure type",
              required));
        }
        const EvalResult converted = convert_to_type(
            right.value,
            target.type,
            false,
            assignment.range,
            required);
        if (converted.status != EvalStatus::Ready) {
          return failed_execution(converted);
        }
        stored = converted.value;
      }
      stored_values.push_back(std::move(stored));
    }

    for (std::size_t index = 0; index < targets.size(); ++index) {
      if (!targets[index].has_value()) continue;
      if (!stored_values[index].has_value() ||
          !store_local_target(*targets[index], std::move(*stored_values[index]))) {
        return failed_execution(fail(
            tree.node(assignment.children[index]).range,
            "compile-time assignment target became unavailable",
            required));
      }
    }
    return {};
  }

  [[nodiscard]] EvalResult evaluate_statement_condition(
      const SyntaxTree &tree,
      NodeId condition,
      ScopeId scope,
      bool required) {
    const SyntaxNode &node = tree.node(condition);
    if (node.kind == NodeKind::ExpressionStatement && !node.children.empty()) {
      condition = node.children.front();
    }
    const EvalResult result =
        evaluate_expression(tree, condition, scope, required);
    if (result.status != EvalStatus::Ready) return result;
    if (result.value.kind != ConstantKind::Bool) {
      return fail(
          tree.node(condition).range,
          "compile-time control-flow condition must be bool",
          required);
    }
    return result;
  }

  [[nodiscard]] ExecutionResult execute_statement_list(
      const SyntaxTree &tree,
      const SyntaxNode &list,
      ScopeId scope,
      bool required) {
    for (NodeId statement : list.children) {
      const ExecutionResult result =
          execute_statement(tree, statement, scope, required);
      if (result.signal != ExecutionSignal::Normal) return result;
    }
    return {};
  }

  // Runs one lexical scope's saved calls in reverse source order.  Move the
  // list out before invoking anything: entering a callee may grow
  // local_frames_, so retaining references into the caller frame across that
  // operation would be unsafe.  A failed deferred call replaces the pending
  // return/break/continue just as a runtime trap prevents that exit.
  [[nodiscard]] ExecutionResult execute_current_defers(bool required) {
    if (local_frames_.empty() ||
        local_frames_.back().defer_scopes.empty()) {
      return {};
    }
    std::vector<PreparedProcedureCall> calls = std::move(
        local_frames_.back().defer_scopes.back());
    for (std::size_t remaining = calls.size(); remaining > 0; --remaining) {
      const EvalResult result = invoke_prepared_procedure_call(
          std::move(calls[remaining - 1]), required, true);
      if (result.status != EvalStatus::Ready) {
        return failed_execution(result);
      }
    }
    return {};
  }

  // Completes a scope after its statements have chosen their control result.
  // Defers run for every ordinary lexical exit, including return, break, and
  // continue.  Evaluation failures model traps and therefore do not start
  // additional cleanup work.
  [[nodiscard]] ExecutionResult finish_local_scope(
      ExecutionResult result,
      bool required) {
    if (result.signal != ExecutionSignal::Failed) {
      const ExecutionResult deferred = execute_current_defers(required);
      if (deferred.signal == ExecutionSignal::Failed) result = deferred;
    }
    local_frames_.back().defer_scopes.pop_back();
    local_frames_.back().scopes.pop_back();
    return result;
  }

  [[nodiscard]] ExecutionResult execute_block(
      const SyntaxTree &tree,
      NodeId block_id,
      ScopeId scope,
      bool required) {
    const SyntaxNode &block = tree.node(block_id);
    if (block.kind != NodeKind::Block || block.children.empty()) {
      return failed_execution(fail(
          block.range, "malformed compile-time procedure block", required));
    }
    local_frames_.back().scopes.emplace_back();
    local_frames_.back().defer_scopes.emplace_back();
    ExecutionResult result = execute_statement_list(
        tree, tree.node(block.children.front()), scope, required);
    return finish_local_scope(std::move(result), required);
  }

  [[nodiscard]] ExecutionResult execute_if(
      const SyntaxTree &tree,
      const SyntaxNode &statement,
      ScopeId scope,
      bool required) {
    if (statement.children.size() < 2) {
      return failed_execution(fail(
          statement.range, "malformed compile-time if statement", required));
    }
    const EvalResult condition = evaluate_statement_condition(
        tree, statement.children[0], scope, required);
    if (condition.status != EvalStatus::Ready) {
      return failed_execution(condition);
    }
    if (condition.value.boolean) {
      return execute_block(tree, statement.children[1], scope, required);
    }
    if (statement.children.size() == 2) return {};
    const SyntaxNode &alternative = tree.node(statement.children[2]);
    if (alternative.kind == NodeKind::IfStatement) {
      return execute_if(tree, alternative, scope, required);
    }
    return execute_block(tree, statement.children[2], scope, required);
  }

  [[nodiscard]] ExecutionResult execute_for(
      const SyntaxTree &tree,
      const SyntaxNode &statement,
      ScopeId scope,
      bool required) {
    if (statement.children.empty()) {
      return failed_execution(fail(
          statement.range, "malformed compile-time for statement", required));
    }
    const NodeId body = statement.children.back();
    const SyntaxNode &header = tree.node(statement.children.front());

    if (header.kind == NodeKind::IterationHeader) {
      if (header.children.size() != 1) {
        return failed_execution(fail(
            header.range, "malformed compile-time iteration header", required));
      }
      const EvalResult iterable = evaluate_expression(
          tree, header.children.front(), scope, required);
      if (iterable.status != EvalStatus::Ready) {
        return failed_execution(iterable);
      }
      if (iterable.value.kind != ConstantKind::Aggregate ||
          !iterable.type.is_valid() ||
          semantic_.types.type(iterable.type).kind != TypeKind::Array) {
        return failed_execution(fail(
            header.range,
            "compile-time iteration currently requires an array constant",
            required));
      }
      std::vector<std::string> bindings;
      const SyntaxNode &iterable_node = tree.node(header.children.front());
      for (std::uint32_t token_index = header.token_begin;
           token_index < iterable_node.token_begin;
           ++token_index) {
        const Token &token = tree.token(token_index);
        if (token_is_contextual_name(token.kind)) {
          bindings.emplace_back(sources_.text(token.range));
        }
      }
      if (bindings.empty() || bindings.size() > 2) {
        return failed_execution(fail(
            header.range,
            "compile-time array iteration requires one value and optional index binding",
            required));
      }

      const Type array_type = semantic_.types.type(iterable.type);
      local_frames_.back().scopes.emplace_back();
      if (bindings.front() != "_") {
        declare_local(
            bindings.front(),
            iterable.value.elements.empty()
                ? zero_value(array_type.element).value_or(ConstantValue{})
                : iterable.value.elements.front(),
            array_type.element);
      }
      if (bindings.size() == 2 && bindings[1] != "_") {
        declare_local(
            bindings[1],
            ConstantValue::make_integer(0),
            semantic_.types.builtins().usize_type);
      }
      for (std::size_t index = 0;
           index < iterable.value.elements.size();
           ++index) {
        if (!consume_execution_step(statement.range, required)) {
          local_frames_.back().scopes.pop_back();
          return failed_execution(EvalStatus::Error);
        }
        if (bindings.front() != "_") {
          (void)assign_local(bindings.front(), iterable.value.elements[index]);
        }
        if (bindings.size() == 2 && bindings[1] != "_") {
          (void)assign_local(
              bindings[1],
              ConstantValue::make_integer(BigInteger::from_u64(index)));
        }
        const ExecutionResult iteration =
            execute_block(tree, body, scope, required);
        if (iteration.signal == ExecutionSignal::Return ||
            iteration.signal == ExecutionSignal::Failed) {
          local_frames_.back().scopes.pop_back();
          return iteration;
        }
        if (iteration.signal == ExecutionSignal::Break) break;
      }
      local_frames_.back().scopes.pop_back();
      return {};
    }

    local_frames_.back().scopes.emplace_back();
    std::optional<NodeId> condition;
    std::optional<NodeId> post;
    if (header.kind == NodeKind::ForClause) {
      if (!header.children.empty()) {
        const ExecutionResult initialized =
            execute_statement(tree, header.children[0], scope, required);
        if (initialized.signal != ExecutionSignal::Normal) {
          local_frames_.back().scopes.pop_back();
          return initialized;
        }
      }
      if (header.children.size() >= 3) {
        condition = header.children[1];
        post = header.children[2];
      } else if (header.children.size() == 2) {
        const NodeKind second = tree.node(header.children[1]).kind;
        if (second == NodeKind::AssignmentStatement ||
            second == NodeKind::ExpressionStatement) {
          post = header.children[1];
        } else {
          condition = header.children[1];
        }
      }
    } else if (statement.children.size() > 1) {
      condition = statement.children.front();
    }

    while (true) {
      if (!consume_execution_step(statement.range, required)) {
        local_frames_.back().scopes.pop_back();
        return failed_execution(EvalStatus::Error);
      }
      if (condition.has_value()) {
        const EvalResult ready_condition = evaluate_statement_condition(
            tree, *condition, scope, required);
        if (ready_condition.status != EvalStatus::Ready) {
          local_frames_.back().scopes.pop_back();
          return failed_execution(ready_condition);
        }
        if (!ready_condition.value.boolean) break;
      }

      const ExecutionResult iteration =
          execute_block(tree, body, scope, required);
      if (iteration.signal == ExecutionSignal::Return ||
          iteration.signal == ExecutionSignal::Failed) {
        local_frames_.back().scopes.pop_back();
        return iteration;
      }
      if (iteration.signal == ExecutionSignal::Break) break;
      if (post.has_value()) {
        const ExecutionResult advanced =
            execute_statement(tree, *post, scope, required);
        if (advanced.signal == ExecutionSignal::Return ||
            advanced.signal == ExecutionSignal::Failed) {
          local_frames_.back().scopes.pop_back();
          return advanced;
        }
        if (advanced.signal == ExecutionSignal::Break) break;
      }
    }
    local_frames_.back().scopes.pop_back();
    return {};
  }

  [[nodiscard]] ExecutionResult execute_switch(
      const SyntaxTree &tree,
      const SyntaxNode &statement,
      ScopeId scope,
      bool required) {
    if (statement.children.empty()) {
      return failed_execution(fail(
          statement.range, "malformed compile-time switch", required));
    }
    const EvalResult subject =
        evaluate_expression(tree, statement.children.front(), scope, required);
    if (subject.status != EvalStatus::Ready) return failed_execution(subject);

    std::optional<NodeId> selected;
    std::optional<NodeId> fallback;
    std::optional<LocalBinding> selected_payload;
    const bool tagged_subject = subject.type.is_valid() &&
        runtime_type(subject.type).kind == TypeKind::TaggedUnion &&
        subject.value.kind == ConstantKind::Aggregate;
    for (std::size_t index = 1; index < statement.children.size(); ++index) {
      const SyntaxNode &switch_case = tree.node(statement.children[index]);
      if (switch_case.children.empty()) continue;
      const NodeId statements = switch_case.children.back();
      if (switch_case.children.size() == 1) {
        fallback = statements;
        continue;
      }
      for (std::size_t label = 0;
           label + 1 < switch_case.children.size();
           ++label) {
        const NodeId label_id = switch_case.children[label];
        const SyntaxNode &label_node = tree.node(label_id);
        if (tagged_subject &&
            label_node.kind == NodeKind::ContextualAlternativeExpression) {
          const std::optional<std::string> name = first_name(tree, label_node);
          const std::optional<std::size_t> alternative = name.has_value()
              ? aggregate_member_index(subject.type, *name)
              : std::nullopt;
          if (!alternative.has_value()) {
            return failed_execution(fail(
                label_node.range,
                "compile-time switch names no tagged-union alternative",
                required));
          }
          if (*alternative != subject.value.variant_index) continue;

          const Type union_type = runtime_type(subject.type);
          const TypeId payload_type = union_type.members[*alternative];
          const bool has_payload =
              semantic_.types.type(payload_type).kind != TypeKind::Void;
          if (has_payload) {
            if (label_node.children.size() != 1 ||
                subject.value.elements.size() != 1) {
              return failed_execution(fail(
                  label_node.range,
                  "compile-time union switch payload has the wrong shape",
                  required));
            }
            const SyntaxNode &binding_node =
                tree.node(label_node.children.front());
            const std::optional<std::string> binding =
                binding_node.kind == NodeKind::NameExpression
                ? final_name(tree, binding_node)
                : std::nullopt;
            if (!binding.has_value()) {
              return failed_execution(fail(
                  binding_node.range,
                  "compile-time union switch payload requires a binding name",
                  required));
            }
            if (*binding != "_") {
              selected_payload = LocalBinding{
                  *binding, subject.value.elements.front(), payload_type};
            }
          } else if (!label_node.children.empty()) {
            return failed_execution(fail(
                label_node.range,
                "payload-free union alternative cannot bind a payload",
                required));
          }
          selected = statements;
          break;
        }

        EvalResult candidate = evaluate_expression(
            tree, label_id, scope, required, subject.type);
        if (candidate.status != EvalStatus::Ready) {
          return failed_execution(candidate);
        }
        if (subject.type.is_valid() && candidate.type != subject.type) {
          candidate = convert_to_type(
              candidate.value,
              subject.type,
              false,
              label_node.range,
              required);
          if (candidate.status != EvalStatus::Ready) {
            return failed_execution(candidate);
          }
        }
        const EvalResult equal =
            subject.type.is_valid() &&
                runtime_type(subject.type).kind == TypeKind::Float
            ? evaluate_typed_float_binary(
                  TokenKind::EqualEqual,
                  subject.value,
                  candidate.value,
                  subject.type,
                  label_node.range,
                  required)
            : evaluate_binary_values(
                  TokenKind::EqualEqual,
                  subject.value,
                  candidate.value,
                  label_node.range,
                  required);
        if (equal.status != EvalStatus::Ready) return failed_execution(equal);
        if (equal.value.boolean) {
          selected = statements;
          break;
        }
      }
      if (selected.has_value()) break;
    }
    if (!selected.has_value()) selected = fallback;
    if (!selected.has_value()) return {};

    local_frames_.back().scopes.emplace_back();
    local_frames_.back().defer_scopes.emplace_back();
    if (selected_payload.has_value()) {
      declare_local(
          selected_payload->name,
          selected_payload->value,
          selected_payload->type);
    }
    ExecutionResult result = execute_statement_list(
        tree, tree.node(*selected), scope, required);
    result = finish_local_scope(std::move(result), required);
    if (result.signal == ExecutionSignal::Break) result.signal = ExecutionSignal::Normal;
    return result;
  }

  [[nodiscard]] ExecutionResult execute_when(
      const SyntaxTree &tree,
      const SyntaxNode &statement,
      ScopeId scope,
      bool required) {
    // Body-level when is evaluated directly.  The ordinary body checker later
    // uses the same constant engine and validates only the selected branch.
    return execute_if(tree, statement, scope, required);
  }

  [[nodiscard]] ExecutionResult execute_defer(
      const SyntaxTree &tree,
      const SyntaxNode &statement,
      ScopeId scope,
      bool required) {
    if (statement.children.size() != 1 ||
        tree.node(statement.children.front()).kind != NodeKind::CallExpression) {
      return failed_execution(fail(
          statement.range,
          "defer requires a compile-time procedure call",
          required));
    }
    if (local_frames_.empty() ||
        local_frames_.back().defer_scopes.empty()) {
      return failed_execution(fail(
          statement.range,
          "compile-time defer has no enclosing lexical scope",
          required));
    }

    PreparedProcedureCall prepared;
    const SyntaxNode &call = tree.node(statement.children.front());
    EvalResult preparation = prepare_procedure_call(
        tree, call, scope, required, prepared);
    if (preparation.status == EvalStatus::Pending && required) {
      preparation = fail(
          call.range,
          "defer target is not a compile-time procedure",
          true);
    }
    if (preparation.status != EvalStatus::Ready) {
      return failed_execution(preparation);
    }
    local_frames_.back().defer_scopes.back().push_back(
        std::move(prepared));
    return {};
  }

  [[nodiscard]] ExecutionResult execute_statement(
      const SyntaxTree &tree,
      NodeId statement_id,
      ScopeId scope,
      bool required) {
    const SyntaxNode &statement = tree.node(statement_id);
    if (!consume_execution_step(statement.range, required)) {
      return failed_execution(EvalStatus::Error);
    }
    switch (statement.kind) {
    case NodeKind::Block:
      return execute_block(tree, statement_id, scope, required);
    case NodeKind::StatementList:
      return execute_statement_list(tree, statement, scope, required);
    case NodeKind::Declaration:
      return execute_declaration(tree, statement, scope, required);
    case NodeKind::DeclarationStatement:
      if (!statement.children.empty()) {
        return execute_statement(tree, statement.children.front(), scope, required);
      }
      return {};
    case NodeKind::ExpressionStatement:
      if (!statement.children.empty()) {
        const SyntaxNode &expression = tree.node(statement.children.front());
        const EvalResult value = expression.kind == NodeKind::CallExpression
            ? evaluate_call_expression(tree, expression, scope, required, true)
            : evaluate_expression(
                  tree, statement.children.front(), scope, required);
        if (value.status != EvalStatus::Ready) return failed_execution(value);
      }
      return {};
    case NodeKind::AssignmentStatement:
      return execute_assignment(tree, statement, scope, required);
    case NodeKind::ReturnStatement: {
      ExecutionResult result;
      result.signal = ExecutionSignal::Return;
      if (!statement.children.empty()) {
        const TypeId expected = local_frames_.empty()
            ? TypeId{}
            : local_frames_.back().result_type;
        const EvalResult value =
            evaluate_expression(
                tree,
                statement.children.front(),
                scope,
                required,
                expected);
        if (value.status != EvalStatus::Ready) return failed_execution(value);
        result.value = value.value;
        result.type = value.type;
      }
      return result;
    }
    case NodeKind::BreakStatement: {
      ExecutionResult result;
      result.signal = ExecutionSignal::Break;
      return result;
    }
    case NodeKind::ContinueStatement: {
      ExecutionResult result;
      result.signal = ExecutionSignal::Continue;
      return result;
    }
    case NodeKind::IfStatement:
      return execute_if(tree, statement, scope, required);
    case NodeKind::ForStatement:
      return execute_for(tree, statement, scope, required);
    case NodeKind::SwitchStatement:
      return execute_switch(tree, statement, scope, required);
    case NodeKind::WhenStatement:
      return execute_when(tree, statement, scope, required);
    case NodeKind::DenyStatement:
      if (!statement.children.empty()) {
        return execute_block(tree, statement.children.back(), scope, required);
      }
      return {};
    case NodeKind::UncheckedStatement:
      if (!statement.children.empty()) {
        return execute_block(tree, statement.children.front(), scope, required);
      }
      return {};
    case NodeKind::Judgment:
      return {};
    case NodeKind::DeferStatement:
      return execute_defer(tree, statement, scope, required);
    case NodeKind::AsmStatement:
    case NodeKind::AsmExpression:
      return failed_execution(fail(
          statement.range,
          "native assembly is unavailable during compile-time evaluation",
          required));
    case NodeKind::SynthesisStatement:
      if (synthesis_mode_ == CompileTimeSynthesisMode::Discover) {
        for (SymbolId procedure : active_procedures_) {
          remember_compile_time_procedure(procedure);
        }
        return failed_execution(blocked_by_synthesis());
      }
      return failed_execution(fail(
          statement.range,
          "unresolved synthesis is unavailable during compile-time evaluation",
          required));
    case NodeKind::SynthesisExpression:
      return failed_execution(unresolved_synthesis_expression(
          tree, statement_id, scope, {}, required));
    default:
      return failed_execution(fail(
          statement.range,
          "statement form is unavailable during compile-time evaluation",
          required));
    }
  }

  // Resolves one package constant lazily. The Evaluating state detects cycles;
  // successfully computed result types are written back to the symbol for later
  // expected-type checking.
  [[nodiscard]] EvalResult evaluate_binding(SymbolId id, bool required) {
    if (static_cast<std::size_t>(id.value) >= states_.size()) return pending();
    BindingState &state = states_[id.value];
    if (state == BindingState::Ready) {
      return ready(values_[id.value], semantic_.symbols.symbol(id).type);
    }
    if (state == BindingState::Pending) return pending();
    if (state == BindingState::BlockedBySynthesis) {
      return blocked_by_synthesis();
    }
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
      return ready(imported.constant, initial.type);
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
    if (binding_dependency_depth_ >= kMaximumBindingDependencyDepth) {
      // A type-context probe may enter this same dependency graph with
      // `required == false`. Resource exhaustion is not an ordinary failed
      // probe: suppressing it would leave only cascading "not evaluable"
      // diagnostics and hide the implementation limit that stopped us.
      return fail(
          initial.name_range,
          "compile-time constant dependency depth exceeds the implementation "
          "limit of " +
              std::to_string(kMaximumBindingDependencyDepth),
          true);
    }

    // No early return follows this increment: the common result path below
    // always restores the depth before publishing a binding state.
    state = BindingState::Evaluating;
    ++binding_dependency_depth_;
    const NodeId expression = declaration.children.back();
    // Package declarations need their file-local import scope. A lexical
    // constant instead evaluates where it was declared so earlier constants
    // and enclosing compile-time bindings remain visible.
    const ScopeKind owner_kind = semantic_.symbols.scope(initial.scope).kind;
    const ScopeId evaluation_scope = owner_kind == ScopeKind::Block
        ? initial.scope
        : file_scope(tree->file());
    const EvalResult result = evaluate_expression(
        *tree, expression, evaluation_scope, required);
    --binding_dependency_depth_;
    if (result.status == EvalStatus::Ready) {
      states_[id.value] = BindingState::Ready;
      values_[id.value] = result.value;
      TypeId type = result.type;
      if (!type.is_valid() && result.value.kind == ConstantKind::Bool) {
        type = semantic_.types.builtins().bool_type;
      } else if (!type.is_valid() && result.value.kind == ConstantKind::Integer) {
        const SyntaxNode &expression_node = tree->node(expression);
        const bool rune = expression_node.kind == NodeKind::LiteralExpression &&
            expression_node.token_begin < expression_node.token_end &&
            tree->token(expression_node.token_begin).kind == TokenKind::RuneLiteral;
        type = rune
            ? semantic_.types.builtins().rune_type
            : semantic_.types.builtins().untyped_integer;
      } else if (!type.is_valid() && result.value.kind == ConstantKind::Float) {
        type = semantic_.types.builtins().untyped_float;
      } else if (!type.is_valid() && result.value.kind == ConstantKind::String) {
        type = semantic_.types.builtins().string_type;
      }
      if (type.is_valid()) semantic_.symbols.symbol_mut(id).type = type;
      return result;
    }
    if (result.status == EvalStatus::Error) {
      states_[id.value] = BindingState::Error;
    } else if (result.status == EvalStatus::BlockedBySynthesis) {
      states_[id.value] = BindingState::BlockedBySynthesis;
    } else {
      states_[id.value] = BindingState::Pending;
    }
    return result;
  }

  // Evaluates the deterministic expression subset in strict source order.
  // Unsupported forms return Pending; the final fixed-point caller supplies the
  // generic "not a ready constant" diagnostic rather than guessing semantics.
  [[nodiscard]] EvalResult evaluate_expression(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      bool required,
      TypeId expected = {}) {
    const SyntaxNode &node = tree.node(expression_id);
    switch (node.kind) {
    case NodeKind::LiteralExpression: {
      if (node.token_begin >= node.token_end) return pending();
      const Token &token = tree.token(node.token_begin);
      if (token.kind == TokenKind::KeywordTrue) {
        return ready(
            ConstantValue::make_bool(true),
            semantic_.types.builtins().bool_type);
      }
      if (token.kind == TokenKind::KeywordFalse) {
        return ready(
            ConstantValue::make_bool(false),
            semantic_.types.builtins().bool_type);
      }
      if (token.kind == TokenKind::KeywordNil) {
        return ready(ConstantValue::make_nil(), expected);
      }
      if (token.kind == TokenKind::IntegerLiteral) {
        const std::optional<BigInteger> value =
            integer_literal(sources_.text(token.range));
        if (!value.has_value()) {
          return fail(token.range, "invalid integer literal", required);
        }
        return ready(
            ConstantValue::make_integer(*value),
            semantic_.types.builtins().untyped_integer);
      }
      if (token.kind == TokenKind::FloatLiteral) {
        const std::optional<ExactRational> value =
            ExactRational::parse_decimal(sources_.text(token.range));
        if (!value.has_value()) {
          return fail(token.range, "invalid or excessive decimal floating literal", required);
        }
        return ready(
            ConstantValue::make_float(*value),
            semantic_.types.builtins().untyped_float);
      }
      if (token.kind == TokenKind::RuneLiteral) {
        const std::optional<std::uint32_t> value =
            rune_literal(sources_.text(token.range));
        if (!value.has_value()) {
          return fail(token.range, "invalid rune literal", required);
        }
        return ready(
            ConstantValue::make_integer(BigInteger::from_u64(*value)),
            semantic_.types.builtins().rune_type);
      }
      if (token.kind == TokenKind::StringLiteral ||
          token.kind == TokenKind::RawStringLiteral) {
        const std::optional<std::string> value =
            string_literal(sources_.text(token.range), token.kind);
        if (!value.has_value()) {
          return fail(token.range, "unsupported string escape in constant", required);
        }
        return ready(
            ConstantValue::make_string(*value),
            semantic_.types.builtins().string_type);
      }
      return pending();
    }

    case NodeKind::NameExpression: {
      const std::optional<std::string> name = final_name(tree, node);
      if (!name.has_value()) return pending();
      if (*name == "target") return ready(ConstantValue::make_target());
      if (const LocalBinding *local = local_binding(*name)) {
        if (contains_uninitialized(local->value)) {
          return fail(
              node.range,
              "compile-time evaluation reads an uninitialized local",
              required);
        }
        return ready(local->value, local->type);
      }
      const std::optional<SymbolId> symbol = semantic_.symbols.lookup(scope, *name);
      if (!symbol.has_value()) return pending();
      const Symbol &binding = semantic_.symbols.symbol(*symbol);
      if (binding.kind == SymbolKind::Procedure) {
        return ready(procedure_value(*symbol), binding.type);
      }
      // Procedure value parameters are not package constants and therefore do
      // not have declaration syntax for evaluate_binding. An instantiation may
      // supply their exact values through this phase-local overlay instead.
      if (local_constants_ != nullptr) {
        if (const ConstantValue *local = local_constants_->find(*symbol)) {
          return ready(*local, semantic_.symbols.symbol(*symbol).type);
        }
      }
      return evaluate_binding(*symbol, required);
    }

    case NodeKind::ContextualAlternativeExpression: {
      const std::optional<std::string> name = first_name(tree, node);
      if (!name.has_value()) return pending();
      TypeId payload_type;
      if (expected.is_valid() &&
          runtime_type(expected).kind == TypeKind::TaggedUnion) {
        const std::optional<std::size_t> alternative =
            aggregate_member_index(expected, *name);
        const Type union_type = runtime_type(expected);
        if (alternative.has_value() &&
            *alternative < union_type.members.size()) {
          payload_type = union_type.members[*alternative];
        }
      }
      std::vector<ConstantValue> payload;
      for (NodeId child : node.children) {
        const EvalResult value = evaluate_expression(
            tree, child, scope, required, payload_type);
        if (value.status != EvalStatus::Ready) return value;
        payload.push_back(value.value);
      }
      return ready(ConstantValue::make_enum_label(*name, std::move(payload)));
    }

    case NodeKind::MemberExpression: {
      if (node.children.empty()) return pending();
      if (const std::optional<SymbolId> imported = imported_member(tree, node, scope)) {
        const Symbol &binding = semantic_.symbols.symbol(*imported);
        if (binding.kind == SymbolKind::Procedure) {
          return ready(procedure_value(*imported), binding.type);
        }
        return evaluate_binding(*imported, required);
      }
      const EvalResult base = evaluate_expression(tree, node.children.front(), scope, required);
      if (base.status != EvalStatus::Ready) return base;
      const std::optional<std::string> member = final_name(tree, node);
      if (base.value.kind == ConstantKind::Target && member.has_value()) {
        return evaluate_target_member(*member, node.range, required);
      }
      if (base.value.kind == ConstantKind::Aggregate && base.type.is_valid()) {
        const Type aggregate = semantic_.types.type(base.type);
        std::optional<std::size_t> index;
        if (aggregate.kind == TypeKind::Struct && member.has_value()) {
          index = aggregate_member_index(base.type, *member);
        } else if (aggregate.kind == TypeKind::Tuple) {
          const SyntaxNode &base_node = tree.node(node.children.front());
          for (std::uint32_t token_index = base_node.token_end;
               token_index < node.token_end;
               ++token_index) {
            const Token &token = tree.token(token_index);
            if (token.kind != TokenKind::IntegerLiteral) continue;
            const std::optional<BigInteger> parsed =
                integer_literal(sources_.text(token.range));
            const std::optional<std::uint64_t> value =
                parsed.has_value() ? parsed->to_u64() : std::nullopt;
            if (value.has_value()) index = static_cast<std::size_t>(*value);
            break;
          }
        }
        if (!index.has_value() || *index >= base.value.elements.size() ||
            *index >= aggregate.members.size()) {
          return fail(
              node.range,
              "constant aggregate member is out of range or unknown",
              required);
        }
        const ConstantValue &selected = base.value.elements[*index];
        if (contains_uninitialized(selected)) {
          return fail(
              node.range,
              "compile-time evaluation reads an uninitialized member",
              required);
        }
        return ready(selected, aggregate.members[*index]);
      }
      return pending();
    }

    case NodeKind::UnaryExpression: {
      if (node.children.empty()) return pending();
      const EvalResult operand =
          evaluate_expression(
              tree, node.children.front(), scope, required, expected);
      if (operand.status != EvalStatus::Ready) return operand;
      const TokenKind operation = tree.token(node.token_begin).kind;
      if (std::optional<EvalResult> invalid = invalid_unary_type(
              operation, operand.type, node.range, required)) {
        return std::move(*invalid);
      }
      if (operation == TokenKind::Bang && operand.value.kind == ConstantKind::Bool) {
        return ready(
            ConstantValue::make_bool(!operand.value.boolean),
            operand.type);
      }
      if (operand.value.kind != ConstantKind::Integer) {
        if (operand.value.kind == ConstantKind::Float && operation == TokenKind::Plus) {
          return operand;
        }
        if (operand.value.kind == ConstantKind::Float && operation == TokenKind::Minus) {
          if (operand.value.float_bit_width != 0) {
            const std::uint64_t sign = std::uint64_t{1} <<
                (operand.value.float_bit_width - 1U);
            const std::optional<ConstantValue> negated = float_from_bits(
                operand.value.float_bits ^ sign,
                operand.value.float_bit_width);
            if (!negated.has_value()) {
              return fail(
                  node.range, "typed float negation failed", required);
            }
            return ready(*negated, operand.type);
          }
          EvalResult result =
              bounded_float(operand.value.floating.negated(), node.range, required);
          result.type = operand.type;
          return result;
        }
        return fail(node.range, "unary operator uses an incompatible constant", required);
      }
      if (operation == TokenKind::Plus) return operand;
      if (operation == TokenKind::Minus) {
        EvalResult result =
            bounded_integer(operand.value.integer.negated(), node.range, required);
        if (result.status == EvalStatus::Ready && concrete_numeric(operand.type)) {
          return convert_to_type(
              result.value, operand.type, true, node.range, required);
        }
        result.type = operand.type;
        return result;
      }
      if (operation == TokenKind::Tilde) {
        EvalResult result =
            bounded_integer(operand.value.integer.bitwise_not(), node.range, required);
        if (result.status == EvalStatus::Ready && concrete_numeric(operand.type)) {
          return convert_to_type(
              result.value, operand.type, true, node.range, required);
        }
        result.type = operand.type;
        return result;
      }
      return pending();
    }

    case NodeKind::BinaryExpression:
      return evaluate_binary(tree, node, scope, required, expected);

    case NodeKind::GroupExpression:
      if (!node.children.empty()) {
        return evaluate_expression(
            tree, node.children.front(), scope, required, expected);
      }
      return pending();

    case NodeKind::TupleExpression: {
      std::vector<ConstantValue> elements;
      std::vector<TypeId> member_types;
      std::vector<TypeId> expected_members;
      if (expected.is_valid() &&
          semantic_.types.type(expected).kind == TypeKind::Tuple) {
        expected_members = semantic_.types.type(expected).members;
      }
      elements.reserve(node.children.size());
      member_types.reserve(node.children.size());
      for (std::size_t index = 0; index < node.children.size(); ++index) {
        const NodeId child = node.children[index];
        const TypeId member_context = index < expected_members.size()
            ? expected_members[index]
            : TypeId{};
        const EvalResult member = evaluate_expression(
            tree, child, scope, required, member_context);
        if (member.status != EvalStatus::Ready) return member;
        // A contextual alternative deliberately remains a symbolic EnumLabel
        // until the enclosing value is converted. Its explicit tuple member
        // context is nevertheless the concrete type of this position. Retain
        // that type so the tuple can be formed without prematurely converting
        // the label (compile-time switch comparison still owns that boundary).
        TypeId member_type = member.type.is_valid()
            ? member.type
            : (member_context.is_valid()
                   ? member_context
                   : default_value_type(member.value));
        if (!member_type.is_valid() ||
            member_type == semantic_.types.builtins().invalid) {
          return fail(
              tree.node(child).range,
              "tuple constant member requires a concrete type",
              required);
        }
        elements.push_back(member.value);
        member_types.push_back(member_type);
      }
      const TypeId tuple_type = !expected_members.empty() &&
              expected_members.size() == member_types.size()
          ? expected
          : semantic_.types.tuple(member_types);
      return ready(
          ConstantValue::make_aggregate(std::move(elements)), tuple_type);
    }

    case NodeKind::CompositeExpression: {
      if (node.children.empty()) return pending();
      std::optional<TypeId> composite_type =
          type_value(tree, node.children.front(), scope);
      // Parametric applications have already been resolved in an explicitly
      // typed global declaration, but this evaluator's small type-expression
      // reader does not recreate applications. The declared expected type is
      // authoritative and is checked again by global-initializer conversion.
      if (!composite_type.has_value() && expected.is_valid() &&
          semantic_.types.type(expected).kind != TypeKind::Invalid) {
        composite_type = expected;
      }
      if (!composite_type.has_value()) {
        return fail(
            tree.node(node.children.front()).range,
            "constant composite literal requires a concrete type",
            required);
      }
      const Type composite = semantic_.types.type(*composite_type);
      if (composite.kind != TypeKind::Array &&
          composite.kind != TypeKind::Simd &&
          composite.kind != TypeKind::Struct &&
          composite.kind != TypeKind::RawUnion) {
        return fail(
            node.range,
            "type does not support a constant composite literal",
            required);
      }
      const std::optional<ConstantValue> zero = zero_value(*composite_type);
      if (!zero.has_value() || zero->kind != ConstantKind::Aggregate) {
        return fail(
            node.range,
            "constant composite type has no complete zero value",
            required);
      }
      ConstantValue result = *zero;
      std::vector<bool> initialized(
          composite.kind == TypeKind::Array || composite.kind == TypeKind::Simd
              ? static_cast<std::size_t>(composite.element_count)
              : composite.members.size(),
          false);
      std::size_t positional_index = 0;
      std::size_t explicit_union_members = 0;
      for (std::size_t child_index = 1;
           child_index < node.children.size();
           ++child_index) {
        const SyntaxNode &element = tree.node(node.children[child_index]);
        if (element.children.empty()) continue;
        const SyntaxNode &value_node = tree.node(element.children.front());
        bool keyed = false;
        std::optional<std::string> key;
        for (std::uint32_t token_index = element.token_begin;
             token_index < value_node.token_begin;
             ++token_index) {
          const Token &token = tree.token(token_index);
          if (token.kind == TokenKind::Equal) keyed = true;
          if (!key.has_value() && token_is_contextual_name(token.kind)) {
            key = std::string(sources_.text(token.range));
          }
        }

        std::optional<std::size_t> destination;
        if (keyed) {
          if (!key.has_value() || composite.kind == TypeKind::Array ||
              composite.kind == TypeKind::Simd) {
            return fail(
                element.range,
                "keyed constant element requires a named aggregate member",
                required);
          }
          destination = aggregate_member_index(*composite_type, *key);
          if (!destination.has_value()) {
            return fail(
                element.range,
                "constant composite literal names no matching member",
                required);
          }
        } else {
          if (composite.kind == TypeKind::Struct ||
              composite.kind == TypeKind::RawUnion) {
            return fail(
                element.range,
                composite.kind == TypeKind::Struct
                    ? "constant struct composite elements must name a field"
                    : "constant raw union composite element must name a field",
                required);
          }
          destination = positional_index;
          ++positional_index;
        }
        if (!destination.has_value() || *destination >= initialized.size()) {
          return fail(
              element.range,
              "constant composite literal has too many elements",
              required);
        }
        if (initialized[*destination]) {
          return fail(
              element.range,
              "constant composite member is initialized more than once",
              required);
        }
        initialized[*destination] = true;

        const TypeId element_type =
            composite.kind == TypeKind::Array || composite.kind == TypeKind::Simd
            ? composite.element
            : composite.members[*destination];
        const EvalResult evaluated = evaluate_expression(
            tree,
            element.children.front(),
            scope,
            required,
            element_type);
        if (evaluated.status != EvalStatus::Ready) return evaluated;
        if (evaluated.type.is_valid() &&
            evaluated.type != semantic_.types.builtins().invalid &&
            evaluated.type != element_type) {
          const Type source_type = runtime_type(evaluated.type);
          const Type target_type = runtime_type(element_type);
          const bool untyped_numeric =
              source_type.kind == TypeKind::UntypedInteger ||
              source_type.kind == TypeKind::UntypedFloat;
          if ((!untyped_numeric && source_type.kind != target_type.kind) ||
              (evaluated.value.kind == ConstantKind::Procedure &&
               evaluated.type != element_type)) {
            return fail(
                value_node.range,
                "constant composite member has the wrong type",
                required);
          }
        }
        const EvalResult converted = convert_to_type(
            evaluated.value,
            element_type,
            false,
            value_node.range,
            required);
        if (converted.status != EvalStatus::Ready) return converted;

        if (composite.kind == TypeKind::RawUnion) {
          ++explicit_union_members;
          if (explicit_union_members > 1) {
            return fail(
                element.range,
                "raw union constant initializes more than one member",
                required);
          }
          result = ConstantValue::make_aggregate(
              {converted.value}, *destination);
        } else {
          result.elements[*destination] = converted.value;
        }
      }
      if (composite.kind == TypeKind::RawUnion &&
          explicit_union_members != 1) {
        return fail(
            node.range,
            "constant raw union composite literal must initialize exactly one field",
            required);
      }
      return ready(std::move(result), *composite_type);
    }

    case NodeKind::ConditionalExpression: {
      if (node.children.size() != 3) return pending();
      const EvalResult condition =
          evaluate_expression(tree, node.children[1], scope, required);
      if (condition.status != EvalStatus::Ready) return condition;
      if (condition.value.kind != ConstantKind::Bool) {
        return fail(node.range, "constant conditional requires a bool condition", required);
      }
      const NodeId selected = condition.value.boolean
          ? node.children[0]
          : node.children[2];
      TypeId selected_context = expected;
      if (!selected_context.is_valid() &&
          needs_value_context(tree, selected)) {
        const NodeId other = condition.value.boolean
            ? node.children[2]
            : node.children[0];
        const TypeId hint = declared_value_type_hint(tree, other, scope);
        if (accepts_context_hint(tree, selected, hint)) {
          selected_context = hint;
        }
      }
      EvalResult result = evaluate_expression(
          tree, selected, scope, required, selected_context);
      if (result.status == EvalStatus::Ready && selected_context.is_valid() &&
          needs_value_context(tree, selected) && !result.type.is_valid()) {
        // Contextual alternatives historically carry their owner separately
        // from ConstantValue so switch-label comparison can resolve labels in
        // the subject domain. Attach the inferred owner only to the complete
        // conditional result, where it becomes the declaration's value type.
        result.type = selected_context;
      }
      return result;
    }

    case NodeKind::BracketExpression: {
      if (node.children.size() != 2) return pending();
      const EvalResult base =
          evaluate_expression(tree, node.children.front(), scope, required);
      if (base.status != EvalStatus::Ready) return base;
      const EvalResult index =
          evaluate_expression(tree, node.children[1], scope, required);
      if (index.status != EvalStatus::Ready) return index;
      if (index.value.kind != ConstantKind::Integer) {
        return fail(node.range, "constant index must be an integer", required);
      }
      if (!usize_index_operand(index.type)) {
        return fail(
            tree.node(node.children[1]).range,
            "constant index must have type usize",
            required);
      }
      const std::optional<std::uint64_t> position = index.value.integer.to_u64();
      if (!position.has_value()) {
        return fail(node.range, "constant index is negative or excessive", required);
      }
      if (base.value.kind == ConstantKind::Aggregate && base.type.is_valid()) {
        const Type aggregate = runtime_type(base.type);
        if (aggregate.kind != TypeKind::Array &&
            aggregate.kind != TypeKind::Simd) {
          return fail(
              node.range,
              "compile-time indexing requires an array, SIMD value, or string",
              required);
        }
        if (*position >= base.value.elements.size()) {
          return fail(
              node.range,
              "constant index " + std::to_string(*position) +
                  " is out of bounds for length " +
                  std::to_string(base.value.elements.size()),
              required);
        }
        const ConstantValue &selected =
            base.value.elements[static_cast<std::size_t>(*position)];
        if (contains_uninitialized(selected)) {
          return fail(
              node.range,
              "compile-time evaluation reads an uninitialized element",
              required);
        }
        return ready(selected, aggregate.element);
      }
      if (base.value.kind == ConstantKind::String &&
          *position < base.value.text.size()) {
        return ready(
            ConstantValue::make_integer(BigInteger::from_u64(
                static_cast<unsigned char>(base.value.text[*position]))),
            semantic_.types.builtins().u8_type);
      }
      if (base.value.kind == ConstantKind::String) {
        return fail(
            node.range,
            "constant index " + std::to_string(*position) +
                " is out of bounds for length " +
                std::to_string(base.value.text.size()),
            required);
      }
      return fail(
          node.range,
          "compile-time indexing requires an array, SIMD value, or string",
          required);
    }

    case NodeKind::SliceExpression: {
      if (node.children.empty()) return pending();
      const EvalResult base =
          evaluate_expression(tree, node.children.front(), scope, required);
      if (base.status != EvalStatus::Ready) return base;

      // A fixed-array constant has no source-level storage identity, and a
      // slice would borrow storage. A string constant is different: its value
      // already is an immutable byte sequence, so selecting a subrange remains
      // a complete string constant and needs no runtime address. Slice only
      // that closed value kind here.
      if (base.value.kind != ConstantKind::String) {
        return fail(
            node.range,
            "compile-time slicing requires a string constant",
            required);
      }

      std::optional<std::uint32_t> colon;
      for (std::uint32_t token_index = node.token_begin;
           token_index < node.token_end;
           ++token_index) {
        if (tree.token(token_index).kind == TokenKind::Colon) {
          colon = token_index;
          break;
        }
      }
      if (!colon.has_value()) return pending();

      std::uint64_t low = 0;
      std::uint64_t high = base.value.text.size();
      for (std::size_t child_index = 1;
           child_index < node.children.size();
           ++child_index) {
        const NodeId child = node.children[child_index];
        const EvalResult bound =
            evaluate_expression(tree, child, scope, required);
        if (bound.status != EvalStatus::Ready) return bound;
        if (bound.value.kind != ConstantKind::Integer) {
          return fail(
              tree.node(child).range,
              "constant slice bound must be an integer",
              required);
        }
        if (!usize_index_operand(bound.type)) {
          return fail(
              tree.node(child).range,
              "constant slice bound must have type usize",
              required);
        }
        const std::optional<std::uint64_t> value =
            bound.value.integer.to_u64();
        if (!value.has_value()) {
          return fail(
              tree.node(child).range,
              "constant slice bound is negative or excessive",
              required);
        }
        if (tree.node(child).token_end <= *colon) low = *value;
        else high = *value;
      }

      if (low > high || high > base.value.text.size()) {
        return fail(
            node.range,
            "constant slice bounds [" + std::to_string(low) + ":" +
                std::to_string(high) + "] are invalid for length " +
                std::to_string(base.value.text.size()),
            required);
      }

      // std::string stores the exact Draft string bytes, including embedded
      // NUL and non-ASCII UTF-8. Byte offsets and byte counts therefore match
      // the language's immutable-byte-view semantics without host decoding.
      const TypeId result_type = base.type.is_valid()
          ? base.type
          : semantic_.types.builtins().string_type;
      return ready(
          ConstantValue::make_string(base.value.text.substr(
              static_cast<std::size_t>(low),
              static_cast<std::size_t>(high - low))),
          result_type);
    }

    case NodeKind::CallExpression: {
      return evaluate_call_expression(tree, node, scope, required, false);
    }

    case NodeKind::DenyExpression:
      if (!node.children.empty()) {
        return evaluate_expression(
            tree, node.children.back(), scope, required, expected);
      }
      return pending();

    case NodeKind::SynthesisExpression:
      return unresolved_synthesis_expression(
          tree, expression_id, scope, expected, required);

    default:
      return pending();
    }
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  SemanticPackage &semantic_;
  const TargetFacts &target_;
  CompileTimeSynthesisMode synthesis_mode_ = CompileTimeSynthesisMode::Reject;
  bool diagnose_unready_ = false;
  DiagnosticSink &diagnostics_;
  const ConstantTable *local_constants_ = nullptr;
  const std::vector<ConstantTypeBinding> *local_types_ = nullptr;
  std::vector<BindingState> states_;
  std::vector<ConstantValue> values_;
  std::vector<LocalFrame> local_frames_;
  // active_procedures_ mirrors local_frames_ only during procedure execution
  // and lets a reached `...` defer context construction to BodyChecker.
  std::vector<SymbolId> active_procedures_;
  std::vector<SymbolId> compile_time_procedures_;
  std::size_t execution_steps_remaining_ = kMaximumExecutionSteps;
  std::size_t binding_dependency_depth_ = 0;
  std::size_t procedure_call_depth_ = 0;
  bool execution_limit_reported_ = false;
};

} // namespace

ConstantValue ConstantValue::make_bool(bool value) {
  ConstantValue result;
  result.kind = ConstantKind::Bool;
  result.boolean = value;
  return result;
}

ConstantValue ConstantValue::make_nil() {
  ConstantValue result;
  result.kind = ConstantKind::Nil;
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

ConstantValue ConstantValue::make_ieee_float(
    std::uint32_t bit_width,
    std::uint64_t bits,
    ExactRational finite_value) {
  ConstantValue result;
  result.kind = ConstantKind::Float;
  result.floating = std::move(finite_value);
  result.float_bit_width = bit_width;
  result.float_bits = bits;
  return result;
}

ConstantValue ConstantValue::make_string(std::string value) {
  ConstantValue result;
  result.kind = ConstantKind::String;
  result.text = std::move(value);
  return result;
}

ConstantValue ConstantValue::make_aggregate(
    std::vector<ConstantValue> elements, std::uint64_t variant_index) {
  ConstantValue result;
  result.kind = ConstantKind::Aggregate;
  result.elements = std::move(elements);
  result.variant_index = variant_index;
  return result;
}

ConstantValue ConstantValue::make_enum_label(
    std::string value, std::vector<ConstantValue> payload) {
  ConstantValue result;
  result.kind = ConstantKind::EnumLabel;
  result.text = std::move(value);
  result.elements = std::move(payload);
  return result;
}

ConstantValue ConstantValue::make_procedure(
    std::uint32_t symbol_index,
    std::string name,
    std::string root_identity,
    std::string root_relative_path) {
  ConstantValue result;
  result.kind = ConstantKind::Procedure;
  result.symbol_index = symbol_index;
  result.text = std::move(name);
  result.root_identity = std::move(root_identity);
  result.root_relative_path = std::move(root_relative_path);
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
    CompileTimeSynthesisMode synthesis_mode,
    bool diagnose_unready,
    DiagnosticSink &diagnostics) {
  ConstantEvaluator evaluator(
      sources,
      loaded,
      package,
      target,
      synthesis_mode,
      diagnose_unready,
      diagnostics);
  return evaluator.run(selections);
}

std::optional<ConstantValue> evaluate_constant_expression(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    DiagnosticSink &diagnostics,
    const ConstantTable *local_constants,
    const std::vector<ConstantTypeBinding> *local_types) {
  const std::optional<EvaluatedConstant> result =
      evaluate_typed_constant_expression(
          sources,
          loaded,
          package,
          target,
          tree,
          expression,
          scope,
          diagnostics,
          local_constants,
          local_types,
          {});
  return result.has_value()
      ? std::optional<ConstantValue>(result->value)
      : std::nullopt;
}

std::optional<EvaluatedConstant> evaluate_typed_constant_expression(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    DiagnosticSink &diagnostics,
    const ConstantTable *local_constants,
    const std::vector<ConstantTypeBinding> *local_types,
    TypeId expected) {
  ConstantEvaluator evaluator(
      sources,
      loaded,
      package,
      target,
      CompileTimeSynthesisMode::Reject,
      true,
      diagnostics,
      local_constants,
      local_types);
  return evaluator.evaluate_required_expression(
      tree, expression, scope, expected);
}

CompileTimeExpressionDiscoveryResult discover_typed_constant_expression(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    const ConstantTable *local_constants,
    const std::vector<ConstantTypeBinding> *local_types,
    TypeId expected) {
  DiagnosticSink ignored_diagnostics;
  ConstantEvaluator evaluator(
      sources,
      loaded,
      package,
      target,
      CompileTimeSynthesisMode::Discover,
      false,
      ignored_diagnostics,
      local_constants,
      local_types);
  return evaluator.discover_required_expression(
      tree, expression, scope, expected);
}

} // namespace draft
