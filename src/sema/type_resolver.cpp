// Source type and signature resolution for the bootstrap semantic graph.

#include "sema/type_resolver.h"

#include "syntax/token.h"

#include <algorithm>
#include <cassert>
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

// One entry per package declaration prevents alias recursion from repeatedly
// walking the same syntax. Member and parameter symbols are appended later and
// never enter this state table because their types resolve at declaration time.
enum class ResolutionState {
  Unvisited,
  Resolving,
  Resolved,
  Failed,
};

// SourceName owns a copied spelling and retains the exact token range used for
// duplicate and unknown-name diagnostics.
struct SourceName {
  std::string text;
  SourceRange range;
};

// MemberData is the parallel-array work record for one aggregate. All vectors
// contain only successfully declared members in the same order. `incomplete`
// means a `when` or synthesis site still withholds part of the member list, so
// no physical layout may be claimed even if the visible members are layable.
struct MemberData {
  std::vector<SymbolId> symbols;
  std::vector<TypeId> types;
  std::vector<std::uint64_t> offsets;
  std::vector<BigInteger> enum_values;
  TypeId enum_value_expected_type;
  bool incomplete = false;
};

struct ResolverTypeSubstitution {
  TypeId parameter;
  TypeId replacement;
};

struct ResolverValueSubstitution {
  std::uint32_t parameter = std::numeric_limits<std::uint32_t>::max();
  BigInteger replacement;
  // A template-to-template substitution retains an expression over the
  // destination's local value parameters instead of manufacturing an integer.
  IntegerExpression symbolic_expression;
};

// Parsed representation attributes are kept small and explicit. Zero means no
// requested alignment; C representation is independent so `@repr(C)` and
// `@align(N)` compose without an attribute object hierarchy.
struct AggregateAttributes {
  bool c_representation = false;
  std::uint32_t requested_alignment = 0;
};

struct BuiltIntegerExpressionNode {
  bool valid = false;
  std::uint32_t node = std::numeric_limits<std::uint32_t>::max();
  TypeId type;
  std::optional<BigInteger> constant;
};

// Mirrors the parser's contextual-name rule so semantic token-span extraction
// accepts `c`, `type`, and constraint spellings where the grammar accepts them.
[[nodiscard]] bool token_is_contextual_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
         kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
         kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
         kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory;
}

// Identifies syntax nodes that are unambiguously types before name resolution.
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

// Rounds a byte count to a power-of-two alignment without unsigned overflow.
[[nodiscard]] std::optional<std::uint64_t> round_up(
    std::uint64_t value, std::uint32_t alignment) {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1);
  if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
    return std::nullopt;
  }
  return (value + mask) & ~mask;
}

// TypeResolver owns no source, syntax, or semantic tables. It is one mutable
// phase context whose references remain valid for the duration of the call;
// stable IDs, never table element pointers, cross operations that append rows.
class TypeResolver {
public:
  TypeResolver(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      SemanticPackage &semantic,
      const ConditionalSelections &selections,
      DiagnosticSink &diagnostics,
      const ConstantTable *active_constants = nullptr,
      const std::vector<ConstantTypeBinding> *active_types = nullptr,
      const std::vector<ResolvedIntegerExpression> *resolved_integers = nullptr,
      const TargetFacts *target = nullptr,
      const std::vector<SyntaxReference> *blocked_integer_synthesis = nullptr)
      : sources_(sources), loaded_(loaded), semantic_(semantic), selections_(selections),
        diagnostics_(diagnostics),
        states_(semantic.symbols.symbol_count(), ResolutionState::Unvisited),
        active_constants_(active_constants), active_types_(active_types),
        resolved_integers_(resolved_integers), target_(target),
        blocked_integer_synthesis_(blocked_integer_synthesis) {}

  // Resolves only the symbols originally installed in the package scope.
  // Nested symbols are resolved synchronously as their owner is processed.
  void resolve() {
    // Copy the package symbol list because resolving aggregates and procedures
    // appends member/parameter symbols to other scopes in the same table.
    const std::vector<SymbolId> package_symbols =
        semantic_.symbols.scope(semantic_.package_scope).symbols;
    for (SymbolId symbol : package_symbols) {
      resolve_symbol(symbol);
    }
  }

  // Public single-node entry used by body checking after package declarations
  // have already been resolved by resolve().
  [[nodiscard]] TypeId resolve_one_type(
      const SyntaxTree &tree, NodeId type, ScopeId scope) {
    return resolve_type(tree, type, scope);
  }

  [[nodiscard]] std::optional<IntegerExpression>
  resolve_one_dependent_integer_expression(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId contextual_type) {
    return dependent_integer_expression(
        tree, expression, scope, contextual_type);
  }

  [[nodiscard]] TypeId instantiate_one_type(
      SymbolId source,
      std::vector<ParametricArgument> arguments,
      SourceRange use_range) {
    return instantiate_parametric_type(
        source, std::move(arguments), use_range);
  }

  // Body checking owns procedure-template substitutions, while this resolver
  // owns the single implementation of deferred layout evaluation. Convert the
  // shared package-local binding records back into the resolver's compact
  // substitution form and run the ordinary recursive type path.
  [[nodiscard]] TypeId instantiate_one_owner_evaluated_type(
      TypeId source,
      const std::vector<DeferredElementCountTypeBinding> &type_bindings,
      const std::vector<DeferredElementCountValueBinding> &value_bindings,
      SourceRange use_range) {
    std::vector<ResolverTypeSubstitution> types;
    for (const DeferredElementCountTypeBinding &binding : type_bindings) {
      types.push_back({binding.parameter, binding.replacement});
    }
    std::vector<ResolverValueSubstitution> values;
    for (const DeferredElementCountValueBinding &binding : value_bindings) {
      ResolverValueSubstitution value;
      value.parameter = binding.parameter.value;
      value.symbolic_expression = binding.symbolic_expression;
      if (!value.symbolic_expression.is_valid()) {
        if (binding.value.kind != ConstantKind::Integer) {
          diagnostics_.error(
              use_range,
              "owner-evaluated layout parameter must be an integer");
          return semantic_.types.builtins().invalid;
        }
        value.replacement = binding.value.integer;
      }
      values.push_back(std::move(value));
    }
    return substitute_type(source, types, values, use_range);
  }

  // Local procedures arrive after the package-wide resolver has finished, but
  // their signatures obey exactly the same rules as package procedures. Keep
  // the operation here so parameter and parametric scope construction has one
  // implementation and one ordering contract.
  [[nodiscard]] TypeId resolve_one_procedure(
      const SyntaxTree &tree,
      NodeId declaration_id,
      NodeId procedure_id,
      ScopeId parent,
      SymbolId owner) {
    const SyntaxNode &declaration = tree.node(declaration_id);
    const ScopeId semantic_parent = ensure_parametric_scope(
        owner, tree, declaration, parent);
    const TypeId type = resolve_procedure_type(
        tree, procedure_id, semantic_parent, owner);
    semantic_.symbols.symbol_mut(owner).type = type;
    return type;
  }

  // Body-local named types are resolved synchronously because their visibility
  // is source ordered. This mirrors resolve_symbol's package-type branch while
  // retaining the lexical parent chosen by body checking and the enclosing
  // procedure instance's concrete generic bindings.
  [[nodiscard]] TypeId resolve_one_local_type(
      const SyntaxTree &tree,
      NodeId declaration_id,
      NodeId type_id,
      ScopeId parent,
      SymbolId owner) {
    const SyntaxNode &declaration = tree.node(declaration_id);
    const ScopeId semantic_parent = ensure_parametric_scope(
        owner, tree, declaration, parent);
    const SyntaxNode &type_node = tree.node(type_id);
    const Symbol initial = semantic_.symbols.symbol(owner);
    TypeId result = semantic_.types.builtins().invalid;

    if (type_node.kind == NodeKind::StructType ||
        type_node.kind == NodeKind::EnumType ||
        type_node.kind == NodeKind::TaggedUnionType ||
        type_node.kind == NodeKind::RawUnionType) {
      if (initial.type.is_valid()) {
        resolve_aggregate(owner, initial.type, tree, type_id, semantic_parent);
        result = initial.type;
      }
    } else if (type_node.kind == NodeKind::DistinctType &&
               !type_node.children.empty()) {
      const TypeId underlying =
          resolve_type(tree, type_node.children.back(), semantic_parent);
      result = semantic_.types.distinct(
          initial.name, underlying, initial.name_range);
    } else if (type_node.kind == NodeKind::NameExpression ||
               type_node.kind == NodeKind::MemberExpression ||
               type_node.kind == NodeKind::BracketExpression ||
               type_node.kind == NodeKind::GroupExpression ||
               type_node.kind == NodeKind::TupleExpression) {
      result = try_type_value(tree, type_id, semantic_parent).value_or(
          semantic_.types.builtins().invalid);
      if (result == semantic_.types.builtins().invalid) {
        diagnostics_.error(type_node.range, "expression does not denote a type");
      }
    } else {
      result = resolve_type(tree, type_id, semantic_parent);
    }
    semantic_.symbols.symbol_mut(owner).type = result;
    return result;
  }

private:
  // Forward aliases and ambiguous name-valued constants form a declaration
  // graph, not parser nesting. Resolve them recursively so cycle handling and
  // source-order independence stay simple, but cap an acyclic chain before it
  // can consume an input-sized host stack. Structural type syntax has the
  // parser's separate shared nesting budget.
  static constexpr std::size_t kMaximumDeclarationResolutionDepth = 256;

  // Finds the immutable parsed tree owning a SyntaxReference. LoadedPackage
  // keeps files in canonical order, making this linear scan deterministic.
  [[nodiscard]] const SyntaxTree *find_tree(FileId file) const {
    for (const LoadedPackageFile &loaded_file : loaded_.files) {
      if (loaded_file.source == file && loaded_file.syntax.has_value()) {
        return &*loaded_file.syntax;
      }
    }
    return nullptr;
  }

  // Returns the file-local import scope for a source file. The package fallback
  // is recovery for malformed manually assembled inputs, not normal behavior.
  [[nodiscard]] ScopeId file_scope(FileId file) const {
    for (const FileSemanticScope &entry : semantic_.files) {
      if (entry.file == file) {
        return entry.scope;
      }
    }
    return semantic_.package_scope;
  }

  // Copies one name token because SourceManager views are non-owning and semantic
  // records can outlive temporary traversal state.
  [[nodiscard]] SourceName token_name(
      const SyntaxTree &tree, std::uint32_t token_index) const {
    const Token &token = tree.token(token_index);
    return {std::string(sources_.text(token.range)), token.range};
  }

  // Collects contextual-name tokens in source order from a grammar-owned span.
  // Callers choose spans that exclude nested type/value syntax where needed.
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

  // Returns the declaration's semantic payload child after its binding pattern
  // and optional parametric parameter list. Missing recovered syntax is empty.
  [[nodiscard]] std::optional<NodeId> declaration_payload(
      const SyntaxTree &tree, const SyntaxNode &declaration) const {
    if (declaration.children.size() < 2) {
      return std::nullopt;
    }
    const NodeId candidate = declaration.children.back();
    const NodeKind kind = tree.node(candidate).kind;
    if (kind == NodeKind::BindingPattern || kind == NodeKind::TuplePattern ||
        kind == NodeKind::ParametricParameterList) {
      return std::nullopt;
    }
    return candidate;
  }

  // Semantic invalid is a real canonical table row, while an invalid TypeId is
  // the "not assigned yet" sentinel. Both represent failure at this boundary.
  [[nodiscard]] bool is_error_type(TypeId type) const {
    return !type.is_valid() || semantic_.types.type(type).kind == TypeKind::Invalid;
  }

  // Only unique TypeParameter rows are replaced here. Structural types are
  // constructed recursively by resolve_type, so applying this at every named
  // leaf produces a fully concrete local aggregate without cloning an already
  // completed nominal type.
  [[nodiscard]] TypeId active_type(TypeId type) const {
    if (active_types_ == nullptr) return type;
    for (const ConstantTypeBinding &binding : *active_types_) {
      if (binding.parameter == type) return binding.replacement;
    }
    return type;
  }

  // Appends provider-independent metadata without changing runtime layout or
  // control flow. owner is invalid only for an anonymous aggregate.
  void add_site(
      SemanticSiteKind kind,
      const SyntaxTree &tree,
      NodeId node,
      ScopeId scope,
      SymbolId owner) {
    semantic_.sites.push_back(
        {kind, {tree.file(), node}, scope, owner, {}, {}, {}});
  }

  // Finds a previously created owner scope of the requested semantic kind.
  // Reuse is required when aliases recursively request the same declaration.
  [[nodiscard]] std::optional<ScopeId> owned_scope(
      SymbolId owner, ScopeKind kind) const {
    for (const OwnedSemanticScope &entry : semantic_.owned_scopes) {
      if (entry.owner == owner && semantic_.symbols.scope(entry.scope).kind == kind) {
        return entry.scope;
      }
    }
    return std::nullopt;
  }

  // Maps the closed source constraint vocabulary to its semantic tag. `type`
  // intentionally uses the default AnyType value.
  [[nodiscard]] TypeConstraintKind constraint_kind(std::string_view name) const {
    if (name == "integer") return TypeConstraintKind::Integer;
    if (name == "float") return TypeConstraintKind::Float;
    if (name == "number") return TypeConstraintKind::Number;
    return TypeConstraintKind::AnyType;
  }

  // Creates and populates the declaration's compile-time parameter scope before
  // resolving its body/signature. Earlier parameters are visible to later value
  // parameter types. Duplicate parameter names do not allocate orphan types.
  [[nodiscard]] ScopeId ensure_parametric_scope(
      SymbolId owner,
      const SyntaxTree &tree,
      const SyntaxNode &declaration,
      ScopeId parent) {
    const std::optional<ScopeId> existing = owned_scope(owner, ScopeKind::Parametric);
    if (existing.has_value()) {
      return *existing;
    }

    std::optional<NodeId> parameters;
    for (NodeId child : declaration.children) {
      if (tree.node(child).kind == NodeKind::ParametricParameterList) {
        parameters = child;
        break;
      }
    }
    if (!parameters.has_value()) {
      return parent;
    }

    const SyntaxNode &list = tree.node(*parameters);
    const ScopeId scope = semantic_.symbols.add_scope(
        ScopeKind::Parametric, parent, list.range);
    semantic_.owned_scopes.push_back({owner, scope});

    for (NodeId parameter_id : list.children) {
      const SyntaxNode &parameter = tree.node(parameter_id);
      const std::vector<SourceName> names = names_in_span(
          tree, parameter.token_begin, parameter.token_end);
      if (names.empty() || parameter.children.empty()) {
        continue;
      }
      const SourceName &name = names.front();
      const NodeId constraint_node = parameter.children.back();
      const SyntaxNode &constraint = tree.node(constraint_node);
      const std::vector<SourceName> constraint_names = names_in_span(
          tree, constraint.token_begin, constraint.token_end);
      const std::string constraint_name = constraint_names.empty()
          ? std::string()
          : constraint_names.front().text;
      const bool is_type_parameter = constraint_name == "type" ||
          constraint_name == "integer" || constraint_name == "float" ||
          constraint_name == "number";

      Symbol symbol;
      symbol.name = name.text;
      symbol.kind = is_type_parameter
          ? SymbolKind::TypeParameter
          : SymbolKind::ValueParameter;
      symbol.scope = scope;
      symbol.syntax = {tree.file(), parameter_id};
      symbol.name_range = name.range;
      if (!is_type_parameter) {
        symbol.type = resolve_type(tree, constraint_node, scope);
      }
      const SymbolId id = semantic_.symbols.declare(std::move(symbol), diagnostics_);
      if (!id.is_valid()) {
        continue;
      }

      TypeConstraintKind kind = TypeConstraintKind::CompileTimeValue;
      if (is_type_parameter) {
        kind = constraint_kind(constraint_name);
        semantic_.symbols.symbol_mut(id).type =
            semantic_.types.type_parameter(name.text, name.range);
      }
      semantic_.parametric_parameters.push_back({owner, id, kind});
    }
    return scope;
  }

  // Reads the calling-convention modifier only before `proc`; a later `c` token
  // can be an import alias in a parameter or body and must not affect the ABI.
  [[nodiscard]] bool c_calling_convention(
      const SyntaxTree &tree, const SyntaxNode &procedure) const {
    for (std::uint32_t index = procedure.token_begin; index < procedure.token_end; ++index) {
      const TokenKind kind = tree.token(index).kind;
      if (kind == TokenKind::KeywordC) {
        return true;
      }
      if (kind == TokenKind::KeywordProc) {
        return false;
      }
    }
    return false;
  }

  // Discovers body-level `when` sites in regions that are present under the
  // current selection set. Runtime branches are all scanned because all are
  // type-checked; unselected compile-time branches are not scanned. Nested
  // procedure declarations are separate owners and are deliberately skipped.
  void scan_statement_conditionals(
      SymbolId owner,
      const SyntaxTree &tree,
      NodeId node_id,
      ScopeId scope) {
    const SyntaxNode &node = tree.node(node_id);
    if (node.kind == NodeKind::WhenStatement) {
      add_site(SemanticSiteKind::ConditionalStatement, tree, node_id, scope, owner);
      const ConditionalSelection *selection =
          selections_.find({tree.file(), node_id});
      if (selection == nullptr) return;
      if (selection->select_true) {
        if (node.children.size() >= 2) {
          scan_statement_conditionals(owner, tree, node.children[1], scope);
        }
      } else if (node.children.size() >= 3) {
        scan_statement_conditionals(owner, tree, node.children[2], scope);
      }
      return;
    }

    switch (node.kind) {
    case NodeKind::Block:
    case NodeKind::StatementList:
    case NodeKind::IfStatement:
    case NodeKind::ForStatement:
    case NodeKind::ForClause:
    case NodeKind::SwitchStatement:
    case NodeKind::SwitchCase:
    case NodeKind::DenyStatement:
    case NodeKind::UncheckedStatement:
      for (NodeId child : node.children) {
        scan_statement_conditionals(owner, tree, child, scope);
      }
      break;
    default:
      break;
    }
  }

