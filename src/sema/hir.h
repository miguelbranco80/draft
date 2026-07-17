// Typed high-level IR for checked Draft procedure bodies.
//
// HIR is the first representation that contains only resolved semantic names and
// TypeIds. It preserves structured source control flow for diagnostics, denial
// summaries, judgments, and later CFG construction; it does not encode LLVM
// values, registers, ABI decomposition, or optimization choices. Nodes live in
// append-only vectors owned by one HirProgram and refer to one another by stable
// process-local integer IDs.
//
// The representation is intentionally broad enough to retain every Draft
// surface operation while body checking is implemented incrementally. Invalid
// nodes remain source-located after diagnostics so independent procedures can
// continue checking.

#pragma once

#include "sema/constant.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "source/source.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace draft {

struct HirExpressionId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  [[nodiscard]] bool is_valid() const;
  bool operator==(const HirExpressionId &) const = default;
};

struct HirStatementId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  [[nodiscard]] bool is_valid() const;
  bool operator==(const HirStatementId &) const = default;
};

struct HirBlockId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  [[nodiscard]] bool is_valid() const;
  bool operator==(const HirBlockId &) const = default;
};

enum class HirExpressionKind {
  Invalid,
  Constant,
  Symbol,
  Unary,
  Binary,
  Call,
  Intrinsic,
  Index,
  Slice,
  Member,
  Dereference,
  Address,
  Tuple,
  Composite,
  Conditional,
  Denial,
  Synthesis,
  Assembly,
};

// HirOperation is the executable meaning retained after source tokens have
// served their parsing purpose. Keeping this vocabulary independent from
// TokenKind prevents later passes from reaching back into a SyntaxTree merely
// to discover whether a checked Binary node meant addition or comparison.
// Assignment uses the same arithmetic operations for compound forms; Assign
// denotes the plain `=` form.
enum class HirOperation {
  None,
  Assign,
  Positive,
  Negate,
  LogicalNot,
  BitwiseNot,
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  ShiftLeft,
  ShiftRight,
  LogicalAnd,
  LogicalOr,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
};

// HirExpression operands are in source evaluation order. symbol is meaningful
// for Symbol and Member nodes; constant is meaningful for Constant. addressable
// records the language property checked by `&` and assignment, not whether MIR
// later chooses a stack slot.
struct HirExpression {
  HirExpressionKind kind = HirExpressionKind::Invalid;
  HirOperation operation = HirOperation::None;
  TypeId type;
  SourceRange range;
  SyntaxReference syntax;
  ScopeId scope;
  SymbolId symbol;
  ConstantValue constant;
  std::vector<HirExpressionId> operands;
  // Composite operands may be positional or name-directed. This parallel
  // vector contains an invalid SymbolId for a positional operand and the
  // resolved field SymbolId for a keyed operand. Other expression kinds leave
  // it empty.
  std::vector<SymbolId> operand_members;
  // Slice syntax permits either bound to be omitted. Operand count alone cannot
  // distinguish `value[low:]` from `value[:high]`, so both bits survive HIR.
  bool slice_has_low = false;
  bool slice_has_high = false;
  bool addressable = false;
};

enum class HirStatementKind {
  Invalid,
  Block,
  LocalDeclaration,
  Expression,
  Assignment,
  Return,
  Break,
  Continue,
  Defer,
  If,
  CompileTimeSelection,
  Denial,
  For,
  Switch,
  Judgment,
  Synthesis,
  Unchecked,
  Assembly,
};

// A switch keeps its case boundaries explicitly. first_label indexes the
// owning statement's expressions vector, whose element zero is always the
// subject. A default case has label_count zero. body is also present in the
// statement's blocks vector in source order, but retaining it here makes the
// relationship unambiguous and cheap to consume during CFG lowering.
struct HirSwitchCase {
  std::size_t first_label = 0;
  std::size_t label_count = 0;
  HirBlockId body;
  bool is_default = false;
};

enum class HirForKind {
  None,
  Infinite,
  Conditional,
  Clause,
  Iteration,
};

// HirStatement owns references to evaluated expressions, nested structured
// blocks, and any local symbols it introduces. Assignment lvalues precede their
// right-hand expressions in expressions, preserving Draft evaluation order.
// header_statements stores three-clause loop initialization and post operations
// without pretending they are body statements.
struct HirStatement {
  HirStatementKind kind = HirStatementKind::Invalid;
  SourceRange range;
  SyntaxReference syntax;
  std::vector<HirExpressionId> expressions;
  std::vector<HirBlockId> blocks;
  std::vector<SymbolId> bindings;
  std::vector<HirStatementId> header_statements;
  HirOperation operation = HirOperation::None;
  std::vector<HirSwitchCase> switch_cases;
  HirForKind for_kind = HirForKind::None;
  // Clause loops retain initialization and post statements in one ordered
  // vector; this boundary tells CFG lowering where the post sequence begins.
  std::size_t for_initialization_count = 0;
  bool local_is_uninitialized = false;
};

// Every runtime block has one lexical ScopeId and an ordered statement list.
// The range covers its braces when present or its case-list region otherwise.
struct HirBlock {
  ScopeId scope;
  SourceRange range;
  std::vector<HirStatementId> statements;
};

// One row exists for each checked procedure definition. Declaration-only foreign
// procedures and procedure types have no row. valid is false when body errors
// occurred, but body still references the recoverable HIR built before them.
struct HirProcedure {
  SymbolId symbol;
  TypeId type;
  HirBlockId body;
  bool valid = false;
  // A parametric declaration is checked once under its constraints so tools,
  // denials, and synthesis see the symbolic body. Only concrete instance rows
  // are executable and proceed to MIR.
  bool parametric_template = false;
};

class HirProgram {
public:
  [[nodiscard]] HirExpressionId add_expression(HirExpression expression);
  [[nodiscard]] HirStatementId add_statement(HirStatement statement);
  [[nodiscard]] HirBlockId add_block(HirBlock block);
  void add_procedure(HirProcedure procedure);

  [[nodiscard]] const HirExpression &expression(HirExpressionId id) const;
  [[nodiscard]] HirExpression &expression_mut(HirExpressionId id);
  [[nodiscard]] const HirStatement &statement(HirStatementId id) const;
  [[nodiscard]] const HirBlock &block(HirBlockId id) const;
  [[nodiscard]] const std::vector<HirProcedure> &procedures() const;
  [[nodiscard]] std::size_t expression_count() const;
  [[nodiscard]] std::size_t statement_count() const;
  [[nodiscard]] std::size_t block_count() const;

private:
  std::vector<HirExpression> expressions_;
  std::vector<HirStatement> statements_;
  std::vector<HirBlock> blocks_;
  std::vector<HirProcedure> procedures_;
};

} // namespace draft
