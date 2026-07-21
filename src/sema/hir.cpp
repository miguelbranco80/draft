// Append-only storage and deterministic composition for typed Draft HIR.
//
// HirProgram owns expressions, statements, blocks, and procedure roots in
// contiguous tables. IDs are stable only within one HirProgram. Body checking
// now produces one such arena per exact procedure product; the composition
// operation at the end of this file rewrites those local ID domains when a
// transitional package-wide consumer needs one projection. Semantic IDs and
// source coordinates are not owned here and are never rewritten.
//
// This module depends only on the HIR representation and base C++ facilities.
// It assumes both sides of a composition belong to the same semantic package
// generation. It performs no checking, target classification, or lowering.

#include "sema/hir.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace draft {

bool HirExpressionId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

bool HirStatementId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

bool HirBlockId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

HirExpressionId HirProgram::add_expression(HirExpression expression) {
  assert(expressions_.size() <
         static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const HirExpressionId id{static_cast<std::uint32_t>(expressions_.size())};
  expressions_.push_back(std::move(expression));
  return id;
}

HirStatementId HirProgram::add_statement(HirStatement statement) {
  assert(statements_.size() <
         static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const HirStatementId id{static_cast<std::uint32_t>(statements_.size())};
  statements_.push_back(std::move(statement));
  return id;
}

HirBlockId HirProgram::add_block(HirBlock block) {
  assert(blocks_.size() <
         static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const HirBlockId id{static_cast<std::uint32_t>(blocks_.size())};
  blocks_.push_back(std::move(block));
  return id;
}

void HirProgram::add_procedure(HirProcedure procedure) {
  procedures_.push_back(std::move(procedure));
}

const HirExpression &HirProgram::expression(HirExpressionId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < expressions_.size());
  return expressions_[id.value];
}

HirExpression &HirProgram::expression_mut(HirExpressionId id) {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < expressions_.size());
  return expressions_[id.value];
}

const HirStatement &HirProgram::statement(HirStatementId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < statements_.size());
  return statements_[id.value];
}

const HirBlock &HirProgram::block(HirBlockId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < blocks_.size());
  return blocks_[id.value];
}

const std::vector<HirProcedure> &HirProgram::procedures() const {
  return procedures_;
}

std::size_t HirProgram::expression_count() const {
  return expressions_.size();
}

std::size_t HirProgram::statement_count() const {
  return statements_.size();
}

std::size_t HirProgram::block_count() const {
  return blocks_.size();
}

namespace {

template <typename Id>
void offset_hir_id(Id &id, std::size_t offset) {
  if (!id.is_valid()) return;
  assert(offset <=
         static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
             id.value);
  id.value += static_cast<std::uint32_t>(offset);
}

} // namespace

void append_hir_program(HirProgram &destination, const HirProgram &source) {
  const std::size_t expression_offset = destination.expression_count();
  const std::size_t statement_offset = destination.statement_count();
  const std::size_t block_offset = destination.block_count();

  // Every expression operand addresses the source expression table. Semantic
  // IDs and source coordinates require no rewriting because the caller has
  // already established one shared semantic package generation.
  for (std::size_t index = 0; index < source.expression_count(); ++index) {
    HirExpression expression = source.expression(
        {static_cast<std::uint32_t>(index)});
    for (HirExpressionId &operand : expression.operands) {
      offset_hir_id(operand, expression_offset);
    }
    (void)destination.add_expression(std::move(expression));
  }

  // Statements can name expressions, blocks, header statements, and switch
  // case bodies. All four domains have fixed offsets before any source row is
  // appended, so source order does not affect rewriting.
  for (std::size_t index = 0; index < source.statement_count(); ++index) {
    HirStatement statement = source.statement(
        {static_cast<std::uint32_t>(index)});
    for (HirExpressionId &expression : statement.expressions) {
      offset_hir_id(expression, expression_offset);
    }
    for (HirBlockId &block : statement.blocks) {
      offset_hir_id(block, block_offset);
    }
    for (HirStatementId &header : statement.header_statements) {
      offset_hir_id(header, statement_offset);
    }
    for (HirSwitchCase &switch_case : statement.switch_cases) {
      offset_hir_id(switch_case.body, block_offset);
    }
    (void)destination.add_statement(std::move(statement));
  }

  for (std::size_t index = 0; index < source.block_count(); ++index) {
    HirBlock block = source.block({static_cast<std::uint32_t>(index)});
    for (HirStatementId &statement : block.statements) {
      offset_hir_id(statement, statement_offset);
    }
    (void)destination.add_block(std::move(block));
  }

  for (HirProcedure procedure : source.procedures()) {
    offset_hir_id(procedure.body, block_offset);
    destination.add_procedure(std::move(procedure));
  }
}

} // namespace draft
