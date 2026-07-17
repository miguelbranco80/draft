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
  Synthesis,
  Assembly,
};

// HirExpression operands are in source evaluation order. symbol is meaningful
// for Symbol and Member nodes; constant is meaningful for Constant. addressable
// records the language property checked by `&` and assignment, not whether MIR
// later chooses a stack slot.
struct HirExpression {
  HirExpressionKind kind = HirExpressionKind::Invalid;
  TypeId type;
  SourceRange range;
  SymbolId symbol;
  ConstantValue constant;
  std::vector<HirExpressionId> operands;
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
  For,
  Switch,
  Judgment,
  Synthesis,
  Unchecked,
  Assembly,
};

// HirStatement owns references to evaluated expressions, nested structured
// blocks, and any local symbols it introduces. Assignment lvalues precede their
// right-hand expressions in expressions, preserving Draft evaluation order.
// header_statements stores three-clause loop initialization and post operations
// without pretending they are body statements.
struct HirStatement {
  HirStatementKind kind = HirStatementKind::Invalid;
  SourceRange range;
  std::vector<HirExpressionId> expressions;
  std::vector<HirBlockId> blocks;
  std::vector<SymbolId> bindings;
  std::vector<HirStatementId> header_statements;
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
};

class HirProgram {
public:
  [[nodiscard]] HirExpressionId add_expression(HirExpression expression);
  [[nodiscard]] HirStatementId add_statement(HirStatement statement);
  [[nodiscard]] HirBlockId add_block(HirBlock block);
  void add_procedure(HirProcedure procedure);

  [[nodiscard]] const HirExpression &expression(HirExpressionId id) const;
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
