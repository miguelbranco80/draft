// Conservative loop-range data flow for agent context.

#include "sema/agent_flow.h"

#include "syntax/token.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace draft {
namespace {

using RangeState = std::vector<SemanticLoopRange>;

[[nodiscard]] bool range_contains(
    SourceRange outer, SourceRange inner) {
  return outer.is_valid() && inner.is_valid() &&
      outer.begin.file == inner.begin.file &&
      outer.begin.offset <= inner.begin.offset &&
      inner.end.offset <= outer.end.offset;
}

[[nodiscard]] RangeState intersect_states(
    const RangeState &left, const RangeState &right) {
  RangeState result;
  for (const SemanticLoopRange &fact : left) {
    if (std::find(right.begin(), right.end(), fact) != right.end()) {
      result.push_back(fact);
    }
  }
  return result;
}

void remove_binding(RangeState &state, SymbolId binding) {
  std::erase_if(
      state,
      [binding](const SemanticLoopRange &fact) {
        return fact.binding == binding;
      });
}

void add_fact(RangeState &state, const SemanticLoopRange &fact) {
  remove_binding(state, fact.binding);
  state.push_back(fact);
}

// Assignment to `record.field` or `array[index]` writes the root storage. A
// dereference is traceable only when its pointer is visibly derived from an
// address expression; other pointer aliases cannot name a fresh loop binding
// unless that binding escaped earlier, which this pass handles separately.
[[nodiscard]] std::optional<SymbolId> root_storage_symbol(
    const HirProgram &hir, HirExpressionId id) {
  if (!id.is_valid()) return std::nullopt;
  const HirExpression &expression = hir.expression(id);
  if (expression.kind == HirExpressionKind::Symbol &&
      expression.symbol.is_valid()) {
    return expression.symbol;
  }
  if ((expression.kind == HirExpressionKind::Member ||
       expression.kind == HirExpressionKind::Index) &&
      !expression.operands.empty()) {
    return root_storage_symbol(hir, expression.operands.front());
  }
  if (expression.kind == HirExpressionKind::Dereference &&
      !expression.operands.empty()) {
    const HirExpression &pointer = hir.expression(expression.operands.front());
    if (pointer.kind == HirExpressionKind::Address &&
        !pointer.operands.empty()) {
      return root_storage_symbol(hir, pointer.operands.front());
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool expression_references_symbol(
    const HirProgram &hir, HirExpressionId id, SymbolId symbol) {
  if (!id.is_valid()) return false;
  const HirExpression &expression = hir.expression(id);
  if (expression.kind == HirExpressionKind::Symbol &&
      expression.symbol == symbol) {
    return true;
  }
  for (HirExpressionId operand : expression.operands) {
    if (expression_references_symbol(hir, operand, symbol)) return true;
  }
  return false;
}

void collect_expression_symbols(
    const HirProgram &hir,
    HirExpressionId id,
    std::vector<SymbolId> &symbols) {
  if (!id.is_valid()) return;
  const HirExpression &expression = hir.expression(id);
  if (expression.kind == HirExpressionKind::Symbol &&
      expression.symbol.is_valid() &&
      std::find(symbols.begin(), symbols.end(), expression.symbol) ==
          symbols.end()) {
    symbols.push_back(expression.symbol);
  }
  for (HirExpressionId operand : expression.operands) {
    collect_expression_symbols(hir, operand, symbols);
  }
}

[[nodiscard]] bool expression_takes_address_of(
    const HirProgram &hir, HirExpressionId id, SymbolId symbol) {
  if (!id.is_valid()) return false;
  const HirExpression &expression = hir.expression(id);
  if (expression.kind == HirExpressionKind::Address &&
      !expression.operands.empty() &&
      root_storage_symbol(hir, expression.operands.front()) == symbol) {
    return true;
  }
  for (HirExpressionId operand : expression.operands) {
    if (expression_takes_address_of(hir, operand, symbol)) return true;
  }
  return false;
}

class LoopRangeAnalysis {
public:
  LoopRangeAnalysis(
      const LoadedPackage &loaded,
      SemanticPackage &package,
      const HirProgram &hir)
      : loaded_(loaded), package_(package), hir_(hir) {}

  void run() {
    for (SemanticSite &site : package_.sites) site.loop_ranges.clear();
    for (const HirProcedure &procedure : hir_.procedures()) {
      current_procedure_ = procedure.symbol;
      (void)walk_block(procedure.body, {}, true);
    }
  }

private:
  [[nodiscard]] const SyntaxTree *find_tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) {
        return &*entry.syntax;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const SyntaxNode *syntax_node(
      SyntaxReference syntax) const {
    const SyntaxTree *tree = find_tree(syntax.file);
    if (tree == nullptr || !syntax.node.is_valid()) return nullptr;
    return &tree->node(syntax.node);
  }

  void annotate_exact(SyntaxReference syntax, const RangeState &state) {
    if (!syntax.file.is_valid() || !syntax.node.is_valid()) return;
    for (SemanticSite &site : package_.sites) {
      if (site.anchor == current_procedure_ && site.syntax == syntax) {
        site.loop_ranges = state;
      }
    }
  }

  // Assembly synthesis is a child of one checked assembly node rather than a
  // standalone HIR operand. Inputs are analyzed first; then every contained
  // assembly site receives the remaining state. Treating all inputs as earlier
  // is conservative if authored source interleaves inputs and synthesis.
  void annotate_assembly_sites(
      SourceRange assembly, const RangeState &state) {
    for (SemanticSite &site : package_.sites) {
      if (site.anchor != current_procedure_ ||
          site.kind != SemanticSiteKind::SynthesisAssembly) {
        continue;
      }
      const SyntaxNode *node = syntax_node(site.syntax);
      if (node != nullptr && range_contains(assembly, node->range)) {
        site.loop_ranges = state;
      }
    }
  }

  [[nodiscard]] RangeState walk_expression(
      HirExpressionId id, RangeState state, bool annotate) {
    if (!id.is_valid()) return state;
    const HirExpression &expression = hir_.expression(id);

    if (expression.kind == HirExpressionKind::Synthesis && annotate) {
      annotate_exact(expression.syntax, state);
      return state;
    }

    if (expression.kind == HirExpressionKind::Conditional &&
        expression.operands.size() == 3) {
      state = walk_expression(expression.operands[0], std::move(state), annotate);
      RangeState selected = walk_expression(
          expression.operands[1], state, annotate);
      RangeState alternative = walk_expression(
          expression.operands[2], state, annotate);
      return intersect_states(selected, alternative);
    }

    if (expression.kind == HirExpressionKind::Binary &&
        (expression.operation == HirOperation::LogicalAnd ||
         expression.operation == HirOperation::LogicalOr) &&
        expression.operands.size() == 2) {
      state = walk_expression(expression.operands[0], std::move(state), annotate);
      const RangeState right = walk_expression(
          expression.operands[1], state, annotate);
      // The right operand may be skipped. A fact remains current after the
      // expression only when it survives both execution paths.
      return intersect_states(state, right);
    }

    for (HirExpressionId operand : expression.operands) {
      state = walk_expression(operand, std::move(state), annotate);
    }

    if (expression.kind == HirExpressionKind::Address &&
        !expression.operands.empty()) {
      const std::optional<SymbolId> root =
          root_storage_symbol(hir_, expression.operands.front());
      if (root.has_value()) remove_binding(state, *root);
    }
    if (expression.kind == HirExpressionKind::Assembly && annotate) {
      annotate_assembly_sites(expression.range, state);
    }
    return state;
  }

  [[nodiscard]] RangeState walk_block(
      HirBlockId id, RangeState state, bool annotate) {
    if (!id.is_valid()) return state;
    for (HirStatementId statement : hir_.block(id).statements) {
      state = walk_statement(statement, std::move(state), annotate);
    }
    return state;
  }

  [[nodiscard]] RangeState walk_if(
      const HirStatement &statement,
      RangeState state,
      bool annotate) {
    for (HirExpressionId expression : statement.expressions) {
      state = walk_expression(expression, std::move(state), annotate);
    }
    if (statement.blocks.empty()) return state;

    RangeState joined = walk_block(statement.blocks.front(), state, annotate);
    if (statement.blocks.size() >= 2) {
      const RangeState alternative =
          walk_block(statement.blocks[1], state, annotate);
      joined = intersect_states(joined, alternative);
    } else {
      // With no else branch the false path preserves the condition state.
      joined = intersect_states(joined, state);
    }
    return joined;
  }

  [[nodiscard]] RangeState walk_switch(
      const HirStatement &statement,
      RangeState state,
      bool annotate) {
    for (HirExpressionId expression : statement.expressions) {
      state = walk_expression(expression, std::move(state), annotate);
    }
    if (statement.switch_cases.empty()) return state;

    std::optional<RangeState> joined;
    for (const HirSwitchCase &switch_case : statement.switch_cases) {
      const RangeState branch = walk_block(switch_case.body, state, annotate);
      joined = joined.has_value()
          ? std::optional<RangeState>(intersect_states(*joined, branch))
          : std::optional<RangeState>(branch);
    }
    if (!statement.switch_is_exhaustive) {
      joined = intersect_states(*joined, state);
    }
    return *joined;
  }

  [[nodiscard]] bool statement_mutates_or_escapes(
      HirStatementId id, SymbolId binding) const {
    if (!id.is_valid()) return false;
    const HirStatement &statement = hir_.statement(id);
    if (statement.kind == HirStatementKind::Assignment) {
      const std::size_t count = std::min(
          statement.assignment_target_count, statement.expressions.size());
      for (std::size_t index = 0; index < count; ++index) {
        if (root_storage_symbol(hir_, statement.expressions[index]) == binding) {
          return true;
        }
      }
    }
    for (HirExpressionId expression : statement.expressions) {
      if (expression_takes_address_of(hir_, expression, binding)) return true;
    }
    for (HirStatementId header : statement.header_statements) {
      if (statement_mutates_or_escapes(header, binding)) return true;
    }
    for (HirBlockId block : statement.blocks) {
      if (block_mutates_or_escapes(block, binding)) return true;
    }
    return false;
  }

  [[nodiscard]] bool block_mutates_or_escapes(
      HirBlockId id, SymbolId binding) const {
    if (!id.is_valid()) return false;
    for (HirStatementId statement : hir_.block(id).statements) {
      if (statement_mutates_or_escapes(statement, binding)) return true;
    }
    return false;
  }

  [[nodiscard]] bool statement_takes_address(
      HirStatementId id, SymbolId binding) const {
    if (!id.is_valid()) return false;
    const HirStatement &statement = hir_.statement(id);
    for (HirExpressionId expression : statement.expressions) {
      if (expression_takes_address_of(hir_, expression, binding)) return true;
    }
    for (HirStatementId header : statement.header_statements) {
      if (statement_takes_address(header, binding)) return true;
    }
    for (HirBlockId block : statement.blocks) {
      if (block_takes_address(block, binding)) return true;
    }
    return false;
  }

  [[nodiscard]] bool block_takes_address(
      HirBlockId id, SymbolId binding) const {
    if (!id.is_valid()) return false;
    for (HirStatementId statement : hir_.block(id).statements) {
      if (statement_takes_address(statement, binding)) return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<NodeId> node_with_range(
      const SyntaxTree &tree, NodeId root, SourceRange range) const {
    if (!root.is_valid()) return std::nullopt;
    const SyntaxNode &node = tree.node(root);
    if (node.range.begin == range.begin && node.range.end == range.end) {
      return root;
    }
    for (NodeId child : node.children) {
      if (!range_contains(tree.node(child).range, range)) continue;
      const std::optional<NodeId> found = node_with_range(tree, child, range);
      if (found.has_value()) return found;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<SemanticLoopRange> iteration_fact(
      const HirStatement &statement) const {
    if (statement.blocks.empty() || statement.expressions.empty() ||
        !statement.syntax.file.is_valid() ||
        !statement.syntax.node.is_valid()) {
      return std::nullopt;
    }
    const SyntaxTree *tree = find_tree(statement.syntax.file);
    if (tree == nullptr) return std::nullopt;
    const SyntaxNode &loop = tree->node(statement.syntax.node);
    if (loop.children.empty()) return std::nullopt;
    const std::optional<IterationHeaderParts> header_parts =
        iteration_header_parts(*tree, loop.children.front());
    if (!header_parts.has_value()) return std::nullopt;
    // Body checking appends the symbol and its source role together. A
    // mismatch is an invalid HIR packet, not recoverable Draft source.
    assert(statement.bindings.size() ==
           statement.iteration_binding_sources.size());
    SymbolId index_binding;
    for (std::size_t index = 0; index < statement.bindings.size(); ++index) {
      if (statement.iteration_binding_sources[index].kind ==
          HirIterationBindingKind::Index) {
        index_binding = statement.bindings[index];
        break;
      }
    }
    if (!index_binding.is_valid()) return std::nullopt;
    // An escaped address can be retained across a backedge and used to mutate
    // the next iteration's source index after its compiler reset. Disable the
    // fact for the whole static body in that case. Plain assignments need no
    // such ban: flow removes the fact after the store and the next entry resets
    // it in the ordinary way.
    if (block_takes_address(statement.blocks.front(), index_binding)) {
      return std::nullopt;
    }

    SemanticLoopRange fact;
    fact.kind = SemanticLoopRangeKind::IterationIndex;
    fact.binding = index_binding;
    fact.upper = {statement.syntax.file, header_parts->iterable};
    fact.upper_type = hir_.expression(statement.expressions.front()).type;
    collect_expression_symbols(
        hir_, statement.expressions.front(), fact.upper_symbols);
    return fact;
  }

  [[nodiscard]] std::optional<SemanticLoopRange> clause_fact(
      const HirStatement &statement) const {
    if (statement.blocks.empty() || statement.expressions.size() != 1 ||
        statement.for_initialization_count == 0 ||
        statement.for_initialization_count >=
            statement.header_statements.size() ||
        !statement.syntax.file.is_valid() ||
        !statement.syntax.node.is_valid()) {
      return std::nullopt;
    }

    const HirStatement &initialization = hir_.statement(
        statement.header_statements.front());
    if (initialization.kind != HirStatementKind::LocalDeclaration ||
        initialization.bindings.size() != 1 ||
        initialization.expressions.size() != 1) {
      return std::nullopt;
    }
    const SymbolId binding = initialization.bindings.front();
    const HirExpression &initial_value = hir_.expression(
        initialization.expressions.front());
    if (initial_value.kind != HirExpressionKind::Constant ||
        initial_value.constant.kind != ConstantKind::Integer ||
        initial_value.constant.integer.to_decimal() != "0" ||
        !package_.types.is_integer(package_.symbols.symbol(binding).type)) {
      return std::nullopt;
    }

    const HirExpression &condition = hir_.expression(
        statement.expressions.front());
    if (condition.kind != HirExpressionKind::Binary ||
        condition.operation != HirOperation::Less ||
        condition.operands.size() != 2) {
      return std::nullopt;
    }
    const HirExpression &left = hir_.expression(condition.operands.front());
    if (left.kind != HirExpressionKind::Symbol || left.symbol != binding ||
        expression_references_symbol(hir_, condition.operands[1], binding) ||
        expression_takes_address_of(hir_, condition.operands[1], binding)) {
      return std::nullopt;
    }

    if (statement.header_statements.size() -
            statement.for_initialization_count != 1) {
      return std::nullopt;
    }
    const HirStatement &post = hir_.statement(
        statement.header_statements[statement.for_initialization_count]);
    if (post.kind != HirStatementKind::Assignment ||
        post.operation != HirOperation::Add ||
        post.assignment_target_count != 1 ||
        post.expressions.size() != 2 ||
        root_storage_symbol(hir_, post.expressions.front()) != binding) {
      return std::nullopt;
    }
    const HirExpression &step = hir_.expression(post.expressions.back());
    if (step.kind != HirExpressionKind::Constant ||
        step.constant.kind != ConstantKind::Integer ||
        step.constant.integer.to_decimal() != "1") {
      return std::nullopt;
    }
    if (block_mutates_or_escapes(statement.blocks.front(), binding)) {
      return std::nullopt;
    }

    const SyntaxTree *tree = find_tree(statement.syntax.file);
    if (tree == nullptr) return std::nullopt;
    const SyntaxNode &loop = tree->node(statement.syntax.node);
    if (loop.children.empty()) return std::nullopt;
    const SyntaxNode &clause = tree->node(loop.children.front());
    if (clause.kind != NodeKind::ForClause) return std::nullopt;
    const SourceRange upper_range =
        hir_.expression(condition.operands[1]).range;
    const std::optional<NodeId> upper = node_with_range(
        *tree, loop.children.front(), upper_range);
    if (!upper.has_value()) return std::nullopt;

    SemanticLoopRange fact;
    fact.kind = SemanticLoopRangeKind::CanonicalInduction;
    fact.binding = binding;
    fact.upper = {statement.syntax.file, *upper};
    fact.upper_type = hir_.expression(condition.operands[1]).type;
    collect_expression_symbols(
        hir_, condition.operands[1], fact.upper_symbols);
    return fact;
  }

  [[nodiscard]] RangeState walk_iteration_loop(
      const HirStatement &statement,
      RangeState state,
      bool annotate) {
    // The iterable is evaluated and captured exactly once.
    for (HirExpressionId expression : statement.expressions) {
      state = walk_expression(expression, std::move(state), annotate);
    }
    if (statement.blocks.empty()) return state;
    const RangeState first_entry = state;
    const std::optional<SemanticLoopRange> local = iteration_fact(statement);

    RangeState fixed = first_entry;
    while (true) {
      RangeState body_entry = fixed;
      if (local.has_value()) add_fact(body_entry, *local);
      RangeState backedge = walk_block(
          statement.blocks.front(), std::move(body_entry), false);
      if (local.has_value()) remove_binding(backedge, local->binding);
      const RangeState next = intersect_states(first_entry, backedge);
      if (next == fixed) break;
      fixed = next;
    }

    RangeState body_entry = fixed;
    if (local.has_value()) add_fact(body_entry, *local);
    RangeState body_out = walk_block(
        statement.blocks.front(), std::move(body_entry), annotate);
    if (local.has_value()) remove_binding(body_out, local->binding);
    // Zero iterations preserve first_entry; one or more iterations preserve
    // only the fixed intersection already represented by `fixed`.
    return intersect_states(fixed, body_out);
  }

  [[nodiscard]] RangeState walk_repeating_condition_loop(
      const HirStatement &statement,
      RangeState state,
      bool annotate) {
    // Clause initialization executes once. Conditional loops have no header
    // statements, so the same code naturally starts at their condition.
    const std::size_t initialization_count = std::min(
        statement.for_initialization_count,
        statement.header_statements.size());
    for (std::size_t index = 0; index < initialization_count; ++index) {
      state = walk_statement(
          statement.header_statements[index], std::move(state), annotate);
    }
    const RangeState first_condition_entry = state;
    const std::optional<SemanticLoopRange> local =
        statement.for_kind == HirForKind::Clause
        ? clause_fact(statement)
        : std::nullopt;

    RangeState fixed = first_condition_entry;
    while (true) {
      RangeState body_entry = fixed;
      for (HirExpressionId condition : statement.expressions) {
        body_entry = walk_expression(condition, std::move(body_entry), false);
      }
      if (local.has_value()) add_fact(body_entry, *local);
      if (!statement.blocks.empty()) {
        body_entry = walk_block(
            statement.blocks.front(), std::move(body_entry), false);
      }
      for (std::size_t index = initialization_count;
           index < statement.header_statements.size(); ++index) {
        body_entry = walk_statement(
            statement.header_statements[index], std::move(body_entry), false);
      }
      if (local.has_value()) remove_binding(body_entry, local->binding);
      const RangeState next = intersect_states(
          first_condition_entry, body_entry);
      if (next == fixed) break;
      fixed = next;
    }

    RangeState body_entry = fixed;
    for (HirExpressionId condition : statement.expressions) {
      body_entry = walk_expression(
          condition, std::move(body_entry), annotate);
    }
    // The false condition exits with this state. A body path can only remove
    // additional facts, and those removals were already folded into `fixed`.
    const RangeState exit_state = body_entry;
    if (local.has_value()) add_fact(body_entry, *local);
    if (!statement.blocks.empty()) {
      body_entry = walk_block(
          statement.blocks.front(), std::move(body_entry), annotate);
    }
    for (std::size_t index = initialization_count;
         index < statement.header_statements.size(); ++index) {
      body_entry = walk_statement(
          statement.header_statements[index], std::move(body_entry), annotate);
    }
    return exit_state;
  }

  [[nodiscard]] RangeState walk_infinite_loop(
      const HirStatement &statement,
      RangeState state,
      bool annotate) {
    if (statement.blocks.empty()) return state;
    const RangeState first_entry = state;
    RangeState fixed = first_entry;
    while (true) {
      const RangeState backedge = walk_block(
          statement.blocks.front(), fixed, false);
      const RangeState next = intersect_states(first_entry, backedge);
      if (next == fixed) break;
      fixed = next;
    }
    (void)walk_block(statement.blocks.front(), fixed, annotate);
    return fixed;
  }

  [[nodiscard]] RangeState walk_for(
      const HirStatement &statement,
      RangeState state,
      bool annotate) {
    switch (statement.for_kind) {
    case HirForKind::Iteration:
      return walk_iteration_loop(statement, std::move(state), annotate);
    case HirForKind::Clause:
    case HirForKind::Conditional:
      return walk_repeating_condition_loop(
          statement, std::move(state), annotate);
    case HirForKind::Infinite:
      return walk_infinite_loop(statement, std::move(state), annotate);
    case HirForKind::None:
      return state;
    }
    return state;
  }

  [[nodiscard]] RangeState walk_statement(
      HirStatementId id, RangeState state, bool annotate) {
    if (!id.is_valid()) return state;
    const HirStatement &statement = hir_.statement(id);
    switch (statement.kind) {
    case HirStatementKind::Judgment:
    case HirStatementKind::Synthesis:
      if (annotate) annotate_exact(statement.syntax, state);
      return state;

    case HirStatementKind::If:
      return walk_if(statement, std::move(state), annotate);

    case HirStatementKind::Switch:
      return walk_switch(statement, std::move(state), annotate);

    case HirStatementKind::For:
      return walk_for(statement, std::move(state), annotate);

    case HirStatementKind::Assignment: {
      for (HirExpressionId expression : statement.expressions) {
        state = walk_expression(expression, std::move(state), annotate);
      }
      const std::size_t count = std::min(
          statement.assignment_target_count, statement.expressions.size());
      for (std::size_t index = 0; index < count; ++index) {
        const std::optional<SymbolId> root =
            root_storage_symbol(hir_, statement.expressions[index]);
        if (root.has_value()) remove_binding(state, *root);
      }
      return state;
    }

    case HirStatementKind::Assembly:
      for (HirExpressionId expression : statement.expressions) {
        state = walk_expression(expression, std::move(state), annotate);
      }
      if (annotate) annotate_assembly_sites(statement.range, state);
      return state;

    case HirStatementKind::Block:
    case HirStatementKind::CompileTimeSelection:
    case HirStatementKind::Denial:
    case HirStatementKind::Unchecked:
      for (HirExpressionId expression : statement.expressions) {
        state = walk_expression(expression, std::move(state), annotate);
      }
      for (HirBlockId block : statement.blocks) {
        state = walk_block(block, std::move(state), annotate);
      }
      return state;

    default:
      for (HirExpressionId expression : statement.expressions) {
        state = walk_expression(expression, std::move(state), annotate);
      }
      // No remaining statement kind has alternative runtime blocks. Walking
      // any recovery blocks sequentially is conservative and keeps malformed
      // HIR from manufacturing facts.
      for (HirBlockId block : statement.blocks) {
        state = walk_block(block, std::move(state), annotate);
      }
      return state;
    }
  }

  const LoadedPackage &loaded_;
  SemanticPackage &package_;
  const HirProgram &hir_;
  SymbolId current_procedure_;
};

} // namespace

void infer_agent_loop_ranges(
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const HirProgram &hir) {
  LoopRangeAnalysis analysis(loaded, package, hir);
  analysis.run();
}

} // namespace draft
