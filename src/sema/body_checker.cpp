// Runtime body checking and structured typed-HIR construction.

#include "sema/body_checker.h"

#include "sema/ieee_float.h"
#include "sema/initialization.h"
#include "sema/runtime_context.h"
#include "sema/type_resolver.h"
#include "sema/target_validation.h"
#include "syntax/literal.h"
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
         kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
         kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory;
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

[[nodiscard]] std::optional<TypeKind> nominal_type_kind(NodeKind kind) {
  switch (kind) {
  case NodeKind::StructType: return TypeKind::Struct;
  case NodeKind::EnumType: return TypeKind::Enum;
  case NodeKind::TaggedUnionType: return TypeKind::TaggedUnion;
  case NodeKind::RawUnionType: return TypeKind::RawUnion;
  default: return std::nullopt;
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

struct TypeSubstitution {
  TypeId parameter;
  TypeId replacement;
};

// Value substitutions use the actual ValueParameter symbol as their key. Type
// parameters have unique TypeIds, while value parameters may share the same
// declared type (`N: usize, M: usize`) and therefore cannot be keyed by type.
// The exact value remains arbitrary precision until a dependent layout converts
// it to the target-independent u64 array/SIMD count domain.
struct ValueSubstitution {
  SymbolId parameter;
  ConstantValue value;
  // During non-lowered template checking, one callee value parameter may map
  // to a caller value parameter rather than an integer. Concrete instances
  // leave this invalid and carry the exact value above.
  SymbolId symbolic_parameter;
};

// One source template can produce several concrete procedure bodies. Instances
// retain both substitution kinds here while the permanent semantic graph owns
// the concrete symbol, signature, and cloned runtime-parameter scope used by
// later passes. Equality is semantic: types compare by canonical TypeId and
// values compare by their exact ConstantValue representation.
struct ProcedureInstance {
  SymbolId source;
  SymbolId symbol;
  std::vector<TypeSubstitution> type_substitutions;
  std::vector<ValueSubstitution> value_substitutions;
  bool checked = false;
};

// BodyChecker is one sequential phase context. It owns only the HIR under
// construction; source, syntax, semantic tables, constants, and selections are
// caller-owned. SymbolTable and the constant table may grow, so operations
// retain stable IDs and copy source records instead of holding element
// references across declarations.
class BodyChecker {
public:
  BodyChecker(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      const ConditionalSelections &selections,
      SemanticPackage &semantic,
      ConstantTable &constants,
      const TargetFacts &target,
      DiagnosticSink &diagnostics,
      const std::vector<ProcedureInstantiationSeed> &seeds)
      : sources_(sources), loaded_(loaded), selections_(selections),
        semantic_(semantic), constants_(constants), target_(target),
        diagnostics_(diagnostics), seeds_(seeds) {}

  [[nodiscard]] BodyCheckResult run() {
    BodyCheckResult result;
    const std::size_t initial_errors = diagnostics_.error_count();
    ensure_runtime_context_type(semantic_, diagnostics_);
    const std::vector<SymbolId> package_symbols =
        semantic_.symbols.scope(semantic_.package_scope).symbols;
    // Capture the original declaration list before seeding. Instantiation adds
    // private package symbols, but those bodies belong to the growing
    // instances_ loop below and must not be checked a second time here.
    instantiate_seeded_procedures();
    for (SymbolId id : package_symbols) {
      const Symbol symbol = semantic_.symbols.symbol(id);
      if (symbol.kind != SymbolKind::Procedure || !symbol.type.is_valid()) {
        continue;
      }
      if (check_procedure(id, symbol.flags.parametric)) {
        ++result.checked_procedures;
      }
    }
    // Checking an ordinary body can discover a first concrete use. A growing
    // index loop is intentional: an instance body may instantiate another
    // template, and every newly appended row is checked exactly once.
    for (std::size_t index = 0; index < instances_.size(); ++index) {
      current_instance_index_ = index;
      if (!instances_[index].checked &&
          check_procedure(instances_[index].symbol, false)) {
        ++result.checked_procedures;
        instances_[index].checked = true;
      }
      current_instance_index_.reset();
    }
    result.ok = diagnostics_.error_count() == initial_errors;
    result.program = std::move(hir_);
    return result;
  }

private:
  void instantiate_seeded_procedures() {
    for (const ProcedureInstantiationSeed &seed : seeds_) {
      const std::optional<SymbolId> source = semantic_.symbols.lookup_direct(
          semantic_.package_scope, seed.public_template_name);
      if (!source.has_value()) {
        diagnostics_.error(
            SourceRange::invalid(),
            "generic instance request names no declaration '" +
                seed.public_template_name + "'");
        continue;
      }
      const Symbol source_symbol = semantic_.symbols.symbol(*source);
      if (source_symbol.kind != SymbolKind::Procedure ||
          !source_symbol.flags.parametric ||
          source_symbol.visibility != Visibility::Public) {
        diagnostics_.error(
            source_symbol.name_range,
            "generic instance request does not name a public parametric procedure");
        continue;
      }
      const std::vector<ParametricParameterRecord> parameters =
          parameters_for(*source);
      if (parameters.size() != seed.arguments.size()) {
        diagnostics_.error(
            source_symbol.name_range,
            "generic instance request has the wrong number of arguments");
        continue;
      }
      std::vector<TypeSubstitution> type_substitutions;
      std::vector<ValueSubstitution> value_substitutions;
      bool valid = true;
      for (std::size_t index = 0; index < parameters.size(); ++index) {
        const ParametricParameterRecord &parameter = parameters[index];
        const ParametricArgument &argument = seed.arguments[index];
        if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
          if (argument.is_type) {
            valid = false;
            break;
          }
          value_substitutions.push_back(
              {parameter.parameter, argument.value, {}});
        } else {
          if (!argument.is_type) {
            valid = false;
            break;
          }
          type_substitutions.push_back({
              semantic_.symbols.symbol(parameter.parameter).type,
              argument.type,
          });
        }
      }
      if (!valid) {
        diagnostics_.error(
            source_symbol.name_range,
            "generic instance request argument kinds do not match the declaration");
        continue;
      }
      (void)instantiate_procedure(
          *source,
          std::move(type_substitutions),
          std::move(value_substitutions),
          source_symbol.name_range,
          seed.instance_name);
    }
  }

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

  // Draft section 4 forbids a nested procedure from capturing an enclosing
  // invocation's runtime state. True when child is ancestor itself or a lexical
  // descendant of ancestor.
  // This small relation is the complete runtime-capture test for nested
  // procedures: their own parameters and locals live at or below their
  // Procedure scope, while an enclosing invocation's bindings live above it.
  [[nodiscard]] bool scope_is_within(
      ScopeId child, ScopeId ancestor) const {
    ScopeId current = child;
    while (current.is_valid()) {
      if (current == ancestor) return true;
      current = semantic_.symbols.scope(current).parent;
    }
    return false;
  }

  [[nodiscard]] bool is_nested_procedure(SymbolId symbol) const {
    if (!symbol.is_valid()) return false;
    const Symbol &candidate = semantic_.symbols.symbol(symbol);
    return candidate.kind == SymbolKind::Procedure &&
        semantic_.symbols.scope(candidate.scope).kind == ScopeKind::Block;
  }

  [[nodiscard]] std::string source_relative_name(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file) return entry.relative_name;
    }
    // A manually assembled test package may omit a canonical relative name.
    // FileId is intentionally not used: it is process-local and must not leak
    // into a native identity. The empty component remains deterministic.
    return {};
  }

  [[nodiscard]] std::string effective_linkage_name(SymbolId symbol) const {
    const Symbol &record = semantic_.symbols.symbol(symbol);
    return record.linkage_name.empty() ? record.name : record.linkage_name;
  }

  // LLVM functions are package-level even when their Draft names are lexical.
  // The complete enclosing linkage identity, canonical relative filename, and
  // declaration byte offset make the generated name stable and collision-free
  // without exposing a source-visible mangling scheme.
  [[nodiscard]] std::string nested_linkage_name(
      const SyntaxTree &tree,
      std::string_view name,
      SourceRange range) const {
    const std::string relative = source_relative_name(tree.file());
    return effective_linkage_name(current_procedure_) + "$nested$" +
        std::to_string(relative.size()) + ":" + relative + "$" +
        std::to_string(range.begin.offset) + "$" + std::string(name);
  }

  [[nodiscard]] bool captures_enclosing_runtime_binding(
      const Symbol &symbol) const {
    if (symbol.kind != SymbolKind::Parameter &&
        symbol.kind != SymbolKind::Local) {
      return false;
    }
    const std::optional<ScopeId> current_scope =
        owned_scope(current_procedure_, ScopeKind::Procedure);
    return current_scope.has_value() &&
        !scope_is_within(symbol.scope, *current_scope);
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

  // A distinct type keeps the operators of its underlying type, with the
  // distinct type substituted for operand and result positions. Conditions
  // remain stricter and continue to use is_bool(): a distinct bool does not
  // acquire implicit truthiness merely because its logical operators exist.
  [[nodiscard]] bool is_logical_bool(TypeId type) const {
    return !is_invalid_type(type) &&
        runtime_scalar_type(type).kind == TypeKind::Bool;
  }

  [[nodiscard]] bool is_untyped_integer(TypeId type) const {
    return type == semantic_.types.builtins().untyped_integer;
  }

  [[nodiscard]] bool is_untyped_float(TypeId type) const {
    return type == semantic_.types.builtins().untyped_float;
  }

  [[nodiscard]] std::optional<TypeConstraintKind> type_constraint(
      TypeId type) const {
    if (!type.is_valid() || semantic_.types.type(type).kind != TypeKind::TypeParameter) {
      return std::nullopt;
    }
    for (const ParametricParameterRecord &parameter :
         semantic_.parametric_parameters) {
      const Symbol &symbol = semantic_.symbols.symbol(parameter.parameter);
      if (symbol.type == type) return parameter.constraint;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool is_integer(TypeId type) const {
    if (is_invalid_type(type)) return false;
    const std::optional<TypeConstraintKind> constraint = type_constraint(type);
    return is_untyped_integer(type) || semantic_.types.is_integer(type) ||
        constraint == TypeConstraintKind::Integer;
  }

  [[nodiscard]] bool is_numeric(TypeId type) const {
    if (is_invalid_type(type)) return false;
    const std::optional<TypeConstraintKind> constraint = type_constraint(type);
    return is_untyped_integer(type) || is_untyped_float(type) ||
           semantic_.types.is_number(type) ||
        constraint == TypeConstraintKind::Integer ||
        constraint == TypeConstraintKind::Float ||
        constraint == TypeConstraintKind::Number;
  }

  [[nodiscard]] TypeId substitute_type(
      TypeId source,
      const std::vector<TypeSubstitution> &type_substitutions,
      const std::vector<ValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    if (!source.is_valid()) return source;
    for (const TypeSubstitution &substitution : type_substitutions) {
      if (substitution.parameter == source) return substitution.replacement;
    }
    const Type value = semantic_.types.type(source);

    // A nominal application cannot be rebuilt by recursively substituting its
    // physical members: doing so would manufacture an unrelated anonymous
    // layout. Substitute its retained arguments, then ask TypeResolver for the
    // canonical application of the original template.
    if (value.kind == TypeKind::Struct || value.kind == TypeKind::Enum ||
        value.kind == TypeKind::TaggedUnion ||
        value.kind == TypeKind::RawUnion) {
      const std::optional<NominalApplication> application =
          nominal_application(source);
      if (application.has_value()) {
        std::vector<ParametricArgument> arguments = *application->arguments;
        bool changed = false;
        for (ParametricArgument &argument : arguments) {
          if (!argument.is_type) continue;
          const TypeId replacement = substitute_type(
              argument.type,
              type_substitutions,
              value_substitutions,
              use_range);
          changed = changed || replacement != argument.type;
          argument.type = replacement;
        }
        if (!changed) return source;

        std::optional<SymbolId> template_source = application->source;
        if (!template_source.has_value() && application->imported != nullptr) {
          for (const ImportedSymbol &imported : semantic_.imported_symbols) {
            if (imported.root_identity != application->imported->root_identity ||
                imported.root_relative_path !=
                    application->imported->root_relative_path ||
                imported.public_name != application->imported->public_name) {
              continue;
            }
            const Symbol &candidate = semantic_.symbols.symbol(imported.proxy);
            if (candidate.kind == SymbolKind::Type &&
                candidate.flags.parametric) {
              template_source = imported.proxy;
              break;
            }
          }
        }
        if (!template_source.has_value()) {
          diagnostics_.error(
              use_range,
              "cannot recover the template declaration for nominal substitution");
          return semantic_.types.builtins().invalid;
        }
        return instantiate_parametric_type_application(
            sources_,
            loaded_,
            semantic_,
            selections_,
            *template_source,
            std::move(arguments),
            use_range,
            diagnostics_);
      }
    }

    switch (value.kind) {
    case TypeKind::Pointer:
      return semantic_.types.pointer(substitute_type(
          value.element, type_substitutions, value_substitutions, use_range));
    case TypeKind::MultiPointer:
      return semantic_.types.multi_pointer(substitute_type(
          value.element, type_substitutions, value_substitutions, use_range));
    case TypeKind::Slice:
      return semantic_.types.slice(substitute_type(
          value.element, type_substitutions, value_substitutions, use_range));
    case TypeKind::Array:
    case TypeKind::Simd: {
      std::uint64_t count = value.element_count;
      if (value.element_count_parameter !=
          std::numeric_limits<std::uint32_t>::max()) {
        bool found = false;
        for (const ValueSubstitution &substitution : value_substitutions) {
          if (substitution.parameter.value != value.element_count_parameter) {
            continue;
          }
          const TypeId element = substitute_type(
              value.element,
              type_substitutions,
              value_substitutions,
              use_range);
          if (substitution.symbolic_parameter.is_valid()) {
            return value.kind == TypeKind::Array
                ? semantic_.types.parametric_array(
                      element, substitution.symbolic_parameter.value)
                : semantic_.types.parametric_simd(
                      element, substitution.symbolic_parameter.value);
          }
          const std::optional<std::uint64_t> concrete =
              substitution.value.integer.to_u64();
          if (!concrete.has_value() || *concrete == 0) {
            diagnostics_.error(
                use_range,
                "array and SIMD value parameters must instantiate to a nonzero u64");
            return semantic_.types.builtins().invalid;
          }
          count = *concrete;
          found = true;
          break;
        }
        // Symbolic template checking intentionally retains the dependent row.
        // Every concrete instance is separately validated to have all values.
        if (!found) return source;
      }
      const TypeId element = substitute_type(
          value.element, type_substitutions, value_substitutions, use_range);
      return value.kind == TypeKind::Array
          ? semantic_.types.array(element, count)
          : semantic_.types.simd(element, count, use_range);
    }
    case TypeKind::Tuple: {
      std::vector<TypeId> members;
      members.reserve(value.members.size());
      for (TypeId member : value.members) {
        members.push_back(substitute_type(
            member, type_substitutions, value_substitutions, use_range));
      }
      return semantic_.types.tuple(members);
    }
    case TypeKind::Procedure: {
      std::vector<TypeId> parameters;
      if (!value.members.empty()) {
        parameters.reserve(value.members.size() - 1);
        for (std::size_t index = 0; index + 1 < value.members.size(); ++index) {
          parameters.push_back(substitute_type(
              value.members[index],
              type_substitutions,
              value_substitutions,
              use_range));
        }
      }
      const TypeId result = value.members.empty()
          ? semantic_.types.builtins().void_type
          : substitute_type(
                value.members.back(),
                type_substitutions,
                value_substitutions,
                use_range);
      return semantic_.types.procedure(
          parameters, result, value.c_calling_convention);
    }
    default:
      return source;
    }
  }

  [[nodiscard]] TypeId substitute_active(
      TypeId source, SourceRange use_range = SourceRange::invalid()) {
    return !current_instance_index_.has_value()
        ? source
        : substitute_type(
              source,
              instances_[*current_instance_index_].type_substitutions,
              instances_[*current_instance_index_].value_substitutions,
              use_range);
  }

  // Presents the current instance's value parameters in the same SymbolId-keyed
  // shape used by package constant evaluation. The table is deliberately built
  // on demand: instances normally contain only one or two values, and avoiding
  // a second mutable constant store keeps their ownership unambiguous.
  [[nodiscard]] ConstantTable active_constant_table() const {
    // Lexical constants and concrete value parameters share one evaluator
    // overlay. SymbolId keys keep unrelated scopes separate even though the
    // table is append-only across all checked procedure bodies.
    ConstantTable result = constants_;
    if (!current_instance_index_.has_value()) return result;
    for (const ValueSubstitution &substitution :
         instances_[*current_instance_index_].value_substitutions) {
      result.bindings.push_back({substitution.parameter, substitution.value});
    }
    return result;
  }

  [[nodiscard]] std::vector<ConstantTypeBinding> active_constant_types() const {
    std::vector<ConstantTypeBinding> result;
    if (!current_instance_index_.has_value()) return result;
    for (const TypeSubstitution &substitution :
         instances_[*current_instance_index_].type_substitutions) {
      result.push_back({substitution.parameter, substitution.replacement});
    }
    return result;
  }

  [[nodiscard]] const ConstantValue *active_constant(SymbolId symbol) const {
    if (!current_instance_index_.has_value()) return nullptr;
    for (const ValueSubstitution &substitution :
         instances_[*current_instance_index_].value_substitutions) {
      if (substitution.parameter == symbol) return &substitution.value;
    }
    return nullptr;
  }

  // Evaluates the integer-only HIR subset needed to prove source bounds. HIR
  // operands are already type-checked, so this routine has no diagnostic role;
  // an operation it cannot prove simply returns nullopt and leaves a runtime
  // check in place. Arithmetic stays in BigInteger to avoid host overflow.
  [[nodiscard]] std::optional<BigInteger> constant_integer_expression(
      HirExpressionId expression_id) const {
    const HirExpression &expression = hir_.expression(expression_id);
    if (expression.kind == HirExpressionKind::Constant &&
        expression.constant.kind == ConstantKind::Integer) {
      return expression.constant.integer;
    }
    if (expression.kind == HirExpressionKind::Unary &&
        expression.operands.size() == 1) {
      const std::optional<BigInteger> operand =
          constant_integer_expression(expression.operands.front());
      if (!operand.has_value()) return std::nullopt;
      if (expression.operation == HirOperation::Positive) return operand;
      if (expression.operation == HirOperation::Negate) return operand->negated();
      if (expression.operation == HirOperation::BitwiseNot) {
        return operand->bitwise_not();
      }
      return std::nullopt;
    }
    if (expression.kind != HirExpressionKind::Binary ||
        expression.operands.size() != 2) {
      return std::nullopt;
    }
    const std::optional<BigInteger> left =
        constant_integer_expression(expression.operands[0]);
    const std::optional<BigInteger> right =
        constant_integer_expression(expression.operands[1]);
    if (!left.has_value() || !right.has_value()) return std::nullopt;
    switch (expression.operation) {
    case HirOperation::Add: return left->added(*right);
    case HirOperation::Subtract: return left->subtracted(*right);
    case HirOperation::Multiply: return left->multiplied(*right);
    case HirOperation::Divide:
    case HirOperation::Remainder: {
      BigInteger quotient;
      BigInteger remainder;
      if (!left->divide(*right, quotient, remainder)) return std::nullopt;
      return expression.operation == HirOperation::Divide
          ? std::optional<BigInteger>(std::move(quotient))
          : std::optional<BigInteger>(std::move(remainder));
    }
    case HirOperation::BitwiseAnd: return left->bitwise_and(*right);
    case HirOperation::BitwiseOr: return left->bitwise_or(*right);
    case HirOperation::BitwiseXor: return left->bitwise_xor(*right);
    case HirOperation::ShiftLeft:
    case HirOperation::ShiftRight: {
      const std::optional<std::uint64_t> count = right->to_u64();
      if (!count.has_value() || *count > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
      }
      return expression.operation == HirOperation::ShiftLeft
          ? left->shifted_left(static_cast<std::size_t>(*count))
          : left->shifted_right(static_cast<std::size_t>(*count));
    }
    default: return std::nullopt;
    }
  }

  // Returns the length known from the base type itself. Arrays always qualify;
  // strings qualify only when the base HIR node is the literal/constant value,
  // whose length is the number of indexed bytes rather than Unicode scalars.
  [[nodiscard]] std::optional<std::uint64_t> compile_time_length(
      HirExpressionId base_id) const {
    const HirExpression &base = hir_.expression(base_id);
    const Type type = semantic_.types.type(base.type);
    if (type.kind == TypeKind::Array) return type.element_count;
    if (type.kind == TypeKind::String &&
        base.kind == HirExpressionKind::Constant &&
        base.constant.kind == ConstantKind::String) {
      return static_cast<std::uint64_t>(base.constant.text.size());
    }
    return std::nullopt;
  }

  // Applies Draft's initial expected-type rule: exact types match, and untyped
  // numeric constants may take a compatible concrete numeric type. Range checks
  // use the constant table/literal value in the completed numeric checker.
  // Reports whether an exact compile-time shape may acquire the surrounding
  // type without an ordinary implicit runtime conversion. Tuple constants do
  // this member by member; concrete tuple members must still match exactly.
  [[nodiscard]] bool accepts_expected_type(
      TypeId actual, TypeId expected) const {
    if (actual == expected) return true;
    if (is_invalid_type(actual) || is_invalid_type(expected)) return false;
    const std::optional<TypeConstraintKind> constraint =
        type_constraint(expected);
    if ((is_untyped_integer(actual) && is_integer(expected)) ||
        ((is_untyped_integer(actual) || is_untyped_float(actual)) &&
         (semantic_.types.is_float(expected) ||
          constraint == TypeConstraintKind::Float)) ||
        (is_untyped_integer(actual) &&
         constraint == TypeConstraintKind::Number)) {
      return true;
    }
    const Type actual_type = semantic_.types.type(actual);
    const Type expected_type = semantic_.types.type(expected);
    if (actual_type.kind != TypeKind::Tuple ||
        expected_type.kind != TypeKind::Tuple ||
        actual_type.members.size() != expected_type.members.size()) {
      return false;
    }
    for (std::size_t index = 0; index < actual_type.members.size(); ++index) {
      if (!accepts_expected_type(
              actual_type.members[index], expected_type.members[index])) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] TypeId apply_expected_type(
      TypeId actual, TypeId expected, SourceRange range) {
    if (is_invalid_type(actual)) return semantic_.types.builtins().invalid;
    if (!expected.is_valid() || is_invalid_type(expected) || actual == expected) {
      return actual;
    }
    if (accepts_expected_type(actual, expected)) return expected;
    diagnostics_.error(
        range,
        "expression of type '" + std::string(type_kind_name(semantic_.types.type(actual).kind)) +
            "' does not match expected type '" +
            std::string(type_kind_name(semantic_.types.type(expected).kind)) + "'");
    return semantic_.types.builtins().invalid;
  }

  // Selects one common numeric operand type without implicit conversion between
  // concrete numeric types. An integer and decimal that are both still
  // untyped are the language-defined exception: their arithmetic stays exact
  // and produces an untyped floating constant until a later context rounds it.
  [[nodiscard]] TypeId common_numeric_type(
      TypeId left, TypeId right, SourceRange range) {
    if (left == right && is_numeric(left)) return left;
    if ((is_untyped_integer(left) && is_untyped_float(right)) ||
        (is_untyped_float(left) && is_untyped_integer(right))) {
      return semantic_.types.builtins().untyped_float;
    }
    if (is_untyped_integer(left) && is_numeric(right) &&
        !is_untyped_integer(right) && !is_untyped_float(right)) {
      return right;
    }
    if (is_untyped_integer(right) && is_numeric(left) &&
        !is_untyped_integer(left) && !is_untyped_float(left)) {
      return left;
    }
    const auto accepts_untyped_float = [&](TypeId concrete) {
      return semantic_.types.is_float(concrete) ||
          type_constraint(concrete) == TypeConstraintKind::Float;
    };
    if (is_untyped_float(left) && accepts_untyped_float(right)) return right;
    if (is_untyped_float(right) && accepts_untyped_float(left)) return left;
    diagnostics_.error(range, "numeric operands require one common type");
    return semantic_.types.builtins().invalid;
  }

  [[nodiscard]] bool integer_representable(
      const BigInteger &value, TypeId target) const {
    Type type = semantic_.types.type(target);
    while (type.kind == TypeKind::Distinct) {
      type = semantic_.types.type(type.element);
    }
    if (type.kind == TypeKind::Enum && type.element.is_valid()) {
      type = semantic_.types.type(type.element);
    }
    if (type.kind == TypeKind::Rune) {
      type.kind = TypeKind::SignedInteger;
    }
    if (type.kind != TypeKind::SignedInteger &&
        type.kind != TypeKind::UnsignedInteger) {
      return true;
    }
    const std::uint32_t bits = type.bit_width;
    if (bits == 0) return false;
    if (type.kind == TypeKind::UnsignedInteger) {
      return !value.is_negative() && value.bit_count() <= bits;
    }
    const BigInteger magnitude =
        BigInteger::from_u64(1).shifted_left(static_cast<std::size_t>(bits - 1U));
    const BigInteger minimum = magnitude.negated();
    const BigInteger maximum = magnitude.subtracted(BigInteger::from_u64(1));
    return value.compare(minimum) >= 0 && value.compare(maximum) <= 0;
  }

  // Returns the type whose operator vocabulary a distinct value inherits.
  // Keep the source TypeId separately whenever an operation returns the same
  // distinct type (for example ptr_offset); this helper is only the semantic
  // view used to validate and decompose the operation.
  [[nodiscard]] TypeId underlying_type_id(TypeId type_id) const {
    while (semantic_.types.type(type_id).kind == TypeKind::Distinct) {
      type_id = semantic_.types.type(type_id).element;
    }
    return type_id;
  }

  [[nodiscard]] Type runtime_scalar_type(TypeId type_id) const {
    return semantic_.types.type(underlying_type_id(type_id));
  }

  [[nodiscard]] bool numeric_value_type(TypeId type_id) const {
    const Type type = runtime_scalar_type(type_id);
    return type.kind == TypeKind::SignedInteger ||
        type.kind == TypeKind::UnsignedInteger ||
        type.kind == TypeKind::Float || type.kind == TypeKind::Rune;
  }

  [[nodiscard]] bool data_pointer_kind(TypeKind kind) const {
    return kind == TypeKind::Pointer || kind == TypeKind::MultiPointer ||
        kind == TypeKind::RawPointer || kind == TypeKind::CString;
  }

  // A value switch is equality dispatch. Tagged unions compare their integer
  // discriminator separately; every other accepted subject must have the
  // built-in scalar equality defined by Draft 1. Strings and aggregates use
  // library comparison and therefore cannot silently acquire switch equality.
  [[nodiscard]] bool switch_subject_type(TypeId type_id) const {
    const TypeKind kind = runtime_scalar_type(type_id).kind;
    return numeric_value_type(type_id) || kind == TypeKind::Bool ||
        kind == TypeKind::BooleanStorage || kind == TypeKind::EndianScalar ||
        kind == TypeKind::Enum || kind == TypeKind::Procedure ||
        data_pointer_kind(kind);
  }

  // Duplicate switch labels are defined by the subject's equality operation,
  // not by byte-for-byte equality of ConstantValue's storage record. IEEE
  // positive and negative zero have different encodings but compare equal at
  // runtime, so accepting both would make the second label unreachable. NaN
  // has the opposite property: even an identical NaN never compares equal.
  //
  // Every value label has already been contextualized to the subject type.
  // Untyped floats are nevertheless handled as a defensive fallback so this
  // helper remains correct if an earlier diagnostic leaves partial HIR behind.
  [[nodiscard]] bool switch_case_values_equal(
      TypeId subject_type,
      const ConstantValue &left,
      const ConstantValue &right) const {
    const Type type = runtime_scalar_type(subject_type);
    if (type.kind != TypeKind::Float ||
        left.kind != ConstantKind::Float ||
        right.kind != ConstantKind::Float) {
      return left == right;
    }
    if (left.float_bit_width == 0 || right.float_bit_width == 0) {
      return left.float_bit_width == right.float_bit_width &&
          left.floating == right.floating;
    }
    if (left.float_bit_width != right.float_bit_width) return false;
    const std::optional<IeeeBinaryFormat> format =
        ieee_format_for_width(left.float_bit_width);
    if (!format.has_value()) return left == right;
    const std::optional<DecodedIeeeValue> left_value =
        decode_ieee_bits(left.float_bits, *format);
    const std::optional<DecodedIeeeValue> right_value =
        decode_ieee_bits(right.float_bits, *format);
    if (!left_value.has_value() || !right_value.has_value()) return left == right;
    if (left_value->kind == IeeeValueKind::NaN ||
        right_value->kind == IeeeValueKind::NaN) {
      return false;
    }
    if (left_value->kind != right_value->kind) return false;
    if (left_value->kind == IeeeValueKind::Infinity) {
      return left_value->negative == right_value->negative;
    }
    return left_value->finite == right_value->finite;
  }

  // A NaN equality label is not merely unusual: the ordered equality emitted
  // for a float switch can never select it. Diagnose the dead label instead of
  // quietly generating a case that no runtime subject can reach.
  [[nodiscard]] bool switch_case_value_is_nan(
      TypeId subject_type, const ConstantValue &value) const {
    const Type type = runtime_scalar_type(subject_type);
    if (type.kind != TypeKind::Float ||
        value.kind != ConstantKind::Float ||
        value.float_bit_width == 0) {
      return false;
    }
    const std::optional<IeeeBinaryFormat> format =
        ieee_format_for_width(value.float_bit_width);
    const std::optional<DecodedIeeeValue> decoded = format.has_value()
        ? decode_ieee_bits(value.float_bits, *format)
        : std::nullopt;
    return decoded.has_value() && decoded->kind == IeeeValueKind::NaN;
  }

  [[nodiscard]] bool current_procedure_uses_c_abi() const {
    if (!current_procedure_.is_valid()) return false;
    const TypeId type = semantic_.symbols.symbol(current_procedure_).type;
    return type.is_valid() &&
        semantic_.types.type(type).kind == TypeKind::Procedure &&
        semantic_.types.type(type).c_calling_convention;
  }

  [[nodiscard]] std::uint32_t integer_width(TypeId type_id) const {
    const Type type = runtime_scalar_type(type_id);
    if (type.kind == TypeKind::Enum) {
      return static_cast<std::uint32_t>(type.layout.size * 8U);
    }
    return type.bit_width;
  }

  [[nodiscard]] bool signed_integer_target(TypeId type_id) const {
    const Type type = runtime_scalar_type(type_id);
    if (type.kind == TypeKind::Enum && type.element.is_valid()) {
      return signed_integer_target(type.element);
    }
    return type.kind == TypeKind::SignedInteger || type.kind == TypeKind::Rune;
  }

  [[nodiscard]] BigInteger wrap_integer(
      const BigInteger &value, TypeId target) const {
    const std::uint32_t bits = integer_width(target);
    if (bits == 0) return value;
    const BigInteger modulus = BigInteger::from_u64(1).shifted_left(bits);
    BigInteger quotient;
    BigInteger remainder;
    if (!value.divide(modulus, quotient, remainder)) return value;
    if (remainder.is_negative()) remainder = remainder.added(modulus);
    if (signed_integer_target(target)) {
      const BigInteger sign = BigInteger::from_u64(1).shifted_left(bits - 1U);
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
        (value.compare(surrogate_begin) < 0 || value.compare(surrogate_end) > 0);
  }

  [[nodiscard]] bool valid_enum_constant(
      TypeId target, const BigInteger &value) const {
    const std::optional<SymbolId> owner = type_owner(target);
    if (!owner.has_value()) return false;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner != *owner) continue;
      for (const EnumMemberValue &enum_value : semantic_.enum_member_values) {
        if (enum_value.member == member.member &&
            value.compare(enum_value.value) == 0) {
          return true;
        }
      }
    }
    return false;
  }

  // Folds numeric casts whose operand is still an exact compile-time value.
  // This prevents untyped pseudo-types from reaching MIR and enforces the same
  // trap conditions at compile time when the conversion is statically known.
  [[nodiscard]] std::optional<ConstantValue> convert_numeric_constant(
      const ConstantValue &value, TypeId target, SourceRange range) {
    const Type target_type = runtime_scalar_type(target);
    if (target_type.kind == TypeKind::Bool &&
        value.kind == ConstantKind::Integer) {
      return ConstantValue::make_bool(!value.integer.is_zero());
    }
    if (target_type.kind == TypeKind::BooleanStorage &&
        value.kind == ConstantKind::Bool) {
      return ConstantValue::make_integer(value.boolean ? 1 : 0);
    }
    const bool integer_target =
        target_type.kind == TypeKind::SignedInteger ||
        target_type.kind == TypeKind::UnsignedInteger ||
        target_type.kind == TypeKind::Rune ||
        target_type.kind == TypeKind::Enum;
    if (integer_target) {
      BigInteger integer;
      if (value.kind == ConstantKind::Integer) {
        integer = wrap_integer(value.integer, target);
      } else if (value.kind == ConstantKind::Float) {
        ExactRational source = value.floating;
        if (value.float_bit_width != 0) {
          const std::optional<IeeeBinaryFormat> format =
              ieee_format_for_width(value.float_bit_width);
          const std::optional<DecodedIeeeValue> decoded = format.has_value()
              ? decode_ieee_bits(value.float_bits, *format)
              : std::nullopt;
          if (!decoded.has_value() ||
              decoded->kind != IeeeValueKind::Finite) {
            diagnostics_.error(
                range, "compile-time non-finite float-to-integer cast traps");
            return std::nullopt;
          }
          source = decoded->finite;
        }
        BigInteger remainder;
        if (!source.numerator().divide(
                source.denominator(), integer, remainder) ||
            !integer_representable(integer, target)) {
          diagnostics_.error(
              range, "compile-time float-to-integer cast is out of range");
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
      if (target_type.kind == TypeKind::Rune && !valid_rune(integer)) {
        diagnostics_.error(range, "compile-time cast does not produce a Unicode scalar");
        return std::nullopt;
      }
      if (target_type.kind == TypeKind::Enum &&
          !valid_enum_constant(target, integer)) {
        diagnostics_.error(range, "compile-time cast does not name an enum member");
        return std::nullopt;
      }
      return ConstantValue::make_integer(std::move(integer));
    }
    if (target_type.kind == TypeKind::Float) {
      const std::optional<IeeeBinaryFormat> format =
          ieee_format_for_width(target_type.bit_width);
      if (!format.has_value()) return std::nullopt;
      std::uint64_t bits = 0;
      if (value.kind == ConstantKind::Integer) {
        const std::optional<std::uint64_t> rounded =
            round_ieee_bits(ExactRational(value.integer), *format);
        if (!rounded.has_value()) return std::nullopt;
        bits = *rounded;
      } else if (value.kind == ConstantKind::Float &&
                 value.float_bit_width == 0) {
        const std::optional<std::uint64_t> rounded =
            round_ieee_bits(value.floating, *format);
        if (!rounded.has_value()) return std::nullopt;
        bits = *rounded;
      } else if (value.kind == ConstantKind::Float) {
        const std::optional<IeeeBinaryFormat> source_format =
            ieee_format_for_width(value.float_bit_width);
        const std::optional<DecodedIeeeValue> decoded = source_format.has_value()
            ? decode_ieee_bits(value.float_bits, *source_format)
            : std::nullopt;
        if (!decoded.has_value()) return std::nullopt;
        if (decoded->kind == IeeeValueKind::NaN) {
          bits = ieee_nan_bits(*format);
        } else if (decoded->kind == IeeeValueKind::Infinity) {
          bits = ieee_infinity_bits(*format, decoded->negative);
        } else if (decoded->finite.is_zero() && decoded->negative) {
          bits = ieee_zero_bits(*format, true);
        } else {
          const std::optional<std::uint64_t> rounded =
              round_ieee_bits(decoded->finite, *format);
          if (!rounded.has_value()) return std::nullopt;
          bits = *rounded;
        }
      } else {
        return std::nullopt;
      }
      const std::optional<DecodedIeeeValue> decoded =
          decode_ieee_bits(bits, *format);
      if (!decoded.has_value()) return std::nullopt;
      return ConstantValue::make_ieee_float(
          target_type.bit_width,
          bits,
          decoded->kind == IeeeValueKind::Finite
              ? decoded->finite
              : ExactRational{});
    }
    return std::nullopt;
  }

  void check_constant_range(
      const ConstantValue &value, TypeId target, SourceRange range) {
    if (value.kind == ConstantKind::Integer &&
        !integer_representable(value.integer, target)) {
      diagnostics_.error(
          range,
          "integer constant is not representable in expected type '" +
              semantic_.types.type(target).name + "'");
    }
  }

  // MIR constants are complete typed values rather than requests for an
  // implicit conversion. When an exact untyped leaf enters a floating context,
  // round its payload here so its ConstantKind agrees with the resulting TypeId.
  // Integer contexts retain BigInteger and use the ordinary range check.
  void contextualize_constant_value(
      ConstantValue &value,
      TypeId source,
      TypeId target,
      SourceRange range) {
    if (value.kind == ConstantKind::Aggregate &&
        !is_invalid_type(source) && !is_invalid_type(target)) {
      const Type source_type = semantic_.types.type(source);
      const Type target_type = semantic_.types.type(target);
      if (source_type.kind == TypeKind::Tuple &&
          target_type.kind == TypeKind::Tuple &&
          source_type.members.size() == target_type.members.size() &&
          value.elements.size() == source_type.members.size()) {
        for (std::size_t index = 0; index < value.elements.size(); ++index) {
          contextualize_constant_value(
              value.elements[index],
              source_type.members[index],
              target_type.members[index],
              range);
        }
        return;
      }
    }
    if ((is_untyped_integer(source) || is_untyped_float(source)) &&
        semantic_.types.is_float(target) &&
        (value.kind == ConstantKind::Integer ||
         value.kind == ConstantKind::Float)) {
      const std::optional<ConstantValue> converted =
          convert_numeric_constant(value, target, range);
      if (converted.has_value()) value = *converted;
    }
    check_constant_range(value, target, range);
  }

  // Applies a concrete numeric context to an already checked untyped tree.
  // This is needed when `:=` chooses int/f64 after seeing the complete
  // initializer. Integer payloads may remain mathematical integers because
  // their LLVM spelling is type-independent after the range check. Floating
  // leaves are rounded here into the concrete IEEE payload expected by MIR;
  // MIR constants are already typed values, not implicit conversion requests.
  void contextualize_numeric_expression(
      HirExpressionId expression_id, TypeId target) {
    HirExpression &expression = hir_.expression_mut(expression_id);
    const bool integer = expression.type == semantic_.types.builtins().untyped_integer &&
        semantic_.types.is_integer(target);
    const bool floating =
        (expression.type == semantic_.types.builtins().untyped_integer ||
         expression.type == semantic_.types.builtins().untyped_float) &&
        semantic_.types.is_float(target);
    if (!integer && !floating) return;
    contextualize_constant_value(
        expression.constant, expression.type, target, expression.range);
    expression.type = target;
    const std::vector<HirExpressionId> operands = expression.operands;
    for (HirExpressionId operand : operands) {
      contextualize_numeric_expression(operand, target);
    }
  }

  // Inferred runtime bindings may not retain the untyped numeric pseudo-types.
  // Apply the ordinary int/f64 defaults recursively to tuple members so
  // `pair := (1, 2.0)` has a complete physical type just like two scalar `:=`
  // declarations would.
  [[nodiscard]] TypeId default_inferred_runtime_type(TypeId source) {
    if (source == semantic_.types.builtins().untyped_integer) {
      return semantic_.types.builtins().int_type;
    }
    if (source == semantic_.types.builtins().untyped_float) {
      const std::optional<TypeId> f64 = semantic_.types.find_builtin("f64");
      return f64.value_or(semantic_.types.builtins().invalid);
    }
    if (is_invalid_type(source)) return source;
    const Type type = semantic_.types.type(source);
    if (type.kind != TypeKind::Tuple) return source;
    std::vector<TypeId> members;
    members.reserve(type.members.size());
    for (TypeId member : type.members) {
      members.push_back(default_inferred_runtime_type(member));
    }
    return semantic_.types.tuple(members);
  }

  // Contextualize tuple elements individually after recursive defaulting. The
  // existing scalar helper deliberately applies one target to an entire numeric
  // expression tree; a tuple instead has one potentially different target per
  // operand.
  void contextualize_inferred_runtime_expression(
      HirExpressionId expression_id, TypeId target) {
    HirExpression &expression = hir_.expression_mut(expression_id);
    if (!is_invalid_type(target) &&
        semantic_.types.type(target).kind == TypeKind::Tuple &&
        expression.kind == HirExpressionKind::Constant &&
        expression.constant.kind == ConstantKind::Aggregate) {
      const TypeId source = expression.type;
      contextualize_constant_value(
          expression.constant, source, target, expression.range);
      expression.type = target;
      return;
    }
    if (!is_invalid_type(target) &&
        semantic_.types.type(target).kind == TypeKind::Tuple &&
        expression.kind == HirExpressionKind::Conditional &&
        expression.operands.size() == 3) {
      // Operand zero is the bool condition. Only the two selected values
      // acquire the inferred tuple context, and each branch applies it through
      // its own literal/constant tree.
      contextualize_inferred_runtime_expression(expression.operands[1], target);
      contextualize_inferred_runtime_expression(expression.operands[2], target);
      expression.type = target;
      return;
    }
    if (!is_invalid_type(target) &&
        semantic_.types.type(target).kind == TypeKind::Tuple &&
        (expression.kind == HirExpressionKind::Tuple ||
         expression.kind == HirExpressionKind::Composite)) {
      const std::vector<TypeId> members = semantic_.types.type(target).members;
      const std::vector<HirExpressionId> operands = expression.operands;
      const std::size_t count = std::min(members.size(), operands.size());
      for (std::size_t index = 0; index < count; ++index) {
        contextualize_inferred_runtime_expression(operands[index], members[index]);
      }
      expression.type = target;
      return;
    }
    contextualize_numeric_expression(expression_id, target);
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

  // Converts the closed source operator vocabulary into the representation
  // consumed by HIR and MIR. Invalid is deliberate for punctuation that is not
  // an executable operator; callers have already emitted the contextual
  // diagnostic when that can occur on malformed syntax.
  [[nodiscard]] HirOperation hir_operation(TokenKind kind) const {
    switch (kind) {
    case TokenKind::Equal: return HirOperation::Assign;
    case TokenKind::Plus: return HirOperation::Add;
    case TokenKind::Minus: return HirOperation::Subtract;
    case TokenKind::Star: return HirOperation::Multiply;
    case TokenKind::Slash: return HirOperation::Divide;
    case TokenKind::Percent: return HirOperation::Remainder;
    case TokenKind::Ampersand: return HirOperation::BitwiseAnd;
    case TokenKind::Pipe: return HirOperation::BitwiseOr;
    case TokenKind::Caret: return HirOperation::BitwiseXor;
    case TokenKind::ShiftLeft: return HirOperation::ShiftLeft;
    case TokenKind::ShiftRight: return HirOperation::ShiftRight;
    case TokenKind::LogicalAnd: return HirOperation::LogicalAnd;
    case TokenKind::LogicalOr: return HirOperation::LogicalOr;
    case TokenKind::EqualEqual: return HirOperation::Equal;
    case TokenKind::BangEqual: return HirOperation::NotEqual;
    case TokenKind::Less: return HirOperation::Less;
    case TokenKind::LessEqual: return HirOperation::LessEqual;
    case TokenKind::Greater: return HirOperation::Greater;
    case TokenKind::GreaterEqual: return HirOperation::GreaterEqual;
    case TokenKind::PlusEqual: return HirOperation::Add;
    case TokenKind::MinusEqual: return HirOperation::Subtract;
    case TokenKind::StarEqual: return HirOperation::Multiply;
    case TokenKind::SlashEqual: return HirOperation::Divide;
    case TokenKind::PercentEqual: return HirOperation::Remainder;
    case TokenKind::AmpersandEqual: return HirOperation::BitwiseAnd;
    case TokenKind::PipeEqual: return HirOperation::BitwiseOr;
    case TokenKind::CaretEqual: return HirOperation::BitwiseXor;
    case TokenKind::ShiftLeftEqual: return HirOperation::ShiftLeft;
    case TokenKind::ShiftRightEqual: return HirOperation::ShiftRight;
    default: return HirOperation::None;
    }
  }

  // A compound assignment is exactly one binary operator followed by a store;
  // it does not gain a wider operator set merely because the parser represents
  // it as a statement. The left side supplies the result type. Shift counts are
  // the one asymmetric case: any integer type is valid, just as it is for an
  // ordinary shift expression, so a concrete count is not coerced to the
  // target's integer type.
  void check_compound_assignment_operator(
      HirOperation operation,
      TypeId target,
      TypeId value,
      SourceRange range) {
    if (is_invalid_type(target) || is_invalid_type(value)) return;
    switch (operation) {
    case HirOperation::Add:
    case HirOperation::Subtract:
    case HirOperation::Multiply:
    case HirOperation::Divide:
      if (!is_numeric(target) || !is_numeric(value)) {
        diagnostics_.error(
            range, "compound assignment operator requires numeric operands");
      }
      return;
    case HirOperation::Remainder:
    case HirOperation::BitwiseAnd:
    case HirOperation::BitwiseOr:
    case HirOperation::BitwiseXor:
      if (!is_integer(target) || !is_integer(value)) {
        diagnostics_.error(
            range, "compound assignment operator requires integer operands");
      }
      return;
    case HirOperation::ShiftLeft:
    case HirOperation::ShiftRight:
      if (!is_integer(target) || !is_integer(value)) {
        diagnostics_.error(
            range, "compound shift assignment requires integer operands");
      }
      return;
    case HirOperation::Assign:
      return;
    default:
      diagnostics_.error(range, "unsupported compound assignment operator");
      return;
    }
  }

  // HIR stores the mathematical integer rather than source spelling. Narrowing
  // is used only for grammar-defined indices such as tuple `.0`, never for an
  // ordinary runtime literal.
  [[nodiscard]] std::optional<BigInteger> big_integer_literal(
      std::string_view spelling) const {
    return BigInteger::parse_literal(spelling);
  }

  [[nodiscard]] std::optional<std::int64_t> integer_literal(
      std::string_view spelling) const {
    const std::optional<BigInteger> value = big_integer_literal(spelling);
    return value.has_value() ? value->to_i64() : std::nullopt;
  }

  // Resolves a member symbol by the nominal base type's owned Type scope.
  [[nodiscard]] std::optional<SymbolId> find_member(
      TypeId base, std::string_view name) const {
    // A distinct aggregate keeps the underlying aggregate's operator
    // vocabulary. Member selection is one of those operators: the wrapper is
    // the base operand, while the selected field keeps its declared type.
    base = underlying_type_id(base);
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (semantic_.symbols.scope(owned.scope).kind != ScopeKind::Type) continue;
      const Symbol &owner = semantic_.symbols.symbol(owned.owner);
      if (owner.type == base) {
        return semantic_.symbols.lookup_direct(owned.scope, name);
      }
    }
    return std::nullopt;
  }

  // Resolves `alias.public_name` into the consumer-local proxy scope created by
  // package-interface binding. The returned SymbolId belongs to semantic_ and
  // is therefore safe to store in HIR; ImportedSymbol retains the dependency
  // identity required by later MIR/package lowering.
  [[nodiscard]] std::optional<SymbolId> imported_member(
      const SyntaxTree &tree, const SyntaxNode &node, ScopeId scope) const {
    if (node.kind != NodeKind::MemberExpression || node.children.empty()) {
      return std::nullopt;
    }
    const SyntaxNode &base = tree.node(node.children.front());
    if (base.kind != NodeKind::NameExpression) {
      return std::nullopt;
    }
    const std::vector<SourceName> base_names =
        names_in_span(tree, base.token_begin, base.token_end);
    const std::vector<SourceName> all_names =
        names_in_span(tree, node.token_begin, node.token_end);
    if (base_names.size() != 1 || all_names.size() < 2) {
      return std::nullopt;
    }
    const std::optional<SymbolId> import =
        semantic_.symbols.lookup(scope, base_names.front().text);
    if (!import.has_value() ||
        semantic_.symbols.symbol(*import).kind != SymbolKind::Import) {
      return std::nullopt;
    }
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner == *import &&
          semantic_.symbols.scope(owned.scope).kind == ScopeKind::ImportedPackage) {
        return semantic_.symbols.lookup_direct(owned.scope, all_names.back().text);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] const ConstantValue *imported_constant(SymbolId proxy) const {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy == proxy && imported.has_constant) {
        return &imported.constant;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool is_runtime_intrinsic(
      SymbolId proxy, std::string_view public_name) const {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy != proxy || imported.public_name != public_name ||
          imported.root_relative_path != "runtime") {
        continue;
      }
      for (const ImportBinding &binding : semantic_.imports) {
        if (binding.symbol == imported.import_symbol &&
            binding.package_path == "core/runtime") {
          return true;
        }
      }
    }
    return false;
  }

  [[nodiscard]] std::optional<HirExpressionId> check_runtime_intrinsic_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      TypeId expected) {
    if (call.children.empty()) return std::nullopt;
    const SyntaxNode &callee = tree.node(call.children.front());
    const std::optional<SymbolId> symbol =
        imported_member(tree, callee, scope);
    if (!symbol.has_value() ||
        !is_runtime_intrinsic(*symbol, "call_with_context")) {
      return std::nullopt;
    }

    HirExpression expression;
    expression.kind = HirExpressionKind::Intrinsic;
    expression.range = call.range;
    expression.constant = ConstantValue::make_string("call_with_context");
    if (call.children.size() < 3) {
      diagnostics_.error(
          call.range,
          "runtime.call_with_context requires a Context pointer and callback");
      expression.type = semantic_.types.builtins().invalid;
      return hir_.add_expression(std::move(expression));
    }

    const TypeId context_pointer =
        semantic_.types.pointer(semantic_.runtime_context_type);
    const HirExpressionId context = check_expression(
        tree, call.children[1], scope, context_pointer);
    expression.operands.push_back(context);
    const HirExpression &context_expression = hir_.expression(context);
    if (context_expression.kind == HirExpressionKind::Constant &&
        context_expression.constant.kind == ConstantKind::Nil) {
      diagnostics_.error(
          context_expression.range,
          "runtime.call_with_context requires a non-nil Context pointer");
    }

    const HirExpressionId callback =
        check_expression(tree, call.children[2], scope);
    expression.operands.push_back(callback);
    const TypeId callback_type = hir_.expression(callback).type;
    if (is_invalid_type(callback_type) ||
        semantic_.types.type(callback_type).kind != TypeKind::Procedure) {
      diagnostics_.error(
          tree.node(call.children[2]).range,
          "runtime.call_with_context callback must be an ordinary procedure");
      expression.type = semantic_.types.builtins().invalid;
      return hir_.add_expression(std::move(expression));
    }
    const Type signature = semantic_.types.type(callback_type);
    if (signature.c_calling_convention) {
      diagnostics_.error(
          tree.node(call.children[2]).range,
          "runtime.call_with_context callback must use the Draft calling convention");
    }
    const std::size_t parameter_count = signature.members.empty()
        ? 0
        : signature.members.size() - 1;
    const std::size_t argument_count = call.children.size() - 3;
    if (argument_count != parameter_count) {
      diagnostics_.error(
          call.range,
          "runtime.call_with_context callback argument count does not match");
    }
    const std::size_t checked = std::min(argument_count, parameter_count);
    for (std::size_t index = 0; index < checked; ++index) {
      expression.operands.push_back(check_expression(
          tree,
          call.children[index + 3],
          scope,
          signature.members[index]));
    }
    for (std::size_t index = checked; index < argument_count; ++index) {
      expression.operands.push_back(check_expression(
          tree, call.children[index + 3], scope));
    }
    const TypeId result = signature.members.empty()
        ? semantic_.types.builtins().void_type
        : signature.members.back();
    expression.type = apply_expected_type(result, expected, call.range);
    return hir_.add_expression(std::move(expression));
  }

  // Recognizes only operations whose proxy came from the compiler-distributed
  // core/atomic package. Matching both canonical package provenance and the
  // closed public-name set prevents an unrelated package from acquiring
  // privileged lowering merely by spelling a procedure `atomic.load`.
  [[nodiscard]] std::optional<ImportedSymbol> atomic_intrinsic(
      SymbolId proxy) const {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy != proxy || imported.root_relative_path != "atomic") {
        continue;
      }
      const bool known = imported.public_name == "load" ||
          imported.public_name == "store" ||
          imported.public_name == "exchange" ||
          imported.public_name == "fetch_add" ||
          imported.public_name == "fetch_sub" ||
          imported.public_name == "fetch_and" ||
          imported.public_name == "fetch_or" ||
          imported.public_name == "fetch_xor" ||
          imported.public_name == "compare_exchange" ||
          imported.public_name == "fence";
      if (!known) continue;
      for (const ImportBinding &binding : semantic_.imports) {
        if (binding.symbol == imported.import_symbol &&
            binding.package_path == "core/atomic") {
          return imported;
        }
      }
    }
    return std::nullopt;
  }

  // Finds Order through the same imported package scope that supplied the
  // operation. This preserves nominal identity even when the source import uses
  // an alias, and avoids treating another five-member enum as a memory order.
  [[nodiscard]] std::optional<TypeId> atomic_order_type(
      const ImportedSymbol &intrinsic) const {
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner != intrinsic.import_symbol ||
          semantic_.symbols.scope(owned.scope).kind !=
              ScopeKind::ImportedPackage) {
        continue;
      }
      const std::optional<SymbolId> order =
          semantic_.symbols.lookup_direct(owned.scope, "Order");
      if (order.has_value()) return semantic_.symbols.symbol(*order).type;
    }
    return std::nullopt;
  }

  // Type-checks and folds one order argument. Orders intentionally disappear
  // as runtime operands: a dynamic order would force backend dispatch and make
  // invalid load/store order combinations reachable only at runtime.
  [[nodiscard]] std::optional<AtomicMemoryOrder> checked_atomic_order(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      TypeId order_type) {
    const HirExpressionId checked =
        check_expression(tree, expression_id, scope, order_type);
    const HirExpression &expression = hir_.expression(checked);
    std::optional<BigInteger> integer;
    if (expression.kind == HirExpressionKind::Constant &&
        expression.constant.kind == ConstantKind::Integer) {
      integer = expression.constant.integer;
    } else if (expression.kind == HirExpressionKind::Constant &&
               expression.constant.kind == ConstantKind::EnumLabel &&
               expression.symbol.is_valid()) {
      // Contextual enum syntax such as `.acquire` keeps its member identity in
      // HIR. Resolve that member's declared integer here; lowering never needs
      // to materialize the Order value because orders are compile-time only.
      for (const EnumMemberValue &member : semantic_.enum_member_values) {
        if (member.member == expression.symbol) {
          integer = member.value;
          break;
        }
      }
    }
    if (!integer.has_value()) {
      diagnostics_.error(
          expression.range,
          "atomic memory order must be a compile-time core/atomic.Order value");
      return std::nullopt;
    }
    const std::optional<std::uint64_t> value = integer->to_u64();
    if (!value.has_value() || *value > 4) {
      diagnostics_.error(expression.range, "atomic memory order is invalid");
      return std::nullopt;
    }
    return static_cast<AtomicMemoryOrder>(*value);
  }

  // The initial AArch64 target exposes naturally lock-free scalar widths only.
  // Symbolic procedure instances are deferred because their concrete instance
  // is checked again before lowering.
  [[nodiscard]] bool atomic_scalar_supported(TypeId type) const {
    if (!type.is_valid() || is_invalid_type(type)) return false;
    const Type value = semantic_.types.type(type);
    if (value.kind == TypeKind::TypeParameter) {
      // A `T: type` wrapper may be instantiated with either an integer or a
      // pointer, and Draft has no union constraint spelling for that set. Each
      // concrete procedure instance is checked again, where unsupported T is
      // rejected before MIR exists.
      return true;
    }
    if (semantic_.types.is_integer(type)) {
      return value.layout.known &&
          (value.layout.size == 1 || value.layout.size == 2 ||
           value.layout.size == 4 || value.layout.size == 8);
    }
    return value.kind == TypeKind::Pointer ||
        value.kind == TypeKind::MultiPointer ||
        value.kind == TypeKind::RawPointer || value.kind == TypeKind::CString;
  }

  // Proves that an object argument is exactly ^core/atomic.Value[T], then
  // returns T. Layout equivalence is deliberately insufficient: ordinary
  // structs with one integer field do not acquire atomic semantics.
  [[nodiscard]] std::optional<TypeId> atomic_value_element(
      TypeId pointer_type,
      const ImportedSymbol &intrinsic,
      SourceRange range) const {
    if (!pointer_type.is_valid() || is_invalid_type(pointer_type) ||
        semantic_.types.type(pointer_type).kind != TypeKind::Pointer) {
      diagnostics_.error(range, "atomic operation requires ^atomic.Value[T]");
      return std::nullopt;
    }
    const TypeId value_type = semantic_.types.type(pointer_type).element;
    const std::optional<NominalApplication> application =
        nominal_application(value_type);
    const std::optional<NominalOrigin> origin = application.has_value()
        ? nominal_origin(*application)
        : std::nullopt;
    if (!application.has_value() || !origin.has_value() ||
        origin->root_identity != intrinsic.root_identity ||
        origin->root_relative_path != intrinsic.root_relative_path ||
        origin->public_name != "Value" ||
        application->arguments->size() != 1 ||
        !application->arguments->front().is_type) {
      diagnostics_.error(range, "atomic operation requires ^atomic.Value[T]");
      return std::nullopt;
    }
    const TypeId element = application->arguments->front().type;
    if (!atomic_scalar_supported(element)) {
      diagnostics_.error(
          range,
          "atomic value type must be a supported integer or pointer type");
      return std::nullopt;
    }
    return element;
  }

  // Enforces the single-order restrictions shared by C11 and LLVM. Exchange
  // and read/modify/write accept all five orders; load and store exclude orders
  // containing a direction they cannot perform.
  [[nodiscard]] bool valid_atomic_order_for_operation(
      std::string_view operation,
      AtomicMemoryOrder order,
      SourceRange range) {
    const bool load = operation == "load";
    const bool store = operation == "store";
    if (load && (order == AtomicMemoryOrder::Release ||
                 order == AtomicMemoryOrder::AcquireRelease)) {
      diagnostics_.error(range, "atomic load cannot use a release order");
      return false;
    }
    if (store && (order == AtomicMemoryOrder::Acquire ||
                  order == AtomicMemoryOrder::AcquireRelease)) {
      diagnostics_.error(range, "atomic store cannot use an acquire order");
      return false;
    }
    return true;
  }

  // Compare-exchange has two orders because failure performs only a load. The
  // failure order therefore cannot release and cannot be stronger than the
  // success order. Diagnostics point at the failure argument, where the pair
  // becomes invalid.
  [[nodiscard]] bool valid_compare_exchange_orders(
      AtomicMemoryOrder success,
      AtomicMemoryOrder failure,
      SourceRange range) {
    if (failure == AtomicMemoryOrder::Release ||
        failure == AtomicMemoryOrder::AcquireRelease) {
      diagnostics_.error(
          range, "compare-exchange failure order cannot release");
      return false;
    }
    const bool allowed =
        failure == AtomicMemoryOrder::Relaxed ||
        (failure == AtomicMemoryOrder::Acquire &&
         (success == AtomicMemoryOrder::Acquire ||
          success == AtomicMemoryOrder::AcquireRelease ||
          success == AtomicMemoryOrder::SequentiallyConsistent)) ||
        (failure == AtomicMemoryOrder::SequentiallyConsistent &&
         success == AtomicMemoryOrder::SequentiallyConsistent);
    if (!allowed) {
      diagnostics_.error(
          range,
          "compare-exchange failure order is stronger than its success order");
    }
    return allowed;
  }

  // Converts a direct core/atomic call into one typed Intrinsic HIR node. Source
  // evaluation order is retained for the object and data operands while the
  // compile-time order operands are recorded as fields. A nullopt return means
  // the callee was not a privileged atomic operation and normal call checking
  // must continue.
  [[nodiscard]] std::optional<HirExpressionId> check_atomic_intrinsic_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      TypeId expected) {
    if (call.children.empty()) return std::nullopt;
    const std::optional<SymbolId> symbol =
        imported_member(tree, tree.node(call.children.front()), scope);
    if (!symbol.has_value()) return std::nullopt;
    const std::optional<ImportedSymbol> intrinsic = atomic_intrinsic(*symbol);
    if (!intrinsic.has_value()) return std::nullopt;

    HirExpression expression;
    expression.kind = HirExpressionKind::Intrinsic;
    expression.range = call.range;
    expression.constant = ConstantValue::make_string(
        "atomic." + intrinsic->public_name);
    const std::optional<TypeId> order_type = atomic_order_type(*intrinsic);
    if (!order_type.has_value()) {
      diagnostics_.error(call.range, "core/atomic interface has no Order type");
      expression.type = semantic_.types.builtins().invalid;
      return hir_.add_expression(std::move(expression));
    }

    const std::string &operation = intrinsic->public_name;
    if (operation == "fence") {
      if (call.children.size() != 2) {
        diagnostics_.error(call.range, "atomic.fence requires one order argument");
      } else if (const std::optional<AtomicMemoryOrder> order =
                     checked_atomic_order(
                         tree, call.children[1], scope, *order_type)) {
        expression.atomic_order = *order;
      }
      expression.type = apply_expected_type(
          semantic_.types.builtins().void_type, expected, call.range);
      return hir_.add_expression(std::move(expression));
    }

    std::size_t required_arguments = 5;
    if (operation == "load") {
      required_arguments = 2;
    } else if (operation == "store" || operation == "exchange" ||
               operation.rfind("fetch_", 0) == 0) {
      required_arguments = 3;
    }
    if (call.children.size() != required_arguments + 1) {
      diagnostics_.error(
          call.range, "atomic operation has the wrong number of arguments");
      expression.type = semantic_.types.builtins().invalid;
      return hir_.add_expression(std::move(expression));
    }

    const HirExpressionId object =
        check_expression(tree, call.children[1], scope);
    expression.operands.push_back(object);
    const std::optional<TypeId> element = atomic_value_element(
        hir_.expression(object).type, *intrinsic, hir_.expression(object).range);
    if (!element.has_value()) {
      expression.type = semantic_.types.builtins().invalid;
      return hir_.add_expression(std::move(expression));
    }

    if (operation == "compare_exchange") {
      const HirExpressionId expected_pointer = check_expression(
          tree,
          call.children[2],
          scope,
          semantic_.types.pointer(*element));
      const HirExpressionId desired =
          check_expression(tree, call.children[3], scope, *element);
      expression.operands.push_back(expected_pointer);
      expression.operands.push_back(desired);
      const std::optional<AtomicMemoryOrder> success = checked_atomic_order(
          tree, call.children[4], scope, *order_type);
      const std::optional<AtomicMemoryOrder> failure = checked_atomic_order(
          tree, call.children[5], scope, *order_type);
      if (success.has_value() && failure.has_value()) {
        expression.atomic_order = *success;
        expression.atomic_failure_order = *failure;
        (void)valid_compare_exchange_orders(
            *success, *failure, tree.node(call.children[5]).range);
      }
      expression.type = apply_expected_type(
          semantic_.types.builtins().bool_type, expected, call.range);
      return hir_.add_expression(std::move(expression));
    }

    std::size_t order_index = 2;
    if (operation != "load") {
      expression.operands.push_back(
          check_expression(tree, call.children[2], scope, *element));
      order_index = 3;
    }
    const std::optional<AtomicMemoryOrder> order = checked_atomic_order(
        tree, call.children[order_index], scope, *order_type);
    if (order.has_value()) {
      expression.atomic_order = *order;
      (void)valid_atomic_order_for_operation(
          operation, *order, tree.node(call.children[order_index]).range);
    }
    if (operation.rfind("fetch_", 0) == 0 &&
        !semantic_.types.is_integer(*element) &&
        semantic_.types.type(*element).kind != TypeKind::TypeParameter) {
      diagnostics_.error(
          call.range, "atomic fetch operation requires an integer type");
    }
    const TypeId result = operation == "store"
        ? semantic_.types.builtins().void_type
        : *element;
    expression.type = apply_expected_type(result, expected, call.range);
    return hir_.add_expression(std::move(expression));
  }

  // Returns the declaration symbol owning a nominal type's Type scope.
  [[nodiscard]] std::optional<SymbolId> type_owner(TypeId type) const {
    // Enum and tagged-union switches over a distinct wrapper still use the
    // alternative set owned by the underlying nominal declaration.
    type = underlying_type_id(type);
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (semantic_.symbols.scope(owned.scope).kind == ScopeKind::Type &&
          semantic_.symbols.symbol(owned.owner).type == type) {
        return owned.owner;
      }
    }
    return std::nullopt;
  }

  // Tagged-union discriminators are source-order integers independent from
  // enum values. Keeping this small lookup in semantic checking makes switch
  // labels scalar constants before MIR and prevents native lowering from
  // comparing whole payload-bearing aggregate values.
  [[nodiscard]] std::optional<std::uint64_t> union_discriminator(
      TypeId union_type, SymbolId alternative) const {
    const std::optional<SymbolId> owner = type_owner(union_type);
    if (!owner.has_value()) return std::nullopt;
    std::uint64_t discriminator = 0;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner != *owner) continue;
      if (member.member == alternative) return discriminator;
      ++discriminator;
    }
    return std::nullopt;
  }

  // Checks a tagged-union case label as a pattern rather than as a value
  // constructor. `.value(name)` introduces one case-local binding; `.value`
  // and `.value(_)` both select the alternative while ignoring its payload.
  [[nodiscard]] HirExpressionId check_union_case_label(
      const SyntaxTree &tree,
      NodeId label_id,
      ScopeId case_scope,
      TypeId subject_type,
      HirSwitchCase &hir_case,
      bool multiple_labels) {
    const SyntaxNode &label = tree.node(label_id);
    const std::vector<SourceName> names =
        names_in_span(tree, label.token_begin, label.token_end);
    if (label.kind != NodeKind::ContextualAlternativeExpression || names.empty()) {
      diagnostics_.error(label.range, "tagged-union case requires a contextual alternative");
      return invalid_expression(label.range);
    }
    const std::optional<SymbolId> alternative =
        find_member(subject_type, names.front().text);
    if (!alternative.has_value()) {
      diagnostics_.error(
          names.front().range, "unknown alternative '" + names.front().text + "'");
      return invalid_expression(label.range);
    }
    const Symbol member = semantic_.symbols.symbol(*alternative);
    const std::optional<std::uint64_t> discriminator =
        union_discriminator(subject_type, *alternative);
    if (!discriminator.has_value()) {
      diagnostics_.error(label.range, "tagged-union alternative has no discriminator");
      return invalid_expression(label.range);
    }

    HirExpression expression;
    expression.kind = HirExpressionKind::Constant;
    expression.range = label.range;
    expression.type = runtime_scalar_type(subject_type).element;
    expression.symbol = *alternative;
    expression.constant = ConstantValue::make_integer(
        BigInteger::from_u64(*discriminator));

    if (!label.children.empty()) {
      if (multiple_labels) {
        diagnostics_.error(
            label.range,
            "a payload-binding case cannot share its body with other labels");
      }
      if (member.type == semantic_.types.builtins().void_type) {
        diagnostics_.error(label.range, "payload-free alternative cannot bind a value");
      } else {
        const std::optional<SourceName> binding =
            single_name_expression(tree, label.children.front());
        if (!binding.has_value()) {
          diagnostics_.error(
              tree.node(label.children.front()).range,
              "tagged-union payload pattern must be a name or '_'");
        } else {
          hir_case.payload_alternative = *alternative;
          if (binding->text != "_") {
            Symbol symbol;
            symbol.name = binding->text;
            symbol.kind = SymbolKind::Local;
            symbol.scope = case_scope;
            symbol.type = member.type;
            symbol.syntax = {tree.file(), label_id};
            symbol.name_range = binding->range;
            hir_case.payload_binding =
                semantic_.symbols.declare(std::move(symbol), diagnostics_);
          }
        }
      }
    }
    return hir_.add_expression(std::move(expression));
  }

  // Case labels are values known during compilation, never expressions that
  // happen to be recomputed at runtime. Check the ordinary expression first so
  // name/type diagnostics and enum-member symbols remain available, then fold
  // its root into one typed HIR constant for MIR dispatch.
  [[nodiscard]] HirExpressionId check_value_case_label(
      const SyntaxTree &tree,
      NodeId label_id,
      ScopeId scope,
      TypeId subject_type) {
    const HirExpressionId checked =
        check_expression(tree, label_id, scope, subject_type);
    if (is_invalid_type(subject_type)) return checked;
    const ConstantTable visible_constants = active_constant_table();
    const std::vector<ConstantTypeBinding> visible_types =
        active_constant_types();
    const std::optional<EvaluatedConstant> evaluated =
        evaluate_typed_constant_expression(
            sources_,
            loaded_,
            semantic_,
            target_,
            tree,
            label_id,
            scope,
            diagnostics_,
            &visible_constants,
            &visible_types,
            subject_type);
    if (!evaluated.has_value()) return checked;

    HirExpression &expression = hir_.expression_mut(checked);
    expression.kind = HirExpressionKind::Constant;
    expression.type = subject_type;
    expression.constant = evaluated->value;
    expression.operands.clear();
    expression.addressable = false;
    return checked;
  }

  // An enum label written through a constant expression, for example
  // `cast[Mode](1)`, still covers the source member carrying that value.
  // Recovering the member keeps exhaustiveness independent of label spelling.
  [[nodiscard]] std::optional<SymbolId> enum_member_for_value(
      TypeId enum_type, const ConstantValue &value) const {
    const std::optional<SymbolId> owner = type_owner(enum_type);
    if (!owner.has_value()) return std::nullopt;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner != *owner) continue;
      const ConstantValue *candidate = constants_.find(member.member);
      if (candidate != nullptr && *candidate == value) return member.member;
    }
    return std::nullopt;
  }

  // Returns one unqualified contextual name from an expression node. This is
  // used only for compiler-defined type values and intrinsic call syntax.
  [[nodiscard]] std::optional<SourceName> single_name_expression(
      const SyntaxTree &tree, NodeId node_id) const {
    const SyntaxNode &node = tree.node(node_id);
    if (node.kind != NodeKind::NameExpression && node.kind != NodeKind::NamedType) {
      return std::nullopt;
    }
    const std::vector<SourceName> names =
        names_in_span(tree, node.token_begin, node.token_end);
    if (names.size() != 1) return std::nullopt;
    return names.front();
  }

  // Detects whether a compile-time expression depends on a symbolic parameter
  // from the current lexical scope. A parametric template must defer such a
  // static assertion; reporting "not evaluable" during symbolic checking would
  // reject every valid `static_assert(N > 0)` before an N exists. Concrete
  // instances run the ordinary evaluator with their exact value overlay.
  [[nodiscard]] bool expression_references_parametric_parameter(
      const SyntaxTree &tree, NodeId node_id, ScopeId scope) const {
    const SyntaxNode &node = tree.node(node_id);
    if (node.kind == NodeKind::NameExpression) {
      const std::optional<SourceName> name =
          single_name_expression(tree, node_id);
      if (name.has_value()) {
        const std::optional<SymbolId> symbol =
            semantic_.symbols.lookup(scope, name->text);
        if (symbol.has_value()) {
          const SymbolKind kind = semantic_.symbols.symbol(*symbol).kind;
          if (kind == SymbolKind::TypeParameter ||
              kind == SymbolKind::ValueParameter) {
            return true;
          }
        }
      }
    }
    for (NodeId child : node.children) {
      if (expression_references_parametric_parameter(tree, child, scope)) {
        return true;
      }
    }
    return false;
  }

  // Resolves a source expression that denotes a type. Type arguments to
  // `cast[T]` are parsed in expression brackets, so bare type names reach this
  // bridge rather than ordinary type syntax.
  [[nodiscard]] TypeId type_value_expression(
      const SyntaxTree &tree, NodeId node_id, ScopeId scope) {
    const SyntaxNode &node = tree.node(node_id);
    if (node_is_type_syntax(node.kind) ||
        node.kind == NodeKind::BracketExpression) {
      return substitute_active(
          resolve_type_syntax(
              sources_, loaded_, semantic_, selections_, tree, node_id, scope,
              diagnostics_),
          node.range);
    }
    if (const std::optional<SymbolId> imported = imported_member(tree, node, scope)) {
      const Symbol binding = semantic_.symbols.symbol(*imported);
      if (binding.kind == SymbolKind::Type || binding.kind == SymbolKind::TypeParameter) {
        return substitute_active(binding.type, node.range);
      }
      diagnostics_.error(node.range, "imported name does not denote a type");
      return semantic_.types.builtins().invalid;
    }
    const std::optional<SourceName> name = single_name_expression(tree, node_id);
    if (!name.has_value()) return semantic_.types.builtins().invalid;
    if (const std::optional<TypeId> builtin = semantic_.types.find_builtin(name->text)) {
      return *builtin;
    }
    const std::optional<SymbolId> symbol = semantic_.symbols.lookup(scope, name->text);
    if (symbol.has_value()) {
      const Symbol binding = semantic_.symbols.symbol(*symbol);
      if (binding.kind == SymbolKind::Type || binding.kind == SymbolKind::TypeParameter) {
        return substitute_active(binding.type, node.range);
      }
    }
    diagnostics_.error(name->range, "name does not denote a type");
    return semantic_.types.builtins().invalid;
  }

  // `::` accepts either a compile-time value expression or a type value. Most
  // type constructors are syntactically unambiguous, but aliases and generic
  // applications arrive through the ordinary expression grammar. Probe only
  // already visible bindings here; malformed arguments are diagnosed later by
  // TypeResolver after the declaration has been classified as a type.
  [[nodiscard]] bool expression_denotes_type(
      const SyntaxTree &tree, NodeId node_id, ScopeId scope) const {
    const SyntaxNode &node = tree.node(node_id);
    if (node_is_type_syntax(node.kind)) return true;
    if (node.kind == NodeKind::GroupExpression && node.children.size() == 1) {
      return expression_denotes_type(tree, node.children.front(), scope);
    }
    if (node.kind == NodeKind::TupleExpression) {
      if (node.children.size() < 2) return false;
      for (NodeId child : node.children) {
        if (!expression_denotes_type(tree, child, scope)) return false;
      }
      return true;
    }
    if (node.kind == NodeKind::MemberExpression) {
      const std::optional<SymbolId> member = imported_member(tree, node, scope);
      if (!member.has_value()) return false;
      const Symbol &binding = semantic_.symbols.symbol(*member);
      return binding.kind == SymbolKind::Type ||
          binding.kind == SymbolKind::TypeParameter;
    }
    if (node.kind == NodeKind::BracketExpression && !node.children.empty()) {
      const SyntaxNode &base = tree.node(node.children.front());
      if (base.kind == NodeKind::MemberExpression) {
        const std::optional<SymbolId> member = imported_member(tree, base, scope);
        return member.has_value() &&
            semantic_.symbols.symbol(*member).kind == SymbolKind::Type &&
            semantic_.symbols.symbol(*member).flags.parametric;
      }
      const std::optional<SourceName> name =
          single_name_expression(tree, node.children.front());
      if (!name.has_value()) return false;
      const std::optional<SymbolId> binding =
          semantic_.symbols.lookup(scope, name->text);
      return binding.has_value() &&
          semantic_.symbols.symbol(*binding).kind == SymbolKind::Type &&
          semantic_.symbols.symbol(*binding).flags.parametric;
    }
    const std::optional<SourceName> name = single_name_expression(tree, node_id);
    if (!name.has_value()) return false;
    if (semantic_.types.find_builtin(name->text).has_value()) return true;
    const std::optional<SymbolId> binding =
        semantic_.symbols.lookup(scope, name->text);
    if (!binding.has_value()) return false;
    const SymbolKind kind = semantic_.symbols.symbol(*binding).kind;
    return kind == SymbolKind::Type || kind == SymbolKind::TypeParameter;
  }

  [[nodiscard]] std::vector<ParametricParameterRecord> parameters_for(
      SymbolId owner) const {
    std::vector<ParametricParameterRecord> result;
    for (const ParametricParameterRecord &parameter :
         semantic_.parametric_parameters) {
      if (parameter.owner == owner) result.push_back(parameter);
    }
    return result;
  }

  [[nodiscard]] bool constraint_accepts(
      TypeConstraintKind constraint, TypeId argument) const {
    if (!argument.is_valid()) return false;
    const TypeKind kind = semantic_.types.type(argument).kind;
    if (kind == TypeKind::Invalid || kind == TypeKind::UntypedInteger ||
        kind == TypeKind::UntypedFloat) {
      return false;
    }
    // Symbolic template checking may pass its own parameter to another
    // template. Accept that only when the caller's constraint is at least as
    // strong as the callee's requirement. Every concrete instance is checked
    // again, so this rule never substitutes a symbolic type into native code.
    if (kind == TypeKind::TypeParameter) {
      const std::optional<TypeConstraintKind> actual = type_constraint(argument);
      if (!actual.has_value()) return false;
      if (constraint == TypeConstraintKind::AnyType) return true;
      if (constraint == TypeConstraintKind::Number) {
        return *actual == TypeConstraintKind::Integer ||
            *actual == TypeConstraintKind::Float ||
            *actual == TypeConstraintKind::Number;
      }
      return *actual == constraint;
    }
    // Distinct scalars inherit their underlying operator vocabulary but do not
    // join the built-in integer/float/number constraint sets. An alias has
    // already resolved to its target TypeId and therefore remains eligible.
    if (kind == TypeKind::Distinct &&
        constraint != TypeConstraintKind::AnyType) {
      return false;
    }
    switch (constraint) {
    case TypeConstraintKind::AnyType:
      return true;
    case TypeConstraintKind::Integer:
      return semantic_.types.is_integer(argument);
    case TypeConstraintKind::Float:
      return semantic_.types.is_float(argument);
    case TypeConstraintKind::Number:
      return semantic_.types.is_number(argument);
    case TypeConstraintKind::CompileTimeValue:
      return false;
    }
    return false;
  }

  [[nodiscard]] bool has_symbolic_type_substitution(
      const std::vector<TypeSubstitution> &substitutions) const {
    for (const TypeSubstitution &substitution : substitutions) {
      if (substitution.replacement.is_valid() &&
          semantic_.types.type(substitution.replacement).kind ==
              TypeKind::TypeParameter) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool has_symbolic_value_substitution(
      const std::vector<ValueSubstitution> &substitutions) const {
    for (const ValueSubstitution &substitution : substitutions) {
      if (substitution.symbolic_parameter.is_valid()) return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<std::size_t> substitution_index(
      const std::vector<TypeSubstitution> &substitutions,
      TypeId parameter) const {
    for (std::size_t index = 0; index < substitutions.size(); ++index) {
      if (substitutions[index].parameter == parameter) return index;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> value_substitution_index(
      const std::vector<ValueSubstitution> &substitutions,
      SymbolId parameter) const {
    for (std::size_t index = 0; index < substitutions.size(); ++index) {
      if (substitutions[index].parameter == parameter) return index;
    }
    return std::nullopt;
  }

  // Binds a dependent array/SIMD count during call inference. The structural
  // type already guarantees a concrete u64 count; this step additionally checks
  // that the number fits the declared parameter type (for example `N: u8`).
  // Repeated appearances of N must infer the same exact value.
  [[nodiscard]] bool infer_value_argument(
      SymbolId owner,
      SymbolId parameter,
      std::uint64_t value,
      std::vector<ValueSubstitution> &substitutions) {
    const std::vector<ParametricParameterRecord> parameters =
        parameters_for(owner);
    for (const ParametricParameterRecord &record : parameters) {
      if (record.parameter != parameter ||
          record.constraint != TypeConstraintKind::CompileTimeValue) {
        continue;
      }
      const TypeId required_type = semantic_.symbols.symbol(parameter).type;
      const ConstantValue constant = ConstantValue::make_integer(
          BigInteger::from_u64(value));
      if (!semantic_.types.is_integer(required_type) ||
          !integer_representable(constant.integer, required_type)) {
        return false;
      }
      const std::optional<std::size_t> existing =
          value_substitution_index(substitutions, parameter);
      if (existing.has_value()) {
        return substitutions[*existing].value == constant;
      }
      substitutions.push_back({parameter, constant, {}});
      return true;
    }
    return false;
  }

  // Unifies one symbolic signature type with an already checked argument type.
  // Draft inference is deliberately unique and structural; it never chooses a
  // default for an untyped literal or guesses a parameter visible only in the
  // result type.
  struct NominalApplication {
    std::optional<SymbolId> source;
    const ImportedType *imported = nullptr;
    const std::vector<ParametricArgument> *arguments = nullptr;
  };

  // Nominal applications are deliberately not reconstructed from aggregate
  // members. Two public structs may have identical layouts without being the
  // same type. The semantic package already retains the template source and
  // ordered arguments for local applications, and stable package provenance
  // for imported applications, so inference can use exact nominal identity.
  [[nodiscard]] std::optional<NominalApplication> nominal_application(
      TypeId type) const {
    for (const ParametricTypeInstanceRecord &instance :
         semantic_.parametric_type_instances) {
      if (semantic_.symbols.symbol(instance.instance).type == type) {
        return NominalApplication{
            instance.source, nullptr, &instance.arguments};
      }
    }
    for (const ImportedType &imported : semantic_.imported_types) {
      if (imported.type == type && !imported.arguments.empty()) {
        return NominalApplication{
            std::nullopt, &imported, &imported.arguments};
      }
    }
    return std::nullopt;
  }

  struct NominalOrigin {
    std::string_view root_identity;
    std::string_view root_relative_path;
    std::string_view public_name;
  };

  [[nodiscard]] std::optional<NominalOrigin> nominal_origin(
      const NominalApplication &application) const {
    if (application.imported != nullptr) {
      return NominalOrigin{
          application.imported->root_identity,
          application.imported->root_relative_path,
          application.imported->public_name};
    }
    if (!application.source.has_value()) return std::nullopt;
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy == *application.source) {
        return NominalOrigin{
            imported.root_identity,
            imported.root_relative_path,
            imported.public_name};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool same_nominal_template(
      const NominalApplication &pattern,
      const NominalApplication &actual) const {
    if (pattern.source.has_value() && actual.source.has_value() &&
        *pattern.source == *actual.source) {
      return true;
    }
    const std::optional<NominalOrigin> pattern_origin = nominal_origin(pattern);
    const std::optional<NominalOrigin> actual_origin = nominal_origin(actual);
    return pattern_origin.has_value() && actual_origin.has_value() &&
        pattern_origin->root_identity == actual_origin->root_identity &&
        pattern_origin->root_relative_path ==
            actual_origin->root_relative_path &&
        pattern_origin->public_name == actual_origin->public_name;
  }

  // An argument only receives an expected type after all compile-time pieces
  // of that type are known. This still lets an earlier `^Dynamic[u64]`
  // argument contextually type a later integer literal as u64, while avoiding
  // any attempt to use an unresolved T as a runtime type.
  [[nodiscard]] bool contains_symbolic_type(TypeId type) const {
    if (!type.is_valid()) return false;
    const Type value = semantic_.types.type(type);
    if (value.kind == TypeKind::TypeParameter) return true;
    if ((value.kind == TypeKind::Array || value.kind == TypeKind::Simd) &&
        value.element_count_parameter !=
            std::numeric_limits<std::uint32_t>::max()) {
      return true;
    }
    if (value.kind == TypeKind::Pointer ||
        value.kind == TypeKind::MultiPointer || value.kind == TypeKind::Slice ||
        value.kind == TypeKind::Array || value.kind == TypeKind::Simd) {
      return contains_symbolic_type(value.element);
    }
    if (value.kind == TypeKind::Tuple || value.kind == TypeKind::Procedure) {
      for (TypeId member : value.members) {
        if (contains_symbolic_type(member)) return true;
      }
      return false;
    }
    if (value.kind == TypeKind::Struct || value.kind == TypeKind::Enum ||
        value.kind == TypeKind::TaggedUnion ||
        value.kind == TypeKind::RawUnion) {
      const std::optional<NominalApplication> application =
          nominal_application(type);
      if (!application.has_value()) return false;
      for (const ParametricArgument &argument : *application->arguments) {
        if (argument.is_type && contains_symbolic_type(argument.type)) {
          return true;
        }
      }
    }
    return false;
  }

  [[nodiscard]] bool infer_type_argument(
      SymbolId owner,
      TypeId pattern_id,
      TypeId actual_id,
      std::vector<TypeSubstitution> &type_substitutions,
      std::vector<ValueSubstitution> &value_substitutions) {
    for (const ParametricParameterRecord &parameter : parameters_for(owner)) {
      if (parameter.constraint == TypeConstraintKind::CompileTimeValue) continue;
      const TypeId parameter_type =
          semantic_.symbols.symbol(parameter.parameter).type;
      if (pattern_id != parameter_type) continue;
      const std::optional<std::size_t> existing =
          substitution_index(type_substitutions, parameter_type);
      if (existing.has_value()) {
        return type_substitutions[*existing].replacement == actual_id;
      }
      if (!constraint_accepts(parameter.constraint, actual_id)) return false;
      type_substitutions.push_back({parameter_type, actual_id});
      return true;
    }

    if (pattern_id == actual_id) return true;
    if (!pattern_id.is_valid() || !actual_id.is_valid()) return false;
    const Type pattern = semantic_.types.type(pattern_id);
    const Type actual = semantic_.types.type(actual_id);
    if (pattern.kind != actual.kind) return false;

    // Aggregate template applications are nominal at the outer layer and
    // structural only in their ordered template arguments. This is the case
    // that permits `append(^Dynamic[T], T)` to infer T from `^Dynamic[u64]`.
    // Comparing members here would both miss T (the concrete layout contains
    // no symbolic type) and incorrectly equate unrelated aggregate templates.
    if (pattern.kind == TypeKind::Struct || pattern.kind == TypeKind::Enum ||
        pattern.kind == TypeKind::TaggedUnion ||
        pattern.kind == TypeKind::RawUnion) {
      const std::optional<NominalApplication> pattern_application =
          nominal_application(pattern_id);
      const std::optional<NominalApplication> actual_application =
          nominal_application(actual_id);
      if (!pattern_application.has_value() ||
          !actual_application.has_value() ||
          !same_nominal_template(*pattern_application, *actual_application) ||
          pattern_application->arguments->size() !=
              actual_application->arguments->size()) {
        return false;
      }
      for (std::size_t index = 0;
           index < pattern_application->arguments->size(); ++index) {
        const ParametricArgument &pattern_argument =
            (*pattern_application->arguments)[index];
        const ParametricArgument &actual_argument =
            (*actual_application->arguments)[index];
        if (pattern_argument.is_type != actual_argument.is_type) return false;
        if (pattern_argument.is_type) {
          if (!infer_type_argument(
                  owner,
                  pattern_argument.type,
                  actual_argument.type,
                  type_substitutions,
                  value_substitutions)) {
            return false;
          }
        } else if (pattern_argument.value_type != actual_argument.value_type ||
                   pattern_argument.value != actual_argument.value) {
          // Symbolic value arguments are not yet represented by
          // ParametricArgument. Concrete applications still compare exactly;
          // dependent arrays and SIMD vectors infer through their dedicated
          // representation below.
          return false;
        }
      }
      return true;
    }

    switch (pattern.kind) {
    case TypeKind::Pointer:
    case TypeKind::MultiPointer:
    case TypeKind::Slice:
      return infer_type_argument(
          owner,
          pattern.element,
          actual.element,
          type_substitutions,
          value_substitutions);
    case TypeKind::Array:
    case TypeKind::Simd: {
      if (actual.element_count_parameter !=
          std::numeric_limits<std::uint32_t>::max()) {
        return false;
      }
      if (pattern.element_count_parameter !=
          std::numeric_limits<std::uint32_t>::max()) {
        if (!infer_value_argument(
                owner,
                SymbolId{pattern.element_count_parameter},
                actual.element_count,
                value_substitutions)) {
          return false;
        }
      } else if (pattern.element_count != actual.element_count) {
        return false;
      }
      return infer_type_argument(
          owner,
          pattern.element,
          actual.element,
          type_substitutions,
          value_substitutions);
    }
    case TypeKind::Tuple:
    case TypeKind::Procedure:
      if (pattern.members.size() != actual.members.size() ||
          (pattern.kind == TypeKind::Procedure &&
           pattern.c_calling_convention != actual.c_calling_convention)) {
        return false;
      }
      for (std::size_t index = 0; index < pattern.members.size(); ++index) {
        if (!infer_type_argument(
                owner,
                pattern.members[index],
                actual.members[index],
                type_substitutions,
                value_substitutions)) {
          return false;
        }
      }
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] std::optional<ScopeId> procedure_scope(SymbolId owner) const {
    return owned_scope(owner, ScopeKind::Procedure);
  }

  [[nodiscard]] std::optional<ScopeId> parametric_scope(SymbolId owner) const {
    return owned_scope(owner, ScopeKind::Parametric);
  }

  [[nodiscard]] std::optional<ImportedSymbol> imported_symbol_record(
      SymbolId proxy) const {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy == proxy) return imported;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<ParametricArgument> ordered_arguments(
      const std::vector<ParametricParameterRecord> &parameters,
      const std::vector<TypeSubstitution> &type_substitutions,
      const std::vector<ValueSubstitution> &value_substitutions) const {
    std::vector<ParametricArgument> result;
    result.reserve(parameters.size());
    for (const ParametricParameterRecord &parameter : parameters) {
      ParametricArgument argument;
      if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
        const std::size_t index = *value_substitution_index(
            value_substitutions, parameter.parameter);
        argument.is_type = false;
        argument.value_type = semantic_.symbols.symbol(parameter.parameter).type;
        argument.value = value_substitutions[index].value;
      } else {
        const TypeId parameter_type =
            semantic_.symbols.symbol(parameter.parameter).type;
        const std::size_t index = *substitution_index(
            type_substitutions, parameter_type);
        argument.is_type = true;
        argument.type = type_substitutions[index].replacement;
      }
      result.push_back(std::move(argument));
    }
    return result;
  }

  // Imported templates have no source body in the consumer. We still create a
  // fully concrete local proxy so call checking, HIR, effects, and MIR can use
  // an ordinary SymbolId. Compiler orchestration later transfers the ordered
  // arguments to the defining package and fills this proxy's linker identity.
  [[nodiscard]] SymbolId instantiate_imported_procedure(
      SymbolId source,
      const ImportedSymbol &origin,
      const std::vector<ParametricParameterRecord> &parameters,
      const std::vector<TypeSubstitution> &type_substitutions,
      const std::vector<ValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    const std::vector<ParametricArgument> arguments = ordered_arguments(
        parameters, type_substitutions, value_substitutions);
    for (const ImportedProcedureInstance &instance :
         semantic_.imported_procedure_instances) {
      if (instance.source_proxy == source && instance.arguments == arguments) {
        return instance.instance_proxy;
      }
    }

    const Symbol source_symbol = semantic_.symbols.symbol(source);
    Symbol instance_symbol;
    instance_symbol.name = source_symbol.name + "$imported_instance";
    for (const ParametricArgument &argument : arguments) {
      if (argument.is_type) {
        instance_symbol.name += "$t" + std::to_string(argument.type.value);
      } else {
        instance_symbol.name += "$v" + argument.value.integer.to_decimal();
      }
    }
    instance_symbol.kind = SymbolKind::Procedure;
    instance_symbol.visibility = Visibility::Private;
    instance_symbol.flags = source_symbol.flags;
    instance_symbol.flags.parametric = false;
    instance_symbol.flags.exported = false;
    instance_symbol.scope = semantic_.package_scope;
    instance_symbol.type = substitute_type(
        source_symbol.type,
        type_substitutions,
        value_substitutions,
        use_range);
    instance_symbol.syntax = source_symbol.syntax;
    instance_symbol.name_range = source_symbol.name_range;
    const SymbolId instance_id =
        semantic_.symbols.declare(std::move(instance_symbol), diagnostics_);
    if (!instance_id.is_valid()) return {};

    ImportedSymbol concrete = origin;
    concrete.proxy = instance_id;
    // The exact generated name depends on canonical, cross-package type
    // graphs, which BodyChecker intentionally does not own. An empty name is a
    // fail-closed placeholder filled before any backend is allowed to run.
    concrete.public_name.clear();
    concrete.native_provider.clear();
    concrete.native_linker_name_spelling.clear();
    semantic_.imported_symbols.push_back(std::move(concrete));

    const std::vector<ImportedEffect> existing_effects = semantic_.imported_effects;
    for (const ImportedEffect &effect : existing_effects) {
      if (effect.procedure_proxy != source) continue;
      ImportedEffect concrete_effect = effect;
      concrete_effect.procedure_proxy = instance_id;
      semantic_.imported_effects.push_back(std::move(concrete_effect));
    }
    const std::vector<ImportedProcedureReturn> existing_returns =
        semantic_.imported_returns;
    for (const ImportedProcedureReturn &returned : existing_returns) {
      if (returned.procedure_proxy != source) continue;
      ImportedProcedureReturn concrete_return = returned;
      concrete_return.procedure_proxy = instance_id;
      for (ImportedEffect &effect : concrete_return.contract_effects) {
        effect.procedure_proxy = instance_id;
      }
      semantic_.imported_returns.push_back(std::move(concrete_return));
    }
    const std::vector<ImportedProcedureWrite> existing_writes =
        semantic_.imported_writes;
    for (const ImportedProcedureWrite &write : existing_writes) {
      if (write.procedure_proxy != source) continue;
      ImportedProcedureWrite concrete_write = write;
      concrete_write.procedure_proxy = instance_id;
      for (ImportedEffect &effect : concrete_write.value_contract_effects) {
        effect.procedure_proxy = instance_id;
      }
      semantic_.imported_writes.push_back(std::move(concrete_write));
    }
    semantic_.imported_procedure_instances.push_back({
        source,
        instance_id,
        origin.root_identity,
        origin.root_relative_path,
        origin.public_name,
        arguments,
    });
    return instance_id;
  }

  // A nested template can mention compile-time parameters owned by every
  // enclosing template. Its concrete instance must therefore retain both its
  // explicitly inferred arguments and the active outer specialization. Keep
  // unrelated package templates independent so their cache keys do not grow
  // with substitutions they can never reference.
  void append_enclosing_substitutions(
      SymbolId source,
      std::vector<TypeSubstitution> &type_substitutions,
      std::vector<ValueSubstitution> &value_substitutions) const {
    if (!is_nested_procedure(source) || !current_instance_index_.has_value()) {
      return;
    }
    const ProcedureInstance &outer = instances_[*current_instance_index_];
    for (const TypeSubstitution &substitution : outer.type_substitutions) {
      if (!substitution_index(
              type_substitutions, substitution.parameter).has_value()) {
        type_substitutions.push_back(substitution);
      }
    }
    for (const ValueSubstitution &substitution : outer.value_substitutions) {
      if (!value_substitution_index(
              value_substitutions, substitution.parameter).has_value()) {
        value_substitutions.push_back(substitution);
      }
    }
  }

  [[nodiscard]] SymbolId instantiate_procedure(
      SymbolId source,
      std::vector<TypeSubstitution> type_substitutions,
      std::vector<ValueSubstitution> value_substitutions,
      SourceRange use_range,
      std::string_view preferred_name = {}) {
    const std::vector<ParametricParameterRecord> parameters =
        parameters_for(source);
    if (parameters.empty()) {
      diagnostics_.error(use_range, "parametric procedure has no parameter metadata");
      return {};
    }
    for (const ParametricParameterRecord &parameter : parameters) {
      if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
        const std::optional<std::size_t> found = value_substitution_index(
            value_substitutions, parameter.parameter);
        if (!found.has_value()) {
          diagnostics_.error(
              use_range,
              "procedure value arguments cannot be inferred uniquely");
          return {};
        }
        const TypeId required_type =
            semantic_.symbols.symbol(parameter.parameter).type;
        const ValueSubstitution &substitution = value_substitutions[*found];
        if (substitution.value.kind != ConstantKind::Integer ||
            !semantic_.types.is_integer(required_type) ||
            !integer_representable(substitution.value.integer, required_type)) {
          diagnostics_.error(
              use_range,
              "compile-time value argument is not representable in its parameter type");
          return {};
        }
        continue;
      }
      const TypeId parameter_type =
          semantic_.symbols.symbol(parameter.parameter).type;
      const std::optional<std::size_t> found =
          substitution_index(type_substitutions, parameter_type);
      if (!found.has_value()) {
        diagnostics_.error(use_range, "procedure type arguments cannot be inferred uniquely");
        return {};
      }
      if (!constraint_accepts(
              parameter.constraint, type_substitutions[*found].replacement)) {
        diagnostics_.error(use_range, "procedure type argument does not satisfy its constraint");
        return {};
      }
    }

    append_enclosing_substitutions(
        source, type_substitutions, value_substitutions);

    if (const std::optional<ImportedSymbol> imported =
            imported_symbol_record(source)) {
      return instantiate_imported_procedure(
          source,
          *imported,
          parameters,
          type_substitutions,
          value_substitutions,
          use_range);
    }

    for (const ProcedureInstance &instance : instances_) {
      if (instance.source != source ||
          instance.type_substitutions.size() != type_substitutions.size() ||
          instance.value_substitutions.size() != value_substitutions.size()) {
        continue;
      }
      bool same = true;
      for (const TypeSubstitution &substitution : type_substitutions) {
        const std::optional<std::size_t> found =
            substitution_index(
                instance.type_substitutions, substitution.parameter);
        if (!found.has_value() ||
            instance.type_substitutions[*found].replacement !=
                substitution.replacement) {
          same = false;
          break;
        }
      }
      if (!same) continue;
      for (const ValueSubstitution &substitution : value_substitutions) {
        const std::optional<std::size_t> found = value_substitution_index(
            instance.value_substitutions, substitution.parameter);
        if (!found.has_value() ||
            instance.value_substitutions[*found].value != substitution.value) {
          same = false;
          break;
        }
      }
      if (same) return instance.symbol;
    }

    const Symbol source_symbol = semantic_.symbols.symbol(source);
    Symbol instance_symbol;
    if (!preferred_name.empty()) {
      instance_symbol.name = std::string(preferred_name);
    } else {
      // A nested source name is only unique inside its block. Its compiler
      // linkage identity includes lexical ancestry, so reuse that identity as
      // the package-private instance binding and avoid collisions between two
      // independent `helper[T]` declarations.
      instance_symbol.name =
          (source_symbol.linkage_name.empty()
               ? source_symbol.name
               : source_symbol.linkage_name) +
          "$instance";
      for (const ParametricParameterRecord &parameter : parameters) {
        if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
          const std::size_t index = *value_substitution_index(
              value_substitutions, parameter.parameter);
          instance_symbol.name += "$v" +
              value_substitutions[index].value.integer.to_decimal();
        } else {
          const TypeId parameter_type =
              semantic_.symbols.symbol(parameter.parameter).type;
          const std::size_t index = *substitution_index(
              type_substitutions, parameter_type);
          instance_symbol.name += "$t" +
              std::to_string(type_substitutions[index].replacement.value);
        }
      }
    }
    if (!source_symbol.linkage_name.empty()) {
      instance_symbol.linkage_name = instance_symbol.name;
    }
    instance_symbol.kind = SymbolKind::Procedure;
    instance_symbol.visibility = Visibility::Private;
    instance_symbol.flags = source_symbol.flags;
    instance_symbol.flags.parametric = false;
    instance_symbol.flags.exported = false;
    instance_symbol.scope = semantic_.package_scope;
    instance_symbol.type = substitute_type(
        source_symbol.type,
        type_substitutions,
        value_substitutions,
        use_range);
    instance_symbol.syntax = source_symbol.syntax;
    instance_symbol.name_range = source_symbol.name_range;
    const SymbolId instance_id =
        semantic_.symbols.declare(std::move(instance_symbol), diagnostics_);
    if (!instance_id.is_valid()) return {};

    const std::optional<ScopeId> source_parameters = procedure_scope(source);
    const std::optional<ScopeId> compile_time_parameters = parametric_scope(source);
    if (!source_parameters.has_value() || !compile_time_parameters.has_value()) {
      diagnostics_.error(use_range, "parametric procedure scopes are incomplete");
      return {};
    }
    const Scope source_scope = semantic_.symbols.scope(*source_parameters);
    const ScopeId instance_scope = semantic_.symbols.add_scope(
        ScopeKind::Procedure, *compile_time_parameters, source_scope.range);
    semantic_.owned_scopes.push_back({instance_id, instance_scope});
    const std::vector<SymbolId> source_parameter_symbols = source_scope.symbols;
    for (SymbolId parameter_id : source_parameter_symbols) {
      const Symbol parameter = semantic_.symbols.symbol(parameter_id);
      if (parameter.kind != SymbolKind::Parameter) continue;
      Symbol concrete = parameter;
      concrete.scope = instance_scope;
      concrete.type = substitute_type(
          parameter.type,
          type_substitutions,
          value_substitutions,
          use_range);
      (void)semantic_.symbols.declare(std::move(concrete), diagnostics_);
    }
    instances_.push_back({
        source,
        instance_id,
        std::move(type_substitutions),
        std::move(value_substitutions),
        false,
    });
    semantic_.parametric_instances.push_back({source, instance_id});
    return instance_id;
  }

  [[nodiscard]] HirExpressionId procedure_symbol_expression(
      SymbolId symbol, SourceRange range) {
    HirExpression expression;
    expression.kind = HirExpressionKind::Symbol;
    expression.range = range;
    expression.symbol = symbol;
    expression.type = semantic_.symbols.symbol(symbol).type;
    return hir_.add_expression(std::move(expression));
  }

  // Recognizes the closed predeclared intrinsic vocabulary before ordinary name
  // lookup. Intrinsics are HIR operations, not hidden package declarations.
  [[nodiscard]] std::optional<HirExpressionId> check_intrinsic_call(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      ScopeId scope,
      TypeId expected) {
    if (call.children.empty()) return std::nullopt;
    const NodeId callee_id = call.children.front();
    const SyntaxNode &callee = tree.node(callee_id);
    std::optional<std::string> intrinsic;
    TypeId cast_target;
    if (const std::optional<SourceName> name =
            single_name_expression(tree, callee_id)) {
      if (name->text == "len" || name->text == "assert" ||
          name->text == "size_of" || name->text == "align_of" ||
          name->text == "static_assert" || name->text == "ptr_offset" ||
          name->text == "ptr_sub") {
        intrinsic = name->text;
      }
    } else if (callee.kind == NodeKind::BracketExpression &&
               callee.children.size() == 2) {
      const std::optional<SourceName> base =
          single_name_expression(tree, callee.children.front());
      if (base.has_value() && base->text == "cast") {
        intrinsic = "cast";
        cast_target = type_value_expression(tree, callee.children[1], scope);
      }
    }
    if (!intrinsic.has_value()) return std::nullopt;

    HirExpression expression;
    expression.kind = HirExpressionKind::Intrinsic;
    expression.range = call.range;
    expression.constant = ConstantValue::make_string(*intrinsic);
    const std::size_t argument_count = call.children.size() - 1;
    if (*intrinsic == "size_of" || *intrinsic == "align_of") {
      if (argument_count != 1) {
        diagnostics_.error(
            call.range, *intrinsic + " requires exactly one type argument");
        expression.type = semantic_.types.builtins().invalid;
      } else {
        TypeId queried = type_value_expression(tree, call.children[1], scope);
        const bool defer_symbolic_layout =
            !current_instance_index_.has_value() &&
            expression_references_parametric_parameter(
                tree, call.children[1], scope);
        if (is_invalid_type(queried) ||
            !semantic_.types.type(queried).layout.known) {
          if (defer_symbolic_layout) {
            // A parametric template has no concrete size yet.  Preserve a
            // typed intrinsic in its non-lowered template HIR; each concrete
            // instance is checked again and replaces it with the exact value.
            expression.type = apply_expected_type(
                semantic_.types.builtins().usize_type, expected, call.range);
          } else {
            diagnostics_.error(
                tree.node(call.children[1]).range,
                *intrinsic + " requires a type with complete layout");
            expression.type = semantic_.types.builtins().invalid;
          }
        } else {
          const TypeLayout layout = semantic_.types.type(queried).layout;
          expression.kind = HirExpressionKind::Constant;
          expression.constant = ConstantValue::make_integer(
              BigInteger::from_u64(
                  *intrinsic == "size_of" ? layout.size : layout.alignment));
          expression.type = apply_expected_type(
              semantic_.types.builtins().usize_type, expected, call.range);
        }
      }
    } else if (*intrinsic == "static_assert") {
      if (argument_count < 1 || argument_count > 2) {
        diagnostics_.error(
            call.range, "static_assert requires a bool and optional string");
      }
      const ConstantTable active_constants = active_constant_table();
      const std::vector<ConstantTypeBinding> active_types =
          active_constant_types();
      const bool defer_symbolic_assertion =
          !current_instance_index_.has_value() && argument_count >= 1 &&
          expression_references_parametric_parameter(
              tree, call.children[1], scope);
      std::optional<ConstantValue> condition;
      if (argument_count >= 1 && !defer_symbolic_assertion) {
        condition = evaluate_constant_expression(
            sources_,
            loaded_,
            semantic_,
            target_,
            tree,
            call.children[1],
            scope,
            diagnostics_,
            &active_constants,
            &active_types);
      }
      std::string message;
      if (argument_count >= 2 && !defer_symbolic_assertion) {
        const std::optional<ConstantValue> evaluated_message =
            evaluate_constant_expression(
                sources_,
                loaded_,
                semantic_,
                target_,
                tree,
                call.children[2],
                scope,
                diagnostics_,
                &active_constants,
                &active_types);
        if (evaluated_message.has_value() &&
            evaluated_message->kind == ConstantKind::String) {
          message = evaluated_message->text;
        } else if (evaluated_message.has_value()) {
          diagnostics_.error(
              tree.node(call.children[2]).range,
              "static_assert message must be a compile-time string");
        }
      }
      if (condition.has_value() && condition->kind != ConstantKind::Bool) {
        diagnostics_.error(
            tree.node(call.children[1]).range,
            "static_assert condition must be a compile-time bool");
      } else if (condition.has_value() && !condition->boolean) {
        diagnostics_.error(
            call.range,
            "static assertion failed" +
                (message.empty() ? std::string() : ": " + message));
      }
      expression.type = semantic_.types.builtins().void_type;
    } else if (*intrinsic == "ptr_offset") {
      if (argument_count != 2) {
        diagnostics_.error(call.range, "ptr_offset requires a pointer and an isize count");
        expression.type = semantic_.types.builtins().invalid;
      } else {
        const HirExpressionId pointer =
            check_expression(tree, call.children[1], scope);
        const TypeId pointer_type = hir_.expression(pointer).type;
        const Type pointer_view = is_invalid_type(pointer_type)
            ? Type{}
            : runtime_scalar_type(pointer_type);
        const TypeKind pointer_kind = pointer_view.kind;
        const HirExpressionId count = check_expression(
            tree,
            call.children[2],
            scope,
            semantic_.types.builtins().isize_type);
        expression.operands = {pointer, count};
        if (pointer_kind != TypeKind::Pointer &&
            pointer_kind != TypeKind::MultiPointer) {
          diagnostics_.error(
              tree.node(call.children[1]).range,
              "ptr_offset requires a ^T or [^]T pointer");
          expression.type = semantic_.types.builtins().invalid;
        } else {
          const Type &pointee = semantic_.types.type(pointer_view.element);
          if (!pointee.layout.known || pointee.layout.size == 0) {
            diagnostics_.error(
                tree.node(call.children[1]).range,
                "ptr_offset requires a pointer to a complete nonempty type");
            expression.type = semantic_.types.builtins().invalid;
          } else {
            expression.type = apply_expected_type(
                pointer_type, expected, call.range);
          }
        }
      }
    } else if (*intrinsic == "ptr_sub") {
      if (argument_count != 2) {
        diagnostics_.error(call.range, "ptr_sub requires two matching pointers");
        expression.type = semantic_.types.builtins().invalid;
      } else {
        const HirExpressionId left =
            check_expression(tree, call.children[1], scope);
        const TypeId left_type = hir_.expression(left).type;
        const HirExpressionId right = check_expression(
            tree, call.children[2], scope, left_type);
        const TypeId right_type = hir_.expression(right).type;
        expression.operands = {left, right};
        const Type left_view = is_invalid_type(left_type)
            ? Type{}
            : runtime_scalar_type(left_type);
        const TypeKind left_kind = left_view.kind;
        if ((left_kind != TypeKind::Pointer &&
             left_kind != TypeKind::MultiPointer) ||
            left_type != right_type) {
          diagnostics_.error(
              call.range, "ptr_sub requires two matching ^T or [^]T pointers");
          expression.type = semantic_.types.builtins().invalid;
        } else {
          const Type &pointee = semantic_.types.type(left_view.element);
          if (!pointee.layout.known || pointee.layout.size == 0) {
            diagnostics_.error(
                call.range,
                "ptr_sub requires pointers to a complete nonempty type");
            expression.type = semantic_.types.builtins().invalid;
          } else {
            expression.type = apply_expected_type(
                semantic_.types.builtins().isize_type, expected, call.range);
          }
        }
      }
    } else if (*intrinsic == "len") {
      if (argument_count != 1) {
        diagnostics_.error(call.range, "len requires exactly one argument");
        expression.type = semantic_.types.builtins().invalid;
      } else {
        const HirExpressionId argument =
            check_expression(tree, call.children[1], scope);
        expression.operands.push_back(argument);
        const Type type = runtime_scalar_type(hir_.expression(argument).type);
        if (type.kind != TypeKind::Array && type.kind != TypeKind::Slice &&
            type.kind != TypeKind::String) {
          diagnostics_.error(call.range, "len requires an array, slice, or string");
        }
        expression.type = apply_expected_type(
            semantic_.types.builtins().usize_type, expected, call.range);
      }
    } else if (*intrinsic == "assert") {
      if (argument_count < 1 || argument_count > 2) {
        diagnostics_.error(call.range, "assert requires a bool and optional string");
      }
      if (current_procedure_uses_c_abi()) {
        diagnostics_.error(
            call.range,
            "runtime assert is unavailable in a c proc without Draft context");
      }
      if (argument_count >= 1) {
        expression.operands.push_back(check_expression(
            tree,
            call.children[1],
            scope,
            semantic_.types.builtins().bool_type));
      }
      if (argument_count >= 2) {
        expression.operands.push_back(check_expression(
            tree,
            call.children[2],
            scope,
            semantic_.types.builtins().string_type));
      }
      expression.type = semantic_.types.builtins().void_type;
    } else {
      if (argument_count != 1) {
        diagnostics_.error(call.range, "cast[T] requires exactly one value argument");
      }
      if (argument_count >= 1) {
        const HirExpressionId argument =
            check_expression(tree, call.children[1], scope);
        expression.operands.push_back(argument);
        const TypeId source = hir_.expression(argument).type;
        const TypeKind source_kind = is_invalid_type(source)
            ? TypeKind::Invalid
            : semantic_.types.type(source).kind;
        const TypeKind target_kind = is_invalid_type(cast_target)
            ? TypeKind::Invalid
            : semantic_.types.type(cast_target).kind;
        const Type source_type = is_invalid_type(source)
            ? Type{}
            : semantic_.types.type(source);
        const Type target_type = is_invalid_type(cast_target)
            ? Type{}
            : semantic_.types.type(cast_target);

        // Cast categories are intentionally direct. In particular, a distinct
        // or enum conversion cannot silently compose with a second numeric
        // conversion; source code must spell both requested operations.
        const bool distinct =
            (source_kind == TypeKind::Distinct &&
             source_type.element == cast_target) ||
            (target_kind == TypeKind::Distinct && target_type.element == source);
        const bool enumeration =
            (source_kind == TypeKind::Enum && source_type.element == cast_target) ||
            (target_kind == TypeKind::Enum && target_type.element == source);
        const bool source_plain_numeric = source_kind != TypeKind::Distinct &&
            source_kind != TypeKind::Enum &&
            (numeric_value_type(source) || is_untyped_integer(source) ||
             is_untyped_float(source));
        const bool target_plain_numeric = target_kind != TypeKind::Distinct &&
            target_kind != TypeKind::Enum && numeric_value_type(cast_target);
        const bool numeric = source_plain_numeric && target_plain_numeric;
        const bool boolean_storage =
            (source_kind == TypeKind::Bool &&
             target_kind == TypeKind::BooleanStorage) ||
            (source_kind == TypeKind::BooleanStorage &&
             target_kind == TypeKind::Bool);
        const bool endian =
            (source_kind == TypeKind::EndianScalar &&
             source_type.element == cast_target) ||
            (target_kind == TypeKind::EndianScalar &&
             target_type.element == source);
        const bool source_data_pointer = data_pointer_kind(source_kind);
        const bool target_data_pointer = data_pointer_kind(target_kind);
        const bool pointers =
            (source_data_pointer && target_data_pointer) ||
            (source_data_pointer &&
             cast_target == semantic_.types.builtins().uintptr_type) ||
            (source == semantic_.types.builtins().uintptr_type &&
             target_data_pointer);
        if (!numeric && !distinct && !enumeration && !boolean_storage &&
            !endian && !pointers) {
          diagnostics_.error(call.range, "cast source and target types are incompatible");
        }
        if ((numeric || boolean_storage ||
             (enumeration && target_kind == TypeKind::Enum)) &&
            hir_.expression(argument).kind == HirExpressionKind::Constant) {
          const std::optional<ConstantValue> converted = convert_numeric_constant(
              hir_.expression(argument).constant, cast_target, call.range);
          if (converted.has_value()) {
            expression.kind = HirExpressionKind::Constant;
            expression.constant = *converted;
            expression.operands.clear();
          }
        }
      }
      expression.type = apply_expected_type(cast_target, expected, call.range);
    }
    return hir_.add_expression(std::move(expression));
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
        const std::optional<BigInteger> value =
            big_integer_literal(sources_.text(token.range));
        if (!value.has_value()) {
          diagnostics_.error(token.range, "invalid integer literal");
          return invalid_expression(node.range);
        }
        expression.type = semantic_.types.builtins().untyped_integer;
        expression.constant = ConstantValue::make_integer(*value);
      } else if (token.kind == TokenKind::FloatLiteral) {
        const std::optional<ExactRational> value =
            ExactRational::parse_decimal(sources_.text(token.range));
        if (!value.has_value()) {
          diagnostics_.error(token.range, "invalid or excessive decimal floating literal");
          return invalid_expression(node.range);
        }
        expression.type = semantic_.types.builtins().untyped_float;
        expression.constant = ConstantValue::make_float(*value);
      } else if (token.kind == TokenKind::RuneLiteral) {
        const std::optional<std::uint32_t> value =
            decode_rune_literal(sources_.text(token.range));
        if (!value.has_value()) {
          diagnostics_.error(token.range, "invalid rune literal");
          return invalid_expression(node.range);
        }
        expression.type = semantic_.types.builtins().rune_type;
        expression.constant = ConstantValue::make_integer(
            BigInteger::from_u64(*value));
      } else if (token.kind == TokenKind::StringLiteral ||
                 token.kind == TokenKind::RawStringLiteral) {
        expression.type = semantic_.types.builtins().string_type;
        const std::optional<std::string> decoded =
            decode_string_literal(sources_.text(token.range), token.kind);
        if (!decoded.has_value()) {
          diagnostics_.error(token.range, "invalid string literal");
          return invalid_expression(node.range);
        }
        expression.constant = ConstantValue::make_string(*decoded);
      } else if (token.kind == TokenKind::KeywordNil) {
        if (!expected.is_valid() || is_invalid_type(expected)) {
          diagnostics_.error(node.range, "nil requires an expected pointer type");
          return invalid_expression(node.range);
        }
        // Distinct pointer and procedure types retain their underlying
        // equality operation. `nil` is the contextual zero-address literal
        // for that operand position, so inspect the runtime scalar category
        // while preserving the distinct TypeId on the HIR constant itself.
        const TypeKind expected_kind = runtime_scalar_type(expected).kind;
        if (expected_kind != TypeKind::Pointer &&
            expected_kind != TypeKind::MultiPointer &&
            expected_kind != TypeKind::RawPointer &&
            expected_kind != TypeKind::CString &&
            expected_kind != TypeKind::Procedure) {
          diagnostics_.error(node.range, "nil is not valid for the expected type");
          return invalid_expression(node.range);
        }
        expression.type = expected;
        // Nil is a real compile-time value, even though it has no standalone
        // type.  Preserve that fact in HIR after contextual typing so later
        // semantic checks can distinguish a known-null pointer from an
        // arbitrary pointer expression.  In particular,
        // runtime.call_with_context must reject nil before MIR lowering.
        expression.constant = ConstantValue::make_nil();
      } else {
        diagnostics_.error(node.range, "literal is not yet valid in a runtime expression");
        return invalid_expression(node.range);
      }
      const TypeId literal_type = expression.type;
      expression.type = apply_expected_type(literal_type, expected, node.range);
      contextualize_constant_value(
          expression.constant, literal_type, expression.type, node.range);
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::NameExpression: {
      const std::vector<SourceName> names =
          names_in_span(tree, node.token_begin, node.token_end);
      if (names.empty()) return invalid_expression(node.range);
      if (names.size() == 1 && names.front().text == "context") {
        const Type &procedure = semantic_.types.type(
            semantic_.symbols.symbol(current_procedure_).type);
        if (procedure.c_calling_convention) {
          diagnostics_.error(
              names.front().range,
              "the built-in context value is unavailable in a c proc");
          return invalid_expression(node.range);
        }
        HirExpression expression;
        expression.kind = HirExpressionKind::Context;
        expression.range = node.range;
        expression.type = apply_expected_type(
            semantic_.runtime_context_type, expected, node.range);
        expression.addressable = true;
        return hir_.add_expression(std::move(expression));
      }
      const std::optional<SymbolId> found =
          semantic_.symbols.lookup(scope, names.front().text);
      if (!found.has_value()) {
        diagnostics_.error(names.front().range, "unknown name '" + names.front().text + "'");
        return invalid_expression(node.range);
      }
      const Symbol symbol = semantic_.symbols.symbol(*found);
      if (captures_enclosing_runtime_binding(symbol)) {
        diagnostics_.error(
            names.front().range,
            "nested procedure cannot capture enclosing runtime binding '" +
                symbol.name + "'; pass it as an explicit parameter");
        return invalid_expression(node.range);
      }
      if (!symbol.type.is_valid() || symbol.kind == SymbolKind::Type ||
          symbol.kind == SymbolKind::Import) {
        diagnostics_.error(names.front().range, "name does not denote a runtime value");
        return invalid_expression(node.range);
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Symbol;
      expression.range = node.range;
      expression.symbol = *found;
      const TypeId symbol_type = substitute_active(symbol.type, node.range);
      expression.type = apply_expected_type(
          symbol_type, expected, node.range);
      // Parameters are immutable value bindings. Pointer and slice parameters
      // may still mutate the storage they explicitly reference through
      // dereference/index operations, but the parameter slot itself and fields
      // or fixed-array elements inside its copied value are not addressable.
      expression.addressable = symbol.kind == SymbolKind::Variable ||
          symbol.kind == SymbolKind::Local;
      if (const ConstantValue *constant = constants_.find(*found)) {
        expression.kind = HirExpressionKind::Constant;
        expression.constant = *constant;
        contextualize_constant_value(
            expression.constant, symbol_type, expression.type, node.range);
      } else if (const ConstantValue *instance_value = active_constant(*found)) {
        // A compile-time value parameter has no runtime storage in a concrete
        // monomorphization. Replacing its Symbol expression here prevents MIR
        // from ever allocating or loading a phantom parameter.
        expression.kind = HirExpressionKind::Constant;
        expression.constant = *instance_value;
        contextualize_constant_value(
            expression.constant, symbol_type, expression.type, node.range);
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::GroupExpression:
      if (!node.children.empty()) {
        return check_expression(tree, node.children.front(), scope, expected);
      }
      return invalid_expression(node.range);

    case NodeKind::TupleExpression: {
      std::vector<TypeId> member_types;
      std::vector<TypeId> expected_members;
      if (expected.is_valid() && !is_invalid_type(expected) &&
          semantic_.types.type(expected).kind == TypeKind::Tuple) {
        expected_members = semantic_.types.type(expected).members;
        if (expected_members.size() != node.children.size()) {
          diagnostics_.error(node.range, "tuple expression has the wrong arity");
        }
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Tuple;
      expression.range = node.range;
      for (std::size_t index = 0; index < node.children.size(); ++index) {
        TypeId member_expected;
        if (index < expected_members.size()) {
          member_expected = expected_members[index];
        }
        const HirExpressionId member =
            check_expression(tree, node.children[index], scope, member_expected);
        expression.operands.push_back(member);
        member_types.push_back(hir_.expression(member).type);
      }
      expression.type = !expected_members.empty()
          ? expected
          : semantic_.types.tuple(member_types);
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::CompositeExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      TypeId composite_type =
          type_value_expression(tree, node.children.front(), scope);
      if (is_invalid_type(composite_type) && expected.is_valid()) {
        composite_type = expected;
      }
      if (is_invalid_type(composite_type)) {
        diagnostics_.error(node.range, "composite literal requires a concrete type");
        return invalid_expression(node.range);
      }
      const Type composite = semantic_.types.type(composite_type);
      if (composite.kind != TypeKind::Array &&
          composite.kind != TypeKind::Struct &&
          composite.kind != TypeKind::RawUnion) {
        diagnostics_.error(node.range, "type does not support a composite literal");
        return invalid_expression(node.range);
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Composite;
      expression.range = node.range;
      expression.type = apply_expected_type(composite_type, expected, node.range);
      std::size_t positional_index = 0;
      std::size_t element_count = 0;
      std::vector<SymbolId> initialized_members;
      for (std::size_t index = 1; index < node.children.size(); ++index) {
        const SyntaxNode &element = tree.node(node.children[index]);
        if (element.children.empty()) continue;
        ++element_count;
        TypeId element_type;
        SymbolId operand_member;
        bool keyed = false;
        for (std::uint32_t token_index = element.token_begin;
             token_index < tree.node(element.children.front()).token_begin;
             ++token_index) {
          if (tree.token(token_index).kind == TokenKind::Equal) keyed = true;
        }
        if (composite.kind == TypeKind::Array) {
          if (keyed) {
            diagnostics_.error(
                element.range,
                "array composite elements must be positional");
          }
          if (positional_index >= composite.element_count) {
            diagnostics_.error(element.range, "array literal has too many elements");
          }
          element_type = composite.element;
          ++positional_index;
        } else if (!keyed) {
          diagnostics_.error(
              element.range,
              composite.kind == TypeKind::Struct
                  ? "struct composite elements must name a field"
                  : "raw union composite element must name a field");
        } else {
          const std::vector<SourceName> names = names_in_span(
              tree,
              element.token_begin,
              tree.node(element.children.front()).token_begin);
          if (!names.empty()) {
            const std::optional<SymbolId> member =
                find_member(composite_type, names.front().text);
            if (member.has_value()) {
              operand_member = *member;
              element_type = semantic_.symbols.symbol(*member).type;
              if (std::find(
                      initialized_members.begin(),
                      initialized_members.end(),
                      *member) != initialized_members.end()) {
                diagnostics_.error(
                    names.front().range,
                    "composite member is initialized more than once");
              } else {
                initialized_members.push_back(*member);
              }
            } else {
              diagnostics_.error(names.front().range, "unknown composite member");
            }
          }
        }
        expression.operands.push_back(check_expression(
            tree, element.children.front(), scope, element_type));
        expression.operand_members.push_back(operand_member);
      }
      if (composite.kind == TypeKind::RawUnion && element_count != 1) {
        diagnostics_.error(
            node.range,
            "raw union composite literal must initialize exactly one field");
      }
      return hir_.add_expression(std::move(expression));
    }

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
        if (!is_logical_bool(operand.type)) {
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
      if (operation == TokenKind::Plus) {
        expression.operation = HirOperation::Positive;
      } else if (operation == TokenKind::Minus) {
        expression.operation = HirOperation::Negate;
      } else if (operation == TokenKind::Bang) {
        expression.operation = HirOperation::LogicalNot;
      } else if (operation == TokenKind::Tilde) {
        expression.operation = HirOperation::BitwiseNot;
      }
      expression.type = apply_expected_type(result, expected, node.range);
      expression.range = node.range;
      expression.operands.push_back(operand_id);
      if ((is_untyped_integer(result) || is_untyped_float(result)) &&
          semantic_.types.is_number(expression.type)) {
        contextualize_numeric_expression(operand_id, expression.type);
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::BinaryExpression: {
      if (node.children.size() != 2) return invalid_expression(node.range);
      const TokenKind operation = binary_operator(tree, node);
      // nil has no standalone type.  A comparison such as `pointer == nil`
      // therefore has to borrow the type of the other operand.  Do this here,
      // where both operands are visible, instead of giving nil a magic raw
      // pointer type that would weaken the rest of contextual type checking.
      const auto is_nil_literal = [&tree](NodeId child) {
        const SyntaxNode &candidate = tree.node(child);
        return candidate.kind == NodeKind::LiteralExpression &&
            candidate.token_begin < candidate.token_end &&
            tree.token(candidate.token_begin).kind == TokenKind::KeywordNil;
      };
      const bool left_is_nil = is_nil_literal(node.children[0]);
      const bool right_is_nil = is_nil_literal(node.children[1]);
      HirExpressionId left_id;
      HirExpressionId right_id;
      if (left_is_nil && !right_is_nil) {
        right_id = check_expression(tree, node.children[1], scope);
        left_id = check_expression(
            tree, node.children[0], scope, hir_.expression(right_id).type);
      } else if (tree.node(node.children[0]).kind ==
          NodeKind::ContextualAlternativeExpression) {
        right_id = check_expression(tree, node.children[1], scope);
        left_id = check_expression(
            tree,
            node.children[0],
            scope,
            hir_.expression(right_id).type);
      } else {
        left_id = check_expression(tree, node.children[0], scope);
        TypeId right_expected;
        if (tree.node(node.children[1]).kind ==
                NodeKind::ContextualAlternativeExpression ||
            right_is_nil) {
          right_expected = hir_.expression(left_id).type;
        }
        right_id = check_expression(
            tree, node.children[1], scope, right_expected);
      }
      const TypeId left = hir_.expression(left_id).type;
      const TypeId right = hir_.expression(right_id).type;
      TypeId result = semantic_.types.builtins().invalid;
      if (operation == TokenKind::LogicalAnd || operation == TokenKind::LogicalOr) {
        if (left == right && is_logical_bool(left)) {
          // Plain bool naturally returns bool. A distinct bool returns the
          // same distinct type, matching the general operator-substitution
          // rule rather than silently erasing its identity.
          result = left;
        } else {
          diagnostics_.error(
              node.range, "logical operators require matching bool operands");
        }
      } else if (operation == TokenKind::EqualEqual || operation == TokenKind::BangEqual ||
                 operation == TokenKind::Less || operation == TokenKind::LessEqual ||
                 operation == TokenKind::Greater || operation == TokenKind::GreaterEqual) {
        const TypeKind left_runtime_kind = is_invalid_type(left)
            ? TypeKind::Invalid
            : runtime_scalar_type(left).kind;
        const bool equality_only = operation == TokenKind::EqualEqual ||
            operation == TokenKind::BangEqual;
        const bool pointer_equality = data_pointer_kind(left_runtime_kind) ||
            left_runtime_kind == TypeKind::Procedure;
        const bool scalar_equality = left_runtime_kind == TypeKind::Bool ||
            left_runtime_kind == TypeKind::BooleanStorage ||
            left_runtime_kind == TypeKind::EndianScalar ||
            left_runtime_kind == TypeKind::Enum || pointer_equality;
        if ((is_numeric(left) && is_numeric(right) &&
             !is_invalid_type(common_numeric_type(left, right, node.range))) ||
            (left == right &&
             ((equality_only && scalar_equality) ||
              left_runtime_kind == TypeKind::Rune))) {
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
      const bool left_untyped_numeric =
          is_untyped_integer(left) || is_untyped_float(left);
      const bool right_untyped_numeric =
          is_untyped_integer(right) || is_untyped_float(right);
      if (result == semantic_.types.builtins().bool_type &&
          left_untyped_numeric && right_untyped_numeric) {
        // An all-untyped comparison has no concrete machine type to inherit.
        // Choosing int/f64 here would reject arbitrary-precision integers or
        // round exact decimal values before comparing them. Both operands are
        // necessarily compile-time values, so preserve their specified exact
        // domains by replacing the complete comparison with its bool result.
        const ConstantTable visible_constants = active_constant_table();
        const std::vector<ConstantTypeBinding> visible_types =
            active_constant_types();
        const std::optional<ConstantValue> folded =
            evaluate_constant_expression(
                sources_,
                loaded_,
                semantic_,
                target_,
                tree,
                expression_id,
                scope,
                diagnostics_,
                &visible_constants,
                &visible_types);
        if (folded.has_value() && folded->kind == ConstantKind::Bool) {
          HirExpression comparison;
          comparison.kind = HirExpressionKind::Constant;
          comparison.type = semantic_.types.builtins().bool_type;
          comparison.range = node.range;
          comparison.constant = *folded;
          return hir_.add_expression(std::move(comparison));
        }
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Binary;
      expression.operation = hir_operation(operation);
      expression.type = apply_expected_type(result, expected, node.range);
      expression.range = node.range;
      expression.operands = {left_id, right_id};
      if (is_numeric(result)) {
        contextualize_numeric_expression(left_id, expression.type);
        contextualize_numeric_expression(right_id, expression.type);
      }
      if (result == semantic_.types.builtins().bool_type &&
          semantic_.types.is_number(left)) {
        contextualize_numeric_expression(right_id, left);
      } else if (result == semantic_.types.builtins().bool_type &&
                 semantic_.types.is_number(right)) {
        contextualize_numeric_expression(left_id, right);
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::CallExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      if (const std::optional<HirExpressionId> intrinsic =
              check_intrinsic_call(tree, node, scope, expected)) {
        return *intrinsic;
      }
      if (const std::optional<HirExpressionId> intrinsic =
              check_runtime_intrinsic_call(tree, node, scope, expected)) {
        return *intrinsic;
      }
      if (const std::optional<HirExpressionId> intrinsic =
              check_atomic_intrinsic_call(tree, node, scope, expected)) {
        return *intrinsic;
      }
      // A plain or package-qualified template name in callee position requests
      // inference. Arguments are first checked without a guessed context;
      // structural unification must discover one concrete type for every type
      // parameter before an instance is created.
      std::optional<SymbolId> inferred_template;
      if (const std::optional<SourceName> callee_name =
              single_name_expression(tree, node.children.front())) {
        inferred_template = semantic_.symbols.lookup(scope, callee_name->text);
      } else {
        const SyntaxNode &callee = tree.node(node.children.front());
        inferred_template = imported_member(tree, callee, scope);
      }
      if (inferred_template.has_value()) {
        const Symbol candidate = semantic_.symbols.symbol(*inferred_template);
        if (candidate.kind == SymbolKind::Procedure &&
            candidate.flags.parametric) {
            const Type template_signature =
                semantic_.types.type(candidate.type);
            const std::size_t parameter_count =
                template_signature.members.empty()
                ? 0
                : template_signature.members.size() - 1;
            if (node.children.size() - 1 != parameter_count) {
              diagnostics_.error(
                  node.range, "procedure call has the wrong number of arguments");
              return invalid_expression(node.range);
            }
            std::vector<HirExpressionId> arguments;
            std::vector<TypeSubstitution> type_substitutions;
            std::vector<ValueSubstitution> value_substitutions;
            for (std::size_t index = 0; index < parameter_count; ++index) {
              const TypeId argument_pattern = substitute_type(
                  template_signature.members[index],
                  type_substitutions,
                  value_substitutions,
                  tree.node(node.children[index + 1]).range);
              const TypeId argument_expected =
                  contains_symbolic_type(argument_pattern)
                  ? TypeId{}
                  : argument_pattern;
              const HirExpressionId argument =
                  check_expression(
                      tree,
                      node.children[index + 1],
                      scope,
                      argument_expected);
              arguments.push_back(argument);
              if (!infer_type_argument(
                      *inferred_template,
                      template_signature.members[index],
                      hir_.expression(argument).type,
                      type_substitutions,
                      value_substitutions)) {
                diagnostics_.error(
                    tree.node(node.children[index + 1]).range,
                    "procedure type arguments cannot be inferred uniquely");
                return invalid_expression(node.range);
              }
            }
            // A template HIR row is semantic evidence, not executable code.
            // Do not let a concrete-looking call inside that row manufacture a
            // native specialization; the concrete enclosing body is checked
            // again and creates every instance it can actually execute.
            const bool symbolic = current_procedure_is_template_ ||
                (!current_instance_index_.has_value() &&
                 has_symbolic_type_substitution(type_substitutions));
            SymbolId callee_symbol = *inferred_template;
            TypeId concrete_signature_id;
            if (symbolic) {
              concrete_signature_id = substitute_type(
                  candidate.type,
                  type_substitutions,
                  value_substitutions,
                  node.range);
            } else {
              callee_symbol = instantiate_procedure(
                  *inferred_template,
                  std::move(type_substitutions),
                  std::move(value_substitutions),
                  node.range);
              if (!callee_symbol.is_valid()) return invalid_expression(node.range);
              concrete_signature_id =
                  semantic_.symbols.symbol(callee_symbol).type;
            }
            const Type concrete_signature =
                semantic_.types.type(concrete_signature_id);
            if (current_procedure_uses_c_abi() &&
                !concrete_signature.c_calling_convention) {
              diagnostics_.error(
                  node.range,
                  "c proc cannot call an ordinary Draft procedure without "
                  "runtime.call_with_context");
            }
            HirExpression expression;
            expression.kind = HirExpressionKind::Call;
            expression.range = node.range;
            const HirExpressionId checked_callee = procedure_symbol_expression(
                callee_symbol, tree.node(node.children.front()).range);
            // Symbolic template-to-template calls keep the original declaration
            // identity for effect composition while using the substituted
            // signature for this non-lowered HIR row.
            hir_.expression_mut(checked_callee).type = concrete_signature_id;
            expression.operands.push_back(checked_callee);
            for (std::size_t index = 0; index < arguments.size(); ++index) {
              HirExpression &argument = hir_.expression_mut(arguments[index]);
              const TypeId concrete = apply_expected_type(
                  argument.type,
                  concrete_signature.members[index],
                  argument.range);
              contextualize_numeric_expression(arguments[index], concrete);
              argument.type = concrete;
              expression.operands.push_back(arguments[index]);
            }
            const TypeId result = concrete_signature.members.empty()
                ? semantic_.types.builtins().void_type
                : concrete_signature.members.back();
            expression.type = apply_expected_type(result, expected, node.range);
            return hir_.add_expression(std::move(expression));
        }
      }
      const HirExpressionId callee = check_expression(tree, node.children.front(), scope);
      const TypeId callee_type = hir_.expression(callee).type;
      // A failed specialization has already emitted the precise type/value
      // argument diagnostic. Do not obscure it with a second complaint that
      // the resulting invalid placeholder is not callable.
      if (is_invalid_type(callee_type)) return invalid_expression(node.range);
      if (runtime_scalar_type(callee_type).kind != TypeKind::Procedure) {
        diagnostics_.error(node.range, "called expression does not have procedure type");
        return invalid_expression(node.range);
      }
      const Type signature = runtime_scalar_type(callee_type);
      if (current_procedure_uses_c_abi() &&
          !signature.c_calling_convention) {
        diagnostics_.error(
            node.range,
            "c proc cannot call an ordinary Draft procedure without "
            "runtime.call_with_context");
      }
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
      if (const std::optional<SymbolId> imported = imported_member(tree, node, scope)) {
        // Atomic operations carry compile-time order and storage information in
        // HIR/MIR, so they are intentionally not first-class procedure values
        // in this initial interface. Direct calls were consumed by the atomic
        // recognizer above; reaching this branch means the operation was taken
        // as a value or explicitly specialized instead.
        if (atomic_intrinsic(*imported).has_value()) {
          diagnostics_.error(
              node.range,
              "core/atomic operations must be called directly");
          return invalid_expression(node.range);
        }
        const Symbol symbol = semantic_.symbols.symbol(*imported);
        if (symbol.kind == SymbolKind::Type || symbol.kind == SymbolKind::TypeParameter ||
            symbol.kind == SymbolKind::Import) {
          diagnostics_.error(node.range, "imported name does not denote a runtime value");
          return invalid_expression(node.range);
        }
        HirExpression expression;
        expression.kind = HirExpressionKind::Symbol;
        expression.range = node.range;
        expression.symbol = *imported;
        const TypeId imported_type =
            substitute_active(symbol.type, node.range);
        expression.type = apply_expected_type(
            imported_type, expected, node.range);
        expression.addressable = symbol.kind == SymbolKind::Variable;
        if (const ConstantValue *constant = imported_constant(*imported)) {
          expression.kind = HirExpressionKind::Constant;
          expression.constant = *constant;
          contextualize_constant_value(
              expression.constant,
              imported_type,
              expression.type,
              node.range);
        }
        return hir_.add_expression(std::move(expression));
      }
      const HirExpressionId base_id = check_expression(tree, node.children.front(), scope);
      const HirExpression base = hir_.expression(base_id);
      const Token &selector = tree.token(node.token_end - 1);
      if (selector.kind == TokenKind::IntegerLiteral) {
        const Type tuple = runtime_scalar_type(base.type);
        const std::optional<std::int64_t> index =
            integer_literal(sources_.text(selector.range));
        if (tuple.kind != TypeKind::Tuple || !index.has_value() || *index < 0 ||
            static_cast<std::uint64_t>(*index) >= tuple.members.size()) {
          diagnostics_.error(selector.range, "tuple selector is invalid or out of range");
          return invalid_expression(node.range);
        }
        const std::size_t member_index = static_cast<std::size_t>(*index);
        const TypeId member_type = tuple.members[member_index];
        if (base.kind == HirExpressionKind::Constant &&
            base.constant.kind == ConstantKind::Aggregate &&
            member_index < base.constant.elements.size()) {
          // A tuple constant has no storage identity. Selecting one member is
          // therefore another constant operation, not a runtime extraction.
          // Folding here is essential when the tuple's members are still
          // untyped: only the selected scalar receives the surrounding
          // concrete context, while the unused aggregate never reaches MIR.
          HirExpression expression;
          expression.kind = HirExpressionKind::Constant;
          expression.range = node.range;
          expression.type = apply_expected_type(
              member_type, expected, node.range);
          expression.constant = base.constant.elements[member_index];
          contextualize_constant_value(
              expression.constant,
              member_type,
              expression.type,
              node.range);
          return hir_.add_expression(std::move(expression));
        }
        HirExpression expression;
        expression.kind = HirExpressionKind::Member;
        expression.range = node.range;
        expression.type = apply_expected_type(
            member_type, expected, node.range);
        expression.operands.push_back(base_id);
        expression.constant = ConstantValue::make_integer(*index);
        expression.addressable = base.addressable;
        return hir_.add_expression(std::move(expression));
      }
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
      expression.type = apply_expected_type(
          substitute_active(member_symbol.type, node.range), expected, node.range);
      expression.operands.push_back(base_id);
      expression.addressable = base.addressable && member_symbol.kind == SymbolKind::Field;
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::DereferenceExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      const HirExpressionId pointer_id = check_expression(tree, node.children.front(), scope);
      const Type pointer = runtime_scalar_type(hir_.expression(pointer_id).type);
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
      const HirExpressionId base_id = check_expression(tree, node.children[0], scope);
      const HirExpression base_expression = hir_.expression(base_id);
      if (base_expression.symbol.is_valid()) {
        const Symbol base_symbol =
            semantic_.symbols.symbol(base_expression.symbol);
        if (base_symbol.kind == SymbolKind::Procedure &&
            base_symbol.flags.parametric) {
          const std::vector<ParametricParameterRecord> parameters =
              parameters_for(base_expression.symbol);
          if (node.children.size() - 1 != parameters.size()) {
            diagnostics_.error(
                node.range,
                "parametric procedure application has the wrong number of arguments");
            return invalid_expression(node.range);
          }
          std::vector<TypeSubstitution> type_substitutions;
          std::vector<ValueSubstitution> value_substitutions;
          for (std::size_t index = 0; index < parameters.size(); ++index) {
            const ParametricParameterRecord &parameter = parameters[index];
            if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
              if (!current_instance_index_.has_value()) {
                const std::optional<SourceName> symbolic_name =
                    single_name_expression(tree, node.children[index + 1]);
                if (symbolic_name.has_value()) {
                  const std::optional<SymbolId> symbolic =
                      semantic_.symbols.lookup(scope, symbolic_name->text);
                  if (symbolic.has_value() &&
                      semantic_.symbols.symbol(*symbolic).kind ==
                          SymbolKind::ValueParameter) {
                    const TypeId required =
                        semantic_.symbols.symbol(parameter.parameter).type;
                    const TypeId supplied =
                        semantic_.symbols.symbol(*symbolic).type;
                    if (required != supplied) {
                      diagnostics_.error(
                          tree.node(node.children[index + 1]).range,
                          "symbolic procedure value argument has the wrong type");
                      return invalid_expression(node.range);
                    }
                    value_substitutions.push_back(
                        {parameter.parameter, {}, *symbolic});
                    continue;
                  }
                }
              }
              const ConstantTable active_constants = active_constant_table();
              const std::optional<ConstantValue> value =
                  evaluate_constant_expression(
                      sources_,
                      loaded_,
                      semantic_,
                      target_,
                      tree,
                      node.children[index + 1],
                      scope,
                      diagnostics_,
                      &active_constants);
              if (!value.has_value()) return invalid_expression(node.range);
              if (value->kind != ConstantKind::Integer) {
                diagnostics_.error(
                    tree.node(node.children[index + 1]).range,
                    "procedure value argument must be a compile-time integer");
                return invalid_expression(node.range);
              }
              value_substitutions.push_back(
                  {parameter.parameter, *value, {}});
              continue;
            }
            const TypeId argument =
                type_value_expression(tree, node.children[index + 1], scope);
            if (!constraint_accepts(parameter.constraint, argument)) {
              diagnostics_.error(
                  tree.node(node.children[index + 1]).range,
                  "procedure type argument does not satisfy its constraint");
              return invalid_expression(node.range);
            }
            type_substitutions.push_back(
                {semantic_.symbols.symbol(parameter.parameter).type, argument});
          }
          if (current_procedure_is_template_ ||
              (!current_instance_index_.has_value() &&
               (has_symbolic_type_substitution(type_substitutions) ||
                has_symbolic_value_substitution(value_substitutions)))) {
            const TypeId symbolic_signature = substitute_type(
                base_symbol.type,
                type_substitutions,
                value_substitutions,
                node.range);
            const HirExpressionId application = procedure_symbol_expression(
                base_expression.symbol, node.range);
            hir_.expression_mut(application).type = symbolic_signature;
            return application;
          }
          const SymbolId instance = instantiate_procedure(
              base_expression.symbol,
              std::move(type_substitutions),
              std::move(value_substitutions),
              node.range);
          if (!instance.is_valid()) return invalid_expression(node.range);
          return procedure_symbol_expression(instance, node.range);
        }
      }
      if (node.children.size() != 2) {
        diagnostics_.error(node.range, "indexing requires exactly one index");
        return invalid_expression(node.range);
      }
      const Type base = runtime_scalar_type(base_expression.type);
      if (base.kind != TypeKind::Array && base.kind != TypeKind::Slice &&
          base.kind != TypeKind::MultiPointer && base.kind != TypeKind::String) {
        diagnostics_.error(
            node.range,
            "indexing requires an array, slice, string, or multi-pointer");
        return invalid_expression(node.range);
      }
      const HirExpressionId index_id = check_expression(
          tree,
          node.children[1],
          scope,
          semantic_.types.builtins().usize_type);
      if (!is_integer(hir_.expression(index_id).type)) {
        diagnostics_.error(tree.node(node.children[1]).range, "index must be an integer");
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Index;
      expression.range = node.range;
      const TypeId element = base.kind == TypeKind::String
          ? semantic_.types.builtins().u8_type
          : base.element;
      expression.type = apply_expected_type(element, expected, node.range);
      expression.operands = {base_id, index_id};
      if (const std::optional<std::uint64_t> length =
              compile_time_length(base_id)) {
        const std::optional<BigInteger> constant =
            constant_integer_expression(index_id);
        if (constant.has_value()) {
          const std::optional<std::uint64_t> index = constant->to_u64();
          if (index.has_value() && *index < *length) {
            expression.bounds_proven = true;
          } else if (index.has_value()) {
            diagnostics_.error(
                tree.node(node.children[1]).range,
                "constant index " + std::to_string(*index) +
                    " is out of bounds for length " +
                    std::to_string(*length));
          }
        }
      }
      // A string is an immutable byte view. Lowering still computes an address
      // internally to load the byte, but source code may not take that address
      // or use the indexed expression as an assignment target.
      expression.addressable =
          base.kind == TypeKind::Slice || base.kind == TypeKind::MultiPointer ||
          (base.kind == TypeKind::Array && base_expression.addressable);
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::SliceExpression: {
      if (node.children.empty()) return invalid_expression(node.range);
      const HirExpressionId base_id = check_expression(tree, node.children[0], scope);
      const Type base = runtime_scalar_type(hir_.expression(base_id).type);
      TypeId result = semantic_.types.builtins().invalid;
      if (base.kind == TypeKind::Slice) {
        result = hir_.expression(base_id).type;
      } else if (base.kind == TypeKind::String) {
        // Slicing a string has the same source and result type. Under the
        // distinct substitution rule, a distinct string therefore remains
        // distinct, just as a distinct slice does above.
        result = hir_.expression(base_id).type;
      } else if (base.kind == TypeKind::Array) {
        result = semantic_.types.slice(base.element);
        // An array slice contains a pointer into the array's storage. Do not
        // manufacture that mutable view from a temporary or from an immutable
        // value parameter; the caller must make an explicit mutable local copy
        // first. Slice and multi-pointer values already are explicit views and
        // therefore do not need this storage check.
        if (!hir_.expression(base_id).addressable) {
          diagnostics_.error(
              tree.node(node.children.front()).range,
              "slicing an array requires addressable storage");
        }
      } else if (base.kind == TypeKind::MultiPointer) {
        result = semantic_.types.slice(base.element);
      } else {
        diagnostics_.error(
            node.range,
            "slicing requires an array, slice, string, or multi-pointer");
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Slice;
      expression.range = node.range;
      expression.type = apply_expected_type(result, expected, node.range);
      expression.operands.push_back(base_id);
      std::optional<std::uint32_t> colon;
      for (std::uint32_t token_index = node.token_begin;
           token_index < node.token_end;
           ++token_index) {
        if (tree.token(token_index).kind == TokenKind::Colon) {
          colon = token_index;
          break;
        }
      }
      for (std::size_t index = 1; index < node.children.size(); ++index) {
        const HirExpressionId bound = check_expression(
            tree,
            node.children[index],
            scope,
            semantic_.types.builtins().usize_type);
        if (!is_integer(hir_.expression(bound).type)) {
          diagnostics_.error(tree.node(node.children[index]).range, "slice bound must be an integer");
        }
        expression.operands.push_back(bound);
        if (colon.has_value() && tree.node(node.children[index]).token_end <= *colon) {
          expression.slice_has_low = true;
        } else {
          expression.slice_has_high = true;
        }
      }
      if (base.kind == TypeKind::MultiPointer &&
          (expression.slice_has_low || !expression.slice_has_high)) {
        // A multi-pointer carries no allocation length, so only `p[:length]`
        // can establish a slice. Accepting `p[low:high]` would imply a bounds
        // fact the pointer does not possess and would make `p[:]` unlowerable.
        diagnostics_.error(
            node.range,
            "multi-pointer slicing requires the form 'pointer[:length]'");
      }
      if (const std::optional<std::uint64_t> length =
              compile_time_length(base_id)) {
        std::size_t operand_index = 1;
        std::optional<std::uint64_t> low = 0;
        std::optional<std::uint64_t> high = length;
        if (expression.slice_has_low) {
          const std::optional<BigInteger> value =
              constant_integer_expression(expression.operands[operand_index++]);
          low = value.has_value() ? value->to_u64() : std::nullopt;
        }
        if (expression.slice_has_high) {
          const std::optional<BigInteger> value =
              constant_integer_expression(expression.operands[operand_index]);
          high = value.has_value() ? value->to_u64() : std::nullopt;
        }
        if (low.has_value() && high.has_value()) {
          if (*low <= *high && *high <= *length) {
            expression.bounds_proven = true;
          } else {
            diagnostics_.error(
                node.range,
                "constant slice bounds [" + std::to_string(*low) + ":" +
                    std::to_string(*high) + "] are invalid for length " +
                    std::to_string(*length));
          }
        }
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::ConditionalExpression: {
      if (node.children.size() != 3) return invalid_expression(node.range);
      const HirExpressionId condition = check_expression(
          tree, node.children[1], scope, semantic_.types.builtins().bool_type);
      const HirExpressionId left = check_expression(tree, node.children[0], scope, expected);
      const TypeId left_type = hir_.expression(left).type;
      // A concrete left branch supplies useful context to `nil`, contextual
      // alternatives, and untyped constants on the right. An untyped numeric
      // left branch does not: both untyped branches must be inspected before
      // deciding whether their common exact domain is integer or floating.
      TypeId right_expected = expected;
      if (!right_expected.is_valid() &&
          !is_untyped_integer(left_type) &&
          !is_untyped_float(left_type)) {
        right_expected = left_type;
      }
      const HirExpressionId right = check_expression(
          tree,
          node.children[2],
          scope,
          right_expected);
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
      const TypeKind expected_kind = runtime_scalar_type(expected).kind;
      if (expected_kind != TypeKind::Enum && expected_kind != TypeKind::TaggedUnion) {
        diagnostics_.error(node.range, "contextual alternative expected type is not an enum or tagged union");
        return invalid_expression(node.range);
      }
      const std::vector<SourceName> names =
          names_in_span(tree, node.token_begin, node.token_end);
      if (names.empty()) return invalid_expression(node.range);
      // The first name follows the leading dot. A payload expression may itself
      // contain names, so using the last name would resolve `.some(value)` as an
      // alternative named `value`.
      const std::optional<SymbolId> alternative = find_member(expected, names.front().text);
      if (!alternative.has_value()) {
        diagnostics_.error(names.front().range, "unknown alternative '" + names.front().text + "'");
        return invalid_expression(node.range);
      }
      const Symbol member = semantic_.symbols.symbol(*alternative);
      HirExpression expression;
      expression.kind = HirExpressionKind::Constant;
      expression.range = node.range;
      expression.type = expected;
      expression.symbol = *alternative;
      expression.constant = ConstantValue::make_enum_label(names.front().text);
      if (expected_kind == TypeKind::TaggedUnion) {
        // Both payload-bearing and payload-free alternatives are aggregate
        // values. MIR inserts their source-order discriminator and optional
        // payload; representing the latter as a scalar constant would lose the
        // tagged union's physical storage type.
        expression.kind = HirExpressionKind::Composite;
        const bool has_payload = member.type != semantic_.types.builtins().void_type;
        if (has_payload != !node.children.empty()) {
          diagnostics_.error(node.range, has_payload
              ? "tagged-union alternative requires a payload"
              : "payload-free alternative cannot carry a value");
        } else if (has_payload) {
          expression.operands.push_back(check_expression(
              tree, node.children.front(), scope, member.type));
        }
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::DenyExpression: {
      // Keep the selector's scope as a semantic fact. The governed expression
      // introduces no scope, but recording the site still prevents later
      // context construction from trying to re-resolve selector text in some
      // deeper synthesis-site scope where a same-named binding may shadow it.
      semantic_.sites.push_back(
          {SemanticSiteKind::DenialExpression,
           {tree.file(), expression_id},
           scope,
           current_procedure_,
           {}});
      if (!node.children.empty()) {
        const HirExpressionId value =
            check_expression(tree, node.children.back(), scope, expected);
        HirExpression expression;
        expression.kind = HirExpressionKind::Denial;
        expression.range = node.range;
        expression.syntax = {tree.file(), expression_id};
        expression.scope = scope;
        expression.type = hir_.expression(value).type;
        expression.operands.push_back(value);
        expression.addressable = hir_.expression(value).addressable;
        return hir_.add_expression(std::move(expression));
      }
      return invalid_expression(node.range);
    }

    case NodeKind::SynthesisExpression: {
      semantic_.sites.push_back(
          {SemanticSiteKind::SynthesisExpression,
           {tree.file(), expression_id},
           scope,
           current_procedure_,
           expected});
      HirExpression expression;
      expression.kind = HirExpressionKind::Synthesis;
      expression.range = node.range;
      expression.type = expected;
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::AsmExpression: {
      TypeId result = semantic_.types.builtins().invalid;
      HirExpression expression;
      expression.kind = HirExpressionKind::Assembly;
      expression.range = node.range;
      expression.syntax = {tree.file(), expression_id};
      expression.scope = scope;
      for (NodeId child : node.children) {
        if (node_is_type_syntax(tree.node(child).kind)) {
          result = resolve_type_syntax(
              sources_, loaded_, semantic_, selections_, tree, child, scope, diagnostics_);
        } else if (tree.node(child).kind == NodeKind::AsmInput &&
                   !tree.node(child).children.empty()) {
          expression.operands.push_back(check_expression(
              tree, tree.node(child).children.front(), scope));
        } else if (tree.node(child).kind == NodeKind::SynthesisAssembly) {
          semantic_.sites.push_back(
              {SemanticSiteKind::SynthesisAssembly,
               {tree.file(), child},
               scope,
               current_procedure_,
               {}});
        }
      }
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

  [[nodiscard]] bool is_discard_target(
      const SyntaxTree &tree, NodeId expression) const {
    const std::optional<SourceName> name =
        single_name_expression(tree, expression);
    return name.has_value() && name->text == "_";
  }

  [[nodiscard]] HirExpressionId discard_expression(SourceRange range) {
    HirExpression expression;
    expression.kind = HirExpressionKind::Discard;
    expression.range = range;
    expression.type = semantic_.types.builtins().invalid;
    return hir_.add_expression(std::move(expression));
  }

  // Checks `(a, _, b) = tuple` as one aggregate evaluation followed by stores
  // to the non-discard targets. Target addresses remain first in HIR source
  // order, and assignment_member_indices retains the corresponding aggregate
  // positions for MIR extraction.
  [[nodiscard]] HirStatementId check_tuple_assignment(
      const SyntaxTree &tree,
      NodeId statement_id,
      ScopeId scope,
      const SyntaxNode &node,
      const SyntaxNode &pattern,
      std::size_t right_count) {
    HirStatement statement;
    statement.kind = HirStatementKind::Assignment;
    statement.range = node.range;
    statement.syntax = {tree.file(), statement_id};
    statement.operation = HirOperation::Assign;
    statement.assignment_destructures_tuple = true;
    if (right_count != 1) {
      diagnostics_.error(
          node.range, "tuple assignment requires exactly one right-hand value");
    }

    std::vector<TypeId> target_types(
        pattern.children.size(), semantic_.types.builtins().invalid);
    for (std::size_t index = 0; index < pattern.children.size(); ++index) {
      const NodeId target_id = pattern.children[index];
      if (is_discard_target(tree, target_id)) continue;
      const HirExpressionId target = check_expression(tree, target_id, scope);
      statement.expressions.push_back(target);
      statement.assignment_member_indices.push_back(index);
      target_types[index] = hir_.expression(target).type;
      if (!hir_.expression(target).addressable) {
        diagnostics_.error(
            tree.node(target_id).range,
            "tuple assignment target is not addressable");
      }
    }
    statement.assignment_target_count = statement.expressions.size();

    if (right_count == 0) return hir_.add_statement(std::move(statement));
    const NodeId value_id = node.children.back();
    const HirExpressionId value = check_expression(tree, value_id, scope);
    statement.expressions.push_back(value);
    const TypeId value_type = hir_.expression(value).type;
    if (is_invalid_type(value_type) ||
        semantic_.types.type(value_type).kind != TypeKind::Tuple) {
      diagnostics_.error(
          tree.node(value_id).range,
          "tuple assignment requires a tuple value");
      return hir_.add_statement(std::move(statement));
    }

    const Type tuple = semantic_.types.type(value_type);
    if (tuple.members.size() != pattern.children.size()) {
      diagnostics_.error(
          pattern.range, "tuple assignment pattern has the wrong arity");
      return hir_.add_statement(std::move(statement));
    }

    // A direct tuple literal may still contain exact untyped numerics. Apply a
    // target's expected type before defaulting discarded positions to int/f64,
    // then replace the tuple's pseudo-type with its complete physical type.
    std::vector<TypeId> concrete_members = tuple.members;
    const HirExpression tuple_expression = hir_.expression(value);
    for (std::size_t index = 0; index < tuple.members.size(); ++index) {
      const bool has_target = !is_invalid_type(target_types[index]);
      concrete_members[index] = has_target
          ? apply_expected_type(
                tuple.members[index],
                target_types[index],
                tree.node(pattern.children[index]).range)
          : default_inferred_runtime_type(tuple.members[index]);
      if (tuple_expression.kind == HirExpressionKind::Tuple &&
          index < tuple_expression.operands.size()) {
        contextualize_inferred_runtime_expression(
            tuple_expression.operands[index], concrete_members[index]);
      }
    }
    const TypeId concrete_tuple = semantic_.types.tuple(concrete_members);
    if (tuple_expression.kind == HirExpressionKind::Tuple) {
      hir_.expression_mut(value).type = concrete_tuple;
    } else if (tuple_expression.kind == HirExpressionKind::Constant) {
      contextualize_inferred_runtime_expression(value, concrete_tuple);
    }
    return hir_.add_statement(std::move(statement));
  }

  // Checks ordinary one-to-one assignment, retaining explicit Discard rows so
  // target and value positions remain parallel. A discard is valid only for
  // plain `=`; compound assignment necessarily reads and writes its target.
  [[nodiscard]] HirStatementId check_assignment_statement(
      const SyntaxTree &tree,
      NodeId statement_id,
      ScopeId scope) {
    const SyntaxNode &node = tree.node(statement_id);
    HirStatement statement;
    statement.kind = HirStatementKind::Assignment;
    statement.range = node.range;
    statement.syntax = {tree.file(), statement_id};
    const std::optional<std::uint32_t> operation =
        assignment_operator_index(tree, node);
    if (!operation.has_value()) {
      diagnostics_.error(node.range, "assignment has no operator");
      return hir_.add_statement(std::move(statement));
    }
    statement.operation = hir_operation(tree.token(*operation).kind);
    std::size_t left_count = 0;
    for (NodeId child : node.children) {
      if (tree.node(child).token_end <= *operation) ++left_count;
    }
    const std::size_t right_count = node.children.size() - left_count;
    if (left_count == 1 && statement.operation == HirOperation::Assign &&
        tree.node(node.children.front()).kind == NodeKind::TupleExpression) {
      return check_tuple_assignment(
          tree,
          statement_id,
          scope,
          node,
          tree.node(node.children.front()),
          right_count);
    }
    if (left_count != right_count) {
      diagnostics_.error(
          node.range, "assignment sides must have equal arity");
    }

    for (std::size_t index = 0; index < left_count; ++index) {
      const NodeId target_id = node.children[index];
      if (is_discard_target(tree, target_id)) {
        if (statement.operation != HirOperation::Assign) {
          diagnostics_.error(
              tree.node(target_id).range,
              "discard cannot be used with compound assignment");
        }
        statement.expressions.push_back(
            discard_expression(tree.node(target_id).range));
        continue;
      }
      const HirExpressionId target = check_expression(tree, target_id, scope);
      statement.expressions.push_back(target);
      if (!hir_.expression(target).addressable) {
        diagnostics_.error(
            tree.node(target_id).range,
            "assignment target is not addressable");
      }
    }
    statement.assignment_target_count = left_count;

    const std::size_t paired = std::min(left_count, right_count);
    for (std::size_t index = 0; index < right_count; ++index) {
      TypeId expected;
      if (index < paired) {
        const HirExpression &target = hir_.expression(
            statement.expressions[index]);
        if (target.kind != HirExpressionKind::Discard &&
            statement.operation != HirOperation::ShiftLeft &&
            statement.operation != HirOperation::ShiftRight) {
          expected = target.type;
        }
      }
      const HirExpressionId value = check_expression(
          tree, node.children[left_count + index], scope, expected);
      if (!expected.is_valid()) {
        const TypeId concrete =
            default_inferred_runtime_type(hir_.expression(value).type);
        contextualize_inferred_runtime_expression(value, concrete);
      }
      statement.expressions.push_back(value);
      if (index < paired && statement.operation != HirOperation::Assign) {
        const HirExpression &target =
            hir_.expression(statement.expressions[index]);
        if (target.kind != HirExpressionKind::Discard) {
          check_compound_assignment_operator(
              statement.operation,
              target.type,
              hir_.expression(value).type,
              node.range);
        }
      }
    }
    return hir_.add_statement(std::move(statement));
  }

  // Declares and resolves one lexical type. Like a package type declaration,
  // its source name is installed before members are visited so recursive
  // pointer fields and nested procedure signatures can refer to it. The HIR row
  // records source ordering only; no runtime storage or initialization exists.
  [[nodiscard]] HirStatementId check_local_type_declaration(
      const SyntaxTree &tree,
      NodeId declaration_id,
      NodeId type_id,
      ScopeId scope,
      const std::vector<SourceName> &names) {
    const SyntaxNode &declaration = tree.node(declaration_id);
    HirStatement statement;
    statement.kind = HirStatementKind::TypeDeclaration;
    statement.range = declaration.range;
    statement.syntax = {tree.file(), declaration_id};
    if (names.size() != 1 || names.front().text == "_") {
      diagnostics_.error(
          declaration.range,
          "local type declaration requires one non-discard binding name");
      return hir_.add_statement(std::move(statement));
    }

    const SourceName &name = names.front();
    Symbol symbol;
    symbol.name = name.text;
    symbol.kind = SymbolKind::Type;
    symbol.visibility = Visibility::Private;
    symbol.scope = scope;
    symbol.syntax = {tree.file(), declaration_id};
    symbol.name_range = name.range;
    for (NodeId child : declaration.children) {
      if (tree.node(child).kind == NodeKind::ParametricParameterList) {
        symbol.flags.parametric = true;
      }
    }
    const SymbolId id =
        semantic_.symbols.declare(std::move(symbol), diagnostics_);
    if (!id.is_valid()) return hir_.add_statement(std::move(statement));
    statement.bindings.push_back(id);

    // Nominal identity must exist before the resolver opens its member scope.
    // Allocate it only after successful declaration so a duplicate name cannot
    // perturb deterministic TypeId assignment.
    if (const std::optional<TypeKind> nominal =
            nominal_type_kind(tree.node(type_id).kind)) {
      semantic_.symbols.symbol_mut(id).type = semantic_.types.begin_nominal(
          *nominal, name.text, name.range);
    }

    const ConstantTable visible_constants = active_constant_table();
    const std::vector<ConstantTypeBinding> visible_types =
        active_constant_types();
    (void)resolve_local_type_declaration(
        sources_,
        loaded_,
        semantic_,
        selections_,
        tree,
        declaration_id,
        type_id,
        scope,
        id,
        visible_constants,
        visible_types,
        diagnostics_);
    return hir_.add_statement(std::move(statement));
  }

  // Evaluates a lexical `::` declaration exactly once and retains its value by
  // SymbolId. Nested procedures may then use the binding because it represents
  // compile-time state, never storage from an enclosing invocation.
  [[nodiscard]] HirStatementId check_compile_time_declaration(
      const SyntaxTree &tree,
      NodeId declaration_id,
      NodeId expression_id,
      ScopeId scope,
      const std::vector<SourceName> &names) {
    const SyntaxNode &declaration = tree.node(declaration_id);
    HirStatement statement;
    statement.kind = HirStatementKind::CompileTimeDeclaration;
    statement.range = declaration.range;
    statement.syntax = {tree.file(), declaration_id};
    if (names.size() != 1 || names.front().text == "_") {
      diagnostics_.error(
          declaration.range,
          "compile-time declaration requires one non-discard binding name");
      return hir_.add_statement(std::move(statement));
    }

    Symbol symbol;
    symbol.name = names.front().text;
    symbol.kind = SymbolKind::Constant;
    symbol.visibility = Visibility::Private;
    symbol.scope = scope;
    symbol.syntax = {tree.file(), declaration_id};
    symbol.name_range = names.front().range;
    const SymbolId id =
        semantic_.symbols.declare(std::move(symbol), diagnostics_);
    if (!id.is_valid()) return hir_.add_statement(std::move(statement));
    statement.bindings.push_back(id);

    // A symbolic generic body is checked before its exact value arguments are
    // known. Retain the expression's semantic type now, then evaluate and fold
    // the declaration when each concrete body is checked. The template HIR is
    // never lowered, so this creates no phantom runtime storage.
    if (current_procedure_is_template_ &&
        expression_references_parametric_parameter(
            tree, expression_id, scope)) {
      const HirExpressionId typed =
          check_expression(tree, expression_id, scope);
      semantic_.symbols.symbol_mut(id).type =
          default_inferred_runtime_type(hir_.expression(typed).type);
      return hir_.add_statement(std::move(statement));
    }

    const ConstantTable visible_constants = active_constant_table();
    const std::vector<ConstantTypeBinding> visible_types =
        active_constant_types();
    const std::optional<EvaluatedConstant> evaluated =
        evaluate_typed_constant_expression(
            sources_,
            loaded_,
            semantic_,
            target_,
            tree,
            expression_id,
            scope,
            diagnostics_,
            &visible_constants,
            &visible_types);
    if (!evaluated.has_value()) {
      semantic_.symbols.symbol_mut(id).type =
          semantic_.types.builtins().invalid;
      return hir_.add_statement(std::move(statement));
    }
    TypeId type = evaluated->type;
    if (!type.is_valid()) {
      switch (evaluated->value.kind) {
      case ConstantKind::Bool:
        type = semantic_.types.builtins().bool_type;
        break;
      case ConstantKind::Integer: {
        const SyntaxNode &expression = tree.node(expression_id);
        const bool rune = expression.kind == NodeKind::LiteralExpression &&
            expression.token_begin < expression.token_end &&
            tree.token(expression.token_begin).kind == TokenKind::RuneLiteral;
        type = rune
            ? semantic_.types.builtins().rune_type
            : semantic_.types.builtins().untyped_integer;
        break;
      }
      case ConstantKind::Float:
        type = semantic_.types.builtins().untyped_float;
        break;
      case ConstantKind::String:
        type = semantic_.types.builtins().string_type;
        break;
      default:
        break;
      }
    }
    semantic_.symbols.symbol_mut(id).type = type;
    // The final obligation pass receives this same table. Appending here makes
    // the lexical constant value available to synthesis/docs/judgment context
    // as well as ordinary expression substitution.
    constants_.bindings.push_back({id, evaluated->value});
    return hir_.add_statement(std::move(statement));
  }

  // Checks a lexical procedure declaration. The procedure is declared before
  // its body is visited so direct recursion works; later declarations retain
  // ordinary source-order visibility, exactly like other local bindings.
  [[nodiscard]] HirStatementId check_nested_procedure_declaration(
      const SyntaxTree &tree,
      NodeId declaration_id,
      NodeId procedure_id,
      ScopeId scope,
      const std::vector<SourceName> &names) {
    const SyntaxNode &declaration = tree.node(declaration_id);
    HirStatement statement;
    statement.kind = HirStatementKind::NestedProcedure;
    statement.range = declaration.range;
    statement.syntax = {tree.file(), declaration_id};

    if (names.size() != 1 || names.front().text == "_") {
      diagnostics_.error(
          declaration.range,
          "nested procedure declaration requires one non-discard binding name");
      return hir_.add_statement(std::move(statement));
    }

    const SourceName &name = names.front();
    Symbol symbol;
    symbol.name = name.text;
    symbol.linkage_name = nested_linkage_name(tree, name.text, name.range);
    symbol.kind = SymbolKind::Procedure;
    symbol.visibility = Visibility::Private;
    symbol.scope = scope;
    symbol.syntax = {tree.file(), declaration_id};
    symbol.name_range = name.range;
    for (NodeId child : declaration.children) {
      if (tree.node(child).kind == NodeKind::ParametricParameterList) {
        symbol.flags.parametric = true;
      }
    }
    const bool parametric = symbol.flags.parametric;
    const SymbolId id =
        semantic_.symbols.declare(std::move(symbol), diagnostics_);
    if (!id.is_valid()) return hir_.add_statement(std::move(statement));
    statement.bindings.push_back(id);

    // A procedure declared inside a statement-level denial is governed by that
    // denial even though its body is represented by a separate HIR procedure.
    // Copying syntax references preserves the same later selector-resolution
    // path used by package declaration denials.
    for (SyntaxReference denial : active_statement_denials_) {
      semantic_.declaration_denials.push_back({id, denial});
    }

    TypeId signature = resolve_local_procedure_signature(
        sources_,
        loaded_,
        semantic_,
        selections_,
        tree,
        declaration_id,
        procedure_id,
        scope,
        id,
        diagnostics_);
    signature = substitute_active(signature, declaration.range);
    semantic_.symbols.symbol_mut(id).type = signature;

    // Signature resolution creates symbols before the enclosing concrete
    // specialization is known to TypeResolver. Apply that specialization to
    // the runtime-bearing parameter rows and compile-time value parameter
    // types now. Type-parameter identities themselves must remain symbolic.
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner != id) continue;
      const Scope scope_record = semantic_.symbols.scope(owned.scope);
      for (SymbolId child : scope_record.symbols) {
        Symbol &owned_symbol = semantic_.symbols.symbol_mut(child);
        if (owned_symbol.kind == SymbolKind::Parameter ||
            owned_symbol.kind == SymbolKind::ValueParameter) {
          owned_symbol.type = substitute_active(
              owned_symbol.type, owned_symbol.name_range);
        }
      }
    }

    // A body nested in a symbolic outer template is itself non-executable even
    // when it has no parameters of its own. The concrete outer body is checked
    // again and creates the executable lexical procedure for that instance.
    (void)check_procedure(
        id, current_procedure_is_template_ || parametric);
    return hir_.add_statement(std::move(statement));
  }

  // Checks one local declaration and appends bindings to the current block scope.
  [[nodiscard]] HirStatementId check_local_declaration(
      const SyntaxTree &tree, NodeId statement_id, ScopeId scope) {
    const SyntaxNode &statement_node = tree.node(statement_id);
    if (statement_node.kind != NodeKind::Declaration &&
        statement_node.children.empty()) {
      HirStatement invalid;
      invalid.kind = HirStatementKind::Invalid;
      invalid.range = statement_node.range;
      return hir_.add_statement(std::move(invalid));
    }
    const NodeId declaration_id = statement_node.kind == NodeKind::Declaration
        ? statement_id
        : statement_node.children.front();
    const SyntaxNode &declaration = tree.node(declaration_id);
    if (declaration.children.empty()) {
      HirStatement invalid;
      invalid.kind = HirStatementKind::Invalid;
      invalid.range = declaration.range;
      return hir_.add_statement(std::move(invalid));
    }
    const SyntaxNode &pattern = tree.node(declaration.children.front());
    const bool destructures_tuple = pattern.kind == NodeKind::TuplePattern;
    std::vector<SourceName> names;
    for (NodeId child : pattern.children) {
      const SyntaxNode &name_list = tree.node(child);
      if (name_list.kind == NodeKind::NameList) {
        names = names_in_span(tree, name_list.token_begin, name_list.token_end);
      }
    }

    for (std::size_t index = 1; index < declaration.children.size(); ++index) {
      const NodeId child = declaration.children[index];
      if (tree.node(child).kind == NodeKind::Procedure) {
        return check_nested_procedure_declaration(
            tree, declaration_id, child, scope, names);
      }
    }

    const NodeId payload = declaration.children.back();
    bool compile_time_declaration = false;
    for (std::uint32_t index = declaration.token_begin;
         index < tree.node(payload).token_begin;
         ++index) {
      if (tree.token(index).kind == TokenKind::ColonColon) {
        compile_time_declaration = true;
        break;
      }
    }
    if (compile_time_declaration &&
        expression_denotes_type(tree, payload, scope)) {
      return check_local_type_declaration(
          tree, declaration_id, payload, scope, names);
    }
    if (compile_time_declaration) {
      return check_compile_time_declaration(
          tree, declaration_id, payload, scope, names);
    }

    TypeId declared_type;
    std::optional<NodeId> initializer;
    for (std::size_t index = 1; index < declaration.children.size(); ++index) {
      const NodeId child = declaration.children[index];
      if (node_is_type_syntax(tree.node(child).kind)) {
        declared_type = resolve_type_syntax(
            sources_, loaded_, semantic_, selections_, tree, child, scope, diagnostics_);
        declared_type = substitute_active(
            declared_type, tree.node(child).range);
      } else if (tree.node(child).kind != NodeKind::ParametricParameterList) {
        initializer = child;
      }
    }

    HirStatement statement;
    statement.kind = HirStatementKind::LocalDeclaration;
    statement.range = declaration.range;
    statement.local_is_uninitialized = initializer.has_value() &&
        tree.node(*initializer).kind == NodeKind::UninitializedExpression;
    if (initializer.has_value() &&
        tree.node(*initializer).kind != NodeKind::UninitializedExpression) {
      const HirExpressionId value = check_expression(
          tree, *initializer, scope, declared_type);
      statement.expressions.push_back(value);
      if (!declared_type.is_valid()) {
        declared_type = default_inferred_runtime_type(hir_.expression(value).type);
        contextualize_inferred_runtime_expression(value, declared_type);
      }
    }
    if (!declared_type.is_valid()) {
      diagnostics_.error(declaration.range, "local declaration requires a type or initializer");
      declared_type = semantic_.types.builtins().invalid;
    }

    std::vector<TypeId> binding_types(names.size(), declared_type);
    if (destructures_tuple) {
      statement.local_destructures_tuple = true;
      if (is_invalid_type(declared_type) ||
          semantic_.types.type(declared_type).kind != TypeKind::Tuple) {
        diagnostics_.error(pattern.range, "tuple pattern requires a tuple value");
      } else {
        const std::vector<TypeId> &members =
            semantic_.types.type(declared_type).members;
        if (members.size() != names.size()) {
          diagnostics_.error(pattern.range, "tuple pattern has the wrong arity");
        }
        const std::size_t count = std::min(members.size(), binding_types.size());
        for (std::size_t index = 0; index < count; ++index) {
          binding_types[index] = members[index];
        }
      }
    }

    for (std::size_t index = 0; index < names.size(); ++index) {
      const SourceName &name = names[index];
      if (name.text == "_") continue;
      Symbol symbol;
      symbol.name = name.text;
      symbol.kind = SymbolKind::Local;
      symbol.scope = scope;
      symbol.type = binding_types[index];
      symbol.syntax = {tree.file(), declaration_id};
      symbol.name_range = name.range;
      const SymbolId id = semantic_.symbols.declare(std::move(symbol), diagnostics_);
      if (id.is_valid()) {
        statement.bindings.push_back(id);
        if (destructures_tuple) statement.binding_member_indices.push_back(index);
      }
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

  // Checks the closed structured-statement vocabulary into typed HIR. Helpers
  // above keep declaration and assignment pattern mechanics out of this main
  // control-flow dispatch.
  [[nodiscard]] HirStatementId check_statement(
      const SyntaxTree &tree,
      NodeId statement_id,
      ScopeId scope,
      TypeId result_type,
      ControlDepth depth) {
    const SyntaxNode &node = tree.node(statement_id);
    HirStatement statement;
    statement.range = node.range;
    statement.syntax = {tree.file(), statement_id};
    switch (node.kind) {
    case NodeKind::DeclarationStatement:
      return check_local_declaration(tree, statement_id, scope);

    case NodeKind::ExpressionStatement:
      statement.kind = HirStatementKind::Expression;
      for (NodeId child : node.children) {
        statement.expressions.push_back(check_expression(tree, child, scope));
      }
      break;

    case NodeKind::AssignmentStatement:
      return check_assignment_statement(tree, statement_id, scope);

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
        statement.for_kind = HirForKind::Iteration;
        const NodeId header_id = node.children.front();
        const SyntaxNode &header = tree.node(header_id);
        if (header.children.empty()) break;
        const ScopeId loop_scope = semantic_.symbols.add_scope(
            ScopeKind::Block, scope, header.range);
        const HirExpressionId iterable =
            check_expression(tree, header.children.front(), scope);
        statement.expressions.push_back(iterable);
        const Type iterable_type = runtime_scalar_type(
            hir_.expression(iterable).type);
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
        statement.for_kind = HirForKind::Clause;
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
            ++statement.for_initialization_count;
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
            statement.for_kind = HirForKind::Conditional;
            statement.expressions.push_back(check_expression(
                tree,
                tree.node(child).children.front(),
                scope,
                semantic_.types.builtins().bool_type));
          } else {
            statement.for_kind = HirForKind::Conditional;
            statement.expressions.push_back(check_expression(
                tree,
                child,
                scope,
                semantic_.types.builtins().bool_type));
          }
        }
        if (statement.expressions.empty()) {
          statement.for_kind = HirForKind::Infinite;
        }
      }
      break;

    case NodeKind::DenyStatement:
      statement.kind = HirStatementKind::Denial;
      // A statement denial's selectors resolve outside its governed block.
      // The block receives a fresh scope and may legally shadow those names.
      semantic_.sites.push_back(
          {SemanticSiteKind::DenialStatement,
           {tree.file(), statement_id},
           scope,
           current_procedure_,
           {}});
      if (!node.children.empty()) {
        active_statement_denials_.push_back({tree.file(), statement_id});
        statement.blocks.push_back(
            check_block(tree, node.children.back(), scope, result_type, depth));
        active_statement_denials_.pop_back();
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
          {SemanticSiteKind::Judgment,
           {tree.file(), statement_id},
           scope,
           current_procedure_,
           {}});
      break;

    case NodeKind::SynthesisStatement:
      statement.kind = HirStatementKind::Synthesis;
      semantic_.sites.push_back(
          {SemanticSiteKind::SynthesisStatement,
           {tree.file(), statement_id},
           scope,
           current_procedure_,
           {}});
      break;

    case NodeKind::AsmStatement:
      statement.kind = HirStatementKind::Assembly;
      for (NodeId child : node.children) {
        if (tree.node(child).kind == NodeKind::AsmInput &&
            !tree.node(child).children.empty()) {
          statement.expressions.push_back(check_expression(
              tree, tree.node(child).children.front(), scope));
        } else if (tree.node(child).kind == NodeKind::SynthesisAssembly) {
          semantic_.sites.push_back(
              {SemanticSiteKind::SynthesisAssembly,
               {tree.file(), child},
               scope,
               current_procedure_,
               {}});
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
        const TypeKind subject_kind = is_invalid_type(subject_type)
            ? TypeKind::Invalid
            : runtime_scalar_type(subject_type).kind;
        if (subject_kind != TypeKind::Invalid &&
            subject_kind != TypeKind::TaggedUnion &&
            !switch_subject_type(subject_type)) {
          diagnostics_.error(
              tree.node(node.children.front()).range,
              "switch subject type does not have built-in scalar equality");
        }
        bool has_default = false;
        std::vector<SymbolId> covered_alternatives;
        std::vector<ConstantValue> covered_values;
        for (std::size_t case_index = 1; case_index < node.children.size(); ++case_index) {
          const SyntaxNode &case_node = tree.node(node.children[case_index]);
          if (case_node.kind != NodeKind::SwitchCase || case_node.children.empty()) continue;
          const NodeId list_id = case_node.children.back();
          const ScopeId case_scope = semantic_.symbols.add_scope(
              ScopeKind::Block, scope, case_node.range);
          HirSwitchCase hir_case;
          hir_case.first_label = statement.expressions.size();
          hir_case.is_default = case_node.children.size() == 1;
          if (hir_case.is_default) {
            if (has_default) {
              diagnostics_.error(case_node.range, "duplicate default switch case");
            }
            has_default = true;
          }
          for (std::size_t label_index = 0;
               label_index + 1 < case_node.children.size();
               ++label_index) {
            const HirExpressionId label =
                subject_kind == TypeKind::TaggedUnion
                ? check_union_case_label(
                      tree,
                      case_node.children[label_index],
                      case_scope,
                      subject_type,
                      hir_case,
                      case_node.children.size() > 2)
                : check_value_case_label(
                      tree, case_node.children[label_index], scope, subject_type);
            statement.expressions.push_back(label);
            ++hir_case.label_count;
            const HirExpression &label_expression = hir_.expression(label);
            std::optional<SymbolId> alternative;
            if ((subject_kind == TypeKind::Enum ||
                 subject_kind == TypeKind::TaggedUnion) &&
                label_expression.symbol.is_valid()) {
              const SymbolKind kind = semantic_.symbols.symbol(
                  label_expression.symbol).kind;
              if (kind == SymbolKind::EnumMember ||
                  kind == SymbolKind::UnionAlternative) {
                alternative = label_expression.symbol;
              }
            }
            if (!alternative.has_value() && subject_kind == TypeKind::Enum &&
                label_expression.kind == HirExpressionKind::Constant) {
              alternative = enum_member_for_value(
                  subject_type, label_expression.constant);
            }
            if (alternative.has_value()) {
              if (std::find(
                      covered_alternatives.begin(),
                      covered_alternatives.end(),
                      *alternative) != covered_alternatives.end()) {
                diagnostics_.error(
                    tree.node(case_node.children[label_index]).range,
                    "duplicate switch alternative");
              } else {
                covered_alternatives.push_back(*alternative);
              }
            } else if (label_expression.kind == HirExpressionKind::Constant) {
              if (switch_case_value_is_nan(
                      subject_type, label_expression.constant)) {
                diagnostics_.error(
                    tree.node(case_node.children[label_index]).range,
                    "NaN switch case value can never match");
              } else if (std::find_if(
                             covered_values.begin(),
                             covered_values.end(),
                             [&](const ConstantValue &covered) {
                               return switch_case_values_equal(
                                   subject_type,
                                   covered,
                                   label_expression.constant);
                             }) != covered_values.end()) {
                diagnostics_.error(
                    tree.node(case_node.children[label_index]).range,
                    "duplicate switch case value");
              } else {
                covered_values.push_back(label_expression.constant);
              }
            }
          }
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
          hir_case.body = hir_.add_block(std::move(case_block));
          statement.blocks.push_back(hir_case.body);
          statement.switch_cases.push_back(hir_case);
        }

        statement.switch_is_exhaustive = has_default;
        if (!has_default &&
            (subject_kind == TypeKind::Enum || subject_kind == TypeKind::TaggedUnion)) {
          const std::optional<SymbolId> owner = type_owner(subject_type);
          if (owner.has_value()) {
            std::size_t alternative_count = 0;
            for (const AggregateMember &member : semantic_.aggregate_members) {
              if (member.owner == *owner) ++alternative_count;
            }
            statement.switch_is_exhaustive =
                covered_alternatives.size() == alternative_count;
            if (!statement.switch_is_exhaustive) {
              diagnostics_.error(
                  node.range,
                  "switch over enum or tagged union is not exhaustive and has no default");
            }
          }
        }
        statement.switch_definitely_returns =
            statement.switch_is_exhaustive && !statement.switch_cases.empty();
        for (const HirSwitchCase &switch_case : statement.switch_cases) {
          statement.switch_definitely_returns =
              statement.switch_definitely_returns &&
              block_definitely_returns(switch_case.body);
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

  [[nodiscard]] bool block_definitely_returns(HirBlockId block_id) const {
    const HirBlock &block = hir_.block(block_id);
    for (HirStatementId statement : block.statements) {
      if (statement_definitely_returns(hir_.statement(statement))) return true;
    }
    return false;
  }

  // Return analysis consumes checked control-flow structure rather than
  // reinterpreting syntax. In particular it can trust the exhaustiveness fact
  // established while resolving enum/tagged-union case labels.
  [[nodiscard]] bool statement_definitely_returns(
      const HirStatement &statement) const {
    if (statement.kind == HirStatementKind::Return) return true;
    if (statement.kind == HirStatementKind::If) {
      return statement.blocks.size() == 2 &&
          block_definitely_returns(statement.blocks[0]) &&
          block_definitely_returns(statement.blocks[1]);
    }
    if (statement.kind == HirStatementKind::Switch) {
      return statement.switch_definitely_returns;
    }
    if (statement.kind == HirStatementKind::Block ||
        statement.kind == HirStatementKind::Denial ||
        statement.kind == HirStatementKind::Unchecked ||
        statement.kind == HirStatementKind::CompileTimeSelection) {
      return statement.blocks.size() == 1 &&
          block_definitely_returns(statement.blocks.front());
    }
    // Loops remain conservative: even a syntactically infinite loop may exit
    // through a break in a nested control-flow path.
    return false;
  }

  // Concrete parametric instances retain their source declaration for denial
  // contracts and source identity. Nested instances use the same table, so one
  // shallow lookup reaches the lexical declaration that owns the syntax.
  [[nodiscard]] SymbolId procedure_declaration_source(
      SymbolId procedure) const {
    for (const ParametricInstanceRecord &instance :
         semantic_.parametric_instances) {
      if (instance.instance == procedure) return instance.source;
    }
    return procedure;
  }

  // Checks one source procedure definition. Signature members contain parameters
  // followed by the result, and the prebuilt Procedure scope contains parameters.
  [[nodiscard]] bool check_procedure(
      SymbolId id, bool parametric_template) {
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

    // Recursive checking of a nested procedure temporarily changes the active
    // procedure and template state. Preserve the enclosing values so checking
    // resumes in the outer body with the correct capture and instantiation
    // rules. Declaration denials are also made lexical here, allowing deeper
    // nested procedures to inherit them when a deferred instance is checked.
    const SymbolId saved_procedure = current_procedure_;
    const bool saved_template = current_procedure_is_template_;
    const std::vector<SyntaxReference> saved_denials =
        active_statement_denials_;
    const SymbolId declaration_source = procedure_declaration_source(id);
    for (const DeclarationDenial &denial : semantic_.declaration_denials) {
      if (denial.declaration != declaration_source) continue;
      if (std::find(
              active_statement_denials_.begin(),
              active_statement_denials_.end(),
              denial.denial) == active_statement_denials_.end()) {
        active_statement_denials_.push_back(denial.denial);
      }
    }
    current_procedure_ = id;
    current_procedure_is_template_ = parametric_template;
    const HirBlockId checked_body = check_block(
        *tree, *body, *parameter_scope, result_type, {});
    if (result_type != semantic_.types.builtins().void_type &&
        !block_definitely_returns(checked_body)) {
      diagnostics_.error(procedure.range, "not every path returns a value");
    }
    hir_.add_procedure(
        {id,
         procedure_symbol.type,
         checked_body,
         diagnostics_.error_count() == initial_errors,
         parametric_template});
    current_procedure_ = saved_procedure;
    current_procedure_is_template_ = saved_template;
    active_statement_denials_ = saved_denials;
    return true;
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const ConditionalSelections &selections_;
  SemanticPackage &semantic_;
  ConstantTable &constants_;
  const TargetFacts &target_;
  DiagnosticSink &diagnostics_;
  const std::vector<ProcedureInstantiationSeed> &seeds_;
  HirProgram hir_;
  SymbolId current_procedure_;
  bool current_procedure_is_template_ = false;
  std::vector<SyntaxReference> active_statement_denials_;
  std::vector<ProcedureInstance> instances_;
  std::optional<std::size_t> current_instance_index_;
};

} // namespace

BodyCheckResult check_package_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    SemanticPackage &package,
    ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &seeds) {
  BodyChecker checker(
      sources, loaded, selections, package, constants, target, diagnostics, seeds);
  BodyCheckResult result = checker.run();
  if (result.ok && !validate_target_types(package.types, target, diagnostics)) {
    result.ok = false;
  }
  if (result.ok &&
      !check_definite_initialization(package, result.program, diagnostics)) {
    result.ok = false;
  }
  return result;
}

} // namespace draft