  // Builds one canonical procedure type. Named declaration procedures also get
  // a parameter scope and immutable Parameter symbols; standalone procedure
  // types contribute only their logical signature.
  [[nodiscard]] TypeId resolve_procedure_type(
      const SyntaxTree &tree,
      NodeId procedure_id,
      ScopeId parent,
      std::optional<SymbolId> owner) {
    const SyntaxNode &procedure = tree.node(procedure_id);
    ScopeId parameter_scope = parent;
    if (owner.has_value()) {
      const std::optional<ScopeId> existing = owned_scope(*owner, ScopeKind::Procedure);
      if (existing.has_value()) {
        parameter_scope = *existing;
      } else {
        parameter_scope = semantic_.symbols.add_scope(
            ScopeKind::Procedure, parent, procedure.range);
        semantic_.owned_scopes.push_back({*owner, parameter_scope});
      }
    }

    std::vector<TypeId> parameters;
    TypeId result = semantic_.types.builtins().void_type;
    for (NodeId child_id : procedure.children) {
      const SyntaxNode &child = tree.node(child_id);
      if (child.kind == NodeKind::ParameterList) {
        for (NodeId parameter_id : child.children) {
          const SyntaxNode &parameter = tree.node(parameter_id);
          if (parameter.children.size() < 2) {
            continue;
          }
          const SyntaxNode &name_list = tree.node(parameter.children.front());
          const TypeId parameter_type =
              resolve_type(tree, parameter.children.back(), parent);
          const std::vector<SourceName> names = names_in_span(
              tree, name_list.token_begin, name_list.token_end);
          for (const SourceName &name : names) {
            parameters.push_back(parameter_type);
            if (!owner.has_value() || name.text == "_") {
              continue;
            }
            Symbol symbol;
            symbol.name = name.text;
            symbol.kind = SymbolKind::Parameter;
            symbol.scope = parameter_scope;
            symbol.type = parameter_type;
            symbol.syntax = {tree.file(), parameter_id};
            symbol.name_range = name.range;
            (void)semantic_.symbols.declare(std::move(symbol), diagnostics_);
          }
        }
      } else if (child.kind == NodeKind::ResultClause && !child.children.empty()) {
        result = resolve_type(tree, child.children.front(), parent);
      }
    }
    if (owner.has_value() && procedure.kind == NodeKind::Procedure) {
      for (NodeId child_id : procedure.children) {
        if (tree.node(child_id).kind == NodeKind::Block) {
          scan_statement_conditionals(
              *owner, tree, child_id, parameter_scope);
        }
      }
    }
    return semantic_.types.procedure(
        parameters, result, c_calling_convention(tree, procedure));
  }

  // Layout syntax accepts the same exact integer expression vocabulary used by
  // enum values and @align. Conversion to u64 happens only after arbitrary-
  // precision evaluation, so overflow and negative values never wrap through
  // the bootstrap host.
  [[nodiscard]] std::optional<std::uint64_t> layout_integer(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) {
    const std::optional<BigInteger> value = integer_constant_expression(
        tree, expression_id, scope);
    return value.has_value() ? value->to_u64() : std::nullopt;
  }

  // Looks up a full-interpreter result produced by a prior semantic round.
  // Returning a pointer is safe only until the resolver returns: the driver
  // keeps the supplied vector immutable for the entire clean rebuild.
  [[nodiscard]] const ResolvedIntegerExpression *resolved_integer(
      const SyntaxTree &tree, NodeId expression) const {
    if (resolved_integers_ == nullptr) return nullptr;
    const SyntaxReference wanted{tree.file(), expression};
    for (const ResolvedIntegerExpression &entry : *resolved_integers_) {
      if (entry.syntax == wanted) return &entry;
    }
    return nullptr;
  }

  // Enforces the contextual type of one ready layout integer. Full-interpreter
  // results carry their round-independent descriptor explicitly. Expressions
  // handled by the early builder recover the same information from their
  // canonical root, so `cast[u64](4)` cannot become a usize merely because both
  // types have the same AArch64 representation. Untyped integers remain
  // contextually convertible; integer-shaped non-integers such as enums fail.
  [[nodiscard]] bool integer_constant_matches(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId expected,
      std::string_view description) {
    const ResolvedIntegerExpression *resolved =
        resolved_integer(tree, expression);
    std::optional<IntegerExpressionType> supplied_type;
    bool type_is_known = false;
    if (resolved != nullptr) {
      type_is_known = true;
      supplied_type = resolved->type;
    } else {
      IntegerExpression built;
      const BuiltIntegerExpressionNode root = build_integer_expression_node(
          tree, expression, scope, built);
      if (root.valid && root.constant.has_value() && root.type.is_valid()) {
        type_is_known = true;
        const TypeKind kind = semantic_.types.type(root.type).kind;
        if (kind == TypeKind::UntypedInteger) {
          supplied_type = IntegerExpressionType{};
        } else if (kind == TypeKind::SignedInteger ||
                   kind == TypeKind::UnsignedInteger) {
          supplied_type = integer_expression_type(root.type);
        }
      }
    }
    if (!type_is_known) return true;
    if (!supplied_type.has_value()) {
      diagnostics_.error(
          tree.node(expression).range,
          std::string(description) + " must have an integer type");
      return false;
    }
    if (supplied_type->representation ==
        IntegerExpressionRepresentation::Untyped) {
      return true;
    }
    const IntegerExpressionType required = integer_expression_type(expected);
    if (*supplied_type == required) return true;
    diagnostics_.error(
        tree.node(expression).range,
        std::string(description) + " must have type '" + required.identity + "'");
    return false;
  }

  // Records one exact syntax site which needs the full interpreter. Multiple
  // type-resolution paths can encounter the same expression while aliases and
  // templates resolve, so source-key deduplication keeps each work item unique
  // without retaining an unstable semantic ID.
  void require_integer_expression(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId expected_type) {
    const SyntaxReference syntax{tree.file(), expression};
    for (RequiredIntegerExpression &entry :
         semantic_.required_integer_expressions) {
      if (entry.syntax != syntax) continue;
      if (!entry.expected_type.is_valid() && expected_type.is_valid()) {
        entry.expected_type = expected_type;
      } else if (entry.expected_type.is_valid() && expected_type.is_valid() &&
                 entry.expected_type != expected_type) {
        diagnostics_.error(
            tree.node(expression).range,
            "integer expression is required with inconsistent types");
      }
      if (!entry.anchor.is_valid() && !active_declaration_owners_.empty()) {
        entry.anchor = active_declaration_owners_.back();
      }
      return;
    }
    const SymbolId anchor = active_declaration_owners_.empty()
        ? SymbolId{}
        : active_declaration_owners_.back();
    semantic_.required_integer_expressions.push_back(
        {syntax, scope, anchor, expected_type});
  }

  [[nodiscard]] bool integer_synthesis_is_blocked(
      const SyntaxTree &tree, NodeId expression) const {
    if (blocked_integer_synthesis_ == nullptr) return false;
    const SyntaxReference syntax{tree.file(), expression};
    return std::find(
               blocked_integer_synthesis_->begin(),
               blocked_integer_synthesis_->end(),
               syntax) != blocked_integer_synthesis_->end();
  }

