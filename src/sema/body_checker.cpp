// Runtime body checking and structured typed-HIR construction.
//
// This module consumes one immutable declaration SemanticPackage, its package
// constants, selected `when` regions, and parsed source. Body work owns an
// enriched semantic generation containing lexical scopes, locals, concrete
// generic procedure instances, body agent sites, and lexical compile-time
// constants. Each exact procedure root owns a separate typed-HIR arena whose
// semantic IDs refer to that enriched generation. No ABI, storage, MIR, LLVM,
// or provider operation belongs here.
//
// A retained successful generation may be extended only with newly demanded
// concrete generic instances. Existing authored and concrete procedure arenas
// are not revisited; current-program selection decides which immutable products
// reach later phases. Definite-initialization and agent loop-range analyses run
// inside the procedure task while its local HIR IDs and semantic suffix are
// still paired. Diagnostic-only package expression validation and early
// compile-time dependency checks use private copies and cannot mutate the
// compiler's authoritative declaration baseline. Relevant rules are
// specification section 1's procedure/compile-time semantics, section 3's
// typed synthesis sites, and section 10's semantic dependency order.

#include "sema/body_checker.h"

#include "sema/body_publication.h"

#include "sema/agent_flow.h"
#include "sema/ieee_float.h"
#include "sema/initialization.h"
#include "sema/runtime_context.h"
#include "sema/type_resolver.h"
#include "sema/type_inspection.h"
#include "sema/target_validation.h"
#include "syntax/literal.h"
#include "syntax/token.h"

