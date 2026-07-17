// Flow-sensitive checking for Draft's deliberately uninitialized `---` locals.
//
// This pass is intentionally conservative and local to one procedure. It does
// not attempt alias analysis. A direct assignment proves initialization, a
// direct read proves misuse, and address escape changes an uninitialized local
// to Maybe because unknown code or a later indirect store may initialize it.
// Runtime branches merge with the three-value lattice below; loop bodies are
// checked but do not make facts true after a loop that may execute zero times.

#include "sema/initialization.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace draft {
namespace {

enum class Initialization {
  Uninitialized,
  Maybe,
  Initialized,
};

enum class ExpressionUse {
  Read,
  Lvalue,
  EscapeAddress,
};

struct LocalFact {
  SymbolId symbol;
  Initialization state = Initialization::Initialized;
};

using State = std::vector<LocalFact>;

[[nodiscard]] Initialization merge_initialization(
    Initialization left, Initialization right) {
  return left == right ? left : Initialization::Maybe;
}

class InitializationChecker {
public:
  InitializationChecker(
      const SemanticPackage &package,
      const HirProgram &hir,
      DiagnosticSink &diagnostics)
      : package_(package), hir_(hir), diagnostics_(diagnostics) {}

  [[nodiscard]] bool run() {
    const std::size_t initial_errors = diagnostics_.error_count();
    for (const HirProcedure &procedure : hir_.procedures()) {
      if (!procedure.valid) continue;
      State state;
      check_block(procedure.body, state);
    }
    return diagnostics_.error_count() == initial_errors;
  }

private:
  [[nodiscard]] LocalFact *fact(State &state, SymbolId symbol) const {
    for (LocalFact &entry : state) {
      if (entry.symbol == symbol) return &entry;
    }
    return nullptr;
  }

  // Only facts that existed before a branch can escape that branch. Locals
  // declared inside its block disappear at scope exit and need no merge row.
  void merge_outer(State &outer, const State &left, const State &right) const {
    for (LocalFact &entry : outer) {
      const LocalFact *left_entry = nullptr;
      const LocalFact *right_entry = nullptr;
      for (const LocalFact &candidate : left) {
        if (candidate.symbol == entry.symbol) left_entry = &candidate;
      }
      for (const LocalFact &candidate : right) {
        if (candidate.symbol == entry.symbol) right_entry = &candidate;
      }
      if (left_entry != nullptr && right_entry != nullptr) {
        entry.state = merge_initialization(
            left_entry->state, right_entry->state);
      }
    }
  }

  void use_symbol(
      const HirExpression &expression,
      ExpressionUse use,
      State &state) {
    LocalFact *entry = fact(state, expression.symbol);
    if (entry == nullptr) return;
    if (use == ExpressionUse::Read &&
        entry->state == Initialization::Uninitialized) {
      const Symbol &symbol = package_.symbols.symbol(expression.symbol);
      diagnostics_.error(
          expression.range,
          "read of uninitialized local '" + symbol.name + "'");
    } else if (use == ExpressionUse::EscapeAddress &&
               entry->state == Initialization::Uninitialized) {
      entry->state = Initialization::Maybe;
    }
  }

  void check_expression(
      HirExpressionId id,
      ExpressionUse use,
      State &state) {
    if (!id.is_valid()) return;
    const HirExpression &expression = hir_.expression(id);
    switch (expression.kind) {
    case HirExpressionKind::Symbol:
      use_symbol(expression, use, state);
      return;
    case HirExpressionKind::Address:
      if (!expression.operands.empty()) {
        check_expression(
            expression.operands.front(), ExpressionUse::EscapeAddress, state);
      }
      return;
    case HirExpressionKind::Member:
      if (!expression.operands.empty()) {
        check_expression(
            expression.operands.front(),
            use == ExpressionUse::Read ? ExpressionUse::Read : ExpressionUse::Lvalue,
            state);
      }
      return;
    case HirExpressionKind::Index:
      if (!expression.operands.empty()) {
        check_expression(
            expression.operands.front(),
            use == ExpressionUse::Read ? ExpressionUse::Read : ExpressionUse::Lvalue,
            state);
      }
      for (std::size_t index = 1; index < expression.operands.size(); ++index) {
        check_expression(expression.operands[index], ExpressionUse::Read, state);
      }
      return;
    case HirExpressionKind::Dereference:
      // Dereferencing reads the pointer value. Whether the pointed-to bytes are
      // initialized is outside this local-storage analysis.
      if (!expression.operands.empty()) {
        check_expression(
            expression.operands.front(), ExpressionUse::Read, state);
      }
      return;
    case HirExpressionKind::Invalid:
    case HirExpressionKind::Constant:
    case HirExpressionKind::Synthesis:
      return;
    default:
      for (HirExpressionId operand : expression.operands) {
        check_expression(operand, ExpressionUse::Read, state);
      }
      return;
    }
  }

  void initialize_lvalue(HirExpressionId id, State &state) {
    if (!id.is_valid()) return;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Symbol) {
      if (LocalFact *entry = fact(state, expression.symbol)) {
        entry->state = Initialization::Initialized;
      }
    }
  }