  // The compact dependent-integer builder intentionally rejects calls and
  // other full language expressions. Before treating such a failure as an
  // ordinary unresolved constant, distinguish a generic recipe which must wait
  // for its value/type parameters. Every parameter reference remains visible
  // in the parsed subtree, including explicit arguments to generic helpers.
  [[nodiscard]] bool expression_references_parametric_parameter(
      const SyntaxTree &tree, NodeId expression, ScopeId scope) const {
    const SyntaxNode &node = tree.node(expression);
    if (node.kind == NodeKind::NameExpression ||
        node.kind == NodeKind::NamedType) {
      const std::vector<SourceName> names = names_in_span(
          tree, node.token_begin, node.token_end);
      if (names.size() == 1) {
        const std::optional<SymbolId> symbol =
            semantic_.symbols.lookup(scope, names.front().text);
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

  [[nodiscard]] bool expression_references_symbol(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      SymbolId wanted) const {
    const SyntaxNode &node = tree.node(expression);
    if (node.kind == NodeKind::NameExpression ||
        node.kind == NodeKind::NamedType) {
      const std::vector<SourceName> names = names_in_span(
          tree, node.token_begin, node.token_end);
      if (names.size() == 1) {
        const std::optional<SymbolId> symbol =
            semantic_.symbols.lookup(scope, names.front().text);
        if (symbol == wanted) return true;
      }
    }
    for (NodeId child : node.children) {
      if (expression_references_symbol(
              tree, child, scope, wanted)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool expression_references_type_parameter(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId wanted) const {
    for (const ParametricParameterRecord &parameter :
         semantic_.parametric_parameters) {
      const Symbol &symbol = semantic_.symbols.symbol(parameter.parameter);
      if (symbol.kind == SymbolKind::TypeParameter &&
          symbol.type == wanted &&
          expression_references_symbol(
              tree, expression, scope, parameter.parameter)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool deferred_expression_has_unbound_parameters(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      const std::vector<DeferredElementCountTypeBinding> &type_bindings,
      const std::vector<DeferredElementCountValueBinding> &value_bindings) const {
    for (const ParametricParameterRecord &parameter :
         semantic_.parametric_parameters) {
      const Symbol &symbol = semantic_.symbols.symbol(parameter.parameter);
      if (!expression_references_symbol(
              tree, expression, scope, parameter.parameter)) {
        continue;
      }
      if (symbol.kind == SymbolKind::TypeParameter) {
        const bool bound = std::any_of(
            type_bindings.begin(),
            type_bindings.end(),
            [&](const DeferredElementCountTypeBinding &binding) {
              return binding.parameter == symbol.type;
            });
        if (!bound) return true;
      } else if (symbol.kind == SymbolKind::ValueParameter) {
        const bool bound = std::any_of(
            value_bindings.begin(),
            value_bindings.end(),
            [&](const DeferredElementCountValueBinding &binding) {
              return binding.parameter == parameter.parameter;
            });
        if (!bound) return true;
      }
    }
    return false;
  }

  [[nodiscard]] TypeId defer_element_count(
      TypeKind kind,
      TypeId element,
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      std::vector<DeferredElementCountTypeBinding> type_bindings = {},
      std::vector<DeferredElementCountValueBinding> value_bindings = {}) {
    const std::uint32_t index = static_cast<std::uint32_t>(
        semantic_.deferred_element_counts.size());
    const TypeId type = kind == TypeKind::Array
        ? semantic_.types.owner_evaluated_array(element, index)
        : semantic_.types.owner_evaluated_simd(element, index);
    semantic_.deferred_element_counts.push_back({
        type,
        {tree.file(), expression},
        scope,
        std::move(type_bindings),
        std::move(value_bindings),
    });
    return type;
  }

  [[nodiscard]] ParametricArgument defer_value_expression(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope,
      TypeId expected_type,
      std::vector<DeferredElementCountTypeBinding> type_bindings = {},
      std::vector<DeferredElementCountValueBinding> value_bindings = {}) {
    ParametricArgument argument;
    argument.is_type = false;
    argument.value_type = expected_type;
    argument.owner_evaluated_value = true;
    argument.deferred_value_index = static_cast<std::uint32_t>(
        semantic_.deferred_value_expressions.size());
    semantic_.deferred_value_expressions.push_back({
        {tree.file(), expression},
        scope,
        expected_type,
        std::move(type_bindings),
        std::move(value_bindings),
    });
    return argument;
  }

  [[nodiscard]] IntegerExpressionType integer_expression_type(
      TypeId type_id) const {
    const Type type = semantic_.types.type(type_id);
    IntegerExpressionType result;
    result.bit_width = type.bit_width;
    if (type.kind == TypeKind::SignedInteger) {
      result.representation = IntegerExpressionRepresentation::Signed;
      result.identity = type.name;
    } else if (type.kind == TypeKind::UnsignedInteger) {
      result.representation = IntegerExpressionRepresentation::Unsigned;
      result.identity = type.name;
    }
    return result;
  }

  [[nodiscard]] std::vector<IntegerExpressionReplacement>
  integer_expression_replacements(
      const std::vector<ResolverValueSubstitution> &substitutions) const {
    std::vector<IntegerExpressionReplacement> result;
    result.reserve(substitutions.size());
    for (const ResolverValueSubstitution &substitution : substitutions) {
      IntegerExpressionReplacement replacement;
      replacement.parameter = substitution.parameter;
      if (substitution.symbolic_expression.is_valid()) {
        replacement.expression = substitution.symbolic_expression;
      } else {
        replacement.value = substitution.replacement;
      }
      result.push_back(std::move(replacement));
    }
    return result;
  }

  [[nodiscard]] std::optional<SymbolId> integer_expression_symbol(
      const SyntaxTree &tree,
      const SyntaxNode &expression,
      ScopeId scope) const {
    const std::vector<SourceName> names = names_in_span(
        tree, expression.token_begin, expression.token_end);
    if (expression.kind == NodeKind::NameExpression && names.size() == 1) {
      return semantic_.symbols.lookup(scope, names.front().text);
    }
    if (expression.kind != NodeKind::MemberExpression || names.size() != 2) {
      return std::nullopt;
    }
    const std::optional<SymbolId> import =
        semantic_.symbols.lookup(scope, names.front().text);
    if (!import.has_value() ||
        semantic_.symbols.symbol(*import).kind != SymbolKind::Import) {
      return std::nullopt;
    }
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner == *import &&
          semantic_.symbols.scope(owned.scope).kind ==
              ScopeKind::ImportedPackage) {
        return semantic_.symbols.lookup_direct(
            owned.scope, names.back().text);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<IntegerExpressionOperation>
  integer_expression_binary_operation(TokenKind operation) const {
    switch (operation) {
    case TokenKind::Plus: return IntegerExpressionOperation::Add;
    case TokenKind::Minus: return IntegerExpressionOperation::Subtract;
    case TokenKind::Star: return IntegerExpressionOperation::Multiply;
    case TokenKind::Slash: return IntegerExpressionOperation::Divide;
    case TokenKind::Percent: return IntegerExpressionOperation::Remainder;
    case TokenKind::ShiftLeft: return IntegerExpressionOperation::ShiftLeft;
    case TokenKind::ShiftRight: return IntegerExpressionOperation::ShiftRight;
    case TokenKind::Ampersand: return IntegerExpressionOperation::BitwiseAnd;
    case TokenKind::Pipe: return IntegerExpressionOperation::BitwiseOr;
    case TokenKind::Caret: return IntegerExpressionOperation::BitwiseXor;
    default: return std::nullopt;
    }
  }

  // Recognizes only the intrinsic spelling `cast[T](value)`. An arbitrary
  // bracketed procedure call is not a conversion and must remain available to
  // the general compile-time evaluator rather than entering this compact
  // dependent-integer tree.
  [[nodiscard]] std::optional<std::pair<NodeId, NodeId>>
  integer_cast_parts(
      const SyntaxTree &tree, const SyntaxNode &call) const {
    if (call.kind != NodeKind::CallExpression || call.children.size() != 2) {
      return std::nullopt;
    }
    const SyntaxNode &callee = tree.node(call.children.front());
    if (callee.kind != NodeKind::BracketExpression ||
        callee.children.size() != 2) {
      return std::nullopt;
    }
    const SyntaxNode &base = tree.node(callee.children.front());
    const std::vector<SourceName> names = names_in_span(
        tree, base.token_begin, base.token_end);
    if (base.kind != NodeKind::NameExpression || names.size() != 1 ||
        names.front().text != "cast") {
      return std::nullopt;
    }
    return std::pair<NodeId, NodeId>{
        callee.children.back(), call.children.back()};
  }

  [[nodiscard]] bool contains_integer_cast(
      const SyntaxTree &tree, NodeId expression_id) const {
    const SyntaxNode &node = tree.node(expression_id);
    if (integer_cast_parts(tree, node).has_value()) return true;
    for (NodeId child : node.children) {
      if (contains_integer_cast(tree, child)) return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<BigInteger> evaluate_constant_integer_node(
      IntegerExpressionOperation operation,
      const BuiltIntegerExpressionNode &left,
      const BuiltIntegerExpressionNode *right,
      TypeId result_type,
      std::string &error) const {
    if (!left.constant.has_value() ||
        (right != nullptr && !right->constant.has_value())) {
      return std::nullopt;
    }
    IntegerExpression expression;
    const std::uint32_t left_node = append_integer_constant(
        expression,
        *left.constant,
        integer_expression_type(left.type));
    if (right == nullptr) {
      expression.root = append_integer_unary(
          expression,
          operation,
          left_node,
          integer_expression_type(result_type));
    } else {
      const std::uint32_t right_node = append_integer_constant(
          expression,
          *right->constant,
          integer_expression_type(right->type));
      expression.root = append_integer_binary(
          expression,
          operation,
          left_node,
          right_node,
          integer_expression_type(result_type));
    }
    const IntegerExpressionResult evaluated =
        evaluate_integer_expression(expression);
    if (!evaluated.ok) {
      error = evaluated.error;
      return std::nullopt;
    }
    return evaluated.value;
  }

  [[nodiscard]] bool contextual_integer_constant_fits(
      const BuiltIntegerExpressionNode &node, TypeId destination) const {
    return node.type != semantic_.types.builtins().untyped_integer ||
        (node.constant.has_value() &&
         integer_fits_type(*node.constant, destination));
  }

  [[nodiscard]] bool integer_fits_expression_type(
      const BigInteger &value, IntegerExpressionType type) const {
    if (type.representation == IntegerExpressionRepresentation::Untyped ||
        type.bit_width == 0) {
      return true;
    }
    if (type.representation == IntegerExpressionRepresentation::Unsigned) {
      return !value.is_negative() && value.bit_count() <= type.bit_width;
    }
    const BigInteger magnitude = BigInteger::from_u64(1).shifted_left(
        static_cast<std::size_t>(type.bit_width - 1U));
    return value.compare(magnitude.negated()) >= 0 &&
        value.compare(magnitude.subtracted(BigInteger::from_u64(1))) <= 0;
  }

  [[nodiscard]] bool contextualize_integer_expression_node(
      IntegerExpression &expression,
      std::uint32_t index,
      std::optional<IntegerExpressionType> context,
      SourceRange range) {
    IntegerExpressionNode &node = expression.nodes[index];
    if (node.type.representation == IntegerExpressionRepresentation::Untyped &&
        context.has_value()) {
      // Context applies recursively to an untyped numeric expression. Reject
      // an out-of-domain literal before changing its node type; otherwise a
      // value such as 256 in a u8 expression would silently wrap merely because
      // the surrounding expression also contains a value parameter.
      if (node.operation == IntegerExpressionOperation::Constant &&
          !integer_fits_expression_type(node.constant, *context)) {
        diagnostics_.error(
            range,
            "dependent integer constant is not representable in its contextual type");
        return false;
      }
      node.type = *context;
    }
    const std::optional<IntegerExpressionType> node_context =
        node.type.representation == IntegerExpressionRepresentation::Untyped
        ? std::nullopt
        : std::optional<IntegerExpressionType>(node.type);
    if (node.operation == IntegerExpressionOperation::Constant ||
        node.operation == IntegerExpressionOperation::Parameter) {
      return true;
    }

    // An explicit cast supplies a result type, not an implicit source context.
    // Preserve an all-untyped operand as mathematical arithmetic. If the
    // operand already inferred a concrete type from one of its own parameters,
    // propagate that independent type through its untyped children.
    if (node.operation == IntegerExpressionOperation::Cast) {
      const IntegerExpressionNode &operand = expression.nodes[node.left];
      const std::optional<IntegerExpressionType> operand_context =
          operand.type.representation ==
              IntegerExpressionRepresentation::Untyped
          ? std::nullopt
          : std::optional<IntegerExpressionType>(operand.type);
      return contextualize_integer_expression_node(
          expression, node.left, operand_context, range);
    }
    if (!contextualize_integer_expression_node(
            expression, node.left, node_context, range)) {
      return false;
    }
    if (node.right != std::numeric_limits<std::uint32_t>::max() &&
        !contextualize_integer_expression_node(
            expression, node.right, node_context, range)) {
      return false;
    }
    return true;
  }

  [[nodiscard]] BuiltIntegerExpressionNode build_integer_expression_node(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      IntegerExpression &expression) {
    const SyntaxNode &node = tree.node(expression_id);
    if (node.kind == NodeKind::LiteralExpression &&
        node.token_begin < node.token_end &&
        tree.token(node.token_begin).kind == TokenKind::IntegerLiteral) {
      const std::optional<BigInteger> value = BigInteger::parse_literal(
          sources_.text(tree.token(node.token_begin).range));
      if (!value.has_value()) return {};
      return {
          true,
          append_integer_constant(expression, *value),
          semantic_.types.builtins().untyped_integer,
          *value,
      };
    }
    if (node.kind == NodeKind::GroupExpression && !node.children.empty()) {
      return build_integer_expression_node(
          tree, node.children.front(), scope, expression);
    }
    if (node.kind == NodeKind::NameExpression ||
        node.kind == NodeKind::MemberExpression) {
      const std::optional<SymbolId> symbol =
          integer_expression_symbol(tree, node, scope);
      if (!symbol.has_value()) return {};
      const Symbol binding = semantic_.symbols.symbol(*symbol);
      if (binding.kind == SymbolKind::ValueParameter) {
        // Value-parameter syntax is intentionally broad enough to diagnose a
        // declaration such as `N: bool` semantically. It must not enter the
        // integer-expression graph, whose node representation assumes an
        // actual signed, unsigned, or untyped integer.
        if (!semantic_.types.is_integer(binding.type)) return {};
        return {
            true,
            append_integer_parameter(
                expression,
                symbol->value,
                integer_expression_type(binding.type)),
            binding.type,
            std::nullopt,
        };
      }
      const std::optional<BigInteger> value =
          named_integer_constant(*symbol, node.range);
      if (!value.has_value()) return {};
      const TypeId value_type = binding.type.is_valid() &&
              semantic_.types.is_integer(binding.type)
          ? binding.type
          : semantic_.types.builtins().untyped_integer;
      return {
          true,
          append_integer_constant(
              expression, *value, integer_expression_type(value_type)),
          value_type,
          *value,
      };
    }
    if (const std::optional<std::pair<NodeId, NodeId>> cast =
            integer_cast_parts(tree, node)) {
      const TypeId destination =
          resolve_type_argument(tree, cast->first, scope);
      if (!semantic_.types.is_integer(destination)) {
        diagnostics_.error(
            tree.node(cast->first).range,
            "dependent integer cast requires an integer destination type");
        return {};
      }
      const BuiltIntegerExpressionNode operand = build_integer_expression_node(
          tree, cast->second, scope, expression);
      if (!operand.valid) return {};
      std::string error;
      const std::optional<BigInteger> constant = evaluate_constant_integer_node(
          IntegerExpressionOperation::Cast,
          operand,
          nullptr,
          destination,
          error);
      if (!error.empty()) {
        diagnostics_.error(node.range, error);
        return {};
      }
      return {
          true,
          append_integer_unary(
              expression,
              IntegerExpressionOperation::Cast,
              operand.node,
              integer_expression_type(destination)),
          destination,
          constant,
      };
    }
    if (node.kind == NodeKind::UnaryExpression && !node.children.empty()) {
      const BuiltIntegerExpressionNode operand = build_integer_expression_node(
          tree, node.children.front(), scope, expression);
      if (!operand.valid) return {};
      IntegerExpressionOperation operation;
      const TokenKind token = tree.token(node.token_begin).kind;
      if (token == TokenKind::Plus) {
        operation = IntegerExpressionOperation::Positive;
      } else if (token == TokenKind::Minus) {
        operation = IntegerExpressionOperation::Negate;
      } else if (token == TokenKind::Tilde) {
        operation = IntegerExpressionOperation::BitwiseNot;
      } else {
        return {};
      }
      std::string error;
      const std::optional<BigInteger> constant = evaluate_constant_integer_node(
          operation, operand, nullptr, operand.type, error);
      if (!error.empty()) {
        diagnostics_.error(node.range, error);
        return {};
      }
      return {
          true,
          append_integer_unary(
              expression,
              operation,
              operand.node,
              integer_expression_type(operand.type)),
          operand.type,
          constant,
      };
    }
    if (node.kind != NodeKind::BinaryExpression ||
        node.children.size() != 2) {
      return {};
    }

    const BuiltIntegerExpressionNode left = build_integer_expression_node(
        tree, node.children.front(), scope, expression);
    const BuiltIntegerExpressionNode right = build_integer_expression_node(
        tree, node.children.back(), scope, expression);
    if (!left.valid || !right.valid) return {};
    const TokenKind token = expression_binary_operator(tree, node);
    const std::optional<IntegerExpressionOperation> operation =
        integer_expression_binary_operation(token);
    if (!operation.has_value()) return {};

    TypeId result_type = left.type;
    const bool shift = token == TokenKind::ShiftLeft ||
        token == TokenKind::ShiftRight;
    if (!shift) {
      if (left.type == right.type) {
        result_type = left.type;
      } else if (left.type == semantic_.types.builtins().untyped_integer &&
                 semantic_.types.is_integer(right.type) &&
                 contextual_integer_constant_fits(left, right.type)) {
        result_type = right.type;
      } else if (right.type == semantic_.types.builtins().untyped_integer &&
                 semantic_.types.is_integer(left.type) &&
                 contextual_integer_constant_fits(right, left.type)) {
        result_type = left.type;
      } else {
        diagnostics_.error(
            node.range,
            "dependent integer expression operands require one common type");
        return {};
      }
    } else if (!semantic_.types.is_integer(left.type) &&
               left.type != semantic_.types.builtins().untyped_integer) {
      diagnostics_.error(
          tree.node(node.children.front()).range,
          "dependent shift left operand must be an integer");
      return {};
    }

    std::string error;
    const std::optional<BigInteger> constant = evaluate_constant_integer_node(
        *operation, left, &right, result_type, error);
    if (!error.empty()) {
      diagnostics_.error(node.range, error);
      return {};
    }
    return {
        true,
        append_integer_binary(
            expression,
            *operation,
            left.node,
            right.node,
            integer_expression_type(result_type)),
        result_type,
        constant,
    };
  }

  [[nodiscard]] std::optional<IntegerExpression>
  dependent_integer_expression(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      TypeId contextual_type) {
    IntegerExpression expression;
    const BuiltIntegerExpressionNode root = build_integer_expression_node(
        tree, expression_id, scope, expression);
    if (!root.valid || !integer_expression_has_parameters(expression)) {
      return std::nullopt;
    }
    // A typed root keeps its inferred type here. An all-untyped root (for
    // example `1 << N`) borrows the surrounding result type. Context then
    // propagates through every still-untyped node, matching ordinary numeric
    // body checking. Parameter leaves remain typed and are separately checked
    // against a callee/template's exact declared value-parameter type.
    expression.root = root.node;
    const TypeId result_type =
        root.type == semantic_.types.builtins().untyped_integer
        ? contextual_type
        : root.type;
    if (!result_type.is_valid() ||
        !semantic_.types.is_integer(result_type) ||
        !contextualize_integer_expression_node(
            expression,
            expression.root,
            integer_expression_type(result_type),
            tree.node(expression_id).range)) {
      return std::nullopt;
    }
    if (!expression.is_valid()) return std::nullopt;
    return expression;
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

  [[nodiscard]] std::optional<TypeConstraintKind> type_constraint(
      TypeId type) const {
    if (!type.is_valid() ||
        semantic_.types.type(type).kind != TypeKind::TypeParameter) {
      return std::nullopt;
    }
    for (const ParametricParameterRecord &parameter :
         semantic_.parametric_parameters) {
      if (semantic_.symbols.symbol(parameter.parameter).type == type) {
        return parameter.constraint;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool type_satisfies_constraint(
      TypeId argument, TypeConstraintKind constraint) const {
    if (!argument.is_valid()) return false;
    const TypeKind kind = semantic_.types.type(argument).kind;
    if (kind == TypeKind::Invalid ||
        kind == TypeKind::UntypedInteger || kind == TypeKind::UntypedFloat) {
      return false;
    }
    // A symbolic parameter can flow into another parametric type when its own
    // declared constraint is at least as strong as the destination's. This is
    // the signature-resolution counterpart to body checker's procedure-template
    // rule. Concrete instantiation still rechecks the actual type.
    if (kind == TypeKind::TypeParameter) {
      const std::optional<TypeConstraintKind> actual =
          type_constraint(argument);
      if (!actual.has_value()) return false;
      if (constraint == TypeConstraintKind::AnyType) return true;
      if (constraint == TypeConstraintKind::Number) {
        return *actual == TypeConstraintKind::Integer ||
            *actual == TypeConstraintKind::Float ||
            *actual == TypeConstraintKind::Number;
      }
      return *actual == constraint;
    }
    if (constraint == TypeConstraintKind::AnyType) return true;
    // A distinct type deliberately keeps the operators of its underlying
    // scalar, but it is not a member of the closed built-in constraint sets.
    // Aliases need no special handling because they resolve to the aliased
    // TypeId before this point.
    if (kind == TypeKind::Distinct) return false;
    if (constraint == TypeConstraintKind::Integer) {
      return semantic_.types.is_integer(argument);
    }
    if (constraint == TypeConstraintKind::Float) {
      return semantic_.types.is_float(argument);
    }
    if (constraint == TypeConstraintKind::Number) {
      return semantic_.types.is_number(argument);
    }
    return false;
  }

  [[nodiscard]] bool type_has_parameters(
      TypeId type, std::vector<TypeId> &active) const {
    if (!type.is_valid()) return false;
    if (std::find(active.begin(), active.end(), type) != active.end()) {
      return false;
    }
    active.push_back(type);
    const Type value = semantic_.types.type(type);
    bool result = value.kind == TypeKind::TypeParameter ||
        value.owner_evaluated_element_count ||
        value.owner_evaluated_type_application ||
        value.element_count_expression.is_valid();
    if (!result &&
        (value.kind == TypeKind::Pointer ||
         value.kind == TypeKind::MultiPointer ||
         value.kind == TypeKind::Slice ||
         value.kind == TypeKind::Array ||
         value.kind == TypeKind::Simd ||
         value.kind == TypeKind::Distinct)) {
      result = type_has_parameters(value.element, active);
    }
    if (!result &&
        (value.kind == TypeKind::Tuple || value.kind == TypeKind::Procedure)) {
      for (TypeId member : value.members) {
        if (type_has_parameters(member, active)) {
          result = true;
          break;
        }
      }
    }
    if (!result &&
        (value.kind == TypeKind::Struct || value.kind == TypeKind::Enum ||
         value.kind == TypeKind::TaggedUnion ||
         value.kind == TypeKind::RawUnion)) {
      for (const ParametricTypeInstanceRecord &instance :
           semantic_.parametric_type_instances) {
        if (semantic_.symbols.symbol(instance.instance).type != type) continue;
        for (const ParametricArgument &argument : instance.arguments) {
          if ((argument.is_type && type_has_parameters(argument.type, active)) ||
              (!argument.is_type &&
               (argument.value_expression.is_valid() ||
                argument.owner_evaluated_value))) {
            result = true;
            break;
          }
        }
        break;
      }
      if (!result) {
        for (const ImportedType &imported : semantic_.imported_types) {
          if (imported.type != type) continue;
          for (const ParametricArgument &argument : imported.arguments) {
            if ((argument.is_type && type_has_parameters(argument.type, active)) ||
                (!argument.is_type &&
                 (argument.value_expression.is_valid() ||
                  argument.owner_evaluated_value))) {
              result = true;
              break;
            }
          }
          break;
        }
      }
    }
    active.pop_back();
    return result;
  }

  [[nodiscard]] bool type_has_parameters(TypeId type) const {
    std::vector<TypeId> active;
    return type_has_parameters(type, active);
  }

  [[nodiscard]] bool type_requires_owner_evaluation(
      TypeId type, std::vector<TypeId> &active) const {
    if (!type.is_valid() ||
        std::find(active.begin(), active.end(), type) != active.end()) {
      return false;
    }
    active.push_back(type);
    const Type value = semantic_.types.type(type);
    bool result = value.owner_evaluated_element_count ||
        value.owner_evaluated_type_application;
    if (!result && value.element.is_valid()) {
      result = type_requires_owner_evaluation(value.element, active);
    }
    if (!result) {
      for (TypeId member : value.members) {
        if (type_requires_owner_evaluation(member, active)) {
          result = true;
          break;
        }
      }
    }
    if (!result &&
        (value.kind == TypeKind::Struct || value.kind == TypeKind::Enum ||
         value.kind == TypeKind::TaggedUnion ||
         value.kind == TypeKind::RawUnion)) {
      for (const ParametricTypeInstanceRecord &instance :
           semantic_.parametric_type_instances) {
        if (semantic_.symbols.symbol(instance.instance).type != type) continue;
        for (const ParametricArgument &argument : instance.arguments) {
          if (argument.owner_evaluated_value) {
            result = true;
            break;
          }
        }
        break;
      }
      if (!result) {
        for (const ImportedType &imported : semantic_.imported_types) {
          if (imported.type != type) continue;
          for (const ParametricArgument &argument : imported.arguments) {
            if (argument.owner_evaluated_value) {
              result = true;
              break;
            }
          }
          break;
        }
      }
    }
    active.pop_back();
    return result;
  }

  [[nodiscard]] bool type_requires_owner_evaluation(TypeId type) const {
    std::vector<TypeId> active;
    return type_requires_owner_evaluation(type, active);
  }

  [[nodiscard]] std::optional<ParametricArgument>
  substitute_deferred_value_argument(
      const ParametricArgument &source,
      const std::vector<ResolverTypeSubstitution> &substitutions,
      const std::vector<ResolverValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    if (!source.owner_evaluated_value) return source;
    if (source.deferred_value_index >=
        semantic_.deferred_value_expressions.size()) {
      // Imported templates carry only the owner marker. The enclosing public
      // type request is sent back to its defining package before this
      // provisional consumer graph is used for body checking.
      return source;
    }

    const DeferredValueExpression recipe =
        semantic_.deferred_value_expressions[source.deferred_value_index];
    const SyntaxTree *tree = find_tree(recipe.syntax.file);
    if (tree == nullptr || !recipe.syntax.node.is_valid()) {
      diagnostics_.error(
          use_range, "owner-evaluated generic argument has no source recipe");
      return std::nullopt;
    }
    std::vector<DeferredElementCountTypeBinding> type_bindings =
        recipe.type_bindings;
    std::vector<DeferredElementCountValueBinding> value_bindings =
        recipe.value_bindings;
    bool changed = false;
    for (DeferredElementCountTypeBinding &binding : type_bindings) {
      const TypeId replacement = substitute_type(
          binding.replacement,
          substitutions,
          value_substitutions,
          use_range);
      changed = changed || replacement != binding.replacement;
      binding.replacement = replacement;
    }
    for (const ResolverTypeSubstitution &substitution : substitutions) {
      const bool present = std::any_of(
          type_bindings.begin(),
          type_bindings.end(),
          [&](const DeferredElementCountTypeBinding &binding) {
            return binding.parameter == substitution.parameter;
          });
      if (!present && expression_references_type_parameter(
              *tree,
              recipe.syntax.node,
              recipe.scope,
              substitution.parameter)) {
        type_bindings.push_back(
            {substitution.parameter, substitution.replacement});
        changed = true;
      }
    }

    const std::vector<IntegerExpressionReplacement> replacements =
        integer_expression_replacements(value_substitutions);
    for (DeferredElementCountValueBinding &binding : value_bindings) {
      if (!binding.symbolic_expression.is_valid()) continue;
      std::string error;
      const std::optional<IntegerExpression> replacement =
          substitute_integer_expression(
              binding.symbolic_expression, replacements, error);
      if (!replacement.has_value()) {
        diagnostics_.error(use_range, error);
        return std::nullopt;
      }
      changed = changed || *replacement != binding.symbolic_expression;
      binding.symbolic_expression = *replacement;
      if (!integer_expression_has_parameters(binding.symbolic_expression)) {
        const IntegerExpressionResult evaluated =
            evaluate_integer_expression(binding.symbolic_expression);
        if (!evaluated.ok) {
          diagnostics_.error(use_range, evaluated.error);
          return std::nullopt;
        }
        binding.value = ConstantValue::make_integer(evaluated.value);
        binding.symbolic_expression = {};
      }
    }
    for (const ResolverValueSubstitution &substitution : value_substitutions) {
      const SymbolId parameter{substitution.parameter};
      const bool present = std::any_of(
          value_bindings.begin(),
          value_bindings.end(),
          [&](const DeferredElementCountValueBinding &binding) {
            return binding.parameter == parameter;
          });
      if (present || !expression_references_symbol(
              *tree,
              recipe.syntax.node,
              recipe.scope,
              parameter)) {
        continue;
      }
      DeferredElementCountValueBinding binding;
      binding.parameter = parameter;
      binding.symbolic_expression = substitution.symbolic_expression;
      if (!binding.symbolic_expression.is_valid()) {
        binding.value = ConstantValue::make_integer(substitution.replacement);
      }
      value_bindings.push_back(std::move(binding));
      changed = true;
    }

    // Body checking may substitute the active outer procedure environment into
    // every visible symbol before it recognizes a parametric application. Do
    // not run this recipe merely because that unrelated environment is
    // nonempty: only a binding actually referenced by the saved source can
    // make semantic progress.
    if (!changed && deferred_expression_has_unbound_parameters(
            *tree,
            recipe.syntax.node,
            recipe.scope,
            type_bindings,
            value_bindings)) {
      return source;
    }

    bool symbolic = false;
    for (const DeferredElementCountTypeBinding &binding : type_bindings) {
      if (type_has_parameters(binding.replacement)) {
        symbolic = true;
        break;
      }
    }
    if (!symbolic) {
      for (const DeferredElementCountValueBinding &binding : value_bindings) {
        if (binding.symbolic_expression.is_valid()) {
          symbolic = true;
          break;
        }
      }
    }
    if (symbolic || target_ == nullptr) {
      return defer_value_expression(
          *tree,
          recipe.syntax.node,
          recipe.scope,
          recipe.expected_type,
          std::move(type_bindings),
          std::move(value_bindings));
    }

    ConstantTable constants;
    for (const DeferredElementCountValueBinding &binding : value_bindings) {
      constants.bindings.push_back({binding.parameter, binding.value});
    }
    if (active_constants_ != nullptr) {
      for (const ConstantBinding &binding : active_constants_->bindings) {
        if (constants.find(binding.symbol) == nullptr) {
          constants.bindings.push_back(binding);
        }
      }
    }
    std::vector<ConstantTypeBinding> types;
    for (const DeferredElementCountTypeBinding &binding : type_bindings) {
      types.push_back({binding.parameter, binding.replacement});
    }
    if (active_types_ != nullptr) {
      for (const ConstantTypeBinding &binding : *active_types_) {
        const bool present = std::any_of(
            types.begin(),
            types.end(),
            [&](const ConstantTypeBinding &candidate) {
              return candidate.parameter == binding.parameter;
            });
        if (!present) types.push_back(binding);
      }
    }
    const std::optional<EvaluatedConstant> evaluated =
        evaluate_typed_constant_expression(
            sources_,
            loaded_,
            semantic_,
            *target_,
            *tree,
            recipe.syntax.node,
            recipe.scope,
            diagnostics_,
            &constants,
            &types,
            recipe.expected_type);
    if (!evaluated.has_value() ||
        evaluated->value.kind != ConstantKind::Integer) {
      return std::nullopt;
    }
    const TypeKind result_kind = evaluated->type.is_valid()
        ? semantic_.types.type(evaluated->type).kind
        : TypeKind::Invalid;
    if (evaluated->type != recipe.expected_type &&
        result_kind != TypeKind::UntypedInteger) {
      diagnostics_.error(
          tree->node(recipe.syntax.node).range,
          "compile-time value argument has the wrong concrete integer type");
      return std::nullopt;
    }
    if (!integer_fits_type(evaluated->value.integer, recipe.expected_type)) {
      diagnostics_.error(
          tree->node(recipe.syntax.node).range,
          "compile-time value argument is not representable in its parameter type");
      return std::nullopt;
    }
    ParametricArgument result;
    result.is_type = false;
    result.value_type = recipe.expected_type;
    result.value = evaluated->value;
    return result;
  }

  // Applies one outer generic environment to an ordered type-application
  // packet. Nominal instances and deferred structural aliases use the same
  // argument rules; keeping them here prevents the two paths from drifting on
  // full owner-evaluated values or compact integer expressions.
  [[nodiscard]] std::optional<bool> substitute_parametric_arguments(
      std::vector<ParametricArgument> &arguments,
      const std::vector<ResolverTypeSubstitution> &substitutions,
      const std::vector<ResolverValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    bool changed = false;
    for (ParametricArgument &argument : arguments) {
      if (argument.is_type) {
        const TypeId replacement = substitute_type(
            argument.type,
            substitutions,
            value_substitutions,
            use_range);
        changed = changed || replacement != argument.type;
        argument.type = replacement;
        continue;
      }
      if (argument.owner_evaluated_value) {
        const std::optional<ParametricArgument> replacement =
            substitute_deferred_value_argument(
                argument,
                substitutions,
                value_substitutions,
                use_range);
        if (!replacement.has_value()) return std::nullopt;
        changed = changed || *replacement != argument;
        argument = *replacement;
        continue;
      }
      if (!argument.value_expression.is_valid()) continue;
      std::string error;
      const std::optional<IntegerExpression> replacement =
          substitute_integer_expression(
              argument.value_expression,
              integer_expression_replacements(value_substitutions),
              error);
      if (!replacement.has_value()) {
        diagnostics_.error(use_range, error);
        return std::nullopt;
      }
      changed = changed || *replacement != argument.value_expression;
      argument.value_expression = *replacement;
      if (!integer_expression_has_parameters(argument.value_expression)) {
        const IntegerExpressionResult evaluated =
            evaluate_integer_expression(argument.value_expression);
        if (!evaluated.ok) {
          diagnostics_.error(use_range, evaluated.error);
          return std::nullopt;
        }
        argument.value = ConstantValue::make_integer(evaluated.value);
        argument.value_expression = {};
      }
    }
    return changed;
  }

  [[nodiscard]] TypeId defer_type_application(
      TypeId shape,
      SymbolId source,
      std::vector<ParametricArgument> arguments) {
    const std::uint32_t index = static_cast<std::uint32_t>(
        semantic_.deferred_type_applications.size());
    const TypeId placeholder =
        semantic_.types.owner_evaluated_application(shape, index);
    semantic_.deferred_type_applications.push_back(
        {placeholder, source, std::move(arguments)});
    return placeholder;
  }

  [[nodiscard]] TypeId instantiate_deferred_type_application(
      TypeId source,
      const Type &value,
      const std::vector<ResolverTypeSubstitution> &substitutions,
      const std::vector<ResolverValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    if (value.deferred_type_application_index >=
        semantic_.deferred_type_applications.size()) {
      // Imported interfaces retain only the marker. Workspace orchestration
      // requests the enclosing public application from its owner and replaces
      // this provisional shape on the next clean consumer rebuild.
      return source;
    }
    const DeferredTypeApplication recipe =
        semantic_.deferred_type_applications[
            value.deferred_type_application_index];
    std::vector<ParametricArgument> arguments = recipe.arguments;
    const std::optional<bool> changed = substitute_parametric_arguments(
        arguments, substitutions, value_substitutions, use_range);
    if (!changed.has_value()) return semantic_.types.builtins().invalid;
    if (!*changed) return source;
    return instantiate_parametric_type(
        recipe.source, std::move(arguments), use_range);
  }

  [[nodiscard]] TypeId instantiate_deferred_element_count(
      const Type &source,
      TypeId element,
      const std::vector<ResolverTypeSubstitution> &substitutions,
      const std::vector<ResolverValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    if (source.deferred_element_count_index >=
        semantic_.deferred_element_counts.size()) {
      // An imported marker has no defining-package recipe. Workspace
      // orchestration replaces it with an owner-produced concrete graph before
      // a consumer may enter body checking. Preserve an explicitly unknown
      // placeholder in the provisional graph so the request can be collected
      // without inventing a zero-length layout or producing a user diagnostic.
      return source.kind == TypeKind::Array
          ? semantic_.types.owner_evaluated_array(element)
          : semantic_.types.owner_evaluated_simd(element);
    }

    // Copy before recursive substitution or interpretation: both operations may
    // append semantic rows and therefore invalidate vector element references.
    const DeferredElementCount recipe =
        semantic_.deferred_element_counts[
            source.deferred_element_count_index];
    std::vector<DeferredElementCountTypeBinding> type_bindings =
        recipe.type_bindings;
    std::vector<DeferredElementCountValueBinding> value_bindings =
        recipe.value_bindings;

    for (DeferredElementCountTypeBinding &binding : type_bindings) {
      binding.replacement = substitute_type(
          binding.replacement,
          substitutions,
          value_substitutions,
          use_range);
    }
    for (const ResolverTypeSubstitution &substitution : substitutions) {
      const bool present = std::any_of(
          type_bindings.begin(),
          type_bindings.end(),
          [&](const DeferredElementCountTypeBinding &binding) {
            return binding.parameter == substitution.parameter;
          });
      if (!present) {
        type_bindings.push_back(
            {substitution.parameter, substitution.replacement});
      }
    }

    const std::vector<IntegerExpressionReplacement> replacements =
        integer_expression_replacements(value_substitutions);
    for (DeferredElementCountValueBinding &binding : value_bindings) {
      if (!binding.symbolic_expression.is_valid()) continue;
      std::string error;
      const std::optional<IntegerExpression> replacement =
          substitute_integer_expression(
              binding.symbolic_expression, replacements, error);
      if (!replacement.has_value()) {
        diagnostics_.error(use_range, error);
        return semantic_.types.builtins().invalid;
      }
      binding.symbolic_expression = *replacement;
      if (!integer_expression_has_parameters(binding.symbolic_expression)) {
        const IntegerExpressionResult evaluated =
            evaluate_integer_expression(binding.symbolic_expression);
        if (!evaluated.ok) {
          diagnostics_.error(use_range, evaluated.error);
          return semantic_.types.builtins().invalid;
        }
        binding.value = ConstantValue::make_integer(evaluated.value);
        binding.symbolic_expression = {};
      }
    }
    for (const ResolverValueSubstitution &substitution : value_substitutions) {
      const SymbolId parameter{substitution.parameter};
      const bool present = std::any_of(
          value_bindings.begin(),
          value_bindings.end(),
          [&](const DeferredElementCountValueBinding &binding) {
            return binding.parameter == parameter;
          });
      if (present) continue;
      DeferredElementCountValueBinding binding;
      binding.parameter = parameter;
      binding.symbolic_expression = substitution.symbolic_expression;
      if (!binding.symbolic_expression.is_valid()) {
        binding.value = ConstantValue::make_integer(substitution.replacement);
      }
      value_bindings.push_back(std::move(binding));
    }

    bool symbolic = false;
    for (const DeferredElementCountTypeBinding &binding : type_bindings) {
      if (type_has_parameters(binding.replacement)) {
        symbolic = true;
        break;
      }
    }
    if (!symbolic) {
      for (const DeferredElementCountValueBinding &binding : value_bindings) {
        if (binding.symbolic_expression.is_valid()) {
          symbolic = true;
          break;
        }
      }
    }

    const SyntaxTree *tree = find_tree(recipe.syntax.file);
    if (tree == nullptr || !recipe.syntax.node.is_valid()) {
      diagnostics_.error(
          use_range, "owner-evaluated generic layout has no source recipe");
      return semantic_.types.builtins().invalid;
    }
    if (symbolic || target_ == nullptr) {
      return defer_element_count(
          source.kind,
          element,
          *tree,
          recipe.syntax.node,
          recipe.scope,
          std::move(type_bindings),
          std::move(value_bindings));
    }

    ConstantTable constants;
    for (const DeferredElementCountValueBinding &binding : value_bindings) {
      constants.bindings.push_back({binding.parameter, binding.value});
    }
    if (active_constants_ != nullptr) {
      for (const ConstantBinding &binding : active_constants_->bindings) {
        if (constants.find(binding.symbol) == nullptr) {
          constants.bindings.push_back(binding);
        }
      }
    }
    std::vector<ConstantTypeBinding> types;
    for (const DeferredElementCountTypeBinding &binding : type_bindings) {
      types.push_back({binding.parameter, binding.replacement});
    }
    if (active_types_ != nullptr) {
      for (const ConstantTypeBinding &binding : *active_types_) {
        const bool present = std::any_of(
            types.begin(),
            types.end(),
            [&](const ConstantTypeBinding &candidate) {
              return candidate.parameter == binding.parameter;
            });
        if (!present) types.push_back(binding);
      }
    }

    const std::optional<EvaluatedConstant> evaluated =
        evaluate_typed_constant_expression(
            sources_,
            loaded_,
            semantic_,
            *target_,
            *tree,
            recipe.syntax.node,
            recipe.scope,
            diagnostics_,
            &constants,
            &types,
            semantic_.types.builtins().usize_type);
    if (!evaluated.has_value() ||
        evaluated->value.kind != ConstantKind::Integer) {
      return semantic_.types.builtins().invalid;
    }
    const TypeKind result_kind = evaluated->type.is_valid()
        ? semantic_.types.type(evaluated->type).kind
        : TypeKind::Invalid;
    if (evaluated->type != semantic_.types.builtins().usize_type &&
        result_kind != TypeKind::UntypedInteger) {
      diagnostics_.error(
          tree->node(recipe.syntax.node).range,
          source.kind == TypeKind::Array
              ? "array length must have type 'usize'"
              : "SIMD lane count must have type 'usize'");
      return semantic_.types.builtins().invalid;
    }
    const std::optional<std::uint64_t> count =
        evaluated->value.integer.to_u64();
    if (!count.has_value() || *count == 0) {
      diagnostics_.error(
          tree->node(recipe.syntax.node).range,
          source.kind == TypeKind::Array
              ? "array length must be a nonzero compile-time usize"
              : "SIMD lane count must be a nonzero compile-time usize");
      return semantic_.types.builtins().invalid;
    }
    return source.kind == TypeKind::Array
        ? semantic_.types.array(element, *count)
        : semantic_.types.simd(element, *count, use_range);
  }

  [[nodiscard]] TypeId substitute_type(
      TypeId source,
      const std::vector<ResolverTypeSubstitution> &substitutions,
      const std::vector<ResolverValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    for (const ResolverTypeSubstitution &substitution : substitutions) {
      if (substitution.parameter == source) return substitution.replacement;
    }
    if (!source.is_valid()) return source;
    const Type value = semantic_.types.type(source);
    if (value.owner_evaluated_type_application) {
      return instantiate_deferred_type_application(
          source,
          value,
          substitutions,
          value_substitutions,
          use_range);
    }

    // Nominal applications keep their identity at the template boundary. A
    // member such as `Dynamic[T]` inside `Map[T, V]` must become the canonical
    // `Dynamic[u64]` application when Map is instantiated; substituting the
    // aggregate's already-laid-out members would leave Dynamic's own T behind.
    if (value.kind == TypeKind::Struct || value.kind == TypeKind::Enum ||
        value.kind == TypeKind::TaggedUnion ||
        value.kind == TypeKind::RawUnion) {
      std::optional<SymbolId> template_source;
      std::vector<ParametricArgument> arguments;
      std::optional<ImportedType> imported_application;
      for (const ParametricTypeInstanceRecord &instance :
           semantic_.parametric_type_instances) {
        if (semantic_.symbols.symbol(instance.instance).type != source) continue;
        template_source = instance.source;
        arguments = instance.arguments;
        break;
      }
      if (arguments.empty()) {
        for (const ImportedType &imported : semantic_.imported_types) {
          if (imported.type != source || imported.arguments.empty()) continue;
          imported_application = imported;
          arguments = imported.arguments;
          break;
        }
      }
      if (!arguments.empty()) {
        const std::optional<bool> changed = substitute_parametric_arguments(
            arguments, substitutions, value_substitutions, use_range);
        if (!changed.has_value()) return semantic_.types.builtins().invalid;
        if (!*changed) return source;

        if (!template_source.has_value() && imported_application.has_value()) {
          for (const ImportedSymbol &imported : semantic_.imported_symbols) {
            if (imported.root_identity !=
                    imported_application->root_identity ||
                imported.root_relative_path !=
                    imported_application->root_relative_path ||
                imported.public_name != imported_application->public_name) {
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
          if (imported_application.has_value()) {
            return instantiate_transitive_nominal(
                source,
                *imported_application,
                std::move(arguments),
                substitutions,
                value_substitutions,
                use_range);
          }
          diagnostics_.error(
              use_range,
              "cannot recover the template declaration for nominal substitution");
          return semantic_.types.builtins().invalid;
        }
        return instantiate_parametric_type(
            *template_source, std::move(arguments), use_range);
      }
    }

    switch (value.kind) {
    case TypeKind::Pointer:
      return semantic_.types.pointer(substitute_type(
          value.element, substitutions, value_substitutions, use_range));
    case TypeKind::MultiPointer:
      return semantic_.types.multi_pointer(substitute_type(
          value.element, substitutions, value_substitutions, use_range));
    case TypeKind::Slice:
      return semantic_.types.slice(substitute_type(
          value.element, substitutions, value_substitutions, use_range));
    case TypeKind::Array:
    case TypeKind::Simd: {
      const TypeId element = substitute_type(
          value.element, substitutions, value_substitutions, use_range);
      if (value.owner_evaluated_element_count) {
        return instantiate_deferred_element_count(
            value,
            element,
            substitutions,
            value_substitutions,
            use_range);
      }
      std::uint64_t count = value.element_count;
      if (value.element_count_expression.is_valid()) {
        std::string error;
        const std::optional<IntegerExpression> replacement =
            substitute_integer_expression(
                value.element_count_expression,
                integer_expression_replacements(value_substitutions),
                error);
        if (!replacement.has_value()) {
          diagnostics_.error(use_range, error);
          return semantic_.types.builtins().invalid;
        }
        if (integer_expression_has_parameters(*replacement)) {
          return value.kind == TypeKind::Array
              ? semantic_.types.parametric_array(element, *replacement)
              : semantic_.types.parametric_simd(element, *replacement);
        }
        const IntegerExpressionResult evaluated =
            evaluate_integer_expression(*replacement);
        const std::optional<std::uint64_t> concrete =
            evaluated.ok ? evaluated.value.to_u64() : std::nullopt;
        if (!evaluated.ok || !concrete.has_value() || *concrete == 0) {
          diagnostics_.error(
              use_range,
              evaluated.ok
                  ? "array and SIMD value expressions must instantiate to a nonzero u64"
                  : evaluated.error);
          return semantic_.types.builtins().invalid;
        }
        count = *concrete;
      }
      return value.kind == TypeKind::Array
          ? semantic_.types.array(element, count)
          : semantic_.types.simd(element, count, use_range);
    }
    case TypeKind::Tuple: {
      std::vector<TypeId> members;
      members.reserve(value.members.size());
      for (TypeId member : value.members) {
        members.push_back(substitute_type(
            member, substitutions, value_substitutions, use_range));
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
              substitutions,
              value_substitutions,
              use_range));
        }
      }
      const TypeId result = value.members.empty()
          ? semantic_.types.builtins().void_type
          : substitute_type(
                value.members.back(),
                substitutions,
                value_substitutions,
                use_range);
      return semantic_.types.procedure(
          parameters, result, value.c_calling_convention);
    }
    default:
      return source;
    }
  }

  // A dependency can expose `array.Dynamic[T]` only as a member of its own
  // public `Map[T, V]` without re-exporting array.Dynamic's declaration. The
  // interface still carries Dynamic's stable origin, symbolic arguments,
  // complete layout, and member packet. That is sufficient to specialize the
  // imported application directly while preserving its original nominal
  // identity; requiring an unrelated direct import would make interfaces
  // semantically incomplete.
  [[nodiscard]] TypeId instantiate_transitive_nominal(
      TypeId pattern,
      const ImportedType &origin,
      std::vector<ParametricArgument> arguments,
      const std::vector<ResolverTypeSubstitution> &substitutions,
      const std::vector<ResolverValueSubstitution> &value_substitutions,
      SourceRange use_range) {
    const Type pattern_type = semantic_.types.type(pattern);
    for (const ImportedType &existing : semantic_.imported_types) {
      if (existing.root_identity == origin.root_identity &&
          existing.root_relative_path == origin.root_relative_path &&
          existing.public_name == origin.public_name &&
          existing.arguments == arguments && existing.type.is_valid() &&
          semantic_.types.type(existing.type).kind == pattern_type.kind) {
        return existing.type;
      }
    }

    std::string instance_name = origin.public_name + "$transitive_instance";
    for (const ParametricArgument &argument : arguments) {
      if (argument.is_type) {
        instance_name += "$t" + std::to_string(argument.type.value);
      } else if (argument.owner_evaluated_value) {
        instance_name += "$owner" +
            std::to_string(argument.deferred_value_index);
      } else if (argument.value_expression.is_valid()) {
        instance_name += "$e" +
            integer_expression_identity(argument.value_expression);
      } else {
        instance_name += "$v" + argument.value.integer.to_decimal();
      }
    }
    const TypeId concrete = semantic_.types.begin_nominal(
        pattern_type.kind, instance_name, use_range);
    semantic_.types.type_mut(concrete).c_representation =
        pattern_type.c_representation;
    semantic_.types.type_mut(concrete).requested_alignment =
        pattern_type.requested_alignment;

    Symbol owner;
    owner.name = "$transitive_nominal$" + std::to_string(concrete.value);
    owner.kind = SymbolKind::Type;
    owner.visibility = Visibility::Private;
    owner.scope = semantic_.package_scope;
    owner.type = concrete;
    owner.name_range = use_range;
    const SymbolId owner_id =
        semantic_.symbols.declare(std::move(owner), diagnostics_);
    if (!owner_id.is_valid()) return semantic_.types.builtins().invalid;
    const ScopeId member_scope = semantic_.symbols.add_scope(
        ScopeKind::Type, semantic_.package_scope, use_range);
    semantic_.owned_scopes.push_back({owner_id, member_scope});

    // Install provenance before following member types. A recursive member
    // such as `^Node[T]` then finds this in-progress TypeId instead of starting
    // a second specialization of the same nominal application.
    semantic_.imported_types.push_back({
        concrete,
        origin.root_identity,
        origin.root_relative_path,
        origin.public_name,
        arguments,
    });

    std::vector<SymbolId> template_members;
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner == owner_id ||
          semantic_.symbols.scope(owned.scope).kind != ScopeKind::Type ||
          semantic_.symbols.symbol(owned.owner).type != pattern) {
        continue;
      }
      template_members = semantic_.symbols.scope(owned.scope).symbols;
      break;
    }

    MemberData data;
    for (SymbolId template_member : template_members) {
      Symbol concrete_member = semantic_.symbols.symbol(template_member);
      concrete_member.scope = member_scope;
      concrete_member.type = substitute_type(
          concrete_member.type,
          substitutions,
          value_substitutions,
          use_range);
      const SymbolId member_id =
          semantic_.symbols.declare(std::move(concrete_member), diagnostics_);
      if (!member_id.is_valid()) continue;
      data.symbols.push_back(member_id);
      data.types.push_back(semantic_.symbols.symbol(member_id).type);
      data.offsets.push_back(0);
      if (pattern_type.kind == TypeKind::Enum) {
        std::optional<BigInteger> enum_value;
        for (const EnumMemberValue &value : semantic_.enum_member_values) {
          if (value.member != template_member) continue;
          enum_value = value.value;
          break;
        }
        if (enum_value.has_value()) {
          data.enum_values.push_back(*enum_value);
          semantic_.enum_member_values.push_back(
              {member_id, std::move(*enum_value)});
        }
      }
    }

    TypeId element = substitute_type(
        pattern_type.element,
        substitutions,
        value_substitutions,
        use_range);
    semantic_.types.type_mut(concrete).element = element;
    TypeLayout layout;
    if (pattern_type.kind == TypeKind::Struct) {
      layout = struct_layout(data);
    } else if (pattern_type.kind == TypeKind::RawUnion) {
      layout = raw_union_layout(data);
    } else if (pattern_type.kind == TypeKind::Enum) {
      layout = semantic_.types.type(element).layout;
    } else {
      layout = tagged_union_layout(element, data);
    }
    layout = apply_requested_alignment(
        layout, pattern_type.requested_alignment, use_range);
    semantic_.types.complete_nominal(
        concrete, layout, data.types, data.offsets);
    for (std::size_t index = 0; index < data.symbols.size(); ++index) {
      semantic_.aggregate_members.push_back(
          {owner_id, data.symbols[index], data.offsets[index]});
    }
    return concrete;
  }

  [[nodiscard]] std::optional<SymbolId> type_symbol_in_span(
      const SyntaxTree &tree,
      std::uint32_t begin,
      std::uint32_t end,
      ScopeId scope) {
    const std::vector<SourceName> names = names_in_span(tree, begin, end);
    if (names.size() == 1) {
      const std::optional<SymbolId> found =
          semantic_.symbols.lookup(scope, names.front().text);
      if (found.has_value() &&
          static_cast<std::size_t>(found->value) < states_.size()) {
        const Symbol &symbol = semantic_.symbols.symbol(*found);
        if (symbol.kind == SymbolKind::UnresolvedDeclaration ||
            (symbol.flags.parametric &&
             states_[found->value] == ResolutionState::Unvisited &&
             !owned_scope(*found, ScopeKind::Parametric).has_value())) {
          resolve_symbol(*found);
        }
      }
      return found;
    }
    if (names.size() != 2) return std::nullopt;
    const std::optional<SymbolId> import =
        semantic_.symbols.lookup(scope, names.front().text);
    if (!import.has_value() ||
        semantic_.symbols.symbol(*import).kind != SymbolKind::Import) {
      return std::nullopt;
    }
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner == *import &&
          semantic_.symbols.scope(owned.scope).kind ==
              ScopeKind::ImportedPackage) {
        return semantic_.symbols.lookup_direct(owned.scope, names.back().text);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] TypeId resolve_type_argument(
      const SyntaxTree &tree, NodeId argument, ScopeId scope) {
    const SyntaxNode &node = tree.node(argument);
    if (node_is_type_syntax(node.kind) ||
        node.kind == NodeKind::BracketExpression) {
      return resolve_type(tree, argument, scope);
    }
    if (const std::optional<TypeId> named = try_named_type(tree, node, scope)) {
      return *named;
    }
    if (const std::optional<TypeId> imported =
            try_imported_type(tree, node, scope)) {
      return *imported;
    }
    diagnostics_.error(node.range, "parametric type argument does not denote a type");
    return semantic_.types.builtins().invalid;
  }

  [[nodiscard]] TypeId instantiate_parametric_type(
      SymbolId source,
      std::vector<ParametricArgument> arguments,
      SourceRange use_range) {
    const Symbol template_symbol = semantic_.symbols.symbol(source);
    const std::vector<ParametricParameterRecord> parameters =
        parameters_for(source);
    if (parameters.size() != arguments.size()) {
      diagnostics_.error(
          use_range, "parametric type application has the wrong number of arguments");
      return semantic_.types.builtins().invalid;
    }

    std::vector<ResolverTypeSubstitution> substitutions;
    std::vector<ResolverValueSubstitution> value_substitutions;
    bool has_owner_evaluated_argument = false;
    substitutions.reserve(parameters.size());
    value_substitutions.reserve(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      const ParametricParameterRecord &parameter = parameters[index];
      if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
        if (arguments[index].is_type) {
          diagnostics_.error(
              use_range, "value parameter requires a compile-time integer argument");
          return semantic_.types.builtins().invalid;
        }
        const TypeId required_type =
            semantic_.symbols.symbol(parameter.parameter).type;
        if (arguments[index].owner_evaluated_value) {
          if (arguments[index].value_type.is_valid() &&
              arguments[index].value_type != required_type) {
            diagnostics_.error(
                use_range,
                "owner-evaluated value argument has the wrong result type");
            return semantic_.types.builtins().invalid;
          }
          arguments[index].value_type = required_type;
          has_owner_evaluated_argument = true;
          continue;
        }
        if (arguments[index].value_expression.is_valid()) {
          const IntegerExpressionNode &root =
              arguments[index].value_expression.nodes[
                  arguments[index].value_expression.root];
          if (root.type != integer_expression_type(required_type)) {
            diagnostics_.error(
                use_range,
                "symbolic type value argument has the wrong result type");
            return semantic_.types.builtins().invalid;
          }
          for (const IntegerExpressionNode &node :
               arguments[index].value_expression.nodes) {
            if (node.operation != IntegerExpressionOperation::Parameter) {
              continue;
            }
            if (node.parameter >= semantic_.symbols.symbol_count()) {
              diagnostics_.error(
                  use_range,
                  "symbolic type value argument names an invalid parameter");
              return semantic_.types.builtins().invalid;
            }
            const Symbol &supplied =
                semantic_.symbols.symbol(SymbolId{node.parameter});
            if (supplied.kind != SymbolKind::ValueParameter) {
              diagnostics_.error(
                  use_range,
                  "symbolic type value argument names a non-parameter value");
              return semantic_.types.builtins().invalid;
            }
          }
          arguments[index].value_type = required_type;
          value_substitutions.push_back({
              parameter.parameter.value,
              {},
              arguments[index].value_expression,
          });
          continue;
        }
        if (arguments[index].value.kind != ConstantKind::Integer) {
          diagnostics_.error(
              use_range, "value parameter requires a compile-time integer argument");
          return semantic_.types.builtins().invalid;
        }
        if (!semantic_.types.is_integer(required_type) ||
            !integer_fits_type(arguments[index].value.integer, required_type)) {
          diagnostics_.error(
              use_range,
              "compile-time value argument is not representable in its parameter type");
          return semantic_.types.builtins().invalid;
        }
        arguments[index].value_type = required_type;
        value_substitutions.push_back({
            parameter.parameter.value,
            arguments[index].value.integer,
            {},
        });
        continue;
      }
      if (!arguments[index].is_type ||
          !type_satisfies_constraint(arguments[index].type, parameter.constraint)) {
        diagnostics_.error(
            use_range,
            "type argument does not satisfy its parametric constraint");
        return semantic_.types.builtins().invalid;
      }
      substitutions.push_back({
          semantic_.symbols.symbol(parameter.parameter).type,
          arguments[index].type,
      });
    }

    for (const ParametricTypeInstanceRecord &instance :
         semantic_.parametric_type_instances) {
      if (instance.source == source && instance.arguments == arguments) {
        return semantic_.symbols.symbol(instance.instance).type;
      }
    }

    // A concrete specialization may already have arrived in the dependency's
    // interface (for example Key_Ops[string] as another procedure's result).
    // Reuse that consumer-local TypeId instead of creating a second nominal
    // type for the same public template identity and arguments.
    std::optional<ImportedSymbol> imported_origin;
    for (const ImportedSymbol &origin : semantic_.imported_symbols) {
      if (origin.proxy != source) continue;
      imported_origin = origin;
      for (const ImportedType &imported : semantic_.imported_types) {
        if (imported.root_identity == origin.root_identity &&
            imported.root_relative_path == origin.root_relative_path &&
            imported.public_name == origin.public_name &&
            imported.arguments == arguments) {
          return imported.type;
        }
      }
      break;
    }

    const Type template_type = semantic_.types.type(template_symbol.type);
    const bool concrete_arguments = std::none_of(
        arguments.begin(),
        arguments.end(),
        [&](const ParametricArgument &argument) {
          if (argument.is_type) return type_has_parameters(argument.type);
          return argument.owner_evaluated_value ||
              argument.value_expression.is_valid() ||
              argument.value.kind != ConstantKind::Integer;
        });
    if (imported_origin.has_value() && concrete_arguments &&
        type_requires_owner_evaluation(template_symbol.type)) {
      bool already_requested = false;
      for (const ImportedTypeInstantiationRequest &request :
           semantic_.imported_type_instantiation_requests) {
        if (request.source_proxy == source && request.arguments == arguments) {
          already_requested = true;
          break;
        }
      }
      if (!already_requested) {
        semantic_.imported_type_instantiation_requests.push_back({
            source,
            imported_origin->root_identity,
            imported_origin->root_relative_path,
            imported_origin->public_name,
            arguments,
        });
      }
    }
    if (template_type.kind != TypeKind::Struct &&
        template_type.kind != TypeKind::Enum &&
        template_type.kind != TypeKind::TaggedUnion &&
        template_type.kind != TypeKind::RawUnion) {
      if (has_owner_evaluated_argument) {
        // Structural aliases intentionally have no nominal instance identity.
        // Retain this particular symbolic application in a non-interned shape
        // row so outer substitution can evaluate its full value recipe, then
        // return the ordinary canonical structural TypeId.
        return defer_type_application(
            template_symbol.type, source, std::move(arguments));
      }
      // Parametric aliases are purely structural and therefore need no member
      // scope or nominal instance identity.
      return substitute_type(
          template_symbol.type, substitutions, value_substitutions, use_range);
    }

    std::string instance_name = template_symbol.name + "$instance";
    for (const ParametricArgument &argument : arguments) {
      if (argument.is_type) {
        instance_name += "$t" + std::to_string(argument.type.value);
      } else if (argument.owner_evaluated_value) {
        instance_name += "$owner" +
            std::to_string(argument.deferred_value_index);
      } else if (argument.value_expression.is_valid()) {
        instance_name += "$e" +
            integer_expression_identity(argument.value_expression);
      } else {
        instance_name += "$v" + argument.value.integer.to_decimal();
      }
    }
    Symbol instance_symbol = template_symbol;
    instance_symbol.name = instance_name;
    instance_symbol.visibility = Visibility::Private;
    instance_symbol.flags.parametric = false;
    instance_symbol.type = {};
    instance_symbol.name_range = use_range;
    const SymbolId instance_id =
        semantic_.symbols.declare(std::move(instance_symbol), diagnostics_);
    if (!instance_id.is_valid()) return semantic_.types.builtins().invalid;

    const TypeId concrete = semantic_.types.begin_nominal(
        template_type.kind, instance_name, use_range);
    semantic_.types.type_mut(concrete).c_representation =
        template_type.c_representation;
    semantic_.types.type_mut(concrete).requested_alignment =
        template_type.requested_alignment;
    semantic_.symbols.symbol_mut(instance_id).type = concrete;
    const ScopeId member_scope = semantic_.symbols.add_scope(
        ScopeKind::Type, template_symbol.scope, use_range);
    semantic_.owned_scopes.push_back({instance_id, member_scope});

    MemberData data;
    // Substituting one member may instantiate another nominal application and
    // append aggregate rows. Snapshot this template's stable IDs before that
    // recursive work so vector growth cannot invalidate the active row.
    std::vector<AggregateMember> template_members;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner == source) template_members.push_back(member);
    }
    for (const AggregateMember &member : template_members) {
      Symbol concrete_member = semantic_.symbols.symbol(member.member);
      const SymbolId template_member = member.member;
      concrete_member.scope = member_scope;
      // An owner-evaluated nominal argument makes this entire instance a
      // symbolic placeholder. Preserve the template member packet unchanged;
      // partially substituting it could accidentally evaluate a nested recipe
      // with one of the callee's value parameters still unbound. Once the outer
      // recipe becomes concrete, a new ordinary instance is built here.
      if (!has_owner_evaluated_argument) {
        concrete_member.type = substitute_type(
            concrete_member.type,
            substitutions,
            value_substitutions,
            use_range);
      }
      const SymbolId member_id =
          semantic_.symbols.declare(std::move(concrete_member), diagnostics_);
      if (!member_id.is_valid()) continue;
      data.symbols.push_back(member_id);
      data.types.push_back(semantic_.symbols.symbol(member_id).type);
      data.offsets.push_back(0);
      if (template_type.kind == TypeKind::Enum) {
        std::optional<BigInteger> concrete_value;
        for (const EnumMemberValue &value : semantic_.enum_member_values) {
          if (value.member == template_member) {
            concrete_value = value.value;
            break;
          }
        }
        if (concrete_value.has_value()) {
          data.enum_values.push_back(*concrete_value);
          semantic_.enum_member_values.push_back(
              {member_id, std::move(*concrete_value)});
        }
      }
    }

    TypeLayout layout;
    TypeId element = template_type.element;
    if (!has_owner_evaluated_argument) {
      element = substitute_type(
          template_type.element,
          substitutions,
          value_substitutions,
          use_range);
    }
    semantic_.types.type_mut(concrete).element = element;
    if (has_owner_evaluated_argument) {
      // The member packet remains useful for symbolic checking, but no physical
      // layout exists until the defining package evaluates the argument.
      layout = {};
    } else if (template_type.kind == TypeKind::Struct) {
      layout = struct_layout(data);
    } else if (template_type.kind == TypeKind::RawUnion) {
      layout = raw_union_layout(data);
    } else if (template_type.kind == TypeKind::Enum) {
      layout = semantic_.types.type(element).layout;
    } else {
      layout = tagged_union_layout(element, data);
    }
    layout = apply_requested_alignment(
        layout, template_type.requested_alignment, use_range);
    semantic_.types.complete_nominal(
        concrete, layout, data.types, data.offsets);
    for (std::size_t index = 0; index < data.symbols.size(); ++index) {
      semantic_.aggregate_members.push_back(
          {instance_id, data.symbols[index], data.offsets[index]});
    }
    semantic_.parametric_type_instances.push_back(
        {source, instance_id, std::move(arguments)});
    return concrete;
  }

  [[nodiscard]] TypeId resolve_parametric_type_application(
      const SyntaxTree &tree,
      std::uint32_t base_begin,
      std::uint32_t base_end,
      const std::vector<NodeId> &argument_nodes,
      ScopeId scope,
      SourceRange range) {
    const std::optional<SymbolId> found =
        type_symbol_in_span(tree, base_begin, base_end, scope);
    if (!found.has_value()) {
      diagnostics_.error(range, "unknown parametric type name");
      return semantic_.types.builtins().invalid;
    }
    const Symbol symbol = semantic_.symbols.symbol(*found);
    if (symbol.kind != SymbolKind::Type || !symbol.flags.parametric) {
      diagnostics_.error(range, "type is not parametric");
      return semantic_.types.builtins().invalid;
    }
    const std::vector<ParametricParameterRecord> parameters =
        parameters_for(*found);
    if (parameters.size() != argument_nodes.size()) {
      diagnostics_.error(
          range, "parametric type application has the wrong number of arguments");
      return semantic_.types.builtins().invalid;
    }
    std::vector<ParametricArgument> arguments;
    arguments.reserve(argument_nodes.size());
    for (std::size_t index = 0; index < argument_nodes.size(); ++index) {
      const NodeId argument_node = argument_nodes[index];
      const ParametricParameterRecord &parameter = parameters[index];
      if (parameter.constraint != TypeConstraintKind::CompileTimeValue) {
        ParametricArgument argument;
        argument.type = resolve_type_argument(tree, argument_node, scope);
        arguments.push_back(std::move(argument));
        continue;
      }
      const TypeId required =
          semantic_.symbols.symbol(parameter.parameter).type;
      std::optional<BigInteger> value;
      std::optional<IntegerExpressionType> supplied_type;
      if (const ResolvedIntegerExpression *resolved =
              resolved_integer(tree, argument_node)) {
        value = resolved->value;
        if (!resolved->type.has_value()) {
          diagnostics_.error(
              tree.node(argument_node).range,
              "compile-time value argument must have an integer type");
          return semantic_.types.builtins().invalid;
        }
        supplied_type = *resolved->type;
      } else {
        // The typed builder covers literals, names, arithmetic, and casts. It
        // lets this value-parameter boundary distinguish same-width concrete
        // integer identities. The layout-local evaluator handles enum members
        // and the early-layout recovery cases that have no canonical tree yet.
        IntegerExpression built;
        const BuiltIntegerExpressionNode root = build_integer_expression_node(
            tree, argument_node, scope, built);
        if (root.valid && root.constant.has_value()) {
          value = root.constant;
          supplied_type = integer_expression_type(root.type);
        } else {
          value = integer_constant_expression(tree, argument_node, scope);
        }
      }
      if (!value.has_value()) {
        const std::optional<IntegerExpression> symbolic =
            dependent_integer_expression(
                tree, argument_node, scope, required);
        if (!symbolic.has_value()) {
          if (expression_references_parametric_parameter(
                  tree, argument_node, scope)) {
            arguments.push_back(defer_value_expression(
                tree, argument_node, scope, required));
            continue;
          }
          require_integer_expression(tree, argument_node, scope, required);
          if (integer_synthesis_is_blocked(tree, argument_node)) {
            return semantic_.types.builtins().invalid;
          }
          diagnostics_.error(
              tree.node(argument_node).range,
              "value parameter argument must be a compile-time integer expression");
          return semantic_.types.builtins().invalid;
        }
        ParametricArgument argument;
        argument.is_type = false;
        argument.value_type = required;
        argument.value_expression = *symbolic;
        arguments.push_back(std::move(argument));
        continue;
      }
      const IntegerExpressionType expected_type =
          integer_expression_type(required);
      if (supplied_type.has_value() &&
          supplied_type->representation !=
              IntegerExpressionRepresentation::Untyped &&
          *supplied_type != expected_type) {
        diagnostics_.error(
            tree.node(argument_node).range,
            "compile-time value argument has the wrong concrete integer type");
        return semantic_.types.builtins().invalid;
      }
      ParametricArgument argument;
      argument.is_type = false;
      argument.value_type =
          semantic_.symbols.symbol(parameter.parameter).type;
      argument.value = ConstantValue::make_integer(*value);
      arguments.push_back(std::move(argument));
    }
    return instantiate_parametric_type(*found, std::move(arguments), range);
  }

  // Returns a single unqualified, unapplied name. Parenthesized grouping and
  // parametric/qualified names deliberately fail this narrow predicate.
  [[nodiscard]] std::optional<SourceName> simple_type_name(
      const SyntaxTree &tree, const SyntaxNode &node) const {
    const std::vector<SourceName> names = names_in_span(
        tree, node.token_begin, node.token_end);
    if (names.size() != 1) {
      return std::nullopt;
    }
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      if (tree.token(index).kind == TokenKind::Dot ||
          tree.token(index).kind == TokenKind::LeftBracket) {
        return std::nullopt;
      }
    }
    return names.front();
  }

  // Attempts a builtin, type-parameter, or package-local type lookup without
  // diagnosing failure. This is used to disambiguate `Alias :: Existing_Name`
  // from an ordinary constant whose value is another constant.
  [[nodiscard]] std::optional<TypeId> try_named_type(
      const SyntaxTree &tree, const SyntaxNode &node, ScopeId scope) {
    const std::optional<SourceName> name = simple_type_name(tree, node);
    if (!name.has_value()) {
      return std::nullopt;
    }
    if (const std::optional<TypeId> builtin = semantic_.types.find_builtin(name->text)) {
      return *builtin;
    }
    const std::optional<SymbolId> found = semantic_.symbols.lookup(scope, name->text);
    if (!found.has_value()) {
      return std::nullopt;
    }
    if (semantic_.symbols.symbol(*found).kind == SymbolKind::UnresolvedDeclaration) {
      resolve_symbol(*found);
    }
    const Symbol &symbol = semantic_.symbols.symbol(*found);
    if ((symbol.kind == SymbolKind::Type || symbol.kind == SymbolKind::TypeParameter) &&
        symbol.type.is_valid()) {
      return active_type(symbol.type);
    }
    return std::nullopt;
  }

  // Resolves `alias.Public_Type` through the ImportedPackage scope installed by
  // interface binding. The proxy's TypeId belongs to this package's TypeStore;
  // no dependency-local integer ID crosses the interface boundary.
  [[nodiscard]] std::optional<TypeId> try_imported_type(
      const SyntaxTree &tree, const SyntaxNode &node, ScopeId scope) {
    const std::vector<SourceName> names = names_in_span(
        tree, node.token_begin, node.token_end);
    if (names.size() != 2) {
      return std::nullopt;
    }
    const std::optional<SymbolId> first = semantic_.symbols.lookup(scope, names.front().text);
    if (!first.has_value() ||
        semantic_.symbols.symbol(*first).kind != SymbolKind::Import) {
      return std::nullopt;
    }
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner != *first ||
          semantic_.symbols.scope(owned.scope).kind != ScopeKind::ImportedPackage) {
        continue;
      }
      const std::optional<SymbolId> member = semantic_.symbols.lookup_direct(
          owned.scope, names.back().text);
      if (!member.has_value()) {
        diagnostics_.error(
            names.back().range,
            "imported package has no public type named '" + names.back().text + "'");
        return semantic_.types.builtins().invalid;
      }
      const Symbol &symbol = semantic_.symbols.symbol(*member);
      if (symbol.kind != SymbolKind::Type && symbol.kind != SymbolKind::TypeParameter) {
        diagnostics_.error(names.back().range, "imported name does not denote a type");
        return semantic_.types.builtins().invalid;
      }
      return active_type(symbol.type);
    }
    diagnostics_.error(names.front().range, "imported package interface is unavailable");
    return semantic_.types.builtins().invalid;
  }

  // Tries the expression-grammar forms that can also denote type values. The
  // probe is silent when the base name is an ordinary value, allowing `::`
  // constants such as `Copy :: Existing_Constant` and `Item :: values[0]` to
  // remain values. Once a parametric type base is identified, malformed
  // arguments are real type diagnostics and resolve_type owns their reporting.
  [[nodiscard]] std::optional<TypeId> try_type_value(
      const SyntaxTree &tree, NodeId node_id, ScopeId scope) {
    const SyntaxNode &node = tree.node(node_id);
    if (node_is_type_syntax(node.kind)) {
      return resolve_type(tree, node_id, scope);
    }
    if (node.kind == NodeKind::NameExpression) {
      return try_named_type(tree, node, scope);
    }
    if (node.kind == NodeKind::MemberExpression) {
      const std::optional<SymbolId> symbol = type_symbol_in_span(
          tree, node.token_begin, node.token_end, scope);
      if (!symbol.has_value()) return std::nullopt;
      const Symbol &binding = semantic_.symbols.symbol(*symbol);
      if ((binding.kind == SymbolKind::Type ||
           binding.kind == SymbolKind::TypeParameter) &&
          binding.type.is_valid()) {
        return active_type(binding.type);
      }
      return std::nullopt;
    }
    if (node.kind == NodeKind::BracketExpression &&
        !node.children.empty()) {
      const SyntaxNode &base = tree.node(node.children.front());
      const std::optional<SymbolId> symbol = type_symbol_in_span(
          tree, base.token_begin, base.token_end, scope);
      if (!symbol.has_value()) return std::nullopt;
      const Symbol &binding = semantic_.symbols.symbol(*symbol);
      if (binding.kind != SymbolKind::Type || !binding.flags.parametric) {
        return std::nullopt;
      }
      return resolve_type(tree, node_id, scope);
    }
    if (node.kind == NodeKind::GroupExpression && node.children.size() == 1) {
      return try_type_value(tree, node.children.front(), scope);
    }
    if (node.kind == NodeKind::TupleExpression) {
      std::vector<TypeId> members;
      members.reserve(node.children.size());
      for (NodeId child : node.children) {
        const std::optional<TypeId> member =
            try_type_value(tree, child, scope);
        if (!member.has_value()) return std::nullopt;
        members.push_back(*member);
      }
      if (members.size() < 2) return std::nullopt;
      return semantic_.types.tuple(members);
    }
    return std::nullopt;
  }

  // Recursively lowers one already-parsed type syntax node into a canonical
  // TypeId. User errors return the canonical invalid type and analysis continues.
  [[nodiscard]] TypeId resolve_type(
      const SyntaxTree &tree, NodeId type_id, ScopeId scope) {
    const SyntaxNode &node = tree.node(type_id);
    const TypeId invalid = semantic_.types.builtins().invalid;
    const bool has_attributes = node.token_begin < node.token_end &&
        tree.token(node.token_begin).kind == TokenKind::At;
    const bool aggregate = node.kind == NodeKind::StructType ||
        node.kind == NodeKind::EnumType ||
        node.kind == NodeKind::TaggedUnionType ||
        node.kind == NodeKind::RawUnionType;
    if (has_attributes && !aggregate) {
      diagnostics_.error(
          node.range,
          "representation attributes are valid only on aggregate type constructors");
      return invalid;
    }
    switch (node.kind) {
    case NodeKind::NamedType: {
      for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
        if (tree.token(index).kind == TokenKind::LeftBracket) {
          return resolve_parametric_type_application(
              tree,
              node.token_begin,
              index,
              node.children,
              scope,
              node.range);
        }
      }
      // Parenthesized grouping retains the inner type as a child.
      for (NodeId child : node.children) {
        if (node_is_type_syntax(tree.node(child).kind)) {
          return resolve_type(tree, child, scope);
        }
      }
      if (const std::optional<SourceName> name = simple_type_name(tree, node)) {
        const std::optional<SymbolId> found =
            semantic_.symbols.lookup(scope, name->text);
        if (found.has_value() &&
            semantic_.symbols.symbol(*found).flags.parametric) {
          diagnostics_.error(
              node.range, "parametric type requires explicit type arguments");
          return invalid;
        }
      }
      if (const std::optional<TypeId> type = try_named_type(tree, node, scope)) {
        return *type;
      }
      if (const std::optional<TypeId> type = try_imported_type(tree, node, scope)) {
        return *type;
      }
      diagnostics_.error(node.range, "unknown type name");
      return invalid;
    }

    case NodeKind::BracketExpression: {
      if (node.children.empty()) return invalid;
      const SyntaxNode &base = tree.node(node.children.front());
      std::vector<NodeId> arguments;
      for (std::size_t index = 1; index < node.children.size(); ++index) {
        arguments.push_back(node.children[index]);
      }
      return resolve_parametric_type_application(
          tree,
          base.token_begin,
          base.token_end,
          arguments,
          scope,
          node.range);
    }

    case NodeKind::PointerType:
      if (!node.children.empty()) {
        return semantic_.types.pointer(resolve_type(tree, node.children.back(), scope));
      }
      return invalid;

    case NodeKind::MultiPointerType:
      if (!node.children.empty()) {
        return semantic_.types.multi_pointer(resolve_type(tree, node.children.back(), scope));
      }
      return invalid;

    case NodeKind::SliceType:
      if (!node.children.empty()) {
        return semantic_.types.slice(resolve_type(tree, node.children.back(), scope));
      }
      return invalid;

    case NodeKind::ArrayType: {
      if (node.children.size() < 2) {
        return invalid;
      }
      const TypeId element =
          resolve_type(tree, node.children.back(), scope);
      const std::optional<std::uint64_t> count =
          layout_integer(tree, node.children.front(), scope);
      if (count.has_value() &&
          !integer_constant_matches(
              tree,
              node.children.front(),
              scope,
              semantic_.types.builtins().usize_type,
              "array length")) {
        return invalid;
      }
      if (!count.has_value()) {
        const std::optional<IntegerExpression> expression =
            dependent_integer_expression(
                tree,
                node.children.front(),
                scope,
                semantic_.types.builtins().usize_type);
        if (expression.has_value()) {
          return semantic_.types.parametric_array(
              element, *expression);
        }
        if (expression_references_parametric_parameter(
                tree, node.children.front(), scope)) {
          return defer_element_count(
              TypeKind::Array,
              element,
              tree,
              node.children.front(),
              scope);
        }
        require_integer_expression(
            tree,
            node.children.front(),
            scope,
            semantic_.types.builtins().usize_type);
        if (integer_synthesis_is_blocked(tree, node.children.front())) {
          return invalid;
        }
      }
      if (!count.has_value() || *count == 0) {
        diagnostics_.error(
            tree.node(node.children.front()).range,
            "array length must be a nonzero compile-time usize");
        return invalid;
      }
      return semantic_.types.array(element, *count);
    }

    case NodeKind::SimdType: {
      if (node.children.size() < 2) {
        return invalid;
      }
      const TypeId element =
          resolve_type(tree, node.children.back(), scope);
      const std::optional<std::uint64_t> lanes =
          layout_integer(tree, node.children.front(), scope);
      if (lanes.has_value() &&
          !integer_constant_matches(
              tree,
              node.children.front(),
              scope,
              semantic_.types.builtins().usize_type,
              "SIMD lane count")) {
        return invalid;
      }
      if (!lanes.has_value()) {
        const std::optional<IntegerExpression> expression =
            dependent_integer_expression(
                tree,
                node.children.front(),
                scope,
                semantic_.types.builtins().usize_type);
        if (expression.has_value()) {
          return semantic_.types.parametric_simd(
              element, *expression);
        }
        if (expression_references_parametric_parameter(
                tree, node.children.front(), scope)) {
          return defer_element_count(
              TypeKind::Simd,
              element,
              tree,
              node.children.front(),
              scope);
        }
        require_integer_expression(
            tree,
            node.children.front(),
            scope,
            semantic_.types.builtins().usize_type);
        if (integer_synthesis_is_blocked(tree, node.children.front())) {
          return invalid;
        }
      }
      if (!lanes.has_value() || *lanes == 0) {
        diagnostics_.error(
            tree.node(node.children.front()).range,
            "SIMD lane count must be a nonzero compile-time usize");
        return invalid;
      }
      return semantic_.types.simd(element, *lanes, node.range);
    }

    case NodeKind::TupleType: {
      std::vector<TypeId> members;
      for (NodeId child : node.children) {
        if (node_is_type_syntax(tree.node(child).kind)) {
          members.push_back(resolve_type(tree, child, scope));
        }
      }
      if (members.size() < 2) {
        diagnostics_.error(node.range, "tuple type requires at least two members");
        return invalid;
      }
      return semantic_.types.tuple(members);
    }

    case NodeKind::ProcedureType:
      return resolve_procedure_type(tree, type_id, scope, std::nullopt);

    case NodeKind::DistinctType:
      diagnostics_.error(node.range, "distinct type must be the value of a named type declaration");
      return invalid;

    case NodeKind::StructType:
    case NodeKind::EnumType:
    case NodeKind::TaggedUnionType:
    case NodeKind::RawUnionType: {
      TypeKind kind = TypeKind::Struct;
      if (node.kind == NodeKind::EnumType) kind = TypeKind::Enum;
      if (node.kind == NodeKind::TaggedUnionType) kind = TypeKind::TaggedUnion;
      if (node.kind == NodeKind::RawUnionType) kind = TypeKind::RawUnion;
      const TypeId anonymous = semantic_.types.begin_nominal(
          kind, "<anonymous>", node.range);
      resolve_aggregate({}, anonymous, tree, type_id, scope);
      return anonymous;
    }

    default:
      diagnostics_.error(node.range, "expected a type");
      return invalid;
    }
  }

  // Adds one member binding to its type scope. The caller appends parallel type
  // and offset entries only when this declaration succeeds.
  [[nodiscard]] std::optional<SymbolId> declare_member(
      const SyntaxTree &tree,
      NodeId syntax,
      ScopeId scope,
      const SourceName &name,
      SymbolKind kind,
      TypeId type) {
    Symbol symbol;
    symbol.name = name.text;
    symbol.kind = kind;
    symbol.scope = scope;
    symbol.type = type;
    symbol.syntax = {tree.file(), syntax};
    symbol.name_range = name.range;
    const SymbolId id = semantic_.symbols.declare(std::move(symbol), diagnostics_);
    if (!id.is_valid()) {
      return std::nullopt;
    }
    return id;
  }

  // Resolves a possibly grouped struct/raw-union field declaration. Every name
  // shares the one parsed type and receives an independent member identity.
  void collect_field_member(
      const SyntaxTree &tree,
      NodeId member_id,
      ScopeId scope,
      MemberData &data) {
    const SyntaxNode &member = tree.node(member_id);
    if (member.children.empty()) {
      data.incomplete = true;
      return;
    }
    const SyntaxNode &type_node = tree.node(member.children.back());
    const TypeId type = resolve_type(tree, member.children.back(), scope);
    const std::vector<SourceName> names = names_in_span(
        tree, member.token_begin, type_node.token_begin);
    for (const SourceName &name : names) {
      if (name.text == "_") {
        diagnostics_.error(name.range, "aggregate member cannot use the discard name '_'");
        continue;
      }
      const std::optional<SymbolId> symbol = declare_member(
          tree, member_id, scope, name, SymbolKind::Field, type);
      if (symbol.has_value()) {
        data.symbols.push_back(*symbol);
        data.types.push_back(type);
        data.offsets.push_back(0);
      }
    }
  }

  // Declares an enum name now and fills its backing type after explicit/inferred
  // backing selection has seen the complete member count.
  [[nodiscard]] TokenKind expression_binary_operator(
      const SyntaxTree &tree, const SyntaxNode &expression) const {
    if (expression.children.size() != 2) return TokenKind::Invalid;
    const SyntaxNode &left = tree.node(expression.children.front());
    const SyntaxNode &right = tree.node(expression.children.back());
    for (std::uint32_t index = left.token_end; index < right.token_begin; ++index) {
      const TokenKind kind = tree.token(index).kind;
      if (kind != TokenKind::Semicolon && kind != TokenKind::Comma) return kind;
    }
    return TokenKind::Invalid;
  }

  // Resolves one package or imported integer constant without requiring the
  // later general constant table. Type layout is an input to that later pass,
  // so required layout constants need this narrow acyclic evaluator rather than
  // a circular "resolve types, then constants, then types" dependency.
  [[nodiscard]] std::optional<BigInteger> named_integer_constant(
      SymbolId symbol_id, SourceRange use_range) {
    // Concrete outer value parameters and already evaluated lexical constants
    // have no declaration payload that this layout-only evaluator can replay.
    // Prefer the body checker's exact SymbolId-keyed overlay before classifying
    // the source symbol kind.
    if (active_constants_ != nullptr) {
      if (const ConstantValue *active = active_constants_->find(symbol_id)) {
        if (active->kind == ConstantKind::Integer) return active->integer;
      }
    }
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy != symbol_id || !imported.has_constant ||
          imported.constant.kind != ConstantKind::Integer) {
        continue;
      }
      return imported.constant.integer;
    }

    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    if (symbol.kind == SymbolKind::Variable || symbol.kind == SymbolKind::Procedure ||
        symbol.kind == SymbolKind::Type || symbol.kind == SymbolKind::TypeParameter ||
        symbol.kind == SymbolKind::ValueParameter) {
      return std::nullopt;
    }
    if (std::find(
            active_integer_constants_.begin(),
            active_integer_constants_.end(),
            symbol_id) != active_integer_constants_.end()) {
      diagnostics_.error(
          use_range, "cyclic integer constant required by type layout");
      return std::nullopt;
    }
    const SyntaxTree *tree = find_tree(symbol.syntax.file);
    if (tree == nullptr || !symbol.syntax.node.is_valid()) {
      return std::nullopt;
    }
    const SyntaxNode &declaration = tree->node(symbol.syntax.node);
    const std::optional<NodeId> payload = declaration_payload(*tree, declaration);
    if (!payload.has_value()) {
      return std::nullopt;
    }

    active_integer_constants_.push_back(symbol_id);
    const ScopeKind owner_kind = semantic_.symbols.scope(symbol.scope).kind;
    const ScopeId evaluation_scope = owner_kind == ScopeKind::Block
        ? symbol.scope
        : file_scope(tree->file());
    const std::optional<BigInteger> result = integer_constant_expression(
        *tree, *payload, evaluation_scope);
    active_integer_constants_.pop_back();
    return result;
  }

  // Enum values participate in layout before the general constant pass runs.
  // This evaluator covers their exact integer vocabulary plus named constants,
  // array/SIMD lengths, and @align without host-width arithmetic.
  [[nodiscard]] std::optional<BigInteger> integer_constant_expression(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) {
    if (const ResolvedIntegerExpression *resolved =
            resolved_integer(tree, expression_id)) {
      return resolved->value;
    }
    // Use the typed builder first. Besides sharing exact fixed-width semantics
    // with dependent expressions, this recognizes explicit integer casts. The
    // direct layout cases below handle enum-member values and recovery paths
    // that do not need a canonical expression tree.
    if (contains_integer_cast(tree, expression_id)) {
      IntegerExpression built;
      const BuiltIntegerExpressionNode built_root =
          build_integer_expression_node(tree, expression_id, scope, built);
      if (built_root.valid && built_root.constant.has_value()) {
        return built_root.constant;
      }
    }

    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind == NodeKind::LiteralExpression &&
        expression.token_begin < expression.token_end &&
        tree.token(expression.token_begin).kind == TokenKind::IntegerLiteral) {
      return BigInteger::parse_literal(
          sources_.text(tree.token(expression.token_begin).range));
    }
    if (expression.kind == NodeKind::GroupExpression &&
        !expression.children.empty()) {
      return integer_constant_expression(tree, expression.children.front(), scope);
    }
    if (expression.kind == NodeKind::NameExpression) {
      const std::vector<SourceName> names = names_in_span(
          tree, expression.token_begin, expression.token_end);
      if (names.size() != 1) return std::nullopt;
      const std::optional<SymbolId> found =
          semantic_.symbols.lookup(scope, names.front().text);
      if (!found.has_value()) return std::nullopt;
      for (const EnumMemberValue &value : semantic_.enum_member_values) {
        if (value.member == *found) return value.value;
      }
      return named_integer_constant(*found, names.front().range);
    }
    if (expression.kind == NodeKind::MemberExpression) {
      const std::vector<SourceName> names = names_in_span(
          tree, expression.token_begin, expression.token_end);
      if (names.size() != 2) return std::nullopt;
      const std::optional<SymbolId> package =
          semantic_.symbols.lookup(scope, names.front().text);
      if (!package.has_value() ||
          semantic_.symbols.symbol(*package).kind != SymbolKind::Import) {
        return std::nullopt;
      }
      for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
        if (owned.owner != *package ||
            semantic_.symbols.scope(owned.scope).kind !=
                ScopeKind::ImportedPackage) {
          continue;
        }
        const std::optional<SymbolId> member =
            semantic_.symbols.lookup_direct(owned.scope, names.back().text);
        return member.has_value()
            ? named_integer_constant(*member, names.back().range)
            : std::nullopt;
      }
      return std::nullopt;
    }
    if (expression.kind == NodeKind::UnaryExpression &&
        !expression.children.empty()) {
      const std::optional<BigInteger> operand =
          integer_constant_expression(tree, expression.children.front(), scope);
      if (!operand.has_value()) return std::nullopt;
      const TokenKind operation = tree.token(expression.token_begin).kind;
      if (operation == TokenKind::Plus) return *operand;
      if (operation == TokenKind::Minus) return operand->negated();
      if (operation == TokenKind::Tilde) return operand->bitwise_not();
      return std::nullopt;
    }
    if (expression.kind != NodeKind::BinaryExpression ||
        expression.children.size() != 2) {
      return std::nullopt;
    }
    const std::optional<BigInteger> left =
        integer_constant_expression(tree, expression.children[0], scope);
    const std::optional<BigInteger> right =
        integer_constant_expression(tree, expression.children[1], scope);
    if (!left.has_value() || !right.has_value()) return std::nullopt;
    switch (expression_binary_operator(tree, expression)) {
    case TokenKind::Plus: return left->added(*right);
    case TokenKind::Minus: return left->subtracted(*right);
    case TokenKind::Star: return left->multiplied(*right);
    case TokenKind::Ampersand: return left->bitwise_and(*right);
    case TokenKind::Pipe: return left->bitwise_or(*right);
    case TokenKind::Caret: return left->bitwise_xor(*right);
    case TokenKind::Slash:
    case TokenKind::Percent: {
      BigInteger quotient;
      BigInteger remainder;
      if (!left->divide(*right, quotient, remainder)) return std::nullopt;
      return expression_binary_operator(tree, expression) == TokenKind::Slash
          ? std::optional<BigInteger>(std::move(quotient))
          : std::optional<BigInteger>(std::move(remainder));
    }
    case TokenKind::ShiftLeft:
    case TokenKind::ShiftRight: {
      if (right->is_negative()) return std::nullopt;
      const std::optional<std::uint64_t> count = right->to_u64();
      if (!count.has_value() ||
          *count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
      }
      return expression_binary_operator(tree, expression) == TokenKind::ShiftLeft
          ? left->shifted_left(static_cast<std::size_t>(*count))
          : left->shifted_right(static_cast<std::size_t>(*count));
    }
    default:
      return std::nullopt;
    }
  }

  // Validates the closed Draft 1 representation-attribute vocabulary. The
  // parser deliberately accepts arbitrary attribute spellings so this phase
  // can issue type-aware diagnostics and keep malformed declarations in the
  // same recovery path as ordinary layout failures.
  [[nodiscard]] AggregateAttributes aggregate_attributes(
      const SyntaxTree &tree,
      const SyntaxNode &aggregate,
      TypeKind kind,
      ScopeId scope) {
    AggregateAttributes result;
    bool saw_repr = false;
    bool saw_align = false;
    for (NodeId child_id : aggregate.children) {
      const SyntaxNode &list = tree.node(child_id);
      if (list.kind != NodeKind::AttributeList) {
        continue;
      }
      for (NodeId attribute_id : list.children) {
        const SyntaxNode &attribute = tree.node(attribute_id);
        const std::vector<SourceName> names = names_in_span(
            tree, attribute.token_begin, attribute.token_end);
        if (names.empty()) {
          diagnostics_.error(attribute.range, "attribute requires a name");
          continue;
        }
        const std::string &name = names.front().text;
        if (name == "repr") {
          if (saw_repr) {
            diagnostics_.error(attribute.range, "duplicate '@repr' attribute");
            continue;
          }
          saw_repr = true;
          if (kind != TypeKind::Struct && kind != TypeKind::RawUnion &&
              kind != TypeKind::Enum) {
            diagnostics_.error(
                attribute.range,
                "'@repr(C)' is valid only on structs, raw unions, and enums");
            continue;
          }
          if (attribute.children.size() != 1) {
            diagnostics_.error(
                attribute.range, "'@repr' requires exactly one argument");
            continue;
          }
          const SyntaxNode &argument = tree.node(attribute.children.front());
          const std::vector<SourceName> argument_names = names_in_span(
              tree, argument.token_begin, argument.token_end);
          if (argument_names.size() != 1 || argument_names.front().text != "C") {
            diagnostics_.error(
                argument.range, "Draft 1 supports only '@repr(C)'");
            continue;
          }
          result.c_representation = true;
          continue;
        }
        if (name == "align") {
          if (saw_align) {
            diagnostics_.error(attribute.range, "duplicate '@align' attribute");
            continue;
          }
          saw_align = true;
          if (kind != TypeKind::Struct && kind != TypeKind::RawUnion) {
            diagnostics_.error(
                attribute.range,
                "'@align' is valid only on structs and raw unions");
            continue;
          }
          if (attribute.children.size() != 1) {
            diagnostics_.error(
                attribute.range, "'@align' requires exactly one argument");
            continue;
          }
          const SyntaxNode &argument = tree.node(attribute.children.front());
          const std::optional<BigInteger> value = integer_constant_expression(
              tree, attribute.children.front(), scope);
          if (!value.has_value()) {
            require_integer_expression(
                tree,
                attribute.children.front(),
                scope,
                semantic_.types.builtins().usize_type);
            if (integer_synthesis_is_blocked(
                    tree, attribute.children.front())) {
              continue;
            }
          }
          const std::optional<std::uint64_t> alignment =
              value.has_value() ? value->to_u64() : std::nullopt;
          if (alignment.has_value() &&
              !integer_constant_matches(
                  tree,
                  attribute.children.front(),
                  scope,
                  semantic_.types.builtins().usize_type,
                  "'@align' argument")) {
            continue;
          }
          if (!alignment.has_value() || *alignment == 0 ||
              (*alignment & (*alignment - 1)) != 0 ||
              *alignment > std::numeric_limits<std::uint32_t>::max()) {
            diagnostics_.error(
                argument.range,
                "'@align' requires a positive power-of-two compile-time usize");
            continue;
          }
          result.requested_alignment =
              static_cast<std::uint32_t>(*alignment);
          continue;
        }
        diagnostics_.error(
            names.front().range,
            "unknown type representation attribute '@" + name + "'");
      }
    }
    return result;
  }

  // Applies the post-layout alignment rule shared by source templates and
  // concrete instantiations. A requested alignment cannot shrink the natural
  // or C layout, and the final size is the array stride required by the spec.
  [[nodiscard]] TypeLayout apply_requested_alignment(
      TypeLayout layout,
      std::uint32_t requested,
      SourceRange range) {
    if (!layout.known || requested == 0) {
      return layout;
    }
    if (requested < layout.alignment) {
      diagnostics_.error(
          range, "'@align' cannot reduce the type's natural alignment");
      return layout;
    }
    layout.alignment = requested;
    const std::optional<std::uint64_t> size = round_up(layout.size, requested);
    if (!size.has_value()) {
      diagnostics_.error(range, "aligned aggregate size overflows u64");
      return {};
    }
    layout.size = *size;
    return layout;
  }

  void collect_enum_member(
      const SyntaxTree &tree,
      NodeId member_id,
      ScopeId scope,
      MemberData &data) {
    const SyntaxNode &member = tree.node(member_id);
    const std::vector<SourceName> names = names_in_span(
        tree, member.token_begin, member.token_end);
    if (names.empty()) {
      data.incomplete = true;
      return;
    }
    const std::optional<SymbolId> symbol = declare_member(
        tree,
        member_id,
        scope,
        names.front(),
        SymbolKind::EnumMember,
        semantic_.types.builtins().invalid);
    if (symbol.has_value()) {
      BigInteger value = data.enum_values.empty()
          ? BigInteger::from_u64(0)
          : data.enum_values.back().added(BigInteger::from_u64(1));
      if (!member.children.empty()) {
        const std::optional<BigInteger> explicit_value =
            integer_constant_expression(tree, member.children.front(), scope);
        if (!explicit_value.has_value()) {
          require_integer_expression(
              tree,
              member.children.front(),
              scope,
              data.enum_value_expected_type);
          if (!integer_synthesis_is_blocked(
                  tree, member.children.front())) {
            diagnostics_.error(
                tree.node(member.children.front()).range,
                "enum value must be a compile-time integer expression");
          }
          data.incomplete = true;
        } else {
          value = *explicit_value;
        }
      }
      for (const BigInteger &existing : data.enum_values) {
        if (existing.compare(value) == 0) {
          diagnostics_.error(member.range, "duplicate enum value");
          break;
        }
      }
      data.symbols.push_back(*symbol);
      data.types.push_back(semantic_.types.builtins().invalid);
      data.offsets.push_back(0);
      data.enum_values.push_back(value);
      semantic_.enum_member_values.push_back({*symbol, std::move(value)});
    }
  }

  // Declares one tagged-union alternative. A payload-free alternative uses the
  // canonical void type whose layout is zero bytes with alignment one.
  void collect_union_alternative(
      const SyntaxTree &tree,
      NodeId member_id,
      ScopeId scope,
      MemberData &data) {
    const SyntaxNode &member = tree.node(member_id);
    const std::vector<SourceName> names = names_in_span(
        tree, member.token_begin, member.token_end);
    if (names.empty()) {
      data.incomplete = true;
      return;
    }
    TypeId type = semantic_.types.builtins().void_type;
    if (!member.children.empty()) {
      type = resolve_type(tree, member.children.back(), scope);
    }
    const std::optional<SymbolId> symbol = declare_member(
        tree,
        member_id,
        scope,
        names.front(),
        SymbolKind::UnionAlternative,
        type);
    if (symbol.has_value()) {
      data.symbols.push_back(*symbol);
      data.types.push_back(type);
      data.offsets.push_back(0);
    }
  }

  // Selects one member-level `when` without introducing a scope. Recursive
  // `else when` chains use the same operation, so arbitrary chain depth neither
  // duplicates code nor treats a nested conditional as a member list.
  void collect_conditional_member(
      SymbolId owner,
      const SyntaxTree &tree,
      NodeId member_id,
      ScopeId scope,
      MemberData &data) {
    const SyntaxNode &member = tree.node(member_id);
    add_site(SemanticSiteKind::ConditionalMember, tree, member_id, scope, owner);
    const ConditionalSelection *selection =
        selections_.find({tree.file(), member_id});
    if (selection == nullptr) {
      data.incomplete = true;
      return;
    }
    if (selection->select_true) {
      if (member.children.size() >= 2) {
        collect_member_list(owner, tree, member.children[1], scope, data);
      }
      return;
    }
    if (member.children.size() < 3) {
      return;
    }
    const NodeId alternative = member.children[2];
    if (tree.node(alternative).kind == NodeKind::WhenMember) {
      collect_conditional_member(owner, tree, alternative, scope, data);
    } else {
      collect_member_list(owner, tree, alternative, scope, data);
    }
  }

  // Walks a member region in source order. Denials are transparent for name and
  // layout purposes; conditionals and synthesis keep the aggregate incomplete
  // until a later pass selects or supplies their members.
  void collect_member_list(
      SymbolId owner,
      const SyntaxTree &tree,
      NodeId list_id,
      ScopeId scope,
      MemberData &data) {
    const SyntaxNode &list = tree.node(list_id);
    for (NodeId member_id : list.children) {
      const SyntaxNode &member = tree.node(member_id);
      switch (member.kind) {
      case NodeKind::FieldMember:
        collect_field_member(tree, member_id, scope, data);
        break;
      case NodeKind::EnumMember:
        collect_enum_member(tree, member_id, scope, data);
        break;
      case NodeKind::UnionAlternative:
        collect_union_alternative(tree, member_id, scope, data);
        break;
      case NodeKind::Documentation:
        add_site(SemanticSiteKind::Documentation, tree, member_id, scope, owner);
        break;
      case NodeKind::Judgment:
        add_site(SemanticSiteKind::Judgment, tree, member_id, scope, owner);
        break;
      case NodeKind::SynthesisMember:
        add_site(SemanticSiteKind::SynthesisMember, tree, member_id, scope, owner);
        data.incomplete = true;
        break;
      case NodeKind::WhenMember:
        collect_conditional_member(owner, tree, member_id, scope, data);
        break;
      case NodeKind::DenyMember:
        add_site(SemanticSiteKind::DenialMember, tree, member_id, scope, owner);
        if (!member.children.empty()) {
          collect_member_list(
              owner, tree, member.children.back(), scope, data);
        }
        break;
      default:
        break;
      }
    }
  }

  // Computes ordinary field-order layout and writes each field offset into the
  // parallel work vector. Overflow or an unknown member yields unknown layout.
  [[nodiscard]] TypeLayout struct_layout(MemberData &data) const {
    TypeLayout result{true, 0, 1};
    for (std::size_t index = 0; index < data.types.size(); ++index) {
      const TypeLayout member = semantic_.types.type(data.types[index]).layout;
      if (!member.known) {
        return {};
      }
      const std::optional<std::uint64_t> offset = round_up(result.size, member.alignment);
      if (!offset.has_value() ||
          member.size > std::numeric_limits<std::uint64_t>::max() - *offset) {
        return {};
      }
      data.offsets[index] = *offset;
      result.size = *offset + member.size;
      result.alignment = std::max(result.alignment, member.alignment);
    }
    const std::optional<std::uint64_t> size = round_up(result.size, result.alignment);
    if (!size.has_value()) {
      return {};
    }
    result.size = *size;
    return result;
  }

  // Computes overlay layout: every field starts at zero, size is the rounded
  // maximum member size, and alignment is the maximum member alignment.
  [[nodiscard]] TypeLayout raw_union_layout(MemberData &data) const {
    TypeLayout result{true, 0, 1};
    for (std::size_t index = 0; index < data.types.size(); ++index) {
      const TypeLayout member = semantic_.types.type(data.types[index]).layout;
      if (!member.known) {
        return {};
      }
      data.offsets[index] = 0;
      result.size = std::max(result.size, member.size);
      result.alignment = std::max(result.alignment, member.alignment);
    }
    const std::optional<std::uint64_t> size = round_up(result.size, result.alignment);
    if (!size.has_value()) {
      return {};
    }
    result.size = *size;
    return result;
  }

  // Chooses the smallest fixed-width unsigned discriminator capable of naming
  // every source-order alternative. Enum signedness is refined with values later.
  [[nodiscard]] TypeId inferred_discriminator(std::size_t alternative_count) const {
    if (alternative_count <= 0x100U) {
      return semantic_.types.builtins().u8_type;
    }
    const std::optional<TypeId> u16 = semantic_.types.find_builtin("u16");
    const std::optional<TypeId> u32 = semantic_.types.find_builtin("u32");
    const std::optional<TypeId> u64 = semantic_.types.find_builtin("u64");
    if (alternative_count <= 0x10000U && u16.has_value()) return *u16;
    if (static_cast<std::uint64_t>(alternative_count) <= 0x100000000ULL &&
        u32.has_value()) return *u32;
    return u64.value_or(semantic_.types.builtins().invalid);
  }

  [[nodiscard]] bool integer_fits_type(
      const BigInteger &value, TypeId type_id) const {
    const Type type = semantic_.types.type(type_id);
    if (type.kind != TypeKind::SignedInteger &&
        type.kind != TypeKind::UnsignedInteger) {
      return false;
    }
    if (type.kind == TypeKind::UnsignedInteger) {
      return !value.is_negative() && value.bit_count() <= type.bit_width;
    }
    const BigInteger magnitude = BigInteger::from_u64(1).shifted_left(
        static_cast<std::size_t>(type.bit_width - 1U));
    return value.compare(magnitude.negated()) >= 0 &&
        value.compare(magnitude.subtracted(BigInteger::from_u64(1))) <= 0;
  }

  [[nodiscard]] TypeId inferred_enum_backing(
      const std::vector<BigInteger> &values) const {
    bool has_negative = false;
    for (const BigInteger &value : values) {
      has_negative = has_negative || value.is_negative();
    }
    static constexpr std::string_view unsigned_names[] = {
        "u8", "u16", "u32", "u64", "u128"};
    static constexpr std::string_view signed_names[] = {
        "i8", "i16", "i32", "i64", "i128"};
    const auto &names = has_negative ? signed_names : unsigned_names;
    for (std::string_view name : names) {
      const std::optional<TypeId> candidate = semantic_.types.find_builtin(name);
      if (!candidate.has_value()) continue;
      bool fits = true;
      for (const BigInteger &value : values) {
        fits = fits && integer_fits_type(value, *candidate);
      }
      if (fits) return *candidate;
    }
    return semantic_.types.builtins().invalid;
  }

  // Selects the compatible integer type used by an unfixed C enum on the
  // current Darwin and GNU AArch64 targets. Their default ABI keeps an enum at
  // C `int` width even when every enumerator would fit in u8 or u16. At that
  // width it uses
  // unsigned int for a wholly nonnegative set and signed int when any member
  // is negative. Values outside 32 bits widen by the same signedness rule to
  // unsigned long or signed long, both 64 bits in this target ABI.
  //
  // Do not reuse inferred_enum_backing(): Draft's ordinary enum rule chooses
  // the smallest fixed-width representation, while @repr(C) explicitly asks
  // for the target C ABI's default. The target ABI identity is already part of
  // every resolved program. A future ABI must add its complete C-enum rule
  // here rather than inheriting this shared AArch64 result accidentally.
  [[nodiscard]] TypeId inferred_c_enum_backing(
      const std::vector<BigInteger> &values) const {
    if (target_ != nullptr && !target_->abi.empty() &&
        target_->abi != "darwin_arm64" &&
        target_->abi != "aapcs64_gnu") {
      // Applying the current AArch64 rule to another ABI would manufacture the
      // wrong public type. Fail closed until that profile supplies a complete
      // rule alongside its ABI classifier and header lowering.
      return semantic_.types.builtins().invalid;
    }
    bool has_negative = false;
    for (const BigInteger &value : values) {
      has_negative = has_negative || value.is_negative();
    }
    static constexpr std::string_view unsigned_names[] = {"u32", "u64"};
    static constexpr std::string_view signed_names[] = {"i32", "i64"};
    const auto &names = has_negative ? signed_names : unsigned_names;
    for (std::string_view name : names) {
      const std::optional<TypeId> candidate = semantic_.types.find_builtin(name);
      if (!candidate.has_value()) continue;
      const bool fits = std::all_of(
          values.begin(),
          values.end(),
          [&](const BigInteger &value) {
            return integer_fits_type(value, *candidate);
          });
      if (fits) return *candidate;
    }
    return semantic_.types.builtins().invalid;
  }

  // Applies the exact tagged-union formula from section 5. All alternatives use
  // the one computed payload offset; payload-free alternatives remain valid.
  [[nodiscard]] TypeLayout tagged_union_layout(
      TypeId discriminator, MemberData &data) const {
    const TypeLayout discriminator_layout = semantic_.types.type(discriminator).layout;
    if (!discriminator_layout.known) {
      return {};
    }
    std::uint64_t payload_size = 0;
    std::uint32_t payload_alignment = 1;
    for (TypeId type : data.types) {
      const TypeLayout payload = semantic_.types.type(type).layout;
      if (!payload.known) {
        return {};
      }
      payload_size = std::max(payload_size, payload.size);
      payload_alignment = std::max(payload_alignment, payload.alignment);
    }
    const std::optional<std::uint64_t> rounded_payload =
        round_up(payload_size, payload_alignment);
    const std::optional<std::uint64_t> payload_offset =
        round_up(discriminator_layout.size, payload_alignment);
    if (!rounded_payload.has_value() || !payload_offset.has_value() ||
        *rounded_payload >
            std::numeric_limits<std::uint64_t>::max() - *payload_offset) {
      return {};
    }
    for (std::uint64_t &offset : data.offsets) {
      offset = *payload_offset;
    }
    const std::uint32_t alignment =
        std::max(discriminator_layout.alignment, payload_alignment);
    const std::optional<std::uint64_t> size = round_up(
        *payload_offset + *rounded_payload, alignment);
    if (!size.has_value()) {
      return {};
    }
    return {true, *size, alignment};
  }

  // Resolves member names/types and completes one pre-interned nominal type.
  // Completion may intentionally retain layout.known=false for generic,
  // conditional, imported, erroneous, or synthesis-dependent members.
  void resolve_aggregate(
      SymbolId owner,
      TypeId nominal,
      const SyntaxTree &tree,
      NodeId aggregate_id,
      ScopeId parent) {
    const SyntaxNode &aggregate = tree.node(aggregate_id);
    ScopeId scope = parent;
    if (owner.is_valid()) {
      const std::optional<ScopeId> existing = owned_scope(owner, ScopeKind::Type);
      if (existing.has_value()) {
        scope = *existing;
      } else {
        scope = semantic_.symbols.add_scope(ScopeKind::Type, parent, aggregate.range);
        semantic_.owned_scopes.push_back({owner, scope});
      }
    } else {
      scope = semantic_.symbols.add_scope(ScopeKind::Type, parent, aggregate.range);
    }

    const TypeKind kind = semantic_.types.type(nominal).kind;
    const AggregateAttributes attributes = aggregate_attributes(
        tree, aggregate, kind, parent);
    semantic_.types.type_mut(nominal).c_representation =
        attributes.c_representation;
    semantic_.types.type_mut(nominal).requested_alignment =
        attributes.requested_alignment;

    std::optional<NodeId> list;
    std::optional<NodeId> explicit_backing;
    for (NodeId child : aggregate.children) {
      if (tree.node(child).kind == NodeKind::MemberList) {
        list = child;
      } else if (node_is_type_syntax(tree.node(child).kind)) {
        explicit_backing = child;
      }
    }
    if (!list.has_value()) {
      diagnostics_.error(aggregate.range, "aggregate type has no member list");
      semantic_.types.complete_nominal(nominal, {}, {});
      return;
    }

    TypeId explicit_backing_type;
    if (explicit_backing.has_value()) {
      explicit_backing_type = resolve_type(tree, *explicit_backing, parent);
    }

    MemberData data;
    if (kind == TypeKind::Enum) {
      data.enum_value_expected_type = explicit_backing_type;
    }
    collect_member_list(owner, tree, *list, scope, data);
    if (data.symbols.empty() && !data.incomplete) {
      diagnostics_.error(aggregate.range, "aggregate type requires at least one member");
    }

    TypeLayout layout;
    if (!data.incomplete) {
      if (kind == TypeKind::Struct) {
        layout = struct_layout(data);
      } else if (kind == TypeKind::RawUnion) {
        layout = raw_union_layout(data);
      } else if (kind == TypeKind::Enum) {
        const bool has_zero_member = std::any_of(
            data.enum_values.begin(),
            data.enum_values.end(),
            [](const BigInteger &value) { return value.is_zero(); });
        if (!has_zero_member) {
          diagnostics_.error(
              aggregate.range,
              "enum must declare a zero-valued member for its zero value");
        }
        TypeId backing = explicit_backing.has_value()
            ? explicit_backing_type
            : (attributes.c_representation
                   ? inferred_c_enum_backing(data.enum_values)
                   : inferred_enum_backing(data.enum_values));
        if (!semantic_.types.is_integer(backing)) {
          diagnostics_.error(
              aggregate.range,
              attributes.c_representation && !explicit_backing.has_value()
                  ? "enum values do not fit the target C ABI's default backing types"
                  : "enum backing type must be an integer type");
          backing = semantic_.types.builtins().invalid;
        } else {
          for (std::size_t index = 0; index < data.enum_values.size(); ++index) {
            if (!integer_fits_type(data.enum_values[index], backing)) {
              diagnostics_.error(
                  semantic_.symbols.symbol(data.symbols[index]).name_range,
                  "enum value is not representable in its backing type");
            }
          }
        }
        semantic_.types.type_mut(nominal).element = backing;
        layout = semantic_.types.type(backing).layout;
        for (std::size_t index = 0; index < data.symbols.size(); ++index) {
          data.types[index] = backing;
          semantic_.symbols.symbol_mut(data.symbols[index]).type = backing;
        }
      } else if (kind == TypeKind::TaggedUnion) {
        TypeId discriminator = explicit_backing.has_value()
            ? explicit_backing_type
            : inferred_discriminator(data.symbols.size());
        if (!semantic_.types.is_integer(discriminator)) {
          diagnostics_.error(
              aggregate.range, "tagged-union discriminator must be an integer type");
          discriminator = semantic_.types.builtins().invalid;
        } else if (!data.symbols.empty()) {
          // Source-order discriminators are 0 through alternative_count - 1.
          // Checking the greatest one proves the complete nonnegative range.
          const BigInteger greatest = BigInteger::from_u64(
              static_cast<std::uint64_t>(data.symbols.size() - 1));
          if (!integer_fits_type(greatest, discriminator)) {
            diagnostics_.error(
                aggregate.range,
                "tagged-union alternatives do not fit its discriminator type");
          }
        }
        semantic_.types.type_mut(nominal).element = discriminator;
        layout = tagged_union_layout(discriminator, data);
      }
    }

    layout = apply_requested_alignment(
        layout, attributes.requested_alignment, aggregate.range);

    semantic_.types.complete_nominal(
        nominal, layout, data.types, data.offsets);
    for (std::size_t index = 0; index < data.symbols.size(); ++index) {
      semantic_.aggregate_members.push_back(
          {owner, data.symbols[index], data.offsets[index]});
    }
  }

  // Resolves one package declaration with cycle detection. It copies the input
  // Symbol before creating child scopes because SymbolTable's vector may grow
  // and invalidate references; every mutation reacquires the symbol by ID.
  void resolve_symbol(SymbolId id) {
    if (static_cast<std::size_t>(id.value) >= states_.size()) {
      return;
    }
    ResolutionState &state = states_[id.value];
    if (state == ResolutionState::Resolved || state == ResolutionState::Failed) {
      return;
    }
    const Symbol initial_symbol = semantic_.symbols.symbol(id);
    if (state == ResolutionState::Resolving) {
      diagnostics_.error(
          initial_symbol.name_range,
          "cyclic type declaration involving '" + initial_symbol.name + "'");
      state = ResolutionState::Failed;
      return;
    }

    const SyntaxTree *tree_pointer = find_tree(initial_symbol.syntax.file);
    if (tree_pointer == nullptr || !initial_symbol.syntax.node.is_valid()) {
      state = ResolutionState::Failed;
      return;
    }
    const SyntaxTree &tree = *tree_pointer;
    const SyntaxNode &declaration = tree.node(initial_symbol.syntax.node);
    if (declaration.kind != NodeKind::Declaration) {
      state = ResolutionState::Resolved;
      return;
    }
    if (declaration_resolution_depth_ >=
        kMaximumDeclarationResolutionDepth) {
      diagnostics_.error(
          initial_symbol.name_range,
          "declaration dependency depth exceeds the implementation "
          "limit of " +
              std::to_string(kMaximumDeclarationResolutionDepth));
      state = ResolutionState::Failed;
      return;
    }

    // Every path below reaches the common epilogue. Keeping the counter
    // increment/decrement visible avoids a general-purpose recursion helper in
    // the central type-resolution loop.
    state = ResolutionState::Resolving;
    ++declaration_resolution_depth_;
    const std::optional<NodeId> payload = declaration_payload(tree, declaration);
    active_declaration_owners_.push_back(id);
    const ScopeId source_scope = file_scope(tree.file());
    const ScopeId semantic_parent = ensure_parametric_scope(
        id, tree, declaration, source_scope);

    if (initial_symbol.kind == SymbolKind::Procedure && payload.has_value()) {
      const TypeId type = resolve_procedure_type(
          tree, *payload, semantic_parent, id);
      semantic_.symbols.symbol_mut(id).type = type;
    } else if (initial_symbol.kind == SymbolKind::Type && payload.has_value()) {
      const SyntaxNode &type_node = tree.node(*payload);
      if (type_node.kind == NodeKind::StructType ||
          type_node.kind == NodeKind::EnumType ||
          type_node.kind == NodeKind::TaggedUnionType ||
          type_node.kind == NodeKind::RawUnionType) {
        if (initial_symbol.type.is_valid()) {
          resolve_aggregate(id, initial_symbol.type, tree, *payload, semantic_parent);
        }
      } else if (type_node.kind == NodeKind::DistinctType &&
                 !type_node.children.empty()) {
        const TypeId underlying =
            resolve_type(tree, type_node.children.back(), semantic_parent);
        semantic_.symbols.symbol_mut(id).type = semantic_.types.distinct(
            initial_symbol.name, underlying, initial_symbol.name_range);
      } else {
        const TypeId type = resolve_type(tree, *payload, semantic_parent);
        semantic_.symbols.symbol_mut(id).type = type;
      }
    } else if (initial_symbol.kind == SymbolKind::UnresolvedDeclaration &&
               payload.has_value()) {
      if (const std::optional<TypeId> type =
              try_type_value(tree, *payload, semantic_parent)) {
        semantic_.symbols.symbol_mut(id).kind = SymbolKind::Type;
        semantic_.symbols.symbol_mut(id).type = *type;
      } else {
        semantic_.symbols.symbol_mut(id).kind = SymbolKind::Constant;
        // A name, member access, or bracket expression is deliberately
        // ambiguous during declaration collection: it may denote either a
        // type value or an ordinary constant. Collection therefore cannot
        // reject parameters on it without also rejecting structural aliases
        // such as `Bytes[N] :: Other_Bytes[N]`. Once type lookup has resolved
        // the ambiguity, report the ordinary-constant case here.
        if (initial_symbol.flags.parametric) {
          diagnostics_.error(
              declaration.range,
              "parametric parameters are valid only on type and procedure declarations");
        }
      }
    } else if (initial_symbol.kind == SymbolKind::Variable) {
      // The first type child is the explicit declaration type. Inferred globals
      // retain an invalid TypeId until expression checking supplies it.
      for (NodeId child : declaration.children) {
        if (node_is_type_syntax(tree.node(child).kind)) {
          const TypeId type = resolve_type(tree, child, semantic_parent);
          semantic_.symbols.symbol_mut(id).type = type;
          break;
        }
      }
    }

    const Symbol &resolved_symbol = semantic_.symbols.symbol(id);
    active_declaration_owners_.pop_back();
    --declaration_resolution_depth_;
    state = is_error_type(resolved_symbol.type) &&
            (resolved_symbol.kind == SymbolKind::Type ||
             resolved_symbol.kind == SymbolKind::Procedure)
        ? ResolutionState::Failed
        : ResolutionState::Resolved;
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  SemanticPackage &semantic_;
  const ConditionalSelections &selections_;
  DiagnosticSink &diagnostics_;
  std::vector<ResolutionState> states_;
  std::size_t declaration_resolution_depth_ = 0;
  std::vector<SymbolId> active_integer_constants_;
  std::vector<SymbolId> active_declaration_owners_;
  const ConstantTable *active_constants_ = nullptr;
  const std::vector<ConstantTypeBinding> *active_types_ = nullptr;
  const std::vector<ResolvedIntegerExpression> *resolved_integers_ = nullptr;
  const TargetFacts *target_ = nullptr;
  const std::vector<SyntaxReference> *blocked_integer_synthesis_ = nullptr;
};

} // namespace

void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    DiagnosticSink &diagnostics) {
  const ConditionalSelections selections;
  TypeResolver resolver(sources, loaded, package, selections, diagnostics);
  resolver.resolve();
}

void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const std::vector<ResolvedIntegerExpression> &resolved_integers,
    const TargetFacts &target,
    const std::vector<SyntaxReference> &blocked_synthesis,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(
      sources,
      loaded,
      package,
      selections,
      diagnostics,
      nullptr,
      nullptr,
      &resolved_integers,
      &target,
      &blocked_synthesis);
  resolver.resolve();
}

