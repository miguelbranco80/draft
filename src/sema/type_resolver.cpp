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
  bool incomplete = false;
};

struct ResolverTypeSubstitution {
  TypeId parameter;
  TypeId replacement;
};

// Parsed representation attributes are kept small and explicit. Zero means no
// requested alignment; C representation is independent so `@repr(C)` and
// `@align(N)` compose without an attribute object hierarchy.
struct AggregateAttributes {
  bool c_representation = false;
  std::uint32_t requested_alignment = 0;
};

// Mirrors the parser's contextual-name rule so semantic token-span extraction
// accepts `c`, `type`, and constraint spellings where the grammar accepts them.
[[nodiscard]] bool token_is_contextual_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
         kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
         kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber;
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
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), semantic_(semantic), selections_(selections),
        diagnostics_(diagnostics),
        states_(semantic.symbols.symbol_count(), ResolutionState::Unvisited) {}

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

private:
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

  // Appends provider-independent metadata without changing runtime layout or
  // control flow. owner is invalid only for an anonymous aggregate.
  void add_site(
      SemanticSiteKind kind,
      const SyntaxTree &tree,
      NodeId node,
      ScopeId scope,
      SymbolId owner) {
    semantic_.sites.push_back({kind, {tree.file(), node}, scope, owner, {}});
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

  // Parses the exact nonnegative integer-literal subset currently sufficient for
  // array/SIMD lengths. General constant expressions are delegated to the next
  // constant-evaluation pass rather than approximated here.
  [[nodiscard]] std::optional<std::uint64_t> integer_literal(
      const SyntaxTree &tree, NodeId expression_id) const {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind != NodeKind::LiteralExpression ||
        expression.token_begin >= expression.token_end) {
      return std::nullopt;
    }
    const Token &token = tree.token(expression.token_begin);
    if (token.kind != TokenKind::IntegerLiteral) {
      return std::nullopt;
    }
    const std::string_view spelling = sources_.text(token.range);
    std::size_t index = 0;
    std::uint32_t base = 10;
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
      if (character == '_') {
        continue;
      }
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
          value > (std::numeric_limits<std::uint64_t>::max() - digit) / base) {
        return std::nullopt;
      }
      value = value * base + digit;
      saw_digit = true;
    }
    if (!saw_digit) {
      return std::nullopt;
    }
    return value;
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

  [[nodiscard]] bool type_satisfies_constraint(
      TypeId argument, TypeConstraintKind constraint) const {
    if (!argument.is_valid()) return false;
    const TypeKind kind = semantic_.types.type(argument).kind;
    if (kind == TypeKind::Invalid || kind == TypeKind::TypeParameter ||
        kind == TypeKind::UntypedInteger || kind == TypeKind::UntypedFloat) {
      return false;
    }
    if (constraint == TypeConstraintKind::AnyType) return true;
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

  [[nodiscard]] TypeId substitute_type(
      TypeId source,
      const std::vector<ResolverTypeSubstitution> &substitutions) {
    for (const ResolverTypeSubstitution &substitution : substitutions) {
      if (substitution.parameter == source) return substitution.replacement;
    }
    if (!source.is_valid()) return source;
    const Type value = semantic_.types.type(source);
    switch (value.kind) {
    case TypeKind::Pointer:
      return semantic_.types.pointer(substitute_type(value.element, substitutions));
    case TypeKind::MultiPointer:
      return semantic_.types.multi_pointer(
          substitute_type(value.element, substitutions));
    case TypeKind::Slice:
      return semantic_.types.slice(substitute_type(value.element, substitutions));
    case TypeKind::Array:
      return semantic_.types.array(
          substitute_type(value.element, substitutions), value.element_count);
    case TypeKind::Simd:
      return semantic_.types.simd(
          substitute_type(value.element, substitutions), value.element_count);
    case TypeKind::Tuple: {
      std::vector<TypeId> members;
      members.reserve(value.members.size());
      for (TypeId member : value.members) {
        members.push_back(substitute_type(member, substitutions));
      }
      return semantic_.types.tuple(members);
    }
    case TypeKind::Procedure: {
      std::vector<TypeId> parameters;
      if (!value.members.empty()) {
        parameters.reserve(value.members.size() - 1);
        for (std::size_t index = 0; index + 1 < value.members.size(); ++index) {
          parameters.push_back(substitute_type(value.members[index], substitutions));
        }
      }
      const TypeId result = value.members.empty()
          ? semantic_.types.builtins().void_type
          : substitute_type(value.members.back(), substitutions);
      return semantic_.types.procedure(
          parameters, result, value.c_calling_convention);
    }
    default:
      return source;
    }
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
      std::vector<TypeId> arguments,
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
    substitutions.reserve(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      const ParametricParameterRecord &parameter = parameters[index];
      if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
        diagnostics_.error(
            use_range,
            "compile-time value type parameters are not yet instantiated");
        return semantic_.types.builtins().invalid;
      }
      if (!type_satisfies_constraint(arguments[index], parameter.constraint)) {
        diagnostics_.error(
            use_range,
            "type argument does not satisfy its parametric constraint");
        return semantic_.types.builtins().invalid;
      }
      substitutions.push_back({
          semantic_.symbols.symbol(parameter.parameter).type,
          arguments[index],
      });
    }

    for (const ParametricTypeInstanceRecord &instance :
         semantic_.parametric_type_instances) {
      if (instance.source == source && instance.arguments == arguments) {
        return semantic_.symbols.symbol(instance.instance).type;
      }
    }

    const Type template_type = semantic_.types.type(template_symbol.type);
    if (template_type.kind != TypeKind::Struct &&
        template_type.kind != TypeKind::Enum &&
        template_type.kind != TypeKind::TaggedUnion &&
        template_type.kind != TypeKind::RawUnion) {
      // Parametric aliases are purely structural and therefore need no member
      // scope or nominal instance identity.
      return substitute_type(template_symbol.type, substitutions);
    }

    std::string instance_name = template_symbol.name + "$instance";
    for (TypeId argument : arguments) {
      instance_name += "$" + std::to_string(argument.value);
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
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner != source) continue;
      Symbol concrete_member = semantic_.symbols.symbol(member.member);
      const SymbolId template_member = member.member;
      concrete_member.scope = member_scope;
      concrete_member.type = substitute_type(concrete_member.type, substitutions);
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
    TypeId element = substitute_type(template_type.element, substitutions);
    semantic_.types.type_mut(concrete).element = element;
    if (template_type.kind == TypeKind::Struct) {
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
    std::vector<TypeId> arguments;
    arguments.reserve(argument_nodes.size());
    for (NodeId argument : argument_nodes) {
      arguments.push_back(resolve_type_argument(tree, argument, scope));
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
      return symbol.type;
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
      return symbol.type;
    }
    diagnostics_.error(names.front().range, "imported package interface is unavailable");
    return semantic_.types.builtins().invalid;
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
      const std::optional<std::uint64_t> count = integer_literal(tree, node.children.front());
      if (!count.has_value() || *count == 0) {
        diagnostics_.error(
            tree.node(node.children.front()).range,
            "array length must be a nonzero compile-time u64 integer");
        return invalid;
      }
      return semantic_.types.array(
          resolve_type(tree, node.children.back(), scope), *count);
    }

    case NodeKind::SimdType: {
      if (node.children.size() < 2) {
        return invalid;
      }
      const std::optional<std::uint64_t> lanes = integer_literal(tree, node.children.front());
      if (!lanes.has_value() || *lanes == 0) {
        diagnostics_.error(
            tree.node(node.children.front()).range,
            "SIMD lane count must be a nonzero compile-time u64 integer");
        return invalid;
      }
      return semantic_.types.simd(
          resolve_type(tree, node.children.back(), scope), *lanes);
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

  // Enum values participate in layout before the general constant pass runs.
  // This evaluator covers the integer constant vocabulary and earlier members
  // without introducing host-width arithmetic into backing-type selection.
  [[nodiscard]] std::optional<BigInteger> enum_integer_expression(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind == NodeKind::LiteralExpression &&
        expression.token_begin < expression.token_end &&
        tree.token(expression.token_begin).kind == TokenKind::IntegerLiteral) {
      return BigInteger::parse_literal(
          sources_.text(tree.token(expression.token_begin).range));
    }
    if (expression.kind == NodeKind::GroupExpression &&
        !expression.children.empty()) {
      return enum_integer_expression(tree, expression.children.front(), scope);
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
      return std::nullopt;
    }
    if (expression.kind == NodeKind::UnaryExpression &&
        !expression.children.empty()) {
      const std::optional<BigInteger> operand =
          enum_integer_expression(tree, expression.children.front(), scope);
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
        enum_integer_expression(tree, expression.children[0], scope);
    const std::optional<BigInteger> right =
        enum_integer_expression(tree, expression.children[1], scope);
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
          const std::optional<BigInteger> value = enum_integer_expression(
              tree, attribute.children.front(), scope);
          const std::optional<std::uint64_t> alignment =
              value.has_value() ? value->to_u64() : std::nullopt;
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
            enum_integer_expression(tree, member.children.front(), scope);
        if (!explicit_value.has_value()) {
          diagnostics_.error(
              tree.node(member.children.front()).range,
              "enum value must be a compile-time integer expression");
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

    MemberData data;
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
        TypeId backing = explicit_backing.has_value()
            ? resolve_type(tree, *explicit_backing, parent)
            : (attributes.c_representation
                   ? semantic_.types.find_builtin("i32").value_or(
                         semantic_.types.builtins().invalid)
                   : inferred_enum_backing(data.enum_values));
        if (!semantic_.types.is_integer(backing)) {
          diagnostics_.error(aggregate.range, "enum backing type must be an integer type");
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
            ? resolve_type(tree, *explicit_backing, parent)
            : inferred_discriminator(data.symbols.size());
        if (!semantic_.types.is_integer(discriminator)) {
          diagnostics_.error(
              aggregate.range, "tagged-union discriminator must be an integer type");
          discriminator = semantic_.types.builtins().invalid;
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
    state = ResolutionState::Resolving;

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
    const std::optional<NodeId> payload = declaration_payload(tree, declaration);
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
      const SyntaxNode &value = tree.node(*payload);
      if (value.kind == NodeKind::NameExpression) {
        if (const std::optional<TypeId> type = try_named_type(tree, value, semantic_parent)) {
          semantic_.symbols.symbol_mut(id).kind = SymbolKind::Type;
          semantic_.symbols.symbol_mut(id).type = *type;
        } else {
          semantic_.symbols.symbol_mut(id).kind = SymbolKind::Constant;
        }
      } else {
        semantic_.symbols.symbol_mut(id).kind = SymbolKind::Constant;
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
    DiagnosticSink &diagnostics) {
  TypeResolver resolver(sources, loaded, package, selections, diagnostics);
  return resolver.resolve_one_type(tree, type, scope);
}

} // namespace draft
