// Append-only storage implementation for typed Draft HIR.

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

} // namespace draft
