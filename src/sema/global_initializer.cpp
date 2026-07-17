// Package storage has no hidden runtime initialization phase. Every explicit
// initializer is therefore evaluated here and handed to the object emitter as
// a closed value. This pass is deliberately separate from procedure-body HIR:
// a global initializer cannot acquire runtime dependencies merely because the
// same expression form is legal inside a procedure.

#include "sema/global_initializer.h"

#include "syntax/syntax_tree.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool type_syntax(NodeKind kind) {
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

class GlobalInitializerChecker {
public:
  GlobalInitializerChecker(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      SemanticPackage &package,
      const TargetFacts &target,
      const ConstantTable &constants,
      ConstantTable &global_initializers,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), package_(package), target_(target),
        constants_(constants), global_initializers_(global_initializers),
        diagnostics_(diagnostics) {}

  [[nodiscard]] bool run() {
    const std::size_t initial_errors = diagnostics_.error_count();
    std::vector<ConstantBinding> initializers;
    const Scope &scope = package_.symbols.scope(package_.package_scope);
    for (SymbolId id : scope.symbols) {
      Symbol &symbol = package_.symbols.symbol_mut(id);
      if (symbol.kind != SymbolKind::Variable || symbol.flags.foreign) continue;
      check_variable(id, symbol, initializers);
    }
    global_initializers_.bindings = std::move(initializers);
    std::sort(
        global_initializers_.bindings.begin(),
        global_initializers_.bindings.end(),
        [](const ConstantBinding &left, const ConstantBinding &right) {
          return left.symbol.value < right.symbol.value;
        });
    return diagnostics_.error_count() == initial_errors;
  }

private:
  [[nodiscard]] const SyntaxTree *tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) return &*entry.syntax;
    }
    return nullptr;
  }

  [[nodiscard]] ScopeId file_scope(FileId file) const {
    for (const FileSemanticScope &entry : package_.files) {
      if (entry.file == file) return entry.scope;
    }
    return package_.package_scope;
  }

  [[nodiscard]] std::optional<NodeId> initializer(
      const SyntaxTree &syntax, const SyntaxNode &declaration) const {
    for (std::size_t index = 1; index < declaration.children.size(); ++index) {
      const NodeId child = declaration.children[index];
      const NodeKind kind = syntax.node(child).kind;
      if (!type_syntax(kind) && kind != NodeKind::ParametricParameterList) {
        return child;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] Type runtime_type(TypeId id) const {
    Type result = package_.types.type(id);
    while (result.kind == TypeKind::Distinct) {
      result = package_.types.type(result.element);
    }
    return result;
  }

  [[nodiscard]] bool integer_representable(
      const BigInteger &value, TypeId target) const {
    Type type = runtime_type(target);
    if (type.kind == TypeKind::Rune) type.kind = TypeKind::SignedInteger;
    if (type.kind != TypeKind::SignedInteger &&
        type.kind != TypeKind::UnsignedInteger) {
      return true;
    }
    if (type.bit_width == 0) return false;
    if (type.kind == TypeKind::UnsignedInteger) {
      return !value.is_negative() && value.bit_count() <= type.bit_width;
    }
    const BigInteger magnitude = BigInteger::from_u64(1).shifted_left(
        static_cast<std::size_t>(type.bit_width - 1U));
    return value.compare(magnitude.negated()) >= 0 &&
        value.compare(magnitude.subtracted(BigInteger::from_u64(1))) <= 0;
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

  [[nodiscard]] std::optional<BigInteger> enum_value(
      TypeId type, std::string_view name) const {
    for (const AggregateMember &member : package_.aggregate_members) {
      if (package_.symbols.symbol(member.owner).type != type) continue;
      if (package_.symbols.symbol(member.member).name != name) continue;
      for (const EnumMemberValue &value : package_.enum_member_values) {
        if (value.member == member.member) return value.value;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] TypeId inferred_type(
      const SyntaxTree &syntax,
      NodeId expression,
      const ConstantValue &value) const {
    switch (value.kind) {
    case ConstantKind::Bool:
      return package_.types.builtins().bool_type;
    case ConstantKind::Integer:
      if (syntax.node(expression).kind == NodeKind::LiteralExpression &&
          syntax.node(expression).token_begin < syntax.node(expression).token_end &&
          syntax.token(syntax.node(expression).token_begin).kind ==
              TokenKind::RuneLiteral) {
        return package_.types.builtins().rune_type;
      }
      return package_.types.builtins().int_type;
    case ConstantKind::Float: {
      const std::optional<TypeId> f64 = package_.types.find_builtin("f64");
      return f64.value_or(package_.types.builtins().invalid);
    }
    case ConstantKind::String:
      return package_.types.builtins().string_type;
    case ConstantKind::Nil:
    case ConstantKind::EnumLabel:
    case ConstantKind::Target:
    case ConstantKind::Unavailable:
      return package_.types.builtins().invalid;
    }
    return package_.types.builtins().invalid;
  }

  [[nodiscard]] std::optional<ConstantValue> convert(
      ConstantValue value, TypeId target, SourceRange range) {
    const Type type = runtime_type(target);
    if (value.kind == ConstantKind::Nil) {
      if (type.kind == TypeKind::Pointer || type.kind == TypeKind::MultiPointer ||
          type.kind == TypeKind::RawPointer || type.kind == TypeKind::CString ||
          type.kind == TypeKind::Procedure) {
        return value;
      }
    } else if (value.kind == ConstantKind::Bool && type.kind == TypeKind::Bool) {
      return value;
    } else if (value.kind == ConstantKind::String &&
               type.kind == TypeKind::String) {
      return value;
    } else if (value.kind == ConstantKind::Integer &&
               (type.kind == TypeKind::SignedInteger ||
                type.kind == TypeKind::UnsignedInteger ||
                type.kind == TypeKind::Rune)) {
      if (!integer_representable(value.integer, target)) {
        diagnostics_.error(range, "global integer initializer is not representable");
        return std::nullopt;
      }
      if (type.kind == TypeKind::Rune && !valid_rune(value.integer)) {
        diagnostics_.error(range, "global rune initializer is not a Unicode scalar");
        return std::nullopt;
      }
      return value;
    } else if (value.kind == ConstantKind::Integer &&
               type.kind == TypeKind::Float) {
      return ConstantValue::make_float(ExactRational(value.integer));
    } else if (value.kind == ConstantKind::Float &&
               type.kind == TypeKind::Float) {
      return value;
    } else if (value.kind == ConstantKind::EnumLabel &&
               type.kind == TypeKind::Enum) {
      const std::optional<BigInteger> integer = enum_value(target, value.text);
      if (integer.has_value()) return ConstantValue::make_integer(*integer);
      diagnostics_.error(range, "global enum initializer names no member");
      return std::nullopt;
    }
    diagnostics_.error(
        range,
        "compile-time initializer is incompatible with global type '" +
            package_.types.type(target).name + "'");
    return std::nullopt;
  }

  void check_variable(
      SymbolId id,
      Symbol &symbol,
      std::vector<ConstantBinding> &initializers) {
    const SyntaxTree *syntax = tree(symbol.syntax.file);
    if (syntax == nullptr || !symbol.syntax.node.is_valid()) return;
    const SyntaxNode &declaration = syntax->node(symbol.syntax.node);
    const std::optional<NodeId> value_node = initializer(*syntax, declaration);
    if (!value_node.has_value()) {
      if (!symbol.type.is_valid() ||
          package_.types.type(symbol.type).kind == TypeKind::Invalid) {
        diagnostics_.error(
            symbol.name_range,
            "global variable requires a type or compile-time initializer");
      }
      return;
    }
    if (syntax->node(*value_node).kind == NodeKind::UninitializedExpression) {
      diagnostics_.error(
          syntax->node(*value_node).range,
          "'---' is valid only on an automatic local declaration");
      return;
    }
    const std::optional<ConstantValue> evaluated = evaluate_constant_expression(
        sources_,
        loaded_,
        package_,
        target_,
        *syntax,
        *value_node,
        file_scope(syntax->file()),
        diagnostics_,
        &constants_);
    if (!evaluated.has_value()) return;
    if (!symbol.type.is_valid() ||
        package_.types.type(symbol.type).kind == TypeKind::Invalid) {
      symbol.type = inferred_type(*syntax, *value_node, *evaluated);
      if (!symbol.type.is_valid() ||
          package_.types.type(symbol.type).kind == TypeKind::Invalid) {
        diagnostics_.error(
            syntax->node(*value_node).range,
            "global initializer requires an explicit type");
        return;
      }
    }
    const std::optional<ConstantValue> converted = convert(
        *evaluated, symbol.type, syntax->node(*value_node).range);
    if (converted.has_value()) initializers.push_back({id, *converted});
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  SemanticPackage &package_;
  const TargetFacts &target_;
  const ConstantTable &constants_;
  ConstantTable &global_initializers_;
  DiagnosticSink &diagnostics_;
};

} // namespace

bool check_global_initializers(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const ConstantTable &constants,
    ConstantTable &initializers,
    DiagnosticSink &diagnostics) {
  return GlobalInitializerChecker(
      sources,
      loaded,
      package,
      target,
      constants,
      initializers,
      diagnostics).run();
}

} // namespace draft