#include <algorithm>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
         kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory ||
         kind == TokenKind::KeywordBits;
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
  case NodeKind::VariantType:
  case NodeKind::UnionType:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] std::optional<TypeKind> nominal_type_kind(NodeKind kind) {
  switch (kind) {
  case NodeKind::StructType: return TypeKind::Struct;
  case NodeKind::EnumType: return TypeKind::Enum;
  case NodeKind::VariantType: return TypeKind::Variant;
  case NodeKind::UnionType: return TypeKind::Union;
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
  // to an expression over caller value parameters rather than an integer.
  // Concrete instances leave this invalid and carry the exact value above.
  IntegerExpression symbolic_expression;
  // Calls and other full compile-time expressions do not fit the compact
  // IntegerExpression tree. A template body still type-checks their source,
  // then carries this marker only in its non-lowered symbolic HIR. The same
  // source is checked again inside every concrete outer instance, where the
  // active parameter table lets the ordinary interpreter produce an exact
  // value. No source coordinate or marker reaches a procedure instance seed.
  bool deferred_expression = false;
};

// Result of checking one bracketed procedure-argument list without creating an
// instance. `valid` distinguishes an empty legal list from a diagnosed failure;
// the two vectors retain declaration-parameter identity and source order for
// the later fixed-prefix and static-tail call operation.
struct ExplicitProcedureArguments {
  bool valid = false;
  std::vector<TypeSubstitution> type_substitutions;
  std::vector<ValueSubstitution> value_substitutions;
};

// One checked direct call first binds surface arguments to fixed physical
// parameters. Explicit rows remain in source order; default_parameter_indices
// are appended later in declaration order as constant HIR operands. The final
// mapping is a permutation even when named arguments reorder the fixed prefix.
struct BoundProcedureArguments {
  bool valid = false;
  std::vector<NodeId> explicit_expressions;
  std::vector<std::uint32_t> explicit_parameter_indices;
  std::vector<std::uint32_t> default_parameter_indices;
};

// One source template can produce several concrete procedure bodies. Instances
// retain both substitution kinds here while the permanent semantic graph owns
// the concrete symbol, signature, and cloned runtime-parameter scope used by
// later passes. Equality is semantic: types compare by canonical TypeId and
// values compare by their exact ConstantValue representation, and pack types
// compare as an ordered vector. This is the active, checker-local form of the
// durable ParametricInstanceRecord. A fresh checker reconstructs exactly the
// instance it is about to check; instances discovered by that body are retained
// in SemanticPackage for later roots. All referenced symbols and types live for
// the complete canonical SemanticPackage tables owned by its body work state.
struct ProcedureInstance {
  SymbolId source;
  SymbolId symbol;
  std::vector<TypeSubstitution> type_substitutions;
  std::vector<ValueSubstitution> value_substitutions;
  // Ordered concrete tail types. They define specialization identity and match
  // the trailing members of the concrete procedure signature one-for-one.
  std::vector<TypeId> pack_types;
  // These are the ordinary parameters appended to the concrete procedure
  // scope and signature. Static loop expansion aliases its per-iteration value
  // binding directly to the corresponding symbol.
  std::vector<SymbolId> pack_parameters;
  // The concrete procedure scope also owns one compile-time-only marker under
  // the source pack name. Constant evaluation uses it for len(pack); ordinary
  // expression checking rejects every other use before HIR lowering.
  SymbolId pack_binding;
};

// ProcedureInstanceActivation distinguishes an authored procedure from a
// concrete instance whose retained environment failed internal reconstruction.
// An empty optional alone could not express that difference safely: treating a
// malformed concrete symbol as an authored procedure would check its source
// template without the required substitutions and manufacture misleading user
// diagnostics.
struct ProcedureInstanceActivation {
  bool concrete = false;
  std::optional<std::size_t> index;
};

using ProcedureBodyRoot = ProcedureBodyWorkItem;

// Converts the checker-local active form into the durable work packet exposed
// at the phase boundary. Symbolic/deferred value substitutions cannot appear in
// a concrete active environment; instantiate_procedure resolves them before a
// concrete instance is created. Copying only exact values makes that invariant
// visible in the public work representation.
[[nodiscard]] ProcedureBodyEnvironment retain_body_environment(
    const ProcedureInstance &instance) {
  ProcedureBodyEnvironment result;
  result.source = instance.source;
  result.symbol = instance.symbol;
  result.pack_types = instance.pack_types;
  result.pack_parameters = instance.pack_parameters;
  result.pack_binding = instance.pack_binding;
  result.type_substitutions.reserve(instance.type_substitutions.size());
  for (const TypeSubstitution &substitution : instance.type_substitutions) {
    result.type_substitutions.push_back(
        {substitution.parameter, substitution.replacement});
  }
  result.value_substitutions.reserve(instance.value_substitutions.size());
  for (const ValueSubstitution &substitution : instance.value_substitutions) {
    result.value_substitutions.push_back(
        {substitution.parameter, substitution.value});
  }
  return result;
}

// Reconstructs the small checker-local view from a retained nested-root
// packet. All IDs still address the PackageBodyWorkState which owns the root;
// no lookup, interning, or semantic mutation occurs during this conversion.
[[nodiscard]] ProcedureInstance activate_body_environment(
    const ProcedureBodyEnvironment &environment) {
  ProcedureInstance result;
  result.source = environment.source;
  result.symbol = environment.symbol;
  result.pack_types = environment.pack_types;
  result.pack_parameters = environment.pack_parameters;
  result.pack_binding = environment.pack_binding;
  result.type_substitutions.reserve(environment.type_substitutions.size());
  for (const ConcreteProcedureTypeSubstitution &substitution :
       environment.type_substitutions) {
    result.type_substitutions.push_back(
        {substitution.parameter, substitution.replacement});
  }
  result.value_substitutions.reserve(environment.value_substitutions.size());
  for (const ConcreteProcedureValueSubstitution &substitution :
       environment.value_substitutions) {
    result.value_substitutions.push_back(
        {substitution.parameter, substitution.value, {}});
  }
  return result;
}

// One exact root returns ordinary append-only HIR plus later lexical roots. It
// deliberately does not run the discovered roots itself. The package
// coordinator publishes them after this result, which makes nested procedure
// bodies visible work rather than hidden recursion inside check_procedure.
struct ProcedureBodyRootResult {
  bool ok = false;
  std::size_t checked_procedures = 0;
  HirProgram program;
  std::vector<ProcedureBodyRoot> discovered_roots;
};

// During one concrete static-pack iteration the source value name denotes one
// already-existing ordinary procedure parameter. The lexical alias still owns
// source visibility and diagnostics, while this short-lived row prevents HIR
// from inventing a local copy or storage slot for the alias.
struct StaticPackValueAlias {
  SymbolId alias;
  SymbolId parameter;
};

// One symbolic fact established by entering a parameter-dependent `when`
// branch. The subject is the ordinary value binding observed by type_of. An
// exact type permits full ordinary checking as that type; allowed_kinds grants
// only the common capability of the remaining Type_Kind set. False exact-type
// branches record exclusions so a chained else-when retains the complement of
// every earlier test without mutating the declaration's public constraint.
struct ActiveTypeRefinement {
  SymbolId subject;
  std::optional<TypeId> exact_type;
  std::vector<TypeKind> allowed_kinds;
  std::vector<TypeId> excluded_exact_types;
};

enum class TypeRefinementPredicateKind {
  ExactType,
  TypeKind,
};

// TypeRefinementPredicate is the recognized, side-effect-free subset of a
// dependent condition which can grant capabilities while symbolically checking
// a branch. Conditions outside this subset are still selected concretely; they
// simply grant no extra type operations during the symbolic pass.
struct TypeRefinementPredicate {
  TypeRefinementPredicateKind kind = TypeRefinementPredicateKind::ExactType;
  SymbolId subject;
  TypeId exact_type;
  TypeKind type_kind = TypeKind::Invalid;
  // Equality makes the true branch the matching branch; inequality reverses
  // the matching and complement facts without changing either fact itself.
  bool true_branch_matches = true;
};

// BodyChecker is one sequential body-generation context. It mutates the
// body-owned SemanticPackage and ConstantTable supplied by the public wrapper
// and owns the HIR under construction. Source, syntax, selections, target, and
// diagnostics remain caller-owned. SymbolTable and constant/type tables may
// grow, so operations retain stable IDs and copy source records instead of
// holding element references across appends.
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

  // Establishes the body-owned runtime context and materializes external
  // specialization seeds without checking any procedure. The public package
  // wrapper invokes this operation once, snapshots the resulting concrete
  // instance records as work roots, and then gives every root a fresh
  // BodyChecker. Keeping seed materialization separate prevents the first
  // authored procedure from accidentally owning unrelated external work.
  [[nodiscard]] bool initialize() {
    const std::size_t initial_errors = diagnostics_.error_count();
    ensure_runtime_context_type(semantic_, diagnostics_);
    instantiate_seeded_procedures();
    return diagnostics_.error_count() == initial_errors;
  }

  // Checks one package-level authored procedure or one retained concrete
  // instance and nothing from the surrounding package work list. A concrete
  // instance rebuilds its active substitutions and pack bindings from the
  // permanent ParametricInstanceRecord before source checking starts. Calls in
  // this body may append new instance records, but their bodies are deliberately
  // left for later roots. A nested declaration likewise publishes a later root
  // with its enclosing concrete environment instead of entering its body
  // recursively.
  [[nodiscard]] ProcedureBodyRootResult run_one(
      const ProcedureBodyRoot &root) {
    ProcedureBodyRootResult result;
    const std::size_t initial_errors = diagnostics_.error_count();
    ensure_runtime_context_type(semantic_, diagnostics_);
    const ProcedureInstanceActivation instance = restore_instance(root.symbol);
    if (instance.index.has_value()) current_instance_index_ = *instance.index;
    if (!instance.concrete && root.enclosing_environment.has_value()) {
      instances_.push_back(
          activate_body_environment(*root.enclosing_environment));
      current_instance_index_ = instances_.size() - 1;
    }
    const Symbol symbol = semantic_.symbols.symbol(root.symbol);
    if ((!instance.concrete || instance.index.has_value()) &&
        symbol.kind == SymbolKind::Procedure && symbol.type.is_valid() &&
        check_procedure(root.symbol, root.parametric_template)) {
      ++result.checked_procedures;
    }
    current_instance_index_.reset();
    result.ok = diagnostics_.error_count() == initial_errors;
    result.program = std::move(hir_);
    result.discovered_roots = std::move(discovered_body_roots_);
    return result;
  }

  [[nodiscard]] bool validate_package_compile_time_expression_types() {
    const std::size_t initial_errors = diagnostics_.error_count();
    type_validation_only_ = true;
    std::vector<SyntaxReference> checked;

    // Several symbols can share one declaration syntax (a grouped binding or
    // tuple pattern). Check its initializer once, in stable package-symbol
    // order, so one source error produces one diagnostic.
    const std::vector<SymbolId> package_symbols =
        semantic_.symbols.symbols_in_scope(semantic_.package_scope);
    for (SymbolId id : package_symbols) {
      const Symbol symbol = semantic_.symbols.symbol(id);
      if (symbol.kind != SymbolKind::Constant &&
          symbol.kind != SymbolKind::UnresolvedDeclaration &&
          symbol.kind != SymbolKind::Variable) {
        continue;
      }
      if (!symbol.syntax.file.is_valid() || !symbol.syntax.node.is_valid()) {
        continue;
      }
      if (std::find(checked.begin(), checked.end(), symbol.syntax) !=
          checked.end()) {
        continue;
      }
      checked.push_back(symbol.syntax);

      const SyntaxTree *tree = find_tree(symbol.syntax.file);
      const std::optional<ScopeId> scope = source_file_scope(symbol.syntax.file);
      if (tree == nullptr || !scope.has_value()) continue;
      const SyntaxNode &declaration = tree->node(symbol.syntax.node);
      if (declaration.kind != NodeKind::Declaration ||
          declaration.children.size() < 2) {
        continue;
      }
      const NodeId initializer = declaration.children.back();
      const NodeKind initializer_kind = tree->node(initializer).kind;
      if (node_is_type_syntax(initializer_kind) ||
          initializer_kind == NodeKind::BindingPattern ||
          initializer_kind == NodeKind::TuplePattern ||
          initializer_kind == NodeKind::ParametricParameterList ||
          initializer_kind == NodeKind::UninitializedExpression) {
        continue;
      }
      // The constant evaluator already validates every expression it actually
      // executes. This preflight exists specifically for forms which suppress
      // evaluation of one or more source operands: conditional/short-circuit
      // selection and type_of's inspected expression. Keeping the entry point
      // narrow also avoids pretending that runtime HIR has values for
      // compile-time-only objects such as `target`.
      if (!initializer_requires_type_preflight(*tree, initializer)) continue;

      TypeId expected = symbol.type;
      const SyntaxNode &pattern = tree->node(declaration.children.front());
      if (pattern.kind == NodeKind::TuplePattern) {
        std::vector<TypeId> members;
        for (SymbolId candidate_id : package_symbols) {
          const Symbol &candidate = semantic_.symbols.symbol(candidate_id);
          if (candidate.syntax == symbol.syntax && candidate.type.is_valid()) {
            members.push_back(candidate.type);
          }
        }
        expected = members.size() >= 2
            ? semantic_.types.tuple(members)
            : TypeId{};
      }
      const std::size_t initial_initializer_errors = diagnostics_.error_count();
      const HirExpressionId checked_initializer =
          check_expression(*tree, initializer, *scope, expected);
      if (is_invalid_type(hir_.expression(checked_initializer).type) &&
          diagnostics_.error_count() == initial_initializer_errors) {
        // An invalid validation-only HIR value without a diagnostic would let
        // a declared constant silently retain the type inferred by constant
        // discovery. Fail closed at the source initializer instead.
        diagnostics_.error(
            tree->node(initializer).range,
            "compile-time expression has no valid static type");
      }
    }

    // Declaration and aggregate-member `when` sites are selected before body
    // checking so the chosen declarations can be collected into their
    // surrounding scopes. Constant evaluation computes the value, but
    // non-evaluating operations such as type_of may need only a declared result
    // hint and therefore do not validate their source operand. Check every
    // selected structural condition through the ordinary expression checker
    // before trusting that selection. Procedure-body conditions are checked at
    // their exact lexical program point in check_statement instead.
    // Expression checking may append denial or agent sites. Iterate a value
    // snapshot so vector growth cannot invalidate the current site reference,
    // and so newly discovered body-level sites cannot accidentally join this
    // package-structural validation round.
    const AppendOnlyTableView<SemanticSite> visible_sites =
        semantic_.sites_for_read();
    const std::vector<SemanticSite> structural_sites(
        visible_sites.begin(), visible_sites.end());
    for (const SemanticSite &site : structural_sites) {
      if (site.kind != SemanticSiteKind::ConditionalDeclaration &&
          site.kind != SemanticSiteKind::ConditionalMember) {
        continue;
      }
      if (selections_.find(site.syntax) == nullptr) continue;

      const SyntaxTree *tree = find_tree(site.syntax.file);
      if (tree == nullptr || !site.syntax.node.is_valid()) continue;
      const SyntaxNode &when = tree->node(site.syntax.node);
      if (when.children.empty()) continue;
      if (!initializer_requires_type_preflight(
              *tree, when.children.front())) {
        continue;
      }

      ScopeId condition_scope = site.scope;
      if (site.kind == SemanticSiteKind::ConditionalDeclaration &&
          condition_scope == semantic_.package_scope) {
        const std::optional<ScopeId> file_scope =
            source_file_scope(site.syntax.file);
        if (!file_scope.has_value()) continue;
        condition_scope = *file_scope;
      }
      const std::size_t initial_condition_errors = diagnostics_.error_count();
      const HirExpressionId checked_condition = check_expression(
          *tree,
          when.children.front(),
          condition_scope,
          semantic_.types.builtins().bool_type);
      if (is_invalid_type(hir_.expression(checked_condition).type) &&
          diagnostics_.error_count() == initial_condition_errors) {
        // Structural selection has already contributed its chosen declaration
        // or member. Never retain it merely because validation produced an
        // unexplained invalid HIR value.
        diagnostics_.error(
            tree->node(when.children.front()).range,
            "compile-time condition has no valid static type");
      }
    }
    type_validation_only_ = false;
    return diagnostics_.error_count() == initial_errors;
  }

private:
  // Detects package initializers whose constant result can be computed without
  // executing every contained expression. The full BodyChecker pass below is
  // what validates those suppressed expressions; the constant evaluator's
  // declared-type hints must never become an alternate source type checker.
  [[nodiscard]] bool initializer_requires_type_preflight(
      const SyntaxTree &tree, NodeId expression) const {
    const SyntaxNode &node = tree.node(expression);
    if (node.kind == NodeKind::ConditionalExpression) return true;
    if (node.kind == NodeKind::BinaryExpression) {
      const TokenKind operation = binary_operator(tree, node);
      if (operation == TokenKind::LogicalAnd ||
          operation == TokenKind::LogicalOr) {
        return true;
      }
    }
    if (node.kind == NodeKind::CallExpression && !node.children.empty()) {
      const std::optional<SourceName> callee =
          single_name_expression(tree, node.children.front());
      if (callee.has_value() && callee->text == "type_of") return true;
    }
    for (NodeId child : node.children) {
      if (initializer_requires_type_preflight(tree, child)) return true;
    }
    return false;
  }

  // Returns the selector of a direct reference to the predeclared target
  // object. A lexical declaration named `target` wins, exactly as it does in
  // ordinary name lookup, so validation never assigns target-profile meaning
  // to a user value merely because it has the conventional spelling.
  [[nodiscard]] std::optional<std::string> direct_target_member(
      const SyntaxTree &tree, NodeId expression, ScopeId scope) const {
    const SyntaxNode &member = tree.node(expression);
    if (member.kind != NodeKind::MemberExpression ||
        member.children.empty()) {
      return std::nullopt;
    }
    NodeId base_id = member.children.front();
    while (tree.node(base_id).kind == NodeKind::GroupExpression &&
           tree.node(base_id).children.size() == 1) {
      // Parentheses preserve the identity of the predeclared target object.
      // Unwrap only that transparent syntax; member access, calls, and every
      // other expression shape still follow ordinary lexical lookup.
      base_id = tree.node(base_id).children.front();
    }
    const std::optional<SourceName> base =
        single_name_expression(tree, base_id);
    if (!base.has_value() || base->text != "target" ||
        semantic_.symbols.lookup(scope, "target").has_value()) {
      return std::nullopt;
    }
    const std::vector<SourceName> names = alternative_names_in_span(
        tree, member.token_begin, member.token_end);
    if (names.empty()) return std::nullopt;
    return names.back().text;
  }

  // Runtime HIR intentionally has no value representing the predeclared
  // `target` object, but each target fact and query is an ordinary compile-time
  // scalar which may be materialized in executable source. The validation-only
  // pass additionally needs to import already-evaluated constant bindings so a
  // complete selected `when` condition can be checked without short-circuiting
  // away malformed source. Import only those exact leaves from the
  // authoritative constant evaluator:
  //
  //   target.pointer_bits
  //   target.has_feature("neon")
  //   target.os == .macos
  //
  // The candidate set is deliberately structural and small. In particular,
  // this function never folds a whole logical or conditional expression: doing
  // so could skip another operand which this pass exists to validate. Each
  // target field becomes an ordinary typed constant, including categorical
  // fields whose compiler-defined enum type then supplies context to a nearby
  // alternative. This is enough for grouped comparisons and target-to-target
  // comparisons without duplicating the target type model in BodyChecker.
  //
  // Target candidates use the diagnosing evaluator rather than discovery.
  // This distinction matters for a short-circuited invalid feature query: the
  // ordinary selection evaluator never visits that operand, but preflight must
  // still report the authoritative unrecognized-feature diagnostic rather than
  // a secondary runtime-HIR error about the predeclared target object.
  [[nodiscard]] std::optional<HirExpressionId>
  check_compile_time_leaf_expression(
      const SyntaxTree &tree,
      NodeId expression_id,
      ScopeId scope,
      TypeId expected) {
    const SyntaxNode &expression = tree.node(expression_id);
    bool candidate = false;
    if (type_validation_only_ && expression.kind == NodeKind::NameExpression) {
      const std::optional<SourceName> name =
          single_name_expression(tree, expression_id);
      const std::optional<SymbolId> symbol = name.has_value()
          ? semantic_.symbols.lookup(scope, name->text)
          : std::nullopt;
      candidate = symbol.has_value() &&
          (constants_.find(*symbol) != nullptr ||
           active_constant(*symbol) != nullptr);
    } else if (direct_target_member(tree, expression_id, scope).has_value()) {
      candidate = true;
    } else if (expression.kind == NodeKind::CallExpression &&
               !expression.children.empty()) {
      const std::optional<std::string> called_member = direct_target_member(
          tree, expression.children.front(), scope);
      candidate = called_member.has_value();

      if (candidate && *called_member == "has_feature") {
        // The constant evaluator owns the call contract and selected feature
        // value, but evaluating the complete call would skip any unselected
        // branch inside an argument such as
        //
        //   target.has_feature("neon" if true else 42)
        //
        // Preflight every supplied argument as an ordinary expression before
        // importing the folded call. Do not impose the string context here:
        // the evaluator will issue its precise arity/type/feature diagnostic
        // for the selected value, while ordinary conditional checking still
        // rejects incompatible hidden branches without duplicating the call's
        // contract error for a simple `target.has_feature(42)`.
        for (std::size_t index = 1;
             index < expression.children.size();
             ++index) {
          (void)check_expression(
              tree, expression.children[index], scope);
        }
      }
    }
    if (!candidate) return std::nullopt;

    const ConstantTable visible_constants = active_constant_table();
    const std::vector<ConstantTypeBinding> visible_types =
        active_constant_types();
    const std::vector<ConstantStaticPackBinding> visible_packs =
        active_constant_packs();
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
            &visible_types,
            expected,
            &visible_packs);
    if (!evaluated.has_value() || !evaluated->type.is_valid() ||
        is_invalid_type(evaluated->type)) {
      HirExpression invalid;
      invalid.kind = HirExpressionKind::Invalid;
      invalid.range = expression.range;
      invalid.type = semantic_.types.builtins().invalid;
      return hir_.add_expression(std::move(invalid));
    }

    HirExpression checked;
    checked.kind = HirExpressionKind::Constant;
    checked.range = expression.range;
    checked.constant = evaluated->value;
    checked.type = apply_expected_type(
        evaluated->type, expected, expression.range);
    contextualize_constant_value(
        checked.constant,
        evaluated->type,
        checked.type,
        expression.range);
    return hir_.add_expression(std::move(checked));
  }

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
          if (argument.is_type || argument.value_expression.is_valid() ||
              argument.owner_evaluated_value) {
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
          seed.pack_types,
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

  [[nodiscard]] std::optional<ScopeId> source_file_scope(FileId file) const {
    for (const FileSemanticScope &entry : semantic_.files_for_read()) {
      if (entry.file == file) return entry.scope;
    }
    return std::nullopt;
  }

  // Locates a scope already owned by a declaration during signature resolution.
  [[nodiscard]] std::optional<ScopeId> owned_scope(
      SymbolId owner, ScopeKind kind) const {
    for (const OwnedSemanticScope &entry : semantic_.owned_scopes_for_read()) {
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

  // Binds one call's syntax to a declaration's fixed parameter rows without
  // checking operand expressions. This separation is important for generics:
  // the checker must know which declared type pattern receives each explicit
  // operand before it can infer substitutions. Positional operands must precede
  // named operands; a static pack is therefore always one source-ordered tail.
  [[nodiscard]] BoundProcedureArguments bind_procedure_arguments(
      const SyntaxTree &tree,
      const SyntaxNode &call,
      const Symbol *declaration,
      std::size_t fixed_parameter_count,
      bool has_static_pack) {
    BoundProcedureArguments result;
    const std::size_t initial_errors = diagnostics_.error_count();
    const std::vector<ProcedureParameter> *parameters = declaration == nullptr
        ? nullptr
        : &declaration->procedure_parameters;
    if (parameters != nullptr && !parameters->empty() &&
        parameters->size() != fixed_parameter_count) {
      diagnostics_.error(
          call.range,
          "procedure call metadata does not match its signature");
      return result;
    }

    std::vector<bool> bound(fixed_parameter_count, false);
    std::size_t next_positional = 0;
    std::size_t pack_count = 0;
    bool saw_named = false;
    for (std::size_t source_index = 1;
         source_index < call.children.size(); ++source_index) {
      const NodeId argument_id = call.children[source_index];
      const SyntaxNode &argument = tree.node(argument_id);
      if (argument.kind == NodeKind::NamedArgument) {
        saw_named = true;
        if (parameters == nullptr || parameters->empty()) {
          diagnostics_.error(
              argument.range,
              "named arguments require a direct procedure declaration");
          continue;
        }
        if (argument.children.size() != 1 ||
            argument.token_begin >= argument.token_end) {
          diagnostics_.error(argument.range, "malformed named argument");
          continue;
        }
        const SourceName name = token_name(tree, argument.token_begin);
        std::optional<std::size_t> parameter_index;
        for (std::size_t index = 0; index < parameters->size(); ++index) {
          if (!(*parameters)[index].name.empty() &&
              (*parameters)[index].name == name.text) {
            parameter_index = index;
            break;
          }
        }
        if (!parameter_index.has_value()) {
          diagnostics_.error(
              name.range,
              "procedure has no parameter named '" + name.text + "'");
          continue;
        }
        if (bound[*parameter_index]) {
          diagnostics_.error(
              name.range,
              "procedure parameter '" + name.text +
                  "' is supplied more than once");
          continue;
        }
        bound[*parameter_index] = true;
        result.explicit_expressions.push_back(argument.children.front());
        result.explicit_parameter_indices.push_back(
            static_cast<std::uint32_t>(*parameter_index));
        continue;
      }

      if (saw_named) {
        diagnostics_.error(
            argument.range,
            "positional argument cannot follow a named argument");
        continue;
      }
      if (next_positional < fixed_parameter_count) {
        bound[next_positional] = true;
        result.explicit_expressions.push_back(argument_id);
        result.explicit_parameter_indices.push_back(
            static_cast<std::uint32_t>(next_positional));
        ++next_positional;
        continue;
      }
      if (!has_static_pack) {
        diagnostics_.error(
            argument.range,
            "procedure call has too many arguments");
        continue;
      }
      result.explicit_expressions.push_back(argument_id);
      result.explicit_parameter_indices.push_back(static_cast<std::uint32_t>(
          fixed_parameter_count + pack_count));
      ++pack_count;
    }

    for (std::size_t index = 0; index < fixed_parameter_count; ++index) {
      if (bound[index]) continue;
      if (parameters != nullptr && !parameters->empty() &&
          (*parameters)[index].has_default &&
          (*parameters)[index].default_is_ready) {
        result.default_parameter_indices.push_back(
            static_cast<std::uint32_t>(index));
        continue;
      }
      const std::string detail = parameters != nullptr && !parameters->empty() &&
              !(*parameters)[index].name.empty()
          ? " '" + (*parameters)[index].name + "'"
          : std::string();
      diagnostics_.error(
          call.range,
          "procedure call is missing required argument" + detail);
    }

    result.valid = diagnostics_.error_count() == initial_errors;
    return result;
  }

  // Materializes one already closed default as an ordinary constant operand.
  // It intentionally has no source expression to execute at the call site:
  // declaration finalization proved and converted the value once, and the
  // package interface carries that exact value to every consumer.
  [[nodiscard]] HirExpressionId default_argument_expression(
      const Symbol &declaration,
      std::uint32_t parameter_index,
      TypeId parameter_type,
      SourceRange call_range) {
    assert(parameter_index < declaration.procedure_parameters.size());
    const ProcedureParameter &parameter =
        declaration.procedure_parameters[parameter_index];
    assert(parameter.has_default && parameter.default_is_ready);
    HirExpression expression;
    expression.kind = HirExpressionKind::Constant;
    expression.type = parameter_type;
    expression.range = parameter.name_range.is_valid()
        ? parameter.name_range
        : call_range;
    expression.constant = parameter.default_value;
    return hir_.add_expression(std::move(expression));
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

  // A contextual alternative has a deliberately narrower keyword exception
  // than an ordinary source name. Keep that exception out of names_in_span so
  // `struct` and `distinct` cannot start behaving like declaration, package,
  // or field names merely because Type_Kind exposes `.struct` and `.distinct`.
  [[nodiscard]] std::vector<SourceName> alternative_names_in_span(
      const SyntaxTree &tree, std::uint32_t begin, std::uint32_t end) const {
    std::vector<SourceName> names;
    for (std::uint32_t index = begin; index < end; ++index) {
      if (token_is_contextual_alternative_name(tree.token(index).kind)) {
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

  // Body sites snapshot static path facts while the checker is inside their
  // structured branch. The semantic site owns only process-local references;
  // canonical source and portable type graphs are built later with the rest of
  // the provider obligation. Denial sites use their separate lexical model and
  // therefore do not pass through this helper.
  void add_body_agent_site(
      SemanticSiteKind kind,
      SyntaxReference syntax,
      ScopeId scope,
      TypeId expected_type = {}) {
    if (current_instance_index_.has_value()) {
      // A synthesis/judgment site replaces or validates source owned by the
      // template, not a separately generated copy of that source for every
      // monomorphization. The symbolic template pass already recorded the site
      // with its parameter contract and active branch refinements. Concrete
      // bodies consume the accepted expansion while ordinary specialization
      // checking verifies its selected path.
      return;
    }
    SemanticSite site;
    site.kind = kind;
    site.syntax = syntax;
    site.scope = scope;
    site.anchor = current_procedure_;
    site.expected_type = expected_type;
    site.branch_refinements = active_branch_refinements_;
    semantic_.sites.push_back(std::move(site));
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
    std::vector<TypeKind> possible;
    for (const ActiveTypeRefinement &refinement : active_type_refinements_) {
      if (!refinement.subject.is_valid() ||
          semantic_.symbols.symbol(refinement.subject).type != type) {
        continue;
      }
      if (refinement.exact_type.has_value()) {
        possible = {runtime_scalar_type(*refinement.exact_type).kind};
      }
      if (!refinement.allowed_kinds.empty()) {
        if (possible.empty()) {
          possible = refinement.allowed_kinds;
        } else {
          std::vector<TypeKind> intersection;
          for (TypeKind candidate : possible) {
            if (std::find(
                    refinement.allowed_kinds.begin(),
                    refinement.allowed_kinds.end(),
                    candidate) != refinement.allowed_kinds.end()) {
              intersection.push_back(candidate);
            }
          }
          possible = std::move(intersection);
        }
      }
    }
    if (!possible.empty()) {
      const bool all_integer = std::all_of(
          possible.begin(), possible.end(), [](TypeKind kind) {
            return kind == TypeKind::SignedInteger ||
                kind == TypeKind::UnsignedInteger;
          });
      if (all_integer) return TypeConstraintKind::Integer;
      const bool all_float = std::all_of(
          possible.begin(), possible.end(), [](TypeKind kind) {
            return kind == TypeKind::Float;
          });
      if (all_float) return TypeConstraintKind::Float;
      const bool all_number = std::all_of(
          possible.begin(), possible.end(), [](TypeKind kind) {
            return kind == TypeKind::SignedInteger ||
                kind == TypeKind::UnsignedInteger || kind == TypeKind::Float;
          });
      if (all_number) return TypeConstraintKind::Number;
    }
    for (const ParametricParameterRecord &parameter :
         semantic_.parametric_parameters_for_read()) {
      const Symbol &symbol = semantic_.symbols.symbol(parameter.parameter);
      if (symbol.type == type) return parameter.constraint;
    }
    return std::nullopt;
  }

  // Exact refinements change only uses of the named value inside the active
  // branch. The declaration and its public generic signature retain the
  // original TypeParameter; no branch-local fact leaks into interface identity.
  [[nodiscard]] TypeId refined_symbol_type(
      SymbolId symbol, TypeId declared) const {
    std::vector<TypeId> excluded;
    for (const ActiveTypeRefinement &refinement : active_type_refinements_) {
      if (refinement.subject != symbol) continue;
      excluded.insert(
          excluded.end(),
          refinement.excluded_exact_types.begin(),
          refinement.excluded_exact_types.end());
    }
    for (auto refinement = active_type_refinements_.rbegin();
         refinement != active_type_refinements_.rend(); ++refinement) {
      if (refinement->subject != symbol) continue;
      if (refinement->exact_type.has_value() &&
          std::find(
              excluded.begin(), excluded.end(), *refinement->exact_type) ==
              excluded.end()) {
        return *refinement->exact_type;
      }
    }
    return declared;
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
      const std::vector<ValueSubstitution> &substitutions) const {
    std::vector<IntegerExpressionReplacement> result;
    result.reserve(substitutions.size());
    for (const ValueSubstitution &substitution : substitutions) {
      if (substitution.deferred_expression) continue;
      IntegerExpressionReplacement replacement;
      replacement.parameter = substitution.parameter.value;
      if (substitution.symbolic_expression.is_valid()) {
        replacement.expression = substitution.symbolic_expression;
      } else if (substitution.value.kind == ConstantKind::Integer) {
        replacement.value = substitution.value.integer;
      }
      result.push_back(std::move(replacement));
    }
    return result;
  }

  [[nodiscard]] bool has_deferred_value_expression(
      const std::vector<ValueSubstitution> &substitutions) const {
    return std::any_of(
        substitutions.begin(),
        substitutions.end(),
        [](const ValueSubstitution &substitution) {
          return substitution.deferred_expression;
        });
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

    if (value.owner_evaluated_type_application) {
      // A call is intentionally outside the compact integer-expression model.
      // During symbolic template checking its source has already been checked
      // with the required integer type, but there is no concrete value to hand
      // to TypeResolver yet. Preserve the fail-closed placeholder; the outer
      // concrete procedure instance rechecks this same source and supplies the
      // exact value before any executable HIR is produced.
      if (has_deferred_value_expression(value_substitutions)) return source;
      std::vector<DeferredElementCountTypeBinding> deferred_types;
      for (const TypeSubstitution &substitution : type_substitutions) {
        deferred_types.push_back(
            {substitution.parameter, substitution.replacement});
      }
      std::vector<DeferredElementCountValueBinding> deferred_values;
      for (const ValueSubstitution &substitution : value_substitutions) {
        deferred_values.push_back({
            substitution.parameter,
            substitution.value,
            substitution.symbolic_expression,
        });
      }
      const ConstantTable visible_constants = active_constant_table();
      return instantiate_owner_evaluated_type_application(
          sources_,
          loaded_,
          semantic_,
          selections_,
          source,
          deferred_types,
          deferred_values,
          use_range,
          visible_constants,
          target_,
          diagnostics_);
    }

    // A nominal application cannot be rebuilt by recursively substituting its
    // physical members: doing so would manufacture an unrelated anonymous
    // layout. Substitute its retained arguments, then ask TypeResolver for the
    // canonical application of the original template.
    if (value.kind == TypeKind::Struct || value.kind == TypeKind::Enum ||
        value.kind == TypeKind::Variant ||
        value.kind == TypeKind::Union) {
      const std::optional<NominalApplication> application =
          nominal_application(source);
      if (application.has_value()) {
        std::vector<ParametricArgument> arguments = *application->arguments;
        const bool owner_evaluated = std::any_of(
            arguments.begin(),
            arguments.end(),
            [](const ParametricArgument &argument) {
              return argument.owner_evaluated_value;
            });
        if (owner_evaluated) {
          if (has_deferred_value_expression(value_substitutions)) return source;
          std::vector<DeferredElementCountTypeBinding> deferred_types;
          for (const TypeSubstitution &substitution : type_substitutions) {
            deferred_types.push_back(
                {substitution.parameter, substitution.replacement});
          }
          std::vector<DeferredElementCountValueBinding> deferred_values;
          for (const ValueSubstitution &substitution : value_substitutions) {
            deferred_values.push_back({
                substitution.parameter,
                substitution.value,
                substitution.symbolic_expression,
            });
          }
          const ConstantTable visible_constants = active_constant_table();
          return instantiate_owner_evaluated_type_application(
              sources_,
              loaded_,
              semantic_,
              selections_,
              source,
              deferred_types,
              deferred_values,
              use_range,
              visible_constants,
              target_,
              diagnostics_);
        }
        bool changed = false;
        for (ParametricArgument &argument : arguments) {
          if (argument.is_type) {
            const TypeId replacement = substitute_type(
                argument.type,
                type_substitutions,
                value_substitutions,
                use_range);
            changed = changed || replacement != argument.type;
            argument.type = replacement;
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
            return semantic_.types.builtins().invalid;
          }
          changed = changed || *replacement != argument.value_expression;
          argument.value_expression = *replacement;
          if (!integer_expression_has_parameters(argument.value_expression)) {
            const IntegerExpressionResult evaluated =
                evaluate_integer_expression(argument.value_expression);
            if (!evaluated.ok) {
              diagnostics_.error(use_range, evaluated.error);
              return semantic_.types.builtins().invalid;
            }
            argument.value = ConstantValue::make_integer(evaluated.value);
            argument.value_expression = {};
          }
        }
        if (!changed) return source;

        std::optional<SymbolId> template_source = application->source;
        if (!template_source.has_value() && application->imported != nullptr) {
          for (const ImportedSymbol &imported :
               semantic_.imported_symbols_for_read()) {
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
            &constants_,
            target_,
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
      if (value.owner_evaluated_element_count) {
        if (has_deferred_value_expression(value_substitutions)) return source;
        std::vector<DeferredElementCountTypeBinding> deferred_types;
        for (const TypeSubstitution &substitution : type_substitutions) {
          deferred_types.push_back(
              {substitution.parameter, substitution.replacement});
        }
        std::vector<DeferredElementCountValueBinding> deferred_values;
        for (const ValueSubstitution &substitution : value_substitutions) {
          deferred_values.push_back({
              substitution.parameter,
              substitution.value,
              substitution.symbolic_expression,
          });
        }
        const ConstantTable visible_constants = active_constant_table();
        return instantiate_owner_evaluated_type_application(
            sources_,
            loaded_,
            semantic_,
            selections_,
            source,
            deferred_types,
            deferred_values,
            use_range,
            visible_constants,
            target_,
            diagnostics_);
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
        const TypeId element = substitute_type(
            value.element,
            type_substitutions,
            value_substitutions,
            use_range);
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
      if (substitution.value.kind != ConstantKind::Integer) continue;
      result.bindings.push_back({
          substitution.parameter,
          substitution.value,
          semantic_.symbols.symbol(substitution.parameter).type,
      });
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

  [[nodiscard]] std::vector<ConstantStaticPackBinding>
  active_constant_packs() const {
    std::vector<ConstantStaticPackBinding> result;
    if (!current_instance_index_.has_value()) return result;
    const ProcedureInstance &instance = instances_[*current_instance_index_];
    if (!current_procedure_.is_valid() ||
        procedure_declaration_source(current_procedure_) != instance.source) {
      // Nested procedures may capture enclosing compile-time type/value
      // parameters, but a static pack also denotes outer runtime arguments.
      // Do not make its length visible through the constant overlay when the
      // nested procedure does not own that pack.
      return result;
    }
    if (instance.pack_binding.is_valid()) {
      result.push_back({
          instance.pack_binding,
          static_cast<std::uint64_t>(instance.pack_types.size()),
      });
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
  [[nodiscard]] bool same_deferred_application_shape(
      TypeId left_id, TypeId right_id) const {
    if (left_id == right_id) return true;
    const Type left = semantic_.types.type(left_id);
    const Type right = semantic_.types.type(right_id);
    if (left.kind != right.kind || left.name != right.name ||
        left.bit_width != right.bit_width ||
        left.c_calling_convention != right.c_calling_convention ||
        left.c_representation != right.c_representation ||
        left.requested_alignment != right.requested_alignment ||
        left.members.size() != right.members.size() ||
        left.element.is_valid() != right.element.is_valid()) {
      return false;
    }
    if (left.element.is_valid() &&
        !same_deferred_application_shape(left.element, right.element)) {
      return false;
    }
    for (std::size_t index = 0; index < left.members.size(); ++index) {
      if (!same_deferred_application_shape(
              left.members[index], right.members[index])) {
        return false;
      }
    }
    return true;
  }

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
    // Full compile-time calls are opaque while a template's value parameters
    // remain symbolic. Two retained structural applications can therefore
    // prove their element/member shape but not yet prove their exact count.
    // Accept that provisional match only in non-executable template HIR. Every
    // concrete instance is checked again after both recipes have evaluated,
    // where the ordinary exact TypeId rule above remains mandatory.
    if (current_procedure_is_template_ &&
        actual_type.owner_evaluated_type_application &&
        expected_type.owner_evaluated_type_application &&
        same_deferred_application_shape(actual, expected)) {
      return true;
    }
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

  // Returns the alignment that a typed value requires when it owns ordinary
  // storage. Body checking runs only after natural layout publication, but the
  // defensive fallback keeps recovery from turning an incomplete user type
  // into a compiler assertion after an earlier diagnostic.
  [[nodiscard]] std::uint32_t natural_alignment(TypeId type_id) const {
    if (!type_id.is_valid()) return 1;
    const Type &type = semantic_.types.type(type_id);
    return type.layout.known ? type.layout.alignment : 1;
  }

  // Offsetting an address can only preserve the powers of two shared by the
  // base guarantee and the byte offset. For example, an 8-byte-aligned base at
  // offset 6 guarantees alignment 2; offset zero preserves all base alignment.
  // Alignments are powers of two, so the least-significant set offset bit is
  // the complete answer without target-specific reasoning.
  [[nodiscard]] static std::uint32_t offset_alignment(
      std::uint32_t base_alignment, std::uint64_t offset) {
    if (base_alignment == 0 || offset == 0) return base_alignment;
    const std::uint64_t offset_power =
        std::uint64_t{1} << std::countr_zero(offset);
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(base_alignment, offset_power));
  }

  [[nodiscard]] std::optional<AggregateMember> aggregate_member(
      SymbolId member_id) const {
    for (const AggregateMember &member :
         semantic_.aggregate_members_for_read()) {
      if (member.member == member_id) return member;
    }
    return std::nullopt;
  }

  // Type member vectors and the owning type scope use the same source order.
  // Recovering the index from the already resolved member SymbolId avoids
  // duplicating layout facts on AggregateMember rows created by the earlier
  // Names product.
  [[nodiscard]] std::optional<std::size_t> aggregate_member_index(
      SymbolId member_id) const {
    if (!member_id.is_valid()) return std::nullopt;
    const Symbol &member = semantic_.symbols.symbol(member_id);
    const Scope &scope = semantic_.symbols.scope(member.scope);
    for (std::size_t index = 0; index < scope.symbols.size(); ++index) {
      if (scope.symbols[index] == member_id) return index;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool numeric_value_type(TypeId type_id) const {
    if (is_invalid_type(type_id)) return false;
    const Type type = runtime_scalar_type(type_id);
    return type.kind == TypeKind::SignedInteger ||
        type.kind == TypeKind::UnsignedInteger ||
        type.kind == TypeKind::Float || type.kind == TypeKind::Rune;
  }

  [[nodiscard]] bool data_pointer_kind(TypeKind kind) const {
    return kind == TypeKind::Pointer || kind == TypeKind::MultiPointer ||
        kind == TypeKind::RawPointer || kind == TypeKind::CString;
  }

  // A value switch is equality dispatch. Variants compare their integer
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
    const std::optional<std::uint64_t> compiler_value = value.to_u64();
    if (compiler_value.has_value() &&
        compiler_enum_member_name(
            semantic_, target, *compiler_value).has_value()) {
      return true;
    }
    const std::optional<SymbolId> owner = type_owner(target);
    if (!owner.has_value()) return false;
    for (const AggregateMember &member :
         semantic_.aggregate_members_for_read()) {
      if (member.owner != *owner) continue;
      for (const EnumMemberValue &enum_value :
           semantic_.enum_member_values_for_read()) {
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

  // `nil` deliberately has no standalone type. Keep this small syntax query
  // next to the other operator-shape helpers so binary comparisons and
  // conditional expressions discover its context in exactly the same way.
  [[nodiscard]] bool is_nil_literal(
      const SyntaxTree &tree, NodeId expression) const {
    const SyntaxNode &node = tree.node(expression);
    return node.kind == NodeKind::LiteralExpression &&
        node.token_begin < node.token_end &&
        tree.token(node.token_begin).kind == TokenKind::KeywordNil;
  }

  // A contextual enum/variant alternative is like `nil`: its spelling identifies
  // a member only after the surrounding expression identifies the owning type.
  // This predicate does not inspect or evaluate values; it only lets a sibling
  // branch provide the missing expected type during semantic checking.
  [[nodiscard]] bool needs_value_context(
      const SyntaxTree &tree, NodeId expression) const {
    if (is_nil_literal(tree, expression)) return true;
    const SyntaxNode &node = tree.node(expression);
    if (node.kind == NodeKind::ContextualAlternativeExpression) return true;
    if ((node.kind == NodeKind::GroupExpression ||
         node.kind == NodeKind::DenyExpression) &&
        !node.children.empty()) {
      return needs_value_context(tree, node.children.back());
    }
    if (node.kind == NodeKind::ConditionalExpression &&
        node.children.size() == 3) {
      // A conditional can infer from either independently typed branch. It
      // needs an outer context only when neither value branch has a type of
      // its own, such as `nil if flag else nil`.
      return needs_value_context(tree, node.children[0]) &&
          needs_value_context(tree, node.children[2]);
    }
    return false;
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
    case TokenKind::Tilde: return HirOperation::BitwiseXor;
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
    case TokenKind::TildeEqual: return HirOperation::BitwiseXor;
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
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes_for_read()) {
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
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes_for_read()) {
      if (owned.owner == *import &&
          semantic_.symbols.scope(owned.scope).kind == ScopeKind::ImportedPackage) {
        return semantic_.symbols.lookup_direct(owned.scope, all_names.back().text);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] const ConstantValue *imported_constant(SymbolId proxy) const {
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
      if (imported.proxy == proxy && imported.has_constant) {
        return &imported.constant;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool is_runtime_intrinsic(
      SymbolId proxy, std::string_view public_name) const {
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
      if (imported.proxy != proxy || imported.public_name != public_name ||
          imported.root_relative_path != "runtime") {
        continue;
      }
      for (const ImportBinding &binding : semantic_.imports_for_read()) {
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
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
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
      for (const ImportBinding &binding : semantic_.imports_for_read()) {
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
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes_for_read()) {
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
      for (const EnumMemberValue &member :
           semantic_.enum_member_values_for_read()) {
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
    // Enum and variant switches over a distinct wrapper still use the
    // alternative set owned by the underlying nominal declaration.
    type = underlying_type_id(type);
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes_for_read()) {
      if (semantic_.symbols.scope(owned.scope).kind == ScopeKind::Type &&
          semantic_.symbols.symbol(owned.owner).type == type) {
        return owned.owner;
      }
    }
    return std::nullopt;
  }

  // Variant discriminators are source-order integers independent from
  // enum values. Keeping this small lookup in semantic checking makes switch
  // labels scalar constants before MIR and prevents native lowering from
  // comparing whole payload-bearing aggregate values.
  [[nodiscard]] std::optional<std::uint64_t> variant_discriminator(
      TypeId variant_type, SymbolId alternative) const {
    const std::optional<SymbolId> owner = type_owner(variant_type);
    if (!owner.has_value()) return std::nullopt;
    std::uint64_t discriminator = 0;
    for (const AggregateMember &member :
         semantic_.aggregate_members_for_read()) {
      if (member.owner != *owner) continue;
      if (member.member == alternative) return discriminator;
      ++discriminator;
    }
    return std::nullopt;
  }

  // Checks a variant case label as a pattern rather than as a value
  // constructor. `.value(name)` introduces one case-local binding; `.value`
  // and `.value(_)` both select the alternative while ignoring its payload.
  [[nodiscard]] HirExpressionId check_variant_case_label(
      const SyntaxTree &tree,
      NodeId label_id,
      ScopeId case_scope,
      TypeId subject_type,
      HirSwitchCase &hir_case,
      bool multiple_labels) {
    const SyntaxNode &label = tree.node(label_id);
    const std::vector<SourceName> names =
        alternative_names_in_span(tree, label.token_begin, label.token_end);
    if (label.kind != NodeKind::ContextualAlternativeExpression || names.empty()) {
      diagnostics_.error(label.range, "variant case requires a contextual alternative");
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
        variant_discriminator(subject_type, *alternative);
    if (!discriminator.has_value()) {
      diagnostics_.error(label.range, "variant alternative has no discriminator");
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
              "variant payload pattern must be a name or '_'");
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
    const std::vector<ConstantStaticPackBinding> visible_packs =
        active_constant_packs();
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
            subject_type,
            &visible_packs);
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
    for (const AggregateMember &member :
         semantic_.aggregate_members_for_read()) {
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

  // Every structural query has a result category known from its name even when
  // the queried type is a symbolic parameter. This lets the template pass
  // check control flow and operators without pretending the query already has
  // a concrete value; concrete instances fold it through inspect_type.
  [[nodiscard]] TypeId symbolic_inspection_result_type(
      std::string_view query) const {
    if (query == "type_element" || query == "type_member_type" ||
        query == "type_underlying" || query == "type_discriminator" ||
        query == "type_parameter_type" || query == "type_result") {
      return semantic_.types.builtins().meta_type;
    }
    if (query == "type_kind") {
      return semantic_.types.builtins().type_kind_type;
    }
    if (query == "type_byte_order") {
      return semantic_.types.builtins().type_byte_order_type;
    }
    if (query == "type_calling_convention") {
      return semantic_.types.builtins().calling_convention_type;
    }
    if (query == "type_name" || query == "type_member_name") {
      return semantic_.types.builtins().string_type;
    }
    if (query == "type_is_c_repr" || query == "type_member_is_packed") {
      return semantic_.types.builtins().bool_type;
    }
    return semantic_.types.builtins().usize_type;
  }

  // Recognizes `type_of(name)` without evaluating name. Refinement attaches to
  // a stable symbol rather than source spelling, so shadowing and generated
  // source preserve the ordinary lexical lookup rule.
  [[nodiscard]] std::optional<SymbolId> type_of_subject(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) const {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind != NodeKind::CallExpression ||
        expression.children.size() != 2) {
      return std::nullopt;
    }
    const std::optional<SourceName> callee =
        single_name_expression(tree, expression.children.front());
    if (!callee.has_value() || callee->text != "type_of") {
      return std::nullopt;
    }
    const std::optional<SourceName> subject =
        single_name_expression(tree, expression.children.back());
    if (!subject.has_value()) return std::nullopt;
    const std::optional<SymbolId> symbol =
        semantic_.symbols.lookup(scope, subject->text);
    if (!symbol.has_value() ||
        !contains_symbolic_type(semantic_.symbols.symbol(*symbol).type)) {
      return std::nullopt;
    }
    return symbol;
  }

  // Recognizes the second refinement form,
  // `type_kind(type_of(name))`. Other structural queries remain valid
  // compile-time conditions, but they do not grant an operator capability.
  [[nodiscard]] std::optional<SymbolId> type_kind_subject(
      const SyntaxTree &tree, NodeId expression_id, ScopeId scope) const {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind != NodeKind::CallExpression ||
        expression.children.size() != 2) {
      return std::nullopt;
    }
    const std::optional<SourceName> callee =
        single_name_expression(tree, expression.children.front());
    if (!callee.has_value() || callee->text != "type_kind") {
      return std::nullopt;
    }
    return type_of_subject(tree, expression.children.back(), scope);
  }

  [[nodiscard]] std::optional<std::string> contextual_alternative_name(
      const SyntaxTree &tree, NodeId expression_id) const {
    const SyntaxNode &expression = tree.node(expression_id);
    if (expression.kind != NodeKind::ContextualAlternativeExpression) {
      return std::nullopt;
    }
    const std::vector<SourceName> names = alternative_names_in_span(
        tree, expression.token_begin, expression.token_end);
    if (names.empty()) return std::nullopt;
    return names.front().text;
  }

  // Extracts only the two condition shapes whose truth changes which
  // operations are valid for a symbolic value. This is intentionally not a
  // general theorem prover: every other dependent bool is still checked and
  // selected per concrete instance, but grants no speculative capability.
  [[nodiscard]] std::optional<TypeRefinementPredicate>
  type_refinement_predicate(
      const SyntaxTree &tree, NodeId condition_id, ScopeId scope) {
    const SyntaxNode &condition = tree.node(condition_id);
    if (condition.kind != NodeKind::BinaryExpression ||
        condition.children.size() != 2) {
      return std::nullopt;
    }
    const TokenKind operation = binary_operator(tree, condition);
    if (operation != TokenKind::EqualEqual &&
        operation != TokenKind::BangEqual) {
      return std::nullopt;
    }

    const auto exact = [&](NodeId observed, NodeId expected)
        -> std::optional<TypeRefinementPredicate> {
      const std::optional<SymbolId> subject =
          type_of_subject(tree, observed, scope);
      if (!subject.has_value()) return std::nullopt;
      const TypeId expected_type = type_value_expression(tree, expected, scope);
      if (is_invalid_type(expected_type)) return std::nullopt;
      TypeRefinementPredicate result;
      result.kind = TypeRefinementPredicateKind::ExactType;
      result.subject = *subject;
      result.exact_type = expected_type;
      result.true_branch_matches = operation == TokenKind::EqualEqual;
      return result;
    };
    const auto kind = [&](NodeId observed, NodeId expected)
        -> std::optional<TypeRefinementPredicate> {
      const std::optional<SymbolId> subject =
          type_kind_subject(tree, observed, scope);
      if (!subject.has_value()) return std::nullopt;
      const std::optional<std::string> name =
          contextual_alternative_name(tree, expected);
      if (!name.has_value()) return std::nullopt;
      const std::optional<TypeKind> expected_kind =
          inspected_type_kind(*name);
      if (!expected_kind.has_value()) return std::nullopt;
      TypeRefinementPredicate result;
      result.kind = TypeRefinementPredicateKind::TypeKind;
      result.subject = *subject;
      result.type_kind = *expected_kind;
      result.true_branch_matches = operation == TokenKind::EqualEqual;
      return result;
    };

    const NodeId left = condition.children.front();
    const NodeId right = condition.children.back();
    if (std::optional<TypeRefinementPredicate> result = exact(left, right)) {
      return result;
    }
    if (std::optional<TypeRefinementPredicate> result = exact(right, left)) {
      return result;
    }
    if (std::optional<TypeRefinementPredicate> result = kind(left, right)) {
      return result;
    }
    return kind(right, left);
  }

  // Produces the matching fact and its exact complement. Chained else-when
  // statements simply push each complement before checking the next
  // condition, so facts accumulate in source order and unwind with the
  // lexical recursion.
  [[nodiscard]] std::pair<ActiveTypeRefinement, ActiveTypeRefinement>
  branch_type_refinements(const TypeRefinementPredicate &predicate) const {
    ActiveTypeRefinement matching;
    matching.subject = predicate.subject;
    ActiveTypeRefinement complement;
    complement.subject = predicate.subject;
    if (predicate.kind == TypeRefinementPredicateKind::ExactType) {
      matching.exact_type = predicate.exact_type;
      complement.excluded_exact_types.push_back(predicate.exact_type);
    } else {
      matching.allowed_kinds.push_back(predicate.type_kind);
      for (TypeKind kind : inspectable_type_kinds()) {
        if (kind != predicate.type_kind) {
          complement.allowed_kinds.push_back(kind);
        }
      }
    }
    if (predicate.true_branch_matches) return {matching, complement};
    return {complement, matching};
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
          const Symbol &binding = semantic_.symbols.symbol(*symbol);
          const SymbolKind kind = binding.kind;
          if (kind == SymbolKind::TypeParameter ||
              kind == SymbolKind::ValueParameter ||
              contains_symbolic_type(binding.type)) {
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
      const ConstantTable visible_constants = active_constant_table();
      const std::vector<ConstantTypeBinding> visible_types =
          active_constant_types();
      return substitute_active(
          resolve_type_syntax(
              sources_, loaded_, semantic_, selections_, tree, node_id, scope,
              visible_constants, visible_types, target_,
              diagnostics_),
          node.range);
    }
    if (const std::optional<SymbolId> imported = imported_member(tree, node, scope)) {
      const Symbol binding = semantic_.symbols.symbol(*imported);
      if (binding.kind == SymbolKind::Type || binding.kind == SymbolKind::TypeParameter) {
        return substitute_active(binding.type, node.range);
      }
      const ConstantValue *constant = imported_constant(*imported);
      if (constant != nullptr && constant->kind == ConstantKind::Type &&
          constant->type_index < semantic_.types.size()) {
        return substitute_active(TypeId{constant->type_index}, node.range);
      }
      diagnostics_.error(node.range, "imported name does not denote a type");
      return semantic_.types.builtins().invalid;
    }
    const std::optional<SourceName> name = single_name_expression(tree, node_id);
    if (!name.has_value()) return semantic_.types.builtins().invalid;
    const std::optional<SymbolId> symbol = semantic_.symbols.lookup(scope, name->text);
    if (symbol.has_value()) {
      const Symbol binding = semantic_.symbols.symbol(*symbol);
      if (binding.kind == SymbolKind::Type || binding.kind == SymbolKind::TypeParameter) {
        return substitute_active(binding.type, node.range);
      }

      // `::` may name a computed type value rather than syntactically declaring
      // a type alias: `T :: type_of(value); cast[T](other)`. Its Symbol type is
      // the compile-time meta-type, while the retained ConstantValue carries
      // the exact TypeId needed by this type position.
      const ConstantValue *constant = constants_.find(*symbol);
      if (constant == nullptr) constant = active_constant(*symbol);
      if (constant != nullptr && constant->kind == ConstantKind::Type &&
          constant->type_index < semantic_.types.size()) {
        return substitute_active(
            TypeId{constant->type_index}, node.range);
      }
      diagnostics_.error(name->range, "name does not denote a type");
      return semantic_.types.builtins().invalid;
    }
    if (const std::optional<TypeId> builtin =
            semantic_.types.find_builtin(name->text)) {
      return *builtin;
    }
    diagnostics_.error(name->range, "name does not denote a type");
    return semantic_.types.builtins().invalid;
  }

  // Checks the bracket arguments shared by ordinary generic procedure values
  // and direct static-pack calls. This operation intentionally stops before
  // instantiation: a pack call must first inspect and default its runtime tail
  // types, while an ordinary generic application can instantiate immediately.
  [[nodiscard]] ExplicitProcedureArguments check_explicit_procedure_arguments(
      const SyntaxTree &tree,
      const SyntaxNode &application,
      ScopeId scope,
      SymbolId source) {
    ExplicitProcedureArguments result;
    const std::vector<ParametricParameterRecord> parameters =
        parameters_for(source);
    if (application.children.size() - 1 != parameters.size()) {
      diagnostics_.error(
          application.range,
          "parametric procedure application has the wrong number of arguments");
      return result;
    }

    for (std::size_t index = 0; index < parameters.size(); ++index) {
      const ParametricParameterRecord &parameter = parameters[index];
      const NodeId argument_syntax = application.children[index + 1];
      if (parameter.constraint == TypeConstraintKind::CompileTimeValue) {
        const TypeId required =
            semantic_.symbols.symbol(parameter.parameter).type;
        if (current_procedure_is_template_ ||
            !current_instance_index_.has_value()) {
          const ConstantTable active_constants = active_constant_table();
          const std::size_t errors_before = diagnostics_.error_count();
          const std::optional<IntegerExpression> symbolic =
              resolve_dependent_integer_expression_syntax(
                  sources_,
                  loaded_,
                  semantic_,
                  selections_,
                  tree,
                  argument_syntax,
                  scope,
                  required,
                  active_constants,
                  diagnostics_);
          if (symbolic.has_value()) {
            const IntegerExpressionNode &symbolic_root =
                symbolic->nodes[symbolic->root];
            if (symbolic_root.type != integer_expression_type(required)) {
              diagnostics_.error(
                  tree.node(argument_syntax).range,
                  "symbolic procedure value argument has the wrong result type");
              return result;
            }
            for (const IntegerExpressionNode &part : symbolic->nodes) {
              if (part.operation != IntegerExpressionOperation::Parameter) {
                continue;
              }
              const bool valid_symbol =
                  part.parameter < semantic_.symbols.symbol_count();
              const Symbol *supplied = valid_symbol
                  ? &semantic_.symbols.symbol(SymbolId{part.parameter})
                  : nullptr;
              if (supplied == nullptr ||
                  supplied->kind != SymbolKind::ValueParameter) {
                diagnostics_.error(
                    tree.node(argument_syntax).range,
                    "symbolic procedure value argument names a non-parameter value");
                return result;
              }
            }
            result.value_substitutions.push_back(
                {parameter.parameter, {}, *symbolic});
            continue;
          }
          if (diagnostics_.error_count() != errors_before) return result;
          if (current_procedure_is_template_ &&
              expression_references_parametric_parameter(
                  tree, argument_syntax, scope)) {
            const HirExpressionId checked = check_expression(
                tree, argument_syntax, scope, required);
            if (is_invalid_type(hir_.expression(checked).type)) return result;
            result.value_substitutions.push_back(
                {parameter.parameter, {}, {}, true});
            continue;
          }
        }

        const ConstantTable active_constants = active_constant_table();
        const std::vector<ConstantTypeBinding> active_types =
            active_constant_types();
        const std::vector<ConstantStaticPackBinding> active_packs =
            active_constant_packs();
        const std::optional<EvaluatedConstant> evaluated =
            evaluate_typed_constant_expression(
                sources_,
                loaded_,
                semantic_,
                target_,
                tree,
                argument_syntax,
                scope,
                diagnostics_,
                &active_constants,
                &active_types,
                required,
                &active_packs);
        if (!evaluated.has_value()) return result;
        if (evaluated->value.kind != ConstantKind::Integer) {
          diagnostics_.error(
              tree.node(argument_syntax).range,
              "procedure value argument must be a compile-time integer");
          return result;
        }
        result.value_substitutions.push_back(
            {parameter.parameter, evaluated->value, {}});
        continue;
      }

      const TypeId argument =
          type_value_expression(tree, argument_syntax, scope);
      if (!constraint_accepts(parameter.constraint, argument)) {
        diagnostics_.error(
            tree.node(argument_syntax).range,
            "procedure type argument does not satisfy its constraint");
        return result;
      }
      result.type_substitutions.push_back({
          semantic_.symbols.symbol(parameter.parameter).type,
          argument,
      });
    }
    result.valid = true;
    return result;
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
         semantic_.parametric_parameters_for_read()) {
      if (parameter.owner == owner) result.push_back(parameter);
    }
    return result;
  }

  // A procedure owns at most one static pack. Keeping lookup as a linear scan
  // mirrors the small parametric-parameter table and avoids a second index
  // whose invalidation would have to follow imported-interface binding and
  // nested declaration discovery. Ordinary procedures return null.
  [[nodiscard]] const StaticArgumentPack *static_argument_pack(
      SymbolId owner) const {
    for (const StaticArgumentPack &pack :
         semantic_.static_argument_packs_for_read()) {
      if (pack.owner == owner) return &pack;
    }
    return nullptr;
  }

  // Identifies a visible marker owned by an enclosing procedure. Unlike an
  // ordinary compile-time scalar parameter, a pack describes concrete runtime
  // arguments as well as their types and length. Nested procedures therefore
  // cannot capture it implicitly; they must declare their own pack or receive
  // whatever ordinary value they need through fixed parameters.
  [[nodiscard]] bool is_enclosing_static_argument_pack_binding(
      SymbolId binding) const {
    if (!current_procedure_.is_valid()) return false;
    const SymbolId current_source =
        procedure_declaration_source(current_procedure_);
    for (const StaticArgumentPack &pack :
         semantic_.static_argument_packs_for_read()) {
      if (pack.binding == binding && pack.owner != current_source) return true;
    }
    if (current_instance_index_.has_value()) {
      const ProcedureInstance &instance =
          instances_[*current_instance_index_];
      if (instance.pack_binding == binding &&
          instance.source != current_source) {
        return true;
      }
    }
    return false;
  }

  // Recognizes the pack binding owned by the procedure currently being
  // checked. Restricting the owner is important for nested procedures: a pack
  // is compile-time structure, but it is no more capturable than an ordinary
  // enclosing runtime parameter.
  [[nodiscard]] const StaticArgumentPack *active_static_argument_pack(
      const SyntaxTree &tree,
      NodeId expression,
      ScopeId scope) const {
    const std::optional<SourceName> name =
        single_name_expression(tree, expression);
    if (!name.has_value() || !current_procedure_.is_valid()) return nullptr;
    const SymbolId source = procedure_declaration_source(current_procedure_);
    const StaticArgumentPack *pack = static_argument_pack(source);
    if (pack == nullptr ||
        semantic_.symbols.symbol(pack->binding).name != name->text) {
      return nullptr;
    }
    // The symbolic source pass resolves the actual marker binding. A concrete
    // instance deliberately has no runtime marker, so its owner/name match is
    // sufficient. The lookup guards against an inner local shadowing the pack.
    const std::optional<SymbolId> visible =
        semantic_.symbols.lookup(scope, name->text);
    if (!current_instance_index_.has_value()) {
      return visible.has_value() && *visible == pack->binding ? pack : nullptr;
    }
    const SymbolId concrete_binding =
        instances_[*current_instance_index_].pack_binding;
    return visible.has_value() && *visible == concrete_binding ? pack : nullptr;
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
      if (substitution.symbolic_expression.is_valid() ||
          substitution.deferred_expression) {
        return true;
      }
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

  // Binds one exact scalar during call inference. Repeated appearances of the
  // same parameter must infer the same value, and the source value type must
  // match the declaration because symbolic value parameters do not perform an
  // implicit narrowing conversion.
  [[nodiscard]] bool infer_exact_value_argument(
      SymbolId owner,
      SymbolId parameter,
      const ConstantValue &value,
      TypeId value_type,
      std::vector<ValueSubstitution> &substitutions) {
    const std::vector<ParametricParameterRecord> parameters =
        parameters_for(owner);
    for (const ParametricParameterRecord &record : parameters) {
      if (record.parameter != parameter ||
          record.constraint != TypeConstraintKind::CompileTimeValue) {
        continue;
      }
      const TypeId required_type = semantic_.symbols.symbol(parameter).type;
      if (value_type != required_type ||
          value.kind != ConstantKind::Integer ||
          !semantic_.types.is_integer(required_type) ||
          !integer_representable(value.integer, required_type)) {
        return false;
      }
      const std::optional<std::size_t> existing =
          value_substitution_index(substitutions, parameter);
      if (existing.has_value()) {
        return !substitutions[*existing].symbolic_expression.is_valid() &&
            !substitutions[*existing].deferred_expression &&
            substitutions[*existing].value == value;
      }
      substitutions.push_back({parameter, value, {}});
      return true;
    }
    return false;
  }

  [[nodiscard]] bool infer_symbolic_value_argument(
      SymbolId owner,
      SymbolId parameter,
      const IntegerExpression &supplied,
      std::vector<ValueSubstitution> &substitutions) {
    for (const ParametricParameterRecord &record : parameters_for(owner)) {
      if (record.parameter != parameter ||
          record.constraint != TypeConstraintKind::CompileTimeValue) {
        continue;
      }
      const Symbol &destination = semantic_.symbols.symbol(parameter);
      for (const IntegerExpressionNode &node : supplied.nodes) {
        if (node.operation != IntegerExpressionOperation::Parameter) continue;
        if (node.parameter >= semantic_.symbols.symbol_count()) return false;
        const Symbol &source =
            semantic_.symbols.symbol(SymbolId{node.parameter});
        if (source.kind != SymbolKind::ValueParameter ||
            source.type != destination.type) {
          return false;
        }
      }
      const std::optional<std::size_t> existing =
          value_substitution_index(substitutions, parameter);
      if (existing.has_value()) {
        return !substitutions[*existing].deferred_expression &&
            substitutions[*existing].symbolic_expression == supplied;
      }
      substitutions.push_back({parameter, {}, supplied});
      return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<IntegerExpression> substitute_inferred_values(
      const IntegerExpression &pattern,
      const std::vector<ValueSubstitution> &substitutions) const {
    std::string error;
    return substitute_integer_expression(
        pattern,
        integer_expression_replacements(substitutions),
        error);
  }

  // Infers one value parameter from a concrete dependent shape. The shared
  // integer-expression solver accepts only a single occurrence surrounded by
  // operations which are provably one-to-one in the exact typed domain. After
  // solving, infer_exact_value_argument still enforces declaration ownership,
  // the parameter's integer identity, and representability.
  [[nodiscard]] bool infer_concrete_value_expression(
      SymbolId owner,
      const IntegerExpression &pattern,
      const BigInteger &actual,
      std::vector<ValueSubstitution> &substitutions) {
    const std::optional<IntegerExpression> substituted =
        substitute_inferred_values(pattern, substitutions);
    if (!substituted.has_value()) return false;
    if (!integer_expression_has_parameters(*substituted)) {
      const IntegerExpressionResult evaluated =
          evaluate_integer_expression(*substituted);
      return evaluated.ok && evaluated.value == actual;
    }
    const std::optional<IntegerExpressionSolution> solution =
        solve_unique_integer_expression(*substituted, actual);
    if (!solution.has_value()) return false;
    const SymbolId parameter{solution->parameter};
    if (static_cast<std::size_t>(parameter.value) >=
        semantic_.symbols.symbol_count()) {
      return false;
    }
    return infer_exact_value_argument(
        owner,
        parameter,
        ConstantValue::make_integer(solution->value),
        semantic_.symbols.symbol(parameter).type,
        substitutions);
  }

  [[nodiscard]] bool infer_symbolic_value_expression(
      SymbolId owner,
      const IntegerExpression &pattern,
      const IntegerExpression &actual,
      std::vector<ValueSubstitution> &substitutions) {
    const std::optional<IntegerExpression> substituted =
        substitute_inferred_values(pattern, substitutions);
    if (!substituted.has_value()) return false;
    if (const std::optional<std::uint32_t> parameter =
            single_integer_parameter(*substituted)) {
      return infer_symbolic_value_argument(
          owner,
          SymbolId{*parameter},
          actual,
          substitutions);
    }
    return *substituted == actual;
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
         semantic_.parametric_type_instances_for_read()) {
      if (semantic_.symbols.symbol(instance.instance).type == type) {
        return NominalApplication{
            instance.source, nullptr, &instance.arguments};
      }
    }
    for (const ImportedType &imported : semantic_.imported_types_for_read()) {
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
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
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
    if (value.owner_evaluated_type_application) return true;
    if ((value.kind == TypeKind::Array || value.kind == TypeKind::Simd) &&
        (value.element_count_expression.is_valid() ||
         value.owner_evaluated_element_count)) {
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
        value.kind == TypeKind::Variant ||
        value.kind == TypeKind::Union) {
      const std::optional<NominalApplication> application =
          nominal_application(type);
      if (!application.has_value()) return false;
      for (const ParametricArgument &argument : *application->arguments) {
        if ((argument.is_type && contains_symbolic_type(argument.type)) ||
            (!argument.is_type &&
             (argument.value_expression.is_valid() ||
              argument.owner_evaluated_value))) {
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
        pattern.kind == TypeKind::Variant ||
        pattern.kind == TypeKind::Union) {
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
        } else {
          if (pattern_argument.value_type != actual_argument.value_type) {
            return false;
          }
          if (pattern_argument.owner_evaluated_value ||
              actual_argument.owner_evaluated_value) {
            if (pattern_argument != actual_argument) return false;
            continue;
          }
          if (pattern_argument.value_expression.is_valid()) {
            const bool inferred = actual_argument.value_expression.is_valid()
                ? infer_symbolic_value_expression(
                      owner,
                      pattern_argument.value_expression,
                      actual_argument.value_expression,
                      value_substitutions)
                : actual_argument.value.kind == ConstantKind::Integer &&
                    infer_concrete_value_expression(
                        owner,
                        pattern_argument.value_expression,
                        actual_argument.value.integer,
                        value_substitutions);
            if (!inferred) return false;
          } else if (actual_argument.value_expression.is_valid() ||
                     pattern_argument.value != actual_argument.value) {
            return false;
          }
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
      if (pattern.element_count_expression.is_valid()) {
        const bool inferred = actual.element_count_expression.is_valid()
            ? infer_symbolic_value_expression(
                  owner,
                  pattern.element_count_expression,
                  actual.element_count_expression,
                  value_substitutions)
            : infer_concrete_value_expression(
                  owner,
                  pattern.element_count_expression,
                  BigInteger::from_u64(actual.element_count),
                  value_substitutions);
        if (!inferred) return false;
      } else if (actual.element_count_expression.is_valid()) {
        return false;
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
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
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
        if (value_substitutions[index].deferred_expression) {
          argument.owner_evaluated_value = true;
        } else if (value_substitutions[index].symbolic_expression.is_valid()) {
          argument.value_expression =
              value_substitutions[index].symbolic_expression;
        } else {
          argument.value = value_substitutions[index].value;
        }
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
      const std::vector<TypeId> &pack_types,
      SourceRange use_range) {
    const std::vector<ParametricArgument> arguments = ordered_arguments(
        parameters, type_substitutions, value_substitutions);
    for (const ImportedProcedureInstance &instance :
         semantic_.imported_procedure_instances_for_read()) {
      if (instance.source_proxy == source && instance.arguments == arguments &&
          instance.pack_types == pack_types) {
        return instance.instance_proxy;
      }
    }

    const Symbol source_symbol = semantic_.symbols.symbol(source);
    Symbol instance_symbol;
    instance_symbol.name = source_symbol.name + "$imported_instance";
    for (const ParametricArgument &argument : arguments) {
      if (argument.is_type) {
        instance_symbol.name += "$t" + std::to_string(argument.type.value);
      } else if (argument.owner_evaluated_value) {
        instance_symbol.name += "$owner";
      } else if (argument.value_expression.is_valid()) {
        instance_symbol.name += "$e" +
            integer_expression_identity(argument.value_expression);
      } else {
        instance_symbol.name += "$v" + argument.value.integer.to_decimal();
      }
    }
    instance_symbol.name += "$pack" + std::to_string(pack_types.size());
    for (TypeId type : pack_types) {
      instance_symbol.name += "$t" + std::to_string(type.value);
    }
    instance_symbol.kind = SymbolKind::Procedure;
    instance_symbol.visibility = Visibility::Private;
    instance_symbol.flags = source_symbol.flags;
    instance_symbol.flags.parametric = false;
    instance_symbol.flags.exported = false;
    instance_symbol.scope = semantic_.package_scope;
    const TypeId fixed_signature = substitute_type(
        source_symbol.type,
        type_substitutions,
        value_substitutions,
        use_range);
    const Type &fixed = semantic_.types.type(fixed_signature);
    std::vector<TypeId> concrete_parameters;
    if (!fixed.members.empty()) {
      concrete_parameters.assign(fixed.members.begin(), fixed.members.end() - 1);
    }
    concrete_parameters.insert(
        concrete_parameters.end(), pack_types.begin(), pack_types.end());
    const TypeId result = fixed.members.empty()
        ? semantic_.types.builtins().void_type
        : fixed.members.back();
    instance_symbol.type = semantic_.types.procedure(
        concrete_parameters, result, fixed.c_calling_convention);
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

    // Snapshot the visible prefix and any earlier task-local clones before
    // appending this instance's rows. Iterating the live two-segment view while
    // growing its suffix would invalidate vector storage and recursively visit
    // the rows being created.
    std::vector<ImportedEffect> existing_effects;
    for (const ImportedEffect &effect :
         semantic_.imported_effects_for_read()) {
      existing_effects.push_back(effect);
    }
    for (const ImportedEffect &effect : existing_effects) {
      if (effect.procedure_proxy != source) continue;
      ImportedEffect concrete_effect = effect;
      concrete_effect.procedure_proxy = instance_id;
      semantic_.imported_effects.push_back(std::move(concrete_effect));
    }
    std::vector<ImportedProcedureReturn> existing_returns;
    for (const ImportedProcedureReturn &returned :
         semantic_.imported_returns_for_read()) {
      existing_returns.push_back(returned);
    }
    for (const ImportedProcedureReturn &returned : existing_returns) {
      if (returned.procedure_proxy != source) continue;
      ImportedProcedureReturn concrete_return = returned;
      concrete_return.procedure_proxy = instance_id;
      for (ImportedEffect &effect : concrete_return.contract_effects) {
        effect.procedure_proxy = instance_id;
      }
      semantic_.imported_returns.push_back(std::move(concrete_return));
    }
    std::vector<ImportedProcedureWrite> existing_writes;
    for (const ImportedProcedureWrite &write :
         semantic_.imported_writes_for_read()) {
      existing_writes.push_back(write);
    }
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
        pack_types,
    });
    return instance_id;
  }

  // Reconstructs the active checking environment for one retained concrete
  // procedure. ParametricInstanceRecord owns the exact type/value bindings;
  // the concrete Procedure scope owns the fixed parameters, static-pack marker,
  // and unnameable pack-element parameters. Rebuilding this small view makes
  // body roots independent of the BodyChecker which happened to discover the
  // specialization. Missing rows indicate a compiler ownership violation, but
  // are still reported through diagnostics so malformed state never reaches an
  // assertion or gets interpreted as ordinary Draft source.
  [[nodiscard]] ProcedureInstanceActivation restore_instance(
      SymbolId procedure) {
    const ParametricInstanceRecord *retained = nullptr;
    for (const ParametricInstanceRecord &candidate :
         semantic_.parametric_instances_for_read()) {
      if (candidate.instance == procedure) {
        retained = &candidate;
        break;
      }
    }
    if (retained == nullptr) return {};

    ProcedureInstance restored;
    restored.source = retained->source;
    restored.symbol = retained->instance;
    restored.pack_types = retained->pack_types;
    restored.type_substitutions.reserve(
        retained->type_substitutions.size());
    for (const ConcreteProcedureTypeSubstitution &substitution :
         retained->type_substitutions) {
      restored.type_substitutions.push_back(
          {substitution.parameter, substitution.replacement});
    }
    restored.value_substitutions.reserve(
        retained->value_substitutions.size());
    for (const ConcreteProcedureValueSubstitution &substitution :
         retained->value_substitutions) {
      restored.value_substitutions.push_back(
          {substitution.parameter, substitution.value, {}});
    }

    const StaticArgumentPack *pack = static_argument_pack(retained->source);
    if (pack == nullptr) {
      if (!retained->pack_types.empty()) {
        diagnostics_.error(
            semantic_.symbols.symbol(procedure).name_range,
            "concrete procedure retains pack types without a source pack");
        return {true, std::nullopt};
      }
    } else {
      const std::optional<ScopeId> scope = procedure_scope(procedure);
      if (!scope.has_value()) {
        diagnostics_.error(
            semantic_.symbols.symbol(procedure).name_range,
            "concrete procedure parameter scope is missing");
        return {true, std::nullopt};
      }
      const std::string &binding_name =
          semantic_.symbols.symbol(pack->binding).name;
      const std::optional<SymbolId> binding =
          semantic_.symbols.lookup_direct(*scope, binding_name);
      if (!binding.has_value()) {
        diagnostics_.error(
            semantic_.symbols.symbol(procedure).name_range,
            "concrete procedure static-pack binding is missing");
        return {true, std::nullopt};
      }
      restored.pack_binding = *binding;
      restored.pack_parameters.reserve(retained->pack_types.size());
      for (std::size_t index = 0; index < retained->pack_types.size(); ++index) {
        const std::optional<SymbolId> parameter =
            semantic_.symbols.lookup_direct(
                *scope, "$pack_element_" + std::to_string(index));
        if (!parameter.has_value()) {
          diagnostics_.error(
              semantic_.symbols.symbol(procedure).name_range,
              "concrete procedure static-pack parameter is missing");
          return {true, std::nullopt};
        }
        restored.pack_parameters.push_back(*parameter);
      }
    }

    instances_.push_back(std::move(restored));
    return {true, instances_.size() - 1};
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
      std::vector<TypeId> pack_types,
      SourceRange use_range,
      std::string_view preferred_name = {}) {
    const std::vector<ParametricParameterRecord> parameters =
        parameters_for(source);
    const StaticArgumentPack *pack = static_argument_pack(source);
    if (parameters.empty() && pack == nullptr) {
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
        if (substitution.deferred_expression ||
            substitution.symbolic_expression.is_valid() ||
            substitution.value.kind != ConstantKind::Integer ||
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
          pack_types,
          use_range);
    }

    const std::vector<ParametricArgument> requested_arguments =
        ordered_arguments(
            parameters, type_substitutions, value_substitutions);
    const AppendOnlyTableView<ParametricInstanceRecord> retained_instances =
        semantic_.parametric_instances_for_read();
    for (std::size_t instance_index = 0;
         instance_index < retained_instances.size(); ++instance_index) {
      const ParametricInstanceRecord &instance =
          retained_instances[instance_index];
      if (instance.source == source &&
          instance.arguments == requested_arguments &&
          instance.pack_types == pack_types) {
        if (!preferred_name.empty() && !instance.externally_requested) {
          // Retained body work may already contain the same
          // specialization because the defining package calls its own public
          // template. A later consumer demand does not require another body:
          // give the existing private symbol the canonical native linkage name
          // and let every existing HIR reference keep its stable SymbolId. Its
          // lexical name must not change: SymbolTable names are immutable after
          // declaration and direct lookup observes them.
          for (SymbolId candidate_id :
               semantic_.symbols.symbols_in_scope(
                   semantic_.package_scope)) {
            if (candidate_id == instance.instance) continue;
            const Symbol &candidate =
                semantic_.symbols.symbol(candidate_id);
            const std::string_view candidate_linkage =
                candidate.linkage_name.empty()
                    ? std::string_view(candidate.name)
                    : std::string_view(candidate.linkage_name);
            if (candidate_linkage == preferred_name) {
              diagnostics_.error(
                  use_range,
                  "generic procedure instance name collides with an existing "
                  "package symbol");
              return {};
            }
          }
          Symbol &promoted =
              semantic_.symbols.symbol_mut(instance.instance);
          promoted.linkage_name = std::string(preferred_name);
          semantic_.parametric_instance_mut(instance_index)
              .externally_requested = true;
        } else if (!preferred_name.empty()) {
          const Symbol &existing =
              semantic_.symbols.symbol(instance.instance);
          const std::string_view existing_linkage =
              existing.linkage_name.empty()
                  ? std::string_view(existing.name)
                  : std::string_view(existing.linkage_name);
          if (existing_linkage == preferred_name) {
            return instance.instance;
          }
          // Equal semantic arguments have exactly one canonical external
          // spelling. Reaching a different spelling indicates inconsistent
          // work-key construction, not a second legal specialization.
          diagnostics_.error(
              use_range,
              "generic procedure instance has inconsistent external identity");
          return {};
        }
        return instance.instance;
      }
    }

    for (const ProcedureInstance &instance : instances_) {
      if (instance.source != source ||
          instance.type_substitutions.size() != type_substitutions.size() ||
          instance.value_substitutions.size() != value_substitutions.size() ||
          instance.pack_types != pack_types) {
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
      instance_symbol.name += "$pack" + std::to_string(pack_types.size());
      for (TypeId type : pack_types) {
        instance_symbol.name += "$t" + std::to_string(type.value);
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
    const TypeId fixed_signature = substitute_type(
        source_symbol.type,
        type_substitutions,
        value_substitutions,
        use_range);
    const Type &fixed = semantic_.types.type(fixed_signature);
    std::vector<TypeId> concrete_parameters;
    if (!fixed.members.empty()) {
      concrete_parameters.assign(fixed.members.begin(), fixed.members.end() - 1);
    }
    concrete_parameters.insert(
        concrete_parameters.end(), pack_types.begin(), pack_types.end());
    const TypeId result = fixed.members.empty()
        ? semantic_.types.builtins().void_type
        : fixed.members.back();
    instance_symbol.type = semantic_.types.procedure(
        concrete_parameters, result, fixed.c_calling_convention);
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
    const std::vector<SymbolId> source_parameter_symbols =
        semantic_.symbols.symbols_in_scope(*source_parameters);
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
    SymbolId concrete_pack_binding;
    if (pack != nullptr) {
      const Symbol &source_binding = semantic_.symbols.symbol(pack->binding);
      Symbol marker = source_binding;
      marker.scope = instance_scope;
      concrete_pack_binding =
          semantic_.symbols.declare(std::move(marker), diagnostics_);
    }
    std::vector<SymbolId> pack_parameters;
    pack_parameters.reserve(pack_types.size());
    for (std::size_t index = 0; index < pack_types.size(); ++index) {
      Symbol concrete;
      // The source pack binding is compile-time structure and therefore is not
      // cloned. Each element receives an internal ordinary parameter whose
      // order exactly matches the tail of the concrete procedure signature.
      // Static iteration aliases source names to these symbols; the spelling is
      // intentionally unnameable in Draft source.
      concrete.name = "$pack_element_" + std::to_string(index);
      concrete.kind = SymbolKind::Parameter;
      concrete.scope = instance_scope;
      concrete.type = pack_types[index];
      concrete.syntax = pack == nullptr ? SyntaxReference{} : pack->syntax;
      concrete.name_range = use_range;
      const SymbolId parameter_id =
          semantic_.symbols.declare(std::move(concrete), diagnostics_);
      if (parameter_id.is_valid()) pack_parameters.push_back(parameter_id);
    }
    const std::vector<ParametricArgument> arguments = requested_arguments;
    std::vector<ConcreteProcedureTypeSubstitution> retained_types;
    retained_types.reserve(type_substitutions.size());
    for (const TypeSubstitution &substitution : type_substitutions) {
      retained_types.push_back(
          {substitution.parameter, substitution.replacement});
    }
    std::vector<ConcreteProcedureValueSubstitution> retained_values;
    retained_values.reserve(value_substitutions.size());
    for (const ValueSubstitution &substitution : value_substitutions) {
      // instantiate_procedure rejects symbolic and deferred bindings before
      // reaching this point. Retaining only the exact value is therefore the
      // complete environment needed by a later concrete body task.
      retained_values.push_back({substitution.parameter, substitution.value});
    }
    instances_.push_back({
        source,
        instance_id,
        std::move(type_substitutions),
        std::move(value_substitutions),
        std::move(pack_types),
        std::move(pack_parameters),
        concrete_pack_binding,
    });
    semantic_.parametric_instances.push_back({
        source,
        instance_id,
        arguments,
        instances_.back().pack_types,
        std::move(retained_types),
        std::move(retained_values),
        !preferred_name.empty(),
    });
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
          name->text == "ptr_sub" || name->text == "raw_data" ||
          name->text == "type_of" ||
          is_type_inspection_query(name->text)) {
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
    if (*intrinsic == "type_of") {
      if (argument_count != 1) {
        diagnostics_.error(call.range, "type_of requires exactly one value argument");
        expression.type = semantic_.types.builtins().invalid;
      } else {
        // Checking the operand establishes its static type but deliberately
        // leaves the resulting HIR node unreferenced. `type_of(call())` must
        // neither execute nor lower call(); it observes only the type assigned
        // by ordinary expression checking.
        const std::size_t initial_operand_errors = diagnostics_.error_count();
        const HirExpressionId operand = check_expression(
            tree, call.children[1], scope);
        TypeId observed = hir_.expression(operand).type;
        observed = default_inferred_runtime_type(observed);
        if (is_invalid_type(observed)) {
          // Preserve the operand's precise diagnostic. The fallback is needed
          // only for an invalid internal result which reached this boundary
          // without its own user-facing error; reporting both would obscure
          // the actual malformed call, member, or operator.
          if (diagnostics_.error_count() == initial_operand_errors) {
            diagnostics_.error(
                tree.node(call.children[1]).range,
                "type_of requires an expression with a known static type");
          }
          expression.type = semantic_.types.builtins().invalid;
        } else if (!current_instance_index_.has_value() &&
                   contains_symbolic_type(observed)) {
          // The symbolic template pass knows that type_of returns a type but
          // does not yet know which type. Preserve that fact as typed,
          // non-lowered HIR. Rechecking a concrete instance substitutes the
          // parameter first and folds this same source expression exactly.
          expression.type = apply_expected_type(
              semantic_.types.builtins().meta_type, expected, call.range);
        } else {
          expression.kind = HirExpressionKind::Constant;
          expression.constant = ConstantValue::make_type(observed.value);
          expression.type = apply_expected_type(
              semantic_.types.builtins().meta_type, expected, call.range);
        }
      }
    } else if (is_type_inspection_query(*intrinsic)) {
      const bool indexed = *intrinsic == "type_member_name" ||
          *intrinsic == "type_member_type" ||
          *intrinsic == "type_member_offset" ||
          *intrinsic == "type_member_is_packed" ||
          *intrinsic == "type_member_bit_width" ||
          *intrinsic == "type_member_bit_offset" ||
          *intrinsic == "type_member_value" ||
          *intrinsic == "type_parameter_type";
      const std::size_t required_arguments = indexed ? 2 : 1;
      if (argument_count != required_arguments) {
        diagnostics_.error(
            call.range,
            *intrinsic + " requires " +
                (indexed ? "a type and one compile-time index"
                         : "exactly one type argument"));
        expression.type = semantic_.types.builtins().invalid;
      } else {
        HirExpressionId type_value;
        const SyntaxNode &type_argument = tree.node(call.children[1]);
        if (node_is_type_syntax(type_argument.kind)) {
          const TypeId resolved = type_value_expression(
              tree, call.children[1], scope);
          HirExpression literal;
          literal.kind = HirExpressionKind::Constant;
          literal.range = type_argument.range;
          literal.type = semantic_.types.builtins().meta_type;
          literal.constant = ConstantValue::make_type(resolved.value);
          type_value = hir_.add_expression(std::move(literal));
        } else {
          type_value = check_expression(
              tree,
              call.children[1],
              scope,
              semantic_.types.builtins().meta_type);
        }
        // Checking the index appends another HIR expression and may grow the
        // owning vector. Copy this small row before that append; retaining a
        // reference here would make indexed reflection depend on vector
        // capacity and could misdiagnose a valid type value.
        const HirExpression checked_type = hir_.expression(type_value);
        const bool defer_symbolic_type =
            !current_instance_index_.has_value() &&
            expression_references_parametric_parameter(
                tree, call.children[1], scope);
        std::optional<std::uint64_t> index;
        bool defer_symbolic_index = false;
        if (indexed) {
          const HirExpressionId checked_index = check_expression(
              tree,
              call.children[2],
              scope,
              semantic_.types.builtins().usize_type);
          const HirExpression &index_expression = hir_.expression(checked_index);
          if (index_expression.kind == HirExpressionKind::Constant &&
              index_expression.constant.kind == ConstantKind::Integer) {
            index = index_expression.constant.integer.to_u64();
          }
          defer_symbolic_index =
              !current_instance_index_.has_value() &&
              expression_references_parametric_parameter(
                  tree, call.children[2], scope);
          if (!index.has_value() && !defer_symbolic_index) {
            diagnostics_.error(
                tree.node(call.children[2]).range,
                *intrinsic + " index must be a compile-time usize");
          }
        }
        if (is_invalid_type(checked_type.type)) {
          // The argument checker has already reported the malformed nested
          // expression. A structural query cannot add useful information when
          // no type value reached this boundary, so propagate the invalid type
          // without replacing the primary diagnostic with a query complaint.
          expression.type = semantic_.types.builtins().invalid;
        } else if (defer_symbolic_type && (!indexed || index.has_value() ||
                                           defer_symbolic_index)) {
          // Query applicability and index bounds depend on the eventual type
          // (and possibly a value parameter). The template pass retains only
          // the query's fixed result category. Concrete instances perform the
          // full inspect_type check and fold the exact result.
          expression.type = apply_expected_type(
              symbolic_inspection_result_type(*intrinsic),
              expected,
              call.range);
        } else if (checked_type.kind != HirExpressionKind::Constant ||
            checked_type.constant.kind != ConstantKind::Type ||
            checked_type.constant.type_index >= semantic_.types.size()) {
          diagnostics_.error(
              tree.node(call.children[1]).range,
              *intrinsic + " requires a compile-time type value");
          expression.type = semantic_.types.builtins().invalid;
        } else if (!indexed || index.has_value()) {
          const TypeInspectionAttempt inspected = inspect_type(
              semantic_,
              *intrinsic,
              TypeId{checked_type.constant.type_index},
              index);
          if (inspected.required_facet.has_value()) {
            diagnostics_.error(
                call.range,
                *intrinsic + " requires " + std::string(type_facet_name(
                    *inspected.required_facet)));
            expression.type = semantic_.types.builtins().invalid;
          } else if (!inspected.result.has_value()) {
            diagnostics_.error(
                call.range,
                inspected.error.empty()
                    ? *intrinsic + " could not inspect the supplied type"
                    : inspected.error);
            expression.type = semantic_.types.builtins().invalid;
          } else {
            expression.kind = HirExpressionKind::Constant;
            expression.constant = inspected.result->value;
            expression.type = apply_expected_type(
                inspected.result->type, expected, call.range);
          }
        } else {
          expression.type = semantic_.types.builtins().invalid;
        }
      }
    } else if (*intrinsic == "size_of" || *intrinsic == "align_of") {
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
      if (type_validation_only_) {
        // A dead conditional branch still has to contain a well-typed call,
        // but its assertion is not executed. Check every supplied operand and
        // leave the value-dependent assertion to ordinary evaluation when the
        // branch is selected.
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
        return hir_.add_expression(std::move(expression));
      }
      const ConstantTable active_constants = active_constant_table();
      const std::vector<ConstantTypeBinding> active_types =
          active_constant_types();
      const std::vector<ConstantStaticPackBinding> active_packs =
          active_constant_packs();
      const bool defer_symbolic_assertion =
          !current_instance_index_.has_value() && argument_count >= 1 &&
          (expression_references_parametric_parameter(
               tree, call.children[1], scope) ||
           active_dependent_when_depth_ != 0);
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
            &active_types,
            &active_packs);
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
                &active_types,
                &active_packs);
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
    } else if (*intrinsic == "raw_data") {
      if (argument_count != 1) {
        diagnostics_.error(
            call.range, "raw_data requires exactly one string argument");
        expression.type = semantic_.types.builtins().invalid;
      } else {
        const HirExpressionId argument =
            check_expression(tree, call.children[1], scope);
        expression.operands.push_back(argument);
        const TypeId argument_type = hir_.expression(argument).type;
        if (!is_invalid_type(argument_type) &&
            argument_type != semantic_.types.builtins().string_type) {
          // raw_data is deliberately narrower than a generic representation
          // escape hatch. Draft 1 exposes immutable string storage for native
          // read-only consumers; slices and arrays already expose addresses
          // through their ordinary element operations.
          diagnostics_.error(
              tree.node(call.children[1]).range,
              "raw_data requires a string argument");
          expression.type = semantic_.types.builtins().invalid;
        } else {
          expression.type = apply_expected_type(
              semantic_.types.multi_pointer(
                  semantic_.types.builtins().u8_type),
              expected,
              call.range);
        }
      }
    } else if (*intrinsic == "len") {
      if (argument_count != 1) {
        diagnostics_.error(call.range, "len requires exactly one argument");
        expression.type = semantic_.types.builtins().invalid;
      } else {
        const StaticArgumentPack *pack = active_static_argument_pack(
            tree, call.children[1], scope);
        if (pack != nullptr) {
          expression.type = apply_expected_type(
              semantic_.types.builtins().usize_type, expected, call.range);
          if (current_instance_index_.has_value()) {
            // A concrete instance owns the exact ordered tail. Fold its length
            // here so neither the pack marker nor an intrinsic reaches MIR.
            expression.kind = HirExpressionKind::Constant;
            expression.constant = ConstantValue::make_integer(
                BigInteger::from_u64(
                    instances_[*current_instance_index_].pack_types.size()));
          }
          return hir_.add_expression(std::move(expression));
        }
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
            (numeric_value_type(source) || is_numeric(source));
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
        if (!type_validation_only_ &&
            (numeric || boolean_storage ||
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
    if (const std::optional<HirExpressionId> target =
            check_compile_time_leaf_expression(
                tree, expression_id, scope, expected)) {
      return *target;
    }

    // Type constructors are first-class compile-time values when expression
    // grammar places them beside another value, for example
    // `type_of(bytes) == [^]u8`. Named types already arrive through the
    // NameExpression case below, while structural constructors retain their
    // dedicated syntax nodes. Resolve those nodes with the ordinary type
    // resolver so validation-only preflight and normal body checking observe
    // the same exact TypeId. The resulting HIR constant is compile-time-only
    // and the existing meta-type boundary prevents it from reaching MIR.
    if (node_is_type_syntax(node.kind)) {
      const TypeId value = type_value_expression(tree, expression_id, scope);
      if (is_invalid_type(value)) return invalid_expression(node.range);
      HirExpression expression;
      expression.kind = HirExpressionKind::Constant;
      expression.range = node.range;
      expression.type = apply_expected_type(
          semantic_.types.builtins().meta_type, expected, node.range);
      expression.constant = ConstantValue::make_type(value.value);
      return hir_.add_expression(std::move(expression));
    }

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
      if (names.size() == 1) {
        const std::optional<SymbolId> visible_symbol =
            semantic_.symbols.lookup(scope, names.front().text);
        std::optional<TypeId> type_value;
        if (visible_symbol.has_value()) {
          const Symbol &binding = semantic_.symbols.symbol(*visible_symbol);
          if (binding.kind == SymbolKind::Type ||
              binding.kind == SymbolKind::TypeParameter) {
            type_value = substitute_active(binding.type, node.range);
          }
        } else {
          // Builtin type spellings participate in expression syntax only when
          // no lexical value shadows them. This matters for the conventional
          // local name `byte`, which must remain a value in `for byte in ...`.
          type_value = semantic_.types.find_builtin(names.front().text);
        }
        if (type_value.has_value()) {
          HirExpression expression;
          expression.kind = HirExpressionKind::Constant;
          expression.range = node.range;
          expression.type = apply_expected_type(
              semantic_.types.builtins().meta_type, expected, node.range);
          expression.constant = ConstantValue::make_type(type_value->value);
          return hir_.add_expression(std::move(expression));
        }
      }
      if (names.size() == 1 && names.front().text == "context") {
        if (!current_procedure_.is_valid()) {
          diagnostics_.error(
              names.front().range,
              "the built-in context value is available only in a procedure");
          return invalid_expression(node.range);
        }
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
        expression.storage_alignment = natural_alignment(expression.type);
        return hir_.add_expression(std::move(expression));
      }
      if (active_static_argument_pack(tree, expression_id, scope) != nullptr) {
        diagnostics_.error(
            names.front().range,
            "static argument pack may be used only by len or static iteration");
        return invalid_expression(node.range);
      }
      std::optional<SymbolId> found =
          semantic_.symbols.lookup(scope, names.front().text);
      if (!found.has_value()) {
        diagnostics_.error(names.front().range, "unknown name '" + names.front().text + "'");
        return invalid_expression(node.range);
      }
      if (is_enclosing_static_argument_pack_binding(*found)) {
        diagnostics_.error(
            names.front().range,
            "nested procedure cannot capture an enclosing static argument pack");
        return invalid_expression(node.range);
      }
      for (const StaticPackValueAlias &alias : active_pack_value_aliases_) {
        if (alias.alias == *found) {
          found = alias.parameter;
          break;
        }
      }
      const Symbol symbol = semantic_.symbols.symbol(*found);
      if (symbol.kind == SymbolKind::Procedure &&
          static_argument_pack(*found) != nullptr) {
        diagnostics_.error(
            names.front().range,
            "procedure with a static argument pack must be called directly");
        return invalid_expression(node.range);
      }
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
      const TypeId symbol_type = refined_symbol_type(
          *found, substitute_active(symbol.type, node.range));
      expression.type = apply_expected_type(
          symbol_type, expected, node.range);
      // Parameters are immutable value bindings. Pointer and slice parameters
      // may still mutate the storage they explicitly reference through
      // dereference/index operations, but the parameter slot itself and fields
      // or fixed-array elements inside its copied value are not addressable.
      expression.addressable = symbol.kind == SymbolKind::Variable ||
          symbol.kind == SymbolKind::Local;
      if (expression.addressable) {
        expression.storage_alignment = natural_alignment(expression.type);
      }
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
          composite.kind != TypeKind::Union) {
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
                  : "union composite element must name a field");
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
      if (composite.kind == TypeKind::Union && element_count != 1) {
        diagnostics_.error(
            node.range,
            "union composite literal must initialize exactly one field");
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
        } else if (operand.bit_field) {
          diagnostics_.error(
              node.range, "address-of is not defined for a bit field");
          result = semantic_.types.builtins().invalid;
        } else if (operand.storage_alignment < natural_alignment(operand.type)) {
          // A typed pointer promises the pointee type's natural alignment.
          // Direct packed-field reads and writes remain valid because MIR can
          // issue an under-aligned memory operation, but manufacturing ^T here
          // would let that weaker occurrence escape behind a stronger type.
          diagnostics_.error(
              node.range,
              "address-of cannot form a naturally aligned pointer to this packed field");
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
      const bool left_needs_context =
          needs_value_context(tree, node.children[0]);
      const bool right_needs_context =
          needs_value_context(tree, node.children[1]);
      HirExpressionId left_id;
      HirExpressionId right_id;
      if (left_needs_context && !right_needs_context) {
        right_id = check_expression(tree, node.children[1], scope);
        left_id = is_invalid_type(hir_.expression(right_id).type)
            ? invalid_expression(tree.node(node.children[0]).range)
            : check_expression(
                  tree,
                  node.children[0],
                  scope,
                  hir_.expression(right_id).type);
      } else {
        left_id = check_expression(tree, node.children[0], scope);
        TypeId right_expected;
        if (right_needs_context) {
          right_expected = hir_.expression(left_id).type;
        }
        right_id = right_needs_context && is_invalid_type(right_expected)
            ? invalid_expression(tree.node(node.children[1]).range)
            : check_expression(
                  tree, node.children[1], scope, right_expected);
      }
      const TypeId left = hir_.expression(left_id).type;
      const TypeId right = hir_.expression(right_id).type;
      if (is_invalid_type(left) || is_invalid_type(right)) {
        // Both non-contextual operands were still checked above, so independent
        // source errors survive. The enclosing operator, however, has no
        // meaningful type relation to diagnose once either child is invalid.
        // A contextual nil/alternative operand is represented by an invalid
        // placeholder when its sibling could not supply the required type.
        return invalid_expression(node.range);
      }
      if ((operation == TokenKind::EqualEqual ||
           operation == TokenKind::BangEqual) &&
          hir_.expression(left_id).kind == HirExpressionKind::Constant &&
          hir_.expression(right_id).kind == HirExpressionKind::Constant &&
          hir_.expression(left_id).constant.kind == ConstantKind::Type &&
          hir_.expression(right_id).constant.kind == ConstantKind::Type) {
        bool equal = hir_.expression(left_id).constant.type_index ==
            hir_.expression(right_id).constant.type_index;
        if (operation == TokenKind::BangEqual) equal = !equal;
        HirExpression comparison;
        comparison.kind = HirExpressionKind::Constant;
        comparison.range = node.range;
        comparison.type = apply_expected_type(
            semantic_.types.builtins().bool_type, expected, node.range);
        comparison.constant = ConstantValue::make_bool(equal);
        return hir_.add_expression(std::move(comparison));
      }
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
            left_runtime_kind == TypeKind::Enum ||
            left_runtime_kind == TypeKind::MetaType || pointer_equality;
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
             operation == TokenKind::Pipe || operation == TokenKind::Tilde) &&
            !is_integer(result)) {
          diagnostics_.error(node.range, "operator requires integer operands");
          result = semantic_.types.builtins().invalid;
        }
      }
      const bool left_untyped_numeric =
          is_untyped_integer(left) || is_untyped_float(left);
      const bool right_untyped_numeric =
          is_untyped_integer(right) || is_untyped_float(right);
      if (!type_validation_only_ &&
          result == semantic_.types.builtins().bool_type &&
          left_untyped_numeric && right_untyped_numeric) {
        // An all-untyped comparison has no concrete machine type to inherit.
        // Choosing int/f64 here would reject arbitrary-precision integers or
        // round exact decimal values before comparing them. Both operands are
        // necessarily compile-time values, so preserve their specified exact
        // domains by replacing the complete comparison with its bool result.
        const ConstantTable visible_constants = active_constant_table();
        const std::vector<ConstantTypeBinding> visible_types =
            active_constant_types();
        const std::vector<ConstantStaticPackBinding> visible_packs =
            active_constant_packs();
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
                &visible_types,
                &visible_packs);
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
      std::optional<ExplicitProcedureArguments> explicit_arguments;
      const SyntaxNode &callee_syntax = tree.node(node.children.front());
      if (callee_syntax.kind == NodeKind::BracketExpression &&
          !callee_syntax.children.empty()) {
        const NodeId base_id = callee_syntax.children.front();
        std::optional<SymbolId> base_symbol;
        if (const std::optional<SourceName> base_name =
                single_name_expression(tree, base_id)) {
          base_symbol = semantic_.symbols.lookup(scope, base_name->text);
        } else {
          base_symbol = imported_member(tree, tree.node(base_id), scope);
        }
        if (base_symbol.has_value() &&
            static_argument_pack(*base_symbol) != nullptr) {
          inferred_template = *base_symbol;
          explicit_arguments = check_explicit_procedure_arguments(
              tree, callee_syntax, scope, *base_symbol);
          if (!explicit_arguments->valid) {
            return invalid_expression(node.range);
          }
        }
      } else if (const std::optional<SourceName> callee_name =
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
            const StaticArgumentPack *pack =
                static_argument_pack(*inferred_template);
            // Resolver and interface import both store the fixed-prefix count
            // beside a source procedure type containing exactly that prefix
            // plus its result. A mismatch is corrupted semantic state, not a
            // recoverable property of source input.
            assert(pack == nullptr ||
                pack->fixed_parameter_count == parameter_count);
            const BoundProcedureArguments bound = bind_procedure_arguments(
                tree,
                node,
                &candidate,
                parameter_count,
                pack != nullptr);
            if (!bound.valid) {
              return invalid_expression(node.range);
            }
            std::vector<HirExpressionId> arguments;
            std::vector<std::uint32_t> argument_parameter_indices;
            std::vector<TypeSubstitution> type_substitutions =
                explicit_arguments.has_value()
                ? std::move(explicit_arguments->type_substitutions)
                : std::vector<TypeSubstitution>{};
            std::vector<ValueSubstitution> value_substitutions =
                explicit_arguments.has_value()
                ? std::move(explicit_arguments->value_substitutions)
                : std::vector<ValueSubstitution>{};
            std::vector<TypeId> pack_types;
            for (std::size_t index = 0;
                 index < bound.explicit_expressions.size(); ++index) {
              const NodeId supplied_id = bound.explicit_expressions[index];
              const std::size_t parameter_index =
                  bound.explicit_parameter_indices[index];
              if (parameter_index >= parameter_count) {
                const HirExpressionId argument = check_expression(
                    tree, supplied_id, scope);
                TypeId concrete = default_inferred_runtime_type(
                    hir_.expression(argument).type);
                contextualize_inferred_runtime_expression(argument, concrete);
                hir_.expression_mut(argument).type = concrete;
                arguments.push_back(argument);
                argument_parameter_indices.push_back(
                    static_cast<std::uint32_t>(parameter_index));
                pack_types.push_back(concrete);
                continue;
              }
              const TypeId argument_pattern = substitute_type(
                  template_signature.members[parameter_index],
                  type_substitutions,
                  value_substitutions,
                  tree.node(supplied_id).range);
              const TypeId argument_expected =
                  contains_symbolic_type(argument_pattern)
                  ? TypeId{}
                  : argument_pattern;
              const HirExpressionId argument =
                  check_expression(
                      tree,
                      supplied_id,
                      scope,
                      argument_expected);
              arguments.push_back(argument);
              argument_parameter_indices.push_back(
                  static_cast<std::uint32_t>(parameter_index));
              if (!infer_type_argument(
                      *inferred_template,
                      template_signature.members[parameter_index],
                      hir_.expression(argument).type,
                      type_substitutions,
                      value_substitutions)) {
                diagnostics_.error(
                    tree.node(supplied_id).range,
                    "procedure type arguments cannot be inferred uniquely");
                return invalid_expression(node.range);
              }
            }
            for (std::uint32_t parameter_index :
                 bound.default_parameter_indices) {
              const TypeId default_type = substitute_type(
                  template_signature.members[parameter_index],
                  type_substitutions,
                  value_substitutions,
                  node.range);
              arguments.push_back(default_argument_expression(
                  candidate, parameter_index, default_type, node.range));
              argument_parameter_indices.push_back(parameter_index);
            }
            // A template HIR row is semantic evidence, not executable code.
            // Do not let a concrete-looking call inside that row manufacture a
            // native specialization; the concrete enclosing body is checked
            // again and creates every instance it can actually execute.
            const bool symbolic = type_validation_only_ ||
                current_procedure_is_template_ ||
                (!current_instance_index_.has_value() &&
                 has_symbolic_type_substitution(type_substitutions));
            bool symbolic_pack = false;
            for (TypeId type : pack_types) {
              if (contains_symbolic_type(type)) {
                symbolic_pack = true;
                break;
              }
            }
            SymbolId callee_symbol = *inferred_template;
            TypeId concrete_signature_id;
            if (symbolic || symbolic_pack) {
              const TypeId fixed_signature = substitute_type(
                  candidate.type,
                  type_substitutions,
                  value_substitutions,
                  node.range);
              const Type &fixed = semantic_.types.type(fixed_signature);
              std::vector<TypeId> concrete_parameters;
              if (!fixed.members.empty()) {
                concrete_parameters.assign(
                    fixed.members.begin(), fixed.members.end() - 1);
              }
              concrete_parameters.insert(
                  concrete_parameters.end(), pack_types.begin(), pack_types.end());
              const TypeId result = fixed.members.empty()
                  ? semantic_.types.builtins().void_type
                  : fixed.members.back();
              concrete_signature_id = semantic_.types.procedure(
                  concrete_parameters, result, fixed.c_calling_convention);
            } else {
              callee_symbol = instantiate_procedure(
                  *inferred_template,
                  std::move(type_substitutions),
                  std::move(value_substitutions),
                  std::move(pack_types),
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
              const std::size_t parameter_index =
                  argument_parameter_indices[index];
              const TypeId concrete = apply_expected_type(
                  argument.type,
                  concrete_signature.members[parameter_index],
                  argument.range);
              contextualize_numeric_expression(arguments[index], concrete);
              argument.type = concrete;
              expression.operands.push_back(arguments[index]);
              expression.call_parameter_indices.push_back(
                  static_cast<std::uint32_t>(parameter_index));
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
      std::optional<Symbol> direct_declaration;
      const HirExpression &callee_expression = hir_.expression(callee);
      if (callee_expression.kind == HirExpressionKind::Symbol &&
          callee_expression.symbol.is_valid()) {
        const Symbol &candidate =
            semantic_.symbols.symbol(callee_expression.symbol);
        if (candidate.kind == SymbolKind::Procedure) {
          direct_declaration = candidate;
        }
      }
      const BoundProcedureArguments bound = bind_procedure_arguments(
          tree,
          node,
          direct_declaration.has_value() ? &*direct_declaration : nullptr,
          parameter_count,
          false);
      if (!bound.valid) {
        return invalid_expression(node.range);
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Call;
      expression.range = node.range;
      expression.operands.push_back(callee);
      for (std::size_t index = 0;
           index < bound.explicit_expressions.size(); ++index) {
        const std::uint32_t parameter_index =
            bound.explicit_parameter_indices[index];
        expression.operands.push_back(check_expression(
            tree,
            bound.explicit_expressions[index],
            scope,
            signature.members[parameter_index]));
        expression.call_parameter_indices.push_back(parameter_index);
      }
      for (std::uint32_t parameter_index :
           bound.default_parameter_indices) {
        assert(direct_declaration.has_value());
        expression.operands.push_back(default_argument_expression(
            *direct_declaration,
            parameter_index,
            signature.members[parameter_index],
            node.range));
        expression.call_parameter_indices.push_back(parameter_index);
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
        if (symbol.kind == SymbolKind::Procedure &&
            static_argument_pack(*imported) != nullptr) {
          diagnostics_.error(
              node.range,
              "procedure with a static argument pack must be called directly");
          return invalid_expression(node.range);
        }
        if (symbol.kind == SymbolKind::Type ||
            symbol.kind == SymbolKind::TypeParameter) {
          HirExpression expression;
          expression.kind = HirExpressionKind::Constant;
          expression.range = node.range;
          expression.type = apply_expected_type(
              semantic_.types.builtins().meta_type, expected, node.range);
          expression.constant = ConstantValue::make_type(
              substitute_active(symbol.type, node.range).value);
          return hir_.add_expression(std::move(expression));
        }
        if (symbol.kind == SymbolKind::Import) {
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
        if (expression.addressable) {
          expression.storage_alignment = natural_alignment(expression.type);
        }
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
        if (expression.addressable && member_index < tuple.member_offsets.size()) {
          expression.storage_alignment = offset_alignment(
              base.storage_alignment, tuple.member_offsets[member_index]);
        }
        return hir_.add_expression(std::move(expression));
      }
      const std::vector<SourceName> names =
          alternative_names_in_span(tree, node.token_begin, node.token_end);
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
      const std::optional<std::size_t> member_index =
          aggregate_member_index(*member);
      const Type &owner_type = semantic_.types.type(
          underlying_type_id(base.type));
      if (member_index.has_value() &&
          *member_index < owner_type.member_layouts.size() &&
          owner_type.member_layouts[*member_index].kind ==
              FieldLayoutKind::BitField) {
        expression.bit_field = true;
        expression.bit_width =
            owner_type.member_layouts[*member_index].bit_width;
        if (*member_index < owner_type.member_bit_offsets.size()) {
          expression.bit_offset = owner_type.member_bit_offsets[*member_index];
        }
      } else if (expression.addressable) {
        const std::optional<AggregateMember> storage =
            aggregate_member(*member);
        if (storage.has_value()) {
          expression.storage_alignment = offset_alignment(
              base.storage_alignment, storage->offset);
        }
      }
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
      expression.storage_alignment = natural_alignment(expression.type);
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
          ExplicitProcedureArguments arguments =
              check_explicit_procedure_arguments(
                  tree, node, scope, base_expression.symbol);
          if (!arguments.valid) return invalid_expression(node.range);
          std::vector<TypeSubstitution> type_substitutions =
              std::move(arguments.type_substitutions);
          std::vector<ValueSubstitution> value_substitutions =
              std::move(arguments.value_substitutions);
          if (type_validation_only_ || current_procedure_is_template_ ||
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
              {},
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
      const TypeId index_type = hir_.expression(index_id).type;
      if (!is_invalid_type(index_type) &&
          index_type != semantic_.types.builtins().usize_type) {
        diagnostics_.error(
            tree.node(node.children[1]).range,
            "index must have type usize");
      }
      HirExpression expression;
      expression.kind = HirExpressionKind::Index;
      expression.range = node.range;
      const TypeId element = base.kind == TypeKind::String
          ? semantic_.types.builtins().u8_type
          : base.element;
      expression.type = apply_expected_type(element, expected, node.range);
      expression.operands = {base_id, index_id};
      if (!type_validation_only_) {
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
      }
      // A string is an immutable byte view. Lowering still computes an address
      // internally to load the byte, but source code may not take that address
      // or use the indexed expression as an assignment target.
      expression.addressable =
          base.kind == TypeKind::Slice || base.kind == TypeKind::MultiPointer ||
          (base.kind == TypeKind::Array && base_expression.addressable);
      if (expression.addressable) {
        expression.storage_alignment =
            base.kind == TypeKind::Array
            ? std::min(
                  base_expression.storage_alignment,
                  natural_alignment(expression.type))
            : natural_alignment(expression.type);
      }
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
        const TypeId bound_type = hir_.expression(bound).type;
        if (!is_invalid_type(bound_type) &&
            bound_type != semantic_.types.builtins().usize_type) {
          diagnostics_.error(
              tree.node(node.children[index]).range,
              "slice bound must have type usize");
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
      if (!type_validation_only_) {
        if (const std::optional<std::uint64_t> length =
              compile_time_length(base_id)) {
          std::size_t operand_index = 1;
          std::optional<std::uint64_t> low = 0;
          std::optional<std::uint64_t> high = length;
          if (expression.slice_has_low) {
            const std::optional<BigInteger> value =
                constant_integer_expression(expression.operands[operand_index++]);
            // Spell the absent branch as reset rather than assigning a
            // temporary nullopt. GCC 13 otherwise reports the engaged storage
            // of std::optional<uint64_t> as maybe-uninitialized in optimized
            // builds even though the optional operation is well-defined.
            if (value.has_value()) {
              low = value->to_u64();
            } else {
              low.reset();
            }
          }
          if (expression.slice_has_high) {
            const std::optional<BigInteger> value =
                constant_integer_expression(expression.operands[operand_index]);
            if (value.has_value()) {
              high = value->to_u64();
            } else {
              high.reset();
            }
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
      }
      return hir_.add_expression(std::move(expression));
    }

    case NodeKind::ConditionalExpression: {
      if (node.children.size() != 3) return invalid_expression(node.range);
      const HirExpressionId condition = check_expression(
          tree, node.children[1], scope, semantic_.types.builtins().bool_type);

      // A direct `nil` or `.alternative` branch cannot be checked without a
      // type. When there is no outer expected type and exactly one branch needs
      // context, check the independently typed branch first and use its type for
      // the contextual branch. This is semantic discovery only: MIR still
      // evaluates the condition first and then only the selected branch.
      HirExpressionId left;
      HirExpressionId right;
      const bool left_needs_context =
          needs_value_context(tree, node.children[0]);
      const bool right_needs_context =
          needs_value_context(tree, node.children[2]);
      if (!expected.is_valid() && left_needs_context &&
          !right_needs_context) {
        right = check_expression(tree, node.children[2], scope);
        left = check_expression(
            tree,
            node.children[0],
            scope,
            hir_.expression(right).type);
      } else {
        left = check_expression(tree, node.children[0], scope, expected);
      }
      const TypeId left_type = hir_.expression(left).type;
      if (!right.is_valid()) {
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
        right = check_expression(
            tree,
            node.children[2],
            scope,
            right_expected);
      }
      TypeId result = left_type;
      const TypeId right_type = hir_.expression(right).type;
      if (is_invalid_type(left_type) || is_invalid_type(right_type)) {
        // Both branches were checked above, so their precise source error is
        // already available. A conditional has no meaningful common-type
        // operation after either branch is invalid; attempting numeric
        // unification here would add a misleading secondary diagnostic to an
        // unrelated mismatch such as `"text" if true else 42`.
        return invalid_expression(node.range);
      }
      if (right_type != left_type) {
        result = common_numeric_type(left_type, right_type, node.range);
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
        diagnostics_.error(
            node.range,
            "contextual alternative requires an expected enum or variant type");
        return invalid_expression(node.range);
      }
      const TypeKind expected_kind = runtime_scalar_type(expected).kind;
      if (expected_kind != TypeKind::Enum && expected_kind != TypeKind::Variant) {
        diagnostics_.error(node.range, "contextual alternative expected type is not an enum or variant");
        return invalid_expression(node.range);
      }
      const std::vector<SourceName> names =
          names_in_span(tree, node.token_begin, node.token_end);
      if (names.empty()) return invalid_expression(node.range);
      if (expected_kind == TypeKind::Enum) {
        if (const std::optional<std::uint64_t> compiler_value =
                compiler_enum_member_value(
                    semantic_, expected, names.front().text)) {
          if (!node.children.empty()) {
            diagnostics_.error(
                node.range,
                "compiler-defined enum alternatives cannot carry a payload");
          }
          HirExpression expression;
          expression.kind = HirExpressionKind::Constant;
          expression.range = node.range;
          expression.type = expected;
          expression.constant = ConstantValue::make_integer(
              BigInteger::from_u64(*compiler_value));
          return hir_.add_expression(std::move(expression));
        }
      }
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
      if (expected_kind == TypeKind::Variant) {
        // Both payload-bearing and payload-free alternatives are aggregate
        // values. MIR inserts their source-order discriminator and optional
        // payload; representing the latter as a scalar constant would lose the
        // variant's physical storage type.
        expression.kind = HirExpressionKind::Composite;
        const bool has_payload = member.type != semantic_.types.builtins().void_type;
        if (has_payload != !node.children.empty()) {
          diagnostics_.error(node.range, has_payload
              ? "variant alternative requires a payload"
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
           {},
           {},
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
        expression.storage_alignment =
            hir_.expression(value).storage_alignment;
        expression.bit_field = hir_.expression(value).bit_field;
        expression.bit_width = hir_.expression(value).bit_width;
        expression.bit_offset = hir_.expression(value).bit_offset;
        return hir_.add_expression(std::move(expression));
      }
      return invalid_expression(node.range);
    }

    case NodeKind::SynthesisExpression: {
      add_body_agent_site(
          SemanticSiteKind::SynthesisExpression,
          {tree.file(), expression_id},
          scope,
          expected);
      HirExpression expression;
      expression.kind = HirExpressionKind::Synthesis;
      expression.range = node.range;
      expression.syntax = {tree.file(), expression_id};
      expression.scope = scope;
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
          const ConstantTable visible_constants = active_constant_table();
          const std::vector<ConstantTypeBinding> visible_types =
              active_constant_types();
          result = resolve_type_syntax(
              sources_, loaded_, semantic_, selections_, tree, child, scope,
              visible_constants, visible_types, target_, diagnostics_);
        } else if (tree.node(child).kind == NodeKind::AsmInput &&
                   !tree.node(child).children.empty()) {
          expression.operands.push_back(check_expression(
              tree, tree.node(child).children.front(), scope));
        } else if (tree.node(child).kind == NodeKind::SynthesisAssembly) {
          add_body_agent_site(
              SemanticSiteKind::SynthesisAssembly,
              {tree.file(), child},
              scope);
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
      case TokenKind::TildeEqual:
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
        target_,
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
    const std::vector<ConstantStaticPackBinding> visible_packs =
        active_constant_packs();
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
            &visible_types,
            {},
            &visible_packs);
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
    constants_.bindings.push_back({id, evaluated->value, type});
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
    for (NodeId child : tree.node(procedure_id).children) {
      if (tree.node(child).kind != NodeKind::ParameterList) continue;
      for (NodeId parameter : tree.node(child).children) {
        for (NodeId part : tree.node(parameter).children) {
          if (tree.node(part).kind == NodeKind::StaticPackType) {
            symbol.flags.parametric = true;
          }
        }
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
        active_constant_table(),
        active_constant_types(),
        target_,
        diagnostics_);
    signature = substitute_active(signature, declaration.range);
    semantic_.symbols.symbol_mut(id).type = signature;

    // Signature resolution creates symbols before the enclosing concrete
    // specialization is known to TypeResolver. Apply that specialization to
    // the runtime-bearing parameter rows and compile-time value parameter
    // types now. Type-parameter identities themselves must remain symbolic.
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes_for_read()) {
      if (owned.owner != id) continue;
      for (SymbolId child :
           semantic_.symbols.symbols_in_scope(owned.scope)) {
        Symbol &owned_symbol = semantic_.symbols.symbol_mut(child);
        if (owned_symbol.kind == SymbolKind::Parameter ||
            owned_symbol.kind == SymbolKind::ValueParameter) {
          owned_symbol.type = substitute_active(
              owned_symbol.type, owned_symbol.name_range);
        }
      }
    }

    // Package procedures close their defaults at the interface barrier. A
    // lexical procedure is discovered later, so close its declaration-owned
    // constants after both its signature and parameter-symbol rows have their
    // concrete enclosing substitutions. The active bindings permit a nested
    // declaration inside a concrete generic body to use enclosing compile-time
    // constants without admitting runtime capture.
    const std::vector<ConstantTypeBinding> default_types =
        active_constant_types();
    (void)finalize_procedure_parameter_defaults(
        sources_,
        loaded_,
        semantic_,
        id,
        target_,
        active_constant_table(),
        diagnostics_,
        &default_types);

    // A body nested in a symbolic outer template is itself non-executable even
    // when it has no parameters of its own. The concrete outer body is checked
    // again and creates the executable lexical procedure for that instance.
    // Publish the body as later work rather than checking it recursively here.
    // A concrete outer specialization contributes more than substitutions: its
    // pack marker identifies an illegal capture boundary. Snapshot the complete
    // active environment so the later root reproduces both the permitted
    // compile-time names and that rejection rule exactly.
    ProcedureBodyRoot nested_root;
    nested_root.symbol = id;
    nested_root.parametric_template =
        current_procedure_is_template_ || parametric;
    if (current_instance_index_.has_value()) {
      nested_root.enclosing_environment = retain_body_environment(
          instances_[*current_instance_index_]);
    }
    discovered_body_roots_.push_back(std::move(nested_root));
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
        const ConstantTable visible_constants = active_constant_table();
        const std::vector<ConstantTypeBinding> visible_types =
            active_constant_types();
        declared_type = resolve_type_syntax(
            sources_, loaded_, semantic_, selections_, tree, child, scope,
            visible_constants, visible_types, target_, diagnostics_);
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

  // Materializes the one branch selected for executable HIR. Braced contents
  // use the surrounding scope because `when` is a compile-time splice, while
  // `else when` needs only an ordered HIR container around its nested
  // statement. A missing branch is represented by no block.
  [[nodiscard]] std::optional<HirBlockId> check_selected_when_branch(
      const SyntaxTree &tree,
      const SyntaxNode &when,
      bool select_true,
      ScopeId scope,
      TypeId result_type,
      ControlDepth depth) {
    if (select_true) {
      if (when.children.size() < 2) return std::nullopt;
      return check_compile_time_block(
          tree, when.children[1], scope, result_type, depth);
    }
    if (when.children.size() < 3) return std::nullopt;
    const NodeId alternative = when.children[2];
    if (tree.node(alternative).kind != NodeKind::WhenStatement) {
      return check_compile_time_block(
          tree, alternative, scope, result_type, depth);
    }
    HirBlock nested;
    nested.scope = scope;
    nested.range = tree.node(alternative).range;
    nested.statements.push_back(check_statement(
        tree, alternative, scope, result_type, depth));
    return hir_.add_block(std::move(nested));
  }

  // Checks one possibility of a non-lowered parametric source template. A
  // temporary lexical scope prevents declarations from two mutually exclusive
  // symbolic possibilities colliding in the shared append-only SymbolTable;
  // concrete checking above still applies Draft's transparent `when` scope.
  // Agent sites snapshot both the ordinary branch path and the type fact.
  [[nodiscard]] HirBlockId check_symbolic_when_branch(
      const SyntaxTree &tree,
      NodeId branch_id,
      NodeId condition_id,
      ScopeId scope,
      TypeId result_type,
      ControlDepth depth,
      SemanticBranchRefinementKind branch_kind,
      const ActiveTypeRefinement *type_fact) {
    SemanticBranchRefinement branch_fact;
    branch_fact.kind = branch_kind;
    branch_fact.subject = {tree.file(), condition_id};
    branch_fact.subject_type = semantic_.types.builtins().bool_type;
    active_branch_refinements_.push_back(std::move(branch_fact));
    if (type_fact != nullptr) active_type_refinements_.push_back(*type_fact);
    // Any assertion in this symbolic possibility belongs to a concrete branch
    // selection, even when the assertion expression itself is a literal and
    // the `when` grants no type refinement (for example `when index == 0`).
    // Count dependent selections separately from ordinary runtime branch facts
    // so an `if` never delays a source-independent static assertion.
    ++active_dependent_when_depth_;

    HirBlockId checked;
    if (tree.node(branch_id).kind == NodeKind::WhenStatement) {
      HirBlock nested;
      nested.scope = semantic_.symbols.add_scope(
          ScopeKind::Block, scope, tree.node(branch_id).range);
      nested.range = tree.node(branch_id).range;
      nested.statements.push_back(check_statement(
          tree, branch_id, nested.scope, result_type, depth));
      checked = hir_.add_block(std::move(nested));
    } else {
      checked = check_block(tree, branch_id, scope, result_type, depth);
    }

    assert(active_dependent_when_depth_ != 0);
    --active_dependent_when_depth_;
    if (type_fact != nullptr) active_type_refinements_.pop_back();
    active_branch_refinements_.pop_back();
    return checked;
  }

  // Expands `for value, index in pack` while HIR is still structured. The
  // symbolic template pass checks one representative body using the pack's
  // element TypeParameter. A concrete instance emits one lexical block per
  // argument, in source order, and aliases the value binding to the ordinary
  // fixed-signature parameter created by instantiate_procedure. Thus MIR sees
  // neither a pack value nor a runtime loop.
  [[nodiscard]] std::optional<HirStatementId> check_static_pack_iteration(
      const SyntaxTree &tree,
      NodeId statement_id,
      ScopeId scope,
      TypeId result_type,
      ControlDepth depth) {
    const SyntaxNode &node = tree.node(statement_id);
    if (node.children.empty()) return std::nullopt;
    const SyntaxNode &header = tree.node(node.children.front());
    if (header.kind != NodeKind::IterationHeader || header.children.empty()) {
      return std::nullopt;
    }
    const StaticArgumentPack *pack = active_static_argument_pack(
        tree, header.children.front(), scope);
    if (pack == nullptr) return std::nullopt;

    HirStatement statement;
    statement.kind = HirStatementKind::CompileTimeSelection;
    statement.range = node.range;
    statement.syntax = {tree.file(), statement_id};
    if (node.children.size() < 2) {
      return hir_.add_statement(std::move(statement));
    }

    const SyntaxNode &iterable = tree.node(header.children.front());
    const std::vector<SourceName> names = names_in_span(
        tree, header.token_begin, iterable.token_begin);
    if (names.empty() || names.size() > 2) {
      diagnostics_.error(
          header.range,
          "static pack iteration requires a value and optional index binding");
      return hir_.add_statement(std::move(statement));
    }

    auto declare_iteration_binding = [&](
        const SourceName &name,
        ScopeId iteration_scope,
        SymbolKind kind,
        TypeId type) -> SymbolId {
      if (name.text == "_") return {};
      Symbol binding;
      binding.name = name.text;
      binding.kind = kind;
      binding.scope = iteration_scope;
      binding.type = type;
      binding.syntax = {tree.file(), node.children.front()};
      binding.name_range = name.range;
      return semantic_.symbols.declare(std::move(binding), diagnostics_);
    };

    if (!current_instance_index_.has_value()) {
      // One representative body is sufficient for source-level diagnostics,
      // refinements, judgments, and synthesis discovery. It is deliberately
      // non-lowered, so its symbolic value/index bindings need no runtime ABI.
      const ScopeId iteration_scope = semantic_.symbols.add_scope(
          ScopeKind::Block, scope, header.range);
      (void)declare_iteration_binding(
          names.front(),
          iteration_scope,
          SymbolKind::Parameter,
          pack->symbolic_element_type);
      if (names.size() == 2) {
        (void)declare_iteration_binding(
            names[1],
            iteration_scope,
            SymbolKind::ValueParameter,
            semantic_.types.builtins().usize_type);
      }
      statement.blocks.push_back(check_block(
          tree,
          node.children.back(),
          iteration_scope,
          result_type,
          depth));
      return hir_.add_statement(std::move(statement));
    }

    // Snapshot rather than retain references into instances_: checking one
    // expanded body can discover another specialization and append to that
    // vector, invalidating its storage before the next element is expanded.
    const std::vector<TypeId> pack_types =
        instances_[*current_instance_index_].pack_types;
    const std::vector<SymbolId> pack_parameters =
        instances_[*current_instance_index_].pack_parameters;
    // Both vectors are constructed together from the call's ordered tail. A
    // mismatch is an internal compiler error, never a property of Draft input.
    assert(pack_types.size() == pack_parameters.size());
    for (std::size_t index = 0; index < pack_types.size(); ++index) {
      const ScopeId iteration_scope = semantic_.symbols.add_scope(
          ScopeKind::Block, scope, header.range);
      const SymbolId value_alias = declare_iteration_binding(
          names.front(),
          iteration_scope,
          SymbolKind::Parameter,
          pack_types[index]);
      if (value_alias.is_valid()) {
        active_pack_value_aliases_.push_back(
            {value_alias, pack_parameters[index]});
      }

      if (names.size() == 2) {
        const SymbolId index_binding = declare_iteration_binding(
            names[1],
            iteration_scope,
            SymbolKind::Constant,
            semantic_.types.builtins().usize_type);
        if (index_binding.is_valid()) {
          constants_.bindings.push_back({
              index_binding,
              ConstantValue::make_integer(BigInteger::from_u64(index)),
              semantic_.types.builtins().usize_type,
          });
        }
      }
      statement.blocks.push_back(check_block(
          tree,
          node.children.back(),
          iteration_scope,
          result_type,
          depth));
      if (value_alias.is_valid()) active_pack_value_aliases_.pop_back();
    }
    return hir_.add_statement(std::move(statement));
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
        const HirExpressionId condition = check_expression(
            tree,
            node.children.front(),
            scope,
            semantic_.types.builtins().bool_type);
        statement.expressions.push_back(condition);

        SemanticBranchRefinement refinement;
        refinement.subject = {tree.file(), node.children.front()};
        refinement.subject_type = hir_.expression(condition).type;
        if (node.children.size() >= 2) {
          refinement.kind = SemanticBranchRefinementKind::ConditionTrue;
          active_branch_refinements_.push_back(refinement);
          statement.blocks.push_back(
              check_block(
                  tree, node.children[1], scope, result_type, depth));
          active_branch_refinements_.pop_back();
        }
        if (node.children.size() >= 3) {
          const NodeId alternative = node.children[2];
          refinement.kind = SemanticBranchRefinementKind::ConditionFalse;
          active_branch_refinements_.push_back(refinement);
          if (tree.node(alternative).kind == NodeKind::Block) {
            statement.blocks.push_back(
                check_block(tree, alternative, scope, result_type, depth));
          } else {
            HirBlock synthetic;
            synthetic.scope = semantic_.symbols.add_scope(
                ScopeKind::Block, scope, tree.node(alternative).range);
            synthetic.range = tree.node(alternative).range;
            synthetic.statements.push_back(check_statement(
                tree,
                alternative,
                synthetic.scope,
                result_type,
                depth));
            statement.blocks.push_back(
                hir_.add_block(std::move(synthetic)));
          }
          active_branch_refinements_.pop_back();
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
      if (const std::optional<HirStatementId> expanded =
              check_static_pack_iteration(
                  tree, statement_id, scope, result_type, depth)) {
        return *expanded;
      }
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
          SemanticBranchRefinement refinement;
          refinement.kind =
              SemanticBranchRefinementKind::LoopIteration;
          refinement.subject = {tree.file(), header.children.front()};
          refinement.subject_type = hir_.expression(iterable).type;
          active_branch_refinements_.push_back(std::move(refinement));
          statement.blocks.push_back(check_block(
              tree,
              node.children.back(),
              loop_scope,
              result_type,
              {depth.breakable + 1, depth.loops + 1}));
          active_branch_refinements_.pop_back();
        }
      } else if (tree.node(node.children.front()).kind == NodeKind::ForClause) {
        statement.for_kind = HirForKind::Clause;
        const SyntaxNode &clause = tree.node(node.children.front());
        const ScopeId loop_scope = semantic_.symbols.add_scope(
            ScopeKind::Block, scope, clause.range);
        std::vector<std::uint32_t> separators;
        std::optional<SemanticBranchRefinement> loop_condition;
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
            const HirExpressionId condition = check_expression(
                tree,
                header_child,
                loop_scope,
                semantic_.types.builtins().bool_type);
            statement.expressions.push_back(condition);
            SemanticBranchRefinement refinement;
            refinement.kind =
                SemanticBranchRefinementKind::LoopConditionTrue;
            refinement.subject = {tree.file(), header_child};
            refinement.subject_type = hir_.expression(condition).type;
            loop_condition = std::move(refinement);
          }
        }
        if (node.children.size() >= 2) {
          if (loop_condition.has_value()) {
            active_branch_refinements_.push_back(*loop_condition);
          }
          statement.blocks.push_back(check_block(
              tree,
              node.children.back(),
              loop_scope,
              result_type,
              {depth.breakable + 1, depth.loops + 1}));
          if (loop_condition.has_value()) {
            active_branch_refinements_.pop_back();
          }
        }
      } else {
        // Infinite loops contain only a block; conditional loops contain a bool
        // expression followed by the block.
        std::optional<SemanticBranchRefinement> loop_condition;
        for (NodeId child : node.children) {
          if (tree.node(child).kind == NodeKind::Block) {
            if (loop_condition.has_value()) {
              active_branch_refinements_.push_back(*loop_condition);
            }
            statement.blocks.push_back(
                check_block(
                    tree,
                    child,
                    scope,
                    result_type,
                    {depth.breakable + 1, depth.loops + 1}));
            if (loop_condition.has_value()) {
              active_branch_refinements_.pop_back();
            }
          } else if (tree.node(child).kind == NodeKind::ExpressionStatement &&
                     tree.node(child).children.size() == 1) {
            statement.for_kind = HirForKind::Conditional;
            const NodeId condition_syntax =
                tree.node(child).children.front();
            const HirExpressionId condition = check_expression(
                tree,
                condition_syntax,
                scope,
                semantic_.types.builtins().bool_type);
            statement.expressions.push_back(condition);
            SemanticBranchRefinement refinement;
            refinement.kind =
                SemanticBranchRefinementKind::LoopConditionTrue;
            refinement.subject = {tree.file(), condition_syntax};
            refinement.subject_type = hir_.expression(condition).type;
            loop_condition = std::move(refinement);
          } else {
            statement.for_kind = HirForKind::Conditional;
            const HirExpressionId condition = check_expression(
                tree,
                child,
                scope,
                semantic_.types.builtins().bool_type);
            statement.expressions.push_back(condition);
            SemanticBranchRefinement refinement;
            refinement.kind =
                SemanticBranchRefinementKind::LoopConditionTrue;
            refinement.subject = {tree.file(), child};
            refinement.subject_type = hir_.expression(condition).type;
            loop_condition = std::move(refinement);
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
           {},
           {},
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
      add_body_agent_site(
          SemanticSiteKind::Judgment,
          {tree.file(), statement_id},
          scope);
      break;

    case NodeKind::SynthesisStatement:
      statement.kind = HirStatementKind::Synthesis;
      add_body_agent_site(
          SemanticSiteKind::SynthesisStatement,
          {tree.file(), statement_id},
          scope);
      break;

    case NodeKind::AsmStatement:
      statement.kind = HirStatementKind::Assembly;
      for (NodeId child : node.children) {
        if (tree.node(child).kind == NodeKind::AsmInput &&
            !tree.node(child).children.empty()) {
          statement.expressions.push_back(check_expression(
              tree, tree.node(child).children.front(), scope));
        } else if (tree.node(child).kind == NodeKind::SynthesisAssembly) {
          add_body_agent_site(
              SemanticSiteKind::SynthesisAssembly,
              {tree.file(), child},
              scope);
        }
      }
      break;

    case NodeKind::WhenStatement:
      statement.kind = HirStatementKind::CompileTimeSelection;
      if (node.children.empty()) break;
      {
        const NodeId condition_id = node.children.front();
        const bool dependent =
            expression_references_parametric_parameter(
                tree, condition_id, scope) ||
            // Concrete parameter symbols already carry substituted types, so
            // the source expression may no longer look symbolic. Every
            // concrete instance nevertheless selects its body conditions with
            // the active substitution overlay at this exact program point.
            current_instance_index_.has_value();

        if ((!dependent || current_instance_index_.has_value()) &&
            initializer_requires_type_preflight(tree, condition_id)) {
          // A compile-time selection proves only the condition's value. The
          // constant evaluator may obtain a type_of result from an operand's
          // declared result shape without evaluating that operand, so the
          // selection is not evidence that calls, slices, members, or nested
          // intrinsics inside it are well typed. Reuse the ordinary checker at
          // the exact lexical program point and discard its condition HIR;
          // compile-time conditions never reach executable lowering.
          const std::size_t initial_condition_errors =
              diagnostics_.error_count();
          const bool prior_validation_only = type_validation_only_;
          type_validation_only_ = true;
          const HirExpressionId checked_condition = check_expression(
              tree,
              condition_id,
              scope,
              semantic_.types.builtins().bool_type);
          type_validation_only_ = prior_validation_only;
          const bool invalid_condition =
              is_invalid_type(hir_.expression(checked_condition).type);
          if (invalid_condition &&
              diagnostics_.error_count() == initial_condition_errors) {
            // The selected statement branch must never disappear silently.
            // If the validation bridge cannot construct a typed condition and
            // no child owned a more precise error, diagnose the condition here.
            diagnostics_.error(
                tree.node(condition_id).range,
                "compile-time condition has no valid static type");
          }
          if (diagnostics_.error_count() != initial_condition_errors ||
              invalid_condition) {
            break;
          }
        }

        if (dependent && current_instance_index_.has_value()) {
          // A concrete procedure instance has an exact overlay for every type
          // and value parameter. Evaluate once, then check only the selected
          // transparent branch. Thus no symbolic-only operation and no
          // rejected static assertion from the other branch reaches HIR/MIR.
          const ConstantTable visible_constants = active_constant_table();
          const std::vector<ConstantTypeBinding> visible_types =
              active_constant_types();
          const std::vector<ConstantStaticPackBinding> visible_packs =
              active_constant_packs();
          const std::optional<EvaluatedConstant> evaluated =
              evaluate_typed_constant_expression(
                  sources_,
                  loaded_,
                  semantic_,
                  target_,
                  tree,
                  condition_id,
                  scope,
                  diagnostics_,
                  &visible_constants,
                  &visible_types,
                  semantic_.types.builtins().bool_type,
                  &visible_packs);
          if (!evaluated.has_value() ||
              evaluated->value.kind != ConstantKind::Bool) {
            if (evaluated.has_value()) {
              diagnostics_.error(
                  tree.node(condition_id).range,
                  "compile-time 'when' condition must be a bool");
            }
            break;
          }
          const bool select_true = evaluated->value.boolean;
          if (const std::optional<HirBlockId> selected =
                  check_selected_when_branch(
                      tree,
                      node,
                      select_true,
                      scope,
                      result_type,
                      depth)) {
            statement.blocks.push_back(*selected);
          }
          break;
        }

        if (dependent) {
          // The source template is permanent semantic evidence but is never
          // lowered. Check the condition and both possible branches once. The
          // narrow predicate recognizer grants exactly the capabilities proved
          // by type_of/type_kind; arbitrary dependent bools grant none.
          statement.expressions.push_back(check_expression(
              tree,
              condition_id,
              scope,
              semantic_.types.builtins().bool_type));
          const std::optional<TypeRefinementPredicate> predicate =
              type_refinement_predicate(tree, condition_id, scope);
          std::optional<std::pair<ActiveTypeRefinement, ActiveTypeRefinement>>
              type_facts;
          if (predicate.has_value()) {
            type_facts = branch_type_refinements(*predicate);
          }

          if (node.children.size() >= 2) {
            statement.blocks.push_back(check_symbolic_when_branch(
                tree,
                node.children[1],
                condition_id,
                scope,
                result_type,
                depth,
                SemanticBranchRefinementKind::ConditionTrue,
                type_facts.has_value() ? &type_facts->first : nullptr));
          }
          if (node.children.size() >= 3) {
            statement.blocks.push_back(check_symbolic_when_branch(
                tree,
                node.children[2],
                condition_id,
                scope,
                result_type,
                depth,
                SemanticBranchRefinementKind::ConditionFalse,
                type_facts.has_value() ? &type_facts->second : nullptr));
          }
          break;
        }

        // Non-parametric body conditions are also selected here rather than in
        // the package graph. At this point every preceding lexical declaration
        // is visible, so ordinary shadowing and source-order rules are exact.
        const ConstantTable visible_constants = active_constant_table();
        const std::vector<ConstantTypeBinding> visible_types =
            active_constant_types();
        const std::vector<ConstantStaticPackBinding> visible_packs =
            active_constant_packs();
        const std::optional<EvaluatedConstant> evaluated =
            evaluate_typed_constant_expression(
                sources_,
                loaded_,
                semantic_,
                target_,
                tree,
                condition_id,
                scope,
                diagnostics_,
                &visible_constants,
                &visible_types,
                semantic_.types.builtins().bool_type,
                &visible_packs);
        if (evaluated.has_value() &&
            evaluated->value.kind == ConstantKind::Bool) {
          if (const std::optional<HirBlockId> selected =
                  check_selected_when_branch(
                      tree,
                      node,
                      evaluated->value.boolean,
                      scope,
                      result_type,
                      depth)) {
            statement.blocks.push_back(*selected);
          }
        } else if (evaluated.has_value()) {
          diagnostics_.error(
              tree.node(condition_id).range,
              "compile-time 'when' condition must be a bool");
        }
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
            subject_kind != TypeKind::Variant &&
            !switch_subject_type(subject_type)) {
          diagnostics_.error(
              tree.node(node.children.front()).range,
              "switch subject type does not have built-in scalar equality");
        }
        bool has_default = false;
        std::vector<SymbolId> covered_alternatives;
        std::vector<ConstantValue> covered_values;
        std::vector<SyntaxReference> all_switch_labels;
        // Default means that no explicit label in the complete switch matched,
        // including labels written after the default clause. Collect their
        // syntax before checking any case body so every default-site snapshot
        // contains the same complete exclusion set.
        for (std::size_t case_index = 1;
             case_index < node.children.size(); ++case_index) {
          const SyntaxNode &case_node = tree.node(node.children[case_index]);
          if (case_node.kind != NodeKind::SwitchCase ||
              case_node.children.empty()) {
            continue;
          }
          for (std::size_t label_index = 0;
               label_index + 1 < case_node.children.size(); ++label_index) {
            all_switch_labels.push_back(
                {tree.file(), case_node.children[label_index]});
          }
        }
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
                subject_kind == TypeKind::Variant
                ? check_variant_case_label(
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
                 subject_kind == TypeKind::Variant) &&
                label_expression.symbol.is_valid()) {
              const SymbolKind kind = semantic_.symbols.symbol(
                  label_expression.symbol).kind;
              if (kind == SymbolKind::EnumMember ||
                  kind == SymbolKind::VariantAlternative) {
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
          SemanticBranchRefinement refinement;
          refinement.kind = hir_case.is_default
              ? SemanticBranchRefinementKind::SwitchDefault
              : SemanticBranchRefinementKind::SwitchCase;
          refinement.subject = {tree.file(), node.children.front()};
          refinement.subject_type = subject_type;
          if (hir_case.is_default) {
            refinement.values = all_switch_labels;
          } else {
            for (std::size_t label_index = 0;
                 label_index + 1 < case_node.children.size(); ++label_index) {
              refinement.values.push_back(
                  {tree.file(), case_node.children[label_index]});
            }
          }
          active_branch_refinements_.push_back(std::move(refinement));
          check_statement_list(
              tree,
              tree.node(list_id),
              case_scope,
              result_type,
              {depth.breakable + 1, depth.loops},
              case_block);
          active_branch_refinements_.pop_back();
          hir_case.body = hir_.add_block(std::move(case_block));
          statement.blocks.push_back(hir_case.body);
          statement.switch_cases.push_back(hir_case);
        }

        statement.switch_is_exhaustive = has_default;
        if (!has_default &&
            (subject_kind == TypeKind::Enum || subject_kind == TypeKind::Variant)) {
          const std::optional<SymbolId> owner = type_owner(subject_type);
          if (owner.has_value()) {
            std::size_t alternative_count = 0;
            for (const AggregateMember &member :
                 semantic_.aggregate_members_for_read()) {
              if (member.owner == *owner) ++alternative_count;
            }
            statement.switch_is_exhaustive =
                covered_alternatives.size() == alternative_count;
            if (!statement.switch_is_exhaustive) {
              diagnostics_.error(
                  node.range,
                  "switch over enum or variant is not exhaustive and has no default");
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
  // established while resolving enum/variant case labels.
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
        statement.kind == HirStatementKind::Unchecked) {
      return statement.blocks.size() == 1 &&
          block_definitely_returns(statement.blocks.front());
    }
    if (statement.kind == HirStatementKind::CompileTimeSelection) {
      // Executable HIR contains exactly one selected block. A non-lowered
      // symbolic template may contain both possibilities; it returns only if
      // both do. A dependent `when` without else retains one symbolic block
      // and is conservatively not treated as a selected executable branch.
      if (statement.blocks.size() == 2) {
        return block_definitely_returns(statement.blocks[0]) &&
            block_definitely_returns(statement.blocks[1]);
      }
      return !current_procedure_is_template_ &&
          statement.blocks.size() == 1 &&
          block_definitely_returns(statement.blocks.front());
    }
    // Loops remain conservative: even a syntactically infinite loop may exit
    // through a break in a nested control-flow path.
    return false;
  }

  // Validates the final boundary between compile-time values and executable
  // HIR. Exact type values are useful throughout name resolution, constant
  // evaluation, and symbolic body checking, so rejecting them in the generic
  // expression checker would also reject `static_assert`, `when`, and
  // compile-time procedure bodies. At this point the enclosing procedure is
  // known to require a physical runtime ABI, making every reachable HIR value
  // an actual storage/pass/return path covered by specification section 4.
  void validate_runtime_expression(
      HirExpressionId expression_id,
      bool direct_callee,
      std::vector<bool> &validated) {
    if (!expression_id.is_valid() ||
        static_cast<std::size_t>(expression_id.value) >= validated.size()) {
      return;
    }
    const HirExpression &expression = hir_.expression(expression_id);

    // An ordinary procedure identity is represented by a runtime pointer even
    // when the procedure's own signature is compile-time-only. In direct callee
    // position it is not itself a passed `type` value; the call result and each
    // argument below still expose every illegal runtime type path. The same
    // procedure used as data is rejected because direct_callee is then false.
    const bool procedure_callee = direct_callee && expression.type.is_valid() &&
        semantic_.types.type(expression.type).kind == TypeKind::Procedure;
    if (!procedure_callee) {
      if (validated[expression_id.value]) return;
      validated[expression_id.value] = true;
      if (expression.type.is_valid() &&
          semantic_.types.contains_compile_time_type(expression.type)) {
        diagnostics_.error(
            expression.range,
            "value containing compile-time 'type' cannot reach runtime");
      }
    }

    for (std::size_t index = 0; index < expression.operands.size(); ++index) {
      validate_runtime_expression(
          expression.operands[index],
          expression.kind == HirExpressionKind::Call && index == 0,
          validated);
    }
  }

  void validate_runtime_statement(
      HirStatementId statement_id, std::vector<bool> &validated) {
    const HirStatement &statement = hir_.statement(statement_id);
    bool invalid_local_storage = false;
    if (statement.kind == HirStatementKind::LocalDeclaration) {
      for (SymbolId binding : statement.bindings) {
        const Symbol &symbol = semantic_.symbols.symbol(binding);
        if (!symbol.type.is_valid() ||
            !semantic_.types.contains_compile_time_type(symbol.type)) {
          continue;
        }
        diagnostics_.error(
            symbol.name_range,
            "runtime storage cannot contain compile-time 'type' values");
        invalid_local_storage = true;
      }
    }

    // The invalid binding already diagnoses the same initializer value. Avoid
    // emitting a second error at its expression while still visiting all other
    // statement forms, including discard assignment and call arguments where
    // no local Symbol exists to carry the forbidden type.
    if (!invalid_local_storage) {
      for (HirExpressionId expression : statement.expressions) {
        validate_runtime_expression(expression, false, validated);
      }
    }
    for (HirStatementId header : statement.header_statements) {
      validate_runtime_statement(header, validated);
    }
    for (HirBlockId block : statement.blocks) {
      validate_runtime_block(block, validated);
    }
  }

  void validate_runtime_block(
      HirBlockId block_id, std::vector<bool> &validated) {
    for (HirStatementId statement : hir_.block(block_id).statements) {
      validate_runtime_statement(statement, validated);
    }
  }

  // Concrete parametric instances retain their source declaration for denial
  // contracts and source identity. Nested instances use the same table, so one
  // shallow lookup reaches the lexical declaration that owns the syntax.
  [[nodiscard]] SymbolId procedure_declaration_source(
      SymbolId procedure) const {
    for (const ParametricInstanceRecord &instance :
         semantic_.parametric_instances_for_read()) {
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
    const std::vector<SemanticBranchRefinement> saved_refinements =
        active_branch_refinements_;
    const std::vector<ActiveTypeRefinement> saved_type_refinements =
        active_type_refinements_;
    const std::size_t saved_dependent_when_depth =
        active_dependent_when_depth_;
    const std::vector<StaticPackValueAlias> saved_pack_aliases =
        active_pack_value_aliases_;
    // A nested procedure declaration may be checked while its declaration is
    // inside an outer runtime branch, but invoking that static procedure later
    // does not imply the declaration-time path. Only its own body branches may
    // refine sites in the nested procedure.
    active_branch_refinements_.clear();
    active_type_refinements_.clear();
    active_dependent_when_depth_ = 0;
    active_pack_value_aliases_.clear();
    const SymbolId declaration_source = procedure_declaration_source(id);
    for (const DeclarationDenial &denial :
         semantic_.declaration_denials_for_read()) {
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
    const bool compile_time_only = !parametric_template &&
        semantic_.types.contains_compile_time_type(procedure_symbol.type);
    if (!parametric_template && !compile_time_only) {
      std::vector<bool> validated(hir_.expression_count(), false);
      validate_runtime_block(checked_body, validated);
    }
    hir_.add_procedure(
        {id,
         procedure_symbol.type,
         checked_body,
         diagnostics_.error_count() == initial_errors,
         parametric_template,
         compile_time_only});
    current_procedure_ = saved_procedure;
    current_procedure_is_template_ = saved_template;
    active_statement_denials_ = saved_denials;
    active_branch_refinements_ = saved_refinements;
    active_type_refinements_ = saved_type_refinements;
    active_dependent_when_depth_ = saved_dependent_when_depth;
    active_pack_value_aliases_ = saved_pack_aliases;
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
  std::vector<SemanticBranchRefinement> active_branch_refinements_;
  // These semantic type facts exist only while the symbolic template pass is
  // checking a dependent `when` branch. Concrete instances select one branch
  // and therefore use their ordinary substituted parameter types instead.
  // Keeping the stack beside the branch-refinement stack makes entry/exit
  // explicit and prevents a branch fact from changing a public signature.
  std::vector<ActiveTypeRefinement> active_type_refinements_;
  // Counts only parameter-dependent `when` possibilities currently being
  // checked in a symbolic template. Runtime `if`/loop/switch facts use the
  // neighboring branch-refinement vector but must not defer static_assert.
  std::size_t active_dependent_when_depth_ = 0;
  std::vector<StaticPackValueAlias> active_pack_value_aliases_;
  // Nested declarations publish exact later roots. This vector belongs to one
  // run_one invocation and is moved out before the checker dies; no discovered
  // body can be lost in transient checker state.
  std::vector<ProcedureBodyRoot> discovered_body_roots_;
  std::vector<ProcedureInstance> instances_;
  std::optional<std::size_t> current_instance_index_;
  // Compile-time-expression preflight reuses the complete expression checker
  // without emitting executable HIR. The package pass enables this mode for
  // initializers and structural `when` conditions; procedure-body `when`
  // enables it temporarily at the exact lexical statement. In both cases it
  // suppresses operations whose diagnostics depend on actually evaluating a
  // branch.
  bool type_validation_only_ = false;
};

// Appends one body root if it has not already been scheduled. Package, nested,
// and concrete-instance procedures occupy one SymbolId domain, so a direct
// linear scan is an exact deterministic set operation. Body root lists are
// normally small and preserve source/discovery order; sorting would change
// diagnostic order.
void append_body_root(
    std::vector<ProcedureBodyRoot> &roots, ProcedureBodyRoot root) {
  const auto existing = std::find_if(
      roots.begin(), roots.end(), [&](const ProcedureBodyRoot &candidate) {
        return candidate.symbol == root.symbol;
      });
  if (existing == roots.end()) {
    roots.push_back(std::move(root));
  }
}

} // namespace

bool validate_package_compile_time_expression_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  // Expression checking interns types and can discover body-shaped semantic
  // rows even when no HIR escapes. Run it against an isolated copy so this
  // diagnostic preflight cannot become an undeclared first body pass.
  SemanticPackage validation_package = package;
  ConstantTable validation_constants = constants;
  const std::vector<ProcedureInstantiationSeed> no_seeds;
  BodyChecker checker(
      sources,
      loaded,
      selections,
      validation_package,
      validation_constants,
      target,
      diagnostics,
      no_seeds);
  return checker.validate_package_compile_time_expression_types();
}

PackageBodyWorkState begin_package_body_work(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &seeds) {
  PackageBodyWorkState state;
  state.package = package;
  state.constants = constants;

  // Runtime context and externally requested concrete instances are package
  // publication facts. Establish them before body products are exposed so no
  // procedure task accidentally owns an unrelated seed's symbol creation.
  BodyChecker initializer(
      sources,
      loaded,
      selections,
      state.package,
      state.constants,
      target,
      diagnostics,
      seeds);
  state.ok = initializer.initialize();

  // Only the declaration baseline identifies authored roots. Seed
  // materialization appends private concrete symbols to package scope; those
  // receive their own instance roots below and must not masquerade as authored
  // source procedures.
  const std::vector<SymbolId> package_symbols =
      package.symbols.symbols_in_scope(package.package_scope);
  for (SymbolId id : package_symbols) {
    const Symbol &symbol = package.symbols.symbol(id);
    if (symbol.kind == SymbolKind::Procedure && symbol.type.is_valid()) {
      append_body_root(
          state.work,
          {id,
           symbol.flags.parametric,
           std::nullopt,
           std::nullopt,
           ProcedureBodyWorkOrigin::Authored});
    }
  }
  for (const ParametricInstanceRecord &instance :
       state.package.parametric_instances_for_read()) {
    append_body_root(
        state.work,
        {instance.instance,
         false,
         std::nullopt,
         std::nullopt,
         ProcedureBodyWorkOrigin::ExternalDemand});
  }
  return state;
}

bool append_package_body_seeds(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    PackageBodyWorkState &state,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &additional_seeds) {
  if (state.active_wave_end.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure instances cannot be appended during an active body wave");
    state.ok = false;
    return false;
  }
  const std::size_t previous_instance_count =
      state.package.parametric_instances_for_read().size();

  BodyChecker initializer(
      sources,
      loaded,
      selections,
      state.package,
      state.constants,
      target,
      diagnostics,
      additional_seeds);
  state.ok = state.ok && initializer.initialize();
  state.finalized = false;
  const AppendOnlyTableView<ParametricInstanceRecord> instances =
      state.package.parametric_instances_for_read();
  for (std::size_t index = previous_instance_count;
       index < instances.size(); ++index) {
    append_body_root(
        state.work,
        {instances[index].instance,
         false,
         std::nullopt,
         std::nullopt,
         ProcedureBodyWorkOrigin::ExternalDemand});
  }
  return state.ok;
}

std::vector<ProcedureBodyTaskInput> take_ready_procedure_body_wave(
    PackageBodyWorkState &state,
    DiagnosticSink &diagnostics) {
  std::vector<ProcedureBodyTaskInput> inputs;
  if (state.active_wave_end.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body scheduler dispatched a second wave before publication");
    return inputs;
  }
  if (state.next_work >= state.work.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body scheduler invoked without a ready work wave");
    return inputs;
  }

  const std::size_t wave_end = state.work.size();
  const SemanticTaskPrefix prefix =
      capture_semantic_task_prefix(state.package, state.constants);
  inputs.reserve(wave_end - state.next_work);
  for (std::size_t index = state.next_work; index < wave_end; ++index) {
    ProcedureBodyTaskInput input;
    input.work_index = index;
    input.valid = true;
    input.work = state.work[index];
    input.prefix = prefix;
    input.package = state.package.fork_body_task_view();
    input.constants = state.constants.fork_append_only();
    inputs.push_back(std::move(input));
  }
  state.active_wave_end = wave_end;
  return inputs;
}

ProcedureBodyTaskResult check_procedure_body_work(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const TargetFacts &target,
    ProcedureBodyTaskInput input,
    DiagnosticSink &diagnostics) {
  ProcedureBodyTaskResult result;
  if (!input.valid || !input.work.symbol.is_valid()) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body worker received an invalid task input");
    return result;
  }
  const ProcedureBodyRoot root = input.work;
  result.work_index = input.work_index;
  result.symbol = root.symbol;
  SemanticPackage package = std::move(input.package);
  ConstantTable constants = std::move(input.constants);
  const std::vector<ProcedureInstantiationSeed> no_seeds;
  BodyChecker checker(
      sources,
      loaded,
      selections,
      package,
      constants,
      target,
      diagnostics,
      no_seeds);
  ProcedureBodyRootResult checked = checker.run_one(root);

  // Both analyses are local to this exact body. Running them before extraction
  // keeps HIR IDs in their owning arena and lets loop-range facts be remapped
  // with the same task-local semantic-site suffix during publication. A bad
  // body invalidates only this result; independent roots in the frozen wave
  // still publish their own diagnostics and recoverable products.
  if (checked.ok &&
      !check_definite_initialization(
          package, checked.program, diagnostics)) {
    checked.ok = false;
  }
  infer_agent_loop_ranges(loaded, package, checked.program);

  result.ok = checked.ok;
  result.checked_procedures = checked.checked_procedures;
  result.program = std::move(checked.program);

  // Nested roots and newly discovered concrete instances consume the semantic
  // publication of the root which exposed them. The worker retains only the
  // discovered packet; the coordinator adds the exact work-index prerequisite
  // when it adopts this result.
  for (ProcedureBodyRoot &nested : checked.discovered_roots) {
    append_body_root(result.discovered_work, std::move(nested));
  }
  for (const ParametricInstanceRecord &instance :
       package.parametric_instances) {
    ProcedureBodyRoot instance_root;
    instance_root.symbol = instance.instance;
    append_body_root(result.discovered_work, std::move(instance_root));
  }
  result.semantic = extract_semantic_task_append(
      input.prefix, package, constants);
  return result;
}

bool publish_procedure_body_wave(
    PackageBodyWorkState &state,
    std::vector<ProcedureBodyTaskResult> results,
    DiagnosticSink &diagnostics) {
  if (!state.active_wave_end.has_value() ||
      *state.active_wave_end < state.next_work ||
      results.size() != *state.active_wave_end - state.next_work) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body result vector does not match the active wave");
    return false;
  }

  const SemanticTaskPrefix frozen_prefix =
      results.empty() ? SemanticTaskPrefix{}
                      : results.front().semantic.prefix;
  for (std::size_t offset = 0; offset < results.size(); ++offset) {
    const std::size_t work_index = state.next_work + offset;
    const ProcedureBodyTaskResult &result = results[offset];
    if (result.work_index != work_index ||
        result.symbol != state.work[work_index].symbol ||
        result.semantic.prefix != frozen_prefix) {
      diagnostics.error(
          SourceRange::invalid(),
          "procedure body task result does not match its frozen wave root");
      return false;
    }
  }

  for (ProcedureBodyTaskResult &result : results) {
    const std::size_t work_index = result.work_index;
    state.ok = state.ok && result.ok;
    state.checked_procedures += result.checked_procedures;
    if (!publish_body_task_semantics(
            state.package, state.constants, result, diagnostics)) {
      state.ok = false;
      return false;
    }
    ProcedureBodyHirResult procedure;
    procedure.ok = result.ok;
    procedure.symbol = result.symbol;
    procedure.program = std::move(result.program);
    procedure.imported_procedure_instances =
        std::move(result.imported_procedure_instances);
    procedure.semantic_site_indices =
        std::move(result.semantic_site_indices);
    procedure.published_types = std::move(result.published_types);
    state.procedures.push_back(std::move(procedure));
    for (ProcedureBodyRoot &discovered : result.discovered_work) {
      discovered.prerequisite = work_index;
      append_body_root(state.work, std::move(discovered));
    }
  }
  state.next_work = *state.active_wave_end;
  state.active_wave_end.reset();
  return true;
}

bool finalize_package_body_work(
    const TargetFacts &target,
    PackageBodyWorkState &state,
    DiagnosticSink &diagnostics) {
  if (state.active_wave_end.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body work finalized while a task still owned its prefix");
    state.ok = false;
  } else if (state.next_work != state.work.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "procedure body work finalized before every discovered root ran");
    state.ok = false;
  }
  // Natural target constraints apply to the canonical type table after every
  // body suffix has published. Procedure-local definite initialization and
  // agent-flow facts were already completed inside their isolated tasks, so
  // finalization neither reconstructs HIR nor re-enters a body product.
  if (state.ok &&
      !validate_target_types(state.package.types, target, diagnostics)) {
    state.ok = false;
  }
  state.finalized = state.ok;
  return state.ok;
}

PackageBodyWorkState check_package_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics,
    const std::vector<ProcedureInstantiationSeed> &seeds) {
  PackageBodyWorkState state = begin_package_body_work(
      sources,
      loaded,
      selections,
      package,
      constants,
      target,
      diagnostics,
      seeds);
  while (state.next_work < state.work.size()) {
    std::vector<ProcedureBodyTaskInput> inputs =
        take_ready_procedure_body_wave(state, diagnostics);
    std::vector<ProcedureBodyTaskResult> results;
    results.reserve(inputs.size());
    for (ProcedureBodyTaskInput &input : inputs) {
      results.push_back(check_procedure_body_work(
          sources,
          loaded,
          selections,
          target,
          std::move(input),
          diagnostics));
    }
    if (!publish_procedure_body_wave(
            state, std::move(results), diagnostics)) {
      break;
    }
  }
  (void)finalize_package_body_work(target, state, diagnostics);
  return state;
}

PackageBodyWorkState check_compile_time_procedure_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const TargetFacts &target,
    std::span<const SymbolId> procedures,
    DiagnosticSink &diagnostics) {
  PackageBodyWorkState state = begin_package_body_work(
      sources,
      loaded,
      selections,
      package,
      constants,
      target,
      diagnostics,
      {});
  state.work.clear();
  state.next_work = 0;
  const std::vector<SymbolId> package_symbols =
      state.package.symbols.symbols_in_scope(state.package.package_scope);
  for (SymbolId id : package_symbols) {
    if (std::find(procedures.begin(), procedures.end(), id) ==
        procedures.end()) {
      continue;
    }
    const Symbol &symbol = state.package.symbols.symbol(id);
    if (symbol.kind == SymbolKind::Procedure && symbol.type.is_valid()) {
      append_body_root(
          state.work,
          {id,
           symbol.flags.parametric,
           std::nullopt,
           std::nullopt,
           ProcedureBodyWorkOrigin::Authored});
    }
  }
  while (state.next_work < state.work.size()) {
    std::vector<ProcedureBodyTaskInput> inputs =
        take_ready_procedure_body_wave(state, diagnostics);
    std::vector<ProcedureBodyTaskResult> results;
    results.reserve(inputs.size());
    for (ProcedureBodyTaskInput &input : inputs) {
      results.push_back(check_procedure_body_work(
          sources,
          loaded,
          selections,
          target,
          std::move(input),
          diagnostics));
    }
    if (!publish_procedure_body_wave(
            state, std::move(results), diagnostics)) {
      break;
    }
  }
  // Procedure tasks already applied definite-initialization validation to the
  // disposable early HIR. Finalization applies the remaining target-wide type
  // constraints before this packet can become provider context.
  (void)finalize_package_body_work(target, state, diagnostics);
  return state;
}

} // namespace draft