void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const std::vector<ResolvedIntegerExpression> &resolved_integers,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(
      sources,
      loaded,
      package,
      selections,
      diagnostics,
      nullptr,
      nullptr,
      &resolved_integers,
      &target);
  resolver.resolve();
}

void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(sources, loaded, package, selections, diagnostics);
  resolver.resolve();
}

TypeId resolve_type_syntax(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId type,
    ScopeId scope,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(
      sources, loaded, package, selections, diagnostics,
      nullptr, nullptr, nullptr, &target);
  return resolver.resolve_one_type(tree, type, scope);
}

std::optional<IntegerExpression>
resolve_dependent_integer_expression_syntax(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    TypeId contextual_type,
    const ConstantTable &active_constants,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(
      sources,
      loaded,
      package,
      selections,
      diagnostics,
      &active_constants);
  return resolver.resolve_one_dependent_integer_expression(
      tree, expression, scope, contextual_type);
}

TypeId resolve_local_procedure_signature(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId declaration,
    NodeId procedure,
    ScopeId scope,
    SymbolId owner,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(
      sources, loaded, package, selections, diagnostics,
      nullptr, nullptr, nullptr, &target);
  return resolver.resolve_one_procedure(
      tree, declaration, procedure, scope, owner);
}

TypeId resolve_local_type_declaration(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId declaration,
    NodeId type,
    ScopeId scope,
    SymbolId owner,
    const ConstantTable &active_constants,
    const std::vector<ConstantTypeBinding> &active_types,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(
      sources,
      loaded,
      package,
      selections,
      diagnostics,
      &active_constants,
      &active_types,
      nullptr,
      &target);
  return resolver.resolve_one_local_type(
      tree, declaration, type, scope, owner);
}

TypeId instantiate_parametric_type_application(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    SymbolId source,
    std::vector<ParametricArgument> arguments,
    SourceRange use_range,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(
      sources, loaded, package, selections, diagnostics,
      nullptr, nullptr, nullptr, &target);
  return resolver.instantiate_one_type(
      source, std::move(arguments), use_range);
}

TypeId instantiate_owner_evaluated_type_application(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    TypeId source,
    const std::vector<DeferredElementCountTypeBinding> &type_bindings,
    const std::vector<DeferredElementCountValueBinding> &value_bindings,
    SourceRange use_range,
    const ConstantTable &active_constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  std::vector<ConstantTypeBinding> active_types;
  for (const DeferredElementCountTypeBinding &binding : type_bindings) {
    active_types.push_back({binding.parameter, binding.replacement});
  }
  TypeResolver resolver(
      sources,
      loaded,
      package,
      selections,
      diagnostics,
      &active_constants,
      &active_types,
      nullptr,
      &target);
  return resolver.instantiate_one_owner_evaluated_type(
      source, type_bindings, value_bindings, use_range);
}

} // namespace draft