  void check_assignment(const HirStatement &statement, State &state) {
    const std::size_t left_count = statement.expressions.size() / 2;
    for (std::size_t index = 0; index < left_count; ++index) {
      check_expression(
          statement.expressions[index],
          statement.operation == HirOperation::Assign
              ? ExpressionUse::Lvalue
              : ExpressionUse::Read,
          state);
    }
    for (std::size_t index = left_count;
         index < statement.expressions.size();
         ++index) {
      check_expression(statement.expressions[index], ExpressionUse::Read, state);
    }
    for (std::size_t index = 0; index < left_count; ++index) {
      initialize_lvalue(statement.expressions[index], state);
    }
  }

  void check_if(const HirStatement &statement, State &state) {
    if (!statement.expressions.empty()) {
      check_expression(
          statement.expressions.front(), ExpressionUse::Read, state);
    }
    const State before = state;
    State true_state = before;
    if (!statement.blocks.empty()) check_block(statement.blocks[0], true_state);
    State false_state = before;
    if (statement.blocks.size() >= 2) check_block(statement.blocks[1], false_state);
    merge_outer(state, true_state, false_state);
  }

  void check_switch(const HirStatement &statement, State &state) {
    for (HirExpressionId expression : statement.expressions) {
      check_expression(expression, ExpressionUse::Read, state);
    }
    const State before = state;
    bool first = true;
    State merged = before;
    bool has_default = false;
    for (const HirSwitchCase &switch_case : statement.switch_cases) {
      State branch = before;
      check_block(switch_case.body, branch);
      has_default = has_default || switch_case.is_default;
      if (first) {
        merged = branch;
        first = false;
      } else {
        State next = merged;
        merge_outer(next, merged, branch);
        merged = std::move(next);
      }
    }
    if (!has_default) {
      State next = before;
      merge_outer(next, merged, before);
      merged = std::move(next);
    }
    for (LocalFact &entry : state) {
      if (LocalFact *result = fact(merged, entry.symbol)) {
        entry.state = result->state;
      }
    }
  }

  void check_for(const HirStatement &statement, State &state) {
    // Clause initialization executes once. Post statements and the body are
    // checked on a copy because neither is guaranteed to execute.
    const std::size_t initialization_count =
        statement.for_kind == HirForKind::Clause
        ? statement.for_initialization_count
        : 0;
    for (std::size_t index = 0;
         index < initialization_count &&
         index < statement.header_statements.size();
         ++index) {
      check_statement(statement.header_statements[index], state);
    }
    for (HirExpressionId expression : statement.expressions) {
      check_expression(expression, ExpressionUse::Read, state);
    }
    State iteration = state;
    const std::size_t binding_begin = iteration.size();
    for (SymbolId binding : statement.bindings) {
      iteration.push_back({binding, Initialization::Initialized});
    }
    for (HirBlockId block : statement.blocks) check_block(block, iteration);
    for (std::size_t index = initialization_count;
         index < statement.header_statements.size();
         ++index) {
      check_statement(statement.header_statements[index], iteration);
    }
    iteration.resize(binding_begin);
  }

  void check_statement(HirStatementId id, State &state) {
    const HirStatement &statement = hir_.statement(id);
    switch (statement.kind) {
    case HirStatementKind::LocalDeclaration:
      for (HirExpressionId expression : statement.expressions) {
        check_expression(expression, ExpressionUse::Read, state);
      }
      for (SymbolId binding : statement.bindings) {
        state.push_back({
            binding,
            statement.local_is_uninitialized
                ? Initialization::Uninitialized
                : Initialization::Initialized});
      }
      break;
    case HirStatementKind::Assignment:
      check_assignment(statement, state);
      break;
    case HirStatementKind::If:
      check_if(statement, state);
      break;
    case HirStatementKind::Switch:
      check_switch(statement, state);
      break;
    case HirStatementKind::For:
      check_for(statement, state);
      break;
    case HirStatementKind::CompileTimeSelection:
    case HirStatementKind::Block:
    case HirStatementKind::Denial:
    case HirStatementKind::Unchecked:
      for (HirBlockId block : statement.blocks) check_block(block, state);
      break;
    case HirStatementKind::Expression:
    case HirStatementKind::Return:
    case HirStatementKind::Defer:
    case HirStatementKind::Assembly:
      for (HirExpressionId expression : statement.expressions) {
        check_expression(expression, ExpressionUse::Read, state);
      }
      break;
    case HirStatementKind::Invalid:
    case HirStatementKind::Break:
    case HirStatementKind::Continue:
    case HirStatementKind::Judgment:
    case HirStatementKind::Synthesis:
      break;
    }
  }

  void check_block(HirBlockId id, State &state) {
    if (!id.is_valid()) return;
    const std::size_t outer_count = state.size();
    for (HirStatementId statement : hir_.block(id).statements) {
      check_statement(statement, state);
      const HirStatementKind kind = hir_.statement(statement).kind;
      if (kind == HirStatementKind::Return ||
          kind == HirStatementKind::Break ||
          kind == HirStatementKind::Continue) {
        break;
      }
    }
    state.resize(outer_count);
  }

  const SemanticPackage &package_;
  const HirProgram &hir_;
  DiagnosticSink &diagnostics_;
};

} // namespace

bool check_definite_initialization(
    const SemanticPackage &package,
    const HirProgram &hir,
    DiagnosticSink &diagnostics) {
  return InitializationChecker(package, hir, diagnostics).run();
}

} // namespace draft
