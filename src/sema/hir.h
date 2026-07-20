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
  // A discard occupies one assignment target position but has no address or
  // value. Its matching right-hand expression is still checked and lowered.
  Discard,
  Constant,
  Symbol,
  Context,
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

// Atomic order is semantic data, not a runtime enum value after checking.
// Direct core/atomic calls require a compile-time Order argument, allowing MIR
// and every backend to retain the exact language memory model without target
// library calls or host-compiler builtins.
enum class AtomicMemoryOrder {
  Relaxed,
  Acquire,
  Release,
  AcquireRelease,
  SequentiallyConsistent,
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
  // Only core/atomic Intrinsic nodes use these fields. The failure order is
  // meaningful only for compare-exchange; all other nodes retain the default.
  AtomicMemoryOrder atomic_order = AtomicMemoryOrder::SequentiallyConsistent;
  AtomicMemoryOrder atomic_failure_order =
      AtomicMemoryOrder::SequentiallyConsistent;
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
  // True only when body checking evaluated every relevant bound against a
  // compile-time-known length. MIR omits the runtime check in that case; false
  // means either dynamic bounds or an unchecked source region decides later.
  bool bounds_proven = false;
  bool addressable = false;
};

enum class HirStatementKind {
  Invalid,
  Block,
  LocalDeclaration,
  // A lexical `::` value is evaluated by the compiler and owns no runtime
  // storage. The binding remains in the semantic scope for later statements
  // and nested procedure bodies.
  CompileTimeDeclaration,
  // A lexical type declaration extends semantic visibility but has no runtime
  // initialization. Nominal member scopes and layouts live in SemanticPackage.
  TypeDeclaration,
  // A nested procedure declaration changes lexical visibility but performs no
  // runtime operation. Its separately checked HirProcedure row is lowered as
  // an ordinary static function.
  NestedProcedure,
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
  // A tagged-union payload binding belongs to exactly one case alternative.
  // The alternative identifies the payload byte offset/type; the binding is
  // invalid when the case ignores the payload or uses `_`.
  SymbolId payload_alternative;
  SymbolId payload_binding;
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
// right-hand expressions in expressions, preserving Draft evaluation order;
// assignment_target_count records the exact boundary even when tuple
// destructuring collapses discarded pattern positions.
// header_statements stores three-clause loop initialization and post operations
// without pretending they are body statements.
struct HirStatement {
  HirStatementKind kind = HirStatementKind::Invalid;
  SourceRange range;
  SyntaxReference syntax;
  std::vector<HirExpressionId> expressions;
  std::vector<HirBlockId> blocks;
  std::vector<SymbolId> bindings;
  // A tuple-pattern declaration evaluates one aggregate initializer and gives
  // each non-discard binding one selected member. This parallel vector records
  // those source member indices; discarded `_` positions simply have no row.
  // Ordinary declarations and iteration bindings leave it empty.
  std::vector<std::size_t> binding_member_indices;
  // A tuple assignment likewise stores only non-discard lvalues. This vector
  // maps each stored target to its source tuple member; ordinary assignments
  // leave it empty and retain one expression row per target, including `_`.
  std::vector<std::size_t> assignment_member_indices;
  std::vector<HirStatementId> header_statements;
  HirOperation operation = HirOperation::None;
  std::vector<HirSwitchCase> switch_cases;
  HirForKind for_kind = HirForKind::None;
  // Clause loops retain initialization and post statements in one ordered
  // vector; this boundary tells CFG lowering where the post sequence begins.
  std::size_t for_initialization_count = 0;
  std::size_t assignment_target_count = 0;
  bool local_is_uninitialized = false;
  bool local_destructures_tuple = false;
  bool assignment_destructures_tuple = false;
  // Set after semantic coverage checking. A default makes any switch
  // exhaustive; enum and tagged-union switches are also exhaustive when every
  // declared alternative is covered exactly once.
  bool switch_is_exhaustive = false;
  // True only when exhaustiveness holds and every checked case body definitely
  // returns. MIR uses this to terminate its otherwise predecessor-free join.
  bool switch_definitely_returns = false;
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
  // A non-parametric procedure whose signature contains the layout-less `type`
  // meta-value is callable only by constant evaluation. Its body is still
  // checked and remains available to agent/effect analysis, but no physical
  // calling convention exists for passing or returning those values, so the
  // row must stop before MIR just like a symbolic template row.
  bool compile_time_only = false;
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
