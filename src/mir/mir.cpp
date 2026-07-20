// Append-only storage and names for Draft MIR.

#include "mir/mir.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace draft {
namespace {

[[nodiscard]] bool valid_index(std::uint32_t value) {
  return value != std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] std::uint32_t checked_size(std::size_t size) {
  assert(size < static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max()));
  return static_cast<std::uint32_t>(size);
}

} // namespace

bool MirValueId::is_valid() const { return valid_index(value); }
bool MirInstructionId::is_valid() const { return valid_index(value); }
bool MirBlockId::is_valid() const { return valid_index(value); }
bool MirLocalId::is_valid() const { return valid_index(value); }

MirLocalId MirProcedure::add_local(MirLocal local) {
  const MirLocalId id{checked_size(locals.size())};
  locals.push_back(std::move(local));
  return id;
}

MirBlockId MirProcedure::add_block(SourceRange block_range) {
  const MirBlockId id{checked_size(blocks.size())};
  MirBlock block;
  block.range = block_range;
  blocks.push_back(std::move(block));
  return id;
}

MirInstructionId MirProcedure::add_instruction(
    MirBlockId block_id,
    MirInstruction instruction,
    bool produces_value) {
  assert(block_id.is_valid());
  assert(static_cast<std::size_t>(block_id.value) < blocks.size());
  const MirInstructionId instruction_id{checked_size(instructions.size())};
  if (produces_value) {
    const MirValueId value_id{checked_size(values.size())};
    instruction.result = value_id;
    values.push_back({instruction.type, instruction_id});
  }
  instructions.push_back(std::move(instruction));
  blocks[block_id.value].instructions.push_back(instruction_id);
  return instruction_id;
}

void MirProcedure::set_terminator(
    MirBlockId block_id, MirTerminator terminator) {
  assert(block_id.is_valid());
  assert(static_cast<std::size_t>(block_id.value) < blocks.size());
  assert(blocks[block_id.value].terminator.kind == MirTerminatorKind::Invalid);
  blocks[block_id.value].terminator = std::move(terminator);
}

const MirLocal &MirProcedure::local(MirLocalId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < locals.size());
  return locals[id.value];
}

const MirValue &MirProcedure::value(MirValueId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < values.size());
  return values[id.value];
}

const MirInstruction &MirProcedure::instruction(MirInstructionId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < instructions.size());
  return instructions[id.value];
}

const MirBlock &MirProcedure::block(MirBlockId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < blocks.size());
  return blocks[id.value];
}

void MirProgram::add_procedure(MirProcedure procedure) {
  procedures_.push_back(std::move(procedure));
}

const std::vector<MirProcedure> &MirProgram::procedures() const {
  return procedures_;
}

const char *mir_instruction_kind_name(MirInstructionKind kind) {
  switch (kind) {
  case MirInstructionKind::Invalid: return "invalid";
  case MirInstructionKind::Constant: return "constant";
  case MirInstructionKind::Zero: return "zero";
  case MirInstructionKind::Context: return "context";
  case MirInstructionKind::LocalAddress: return "local_address";
  case MirInstructionKind::GlobalAddress: return "global_address";
  case MirInstructionKind::ProcedureReference: return "procedure_reference";
  case MirInstructionKind::Load: return "load";
  case MirInstructionKind::Store: return "store";
  case MirInstructionKind::AtomicLoad: return "atomic_load";
  case MirInstructionKind::AtomicStore: return "atomic_store";
  case MirInstructionKind::AtomicExchange: return "atomic_exchange";
  case MirInstructionKind::AtomicReadModifyWrite: return "atomic_read_modify_write";
  case MirInstructionKind::AtomicCompareExchange: return "atomic_compare_exchange";
  case MirInstructionKind::AtomicFence: return "atomic_fence";
  case MirInstructionKind::Unary: return "unary";
  case MirInstructionKind::Binary: return "binary";
  case MirInstructionKind::Convert: return "convert";
  case MirInstructionKind::PointerOffset: return "pointer_offset";
  case MirInstructionKind::PointerSubtract: return "pointer_subtract";
  case MirInstructionKind::Call: return "call";
  case MirInstructionKind::Length: return "length";
  case MirInstructionKind::RawData: return "raw_data";
  case MirInstructionKind::Assert: return "assert";
  case MirInstructionKind::MemberAddress: return "member_address";
  case MirInstructionKind::ExtractMember: return "extract_member";
  case MirInstructionKind::IndexAddress: return "index_address";
  case MirInstructionKind::BoundsCheck: return "bounds_check";
  case MirInstructionKind::SliceBoundsCheck: return "slice_bounds_check";
  case MirInstructionKind::Slice: return "slice";
  case MirInstructionKind::Aggregate: return "aggregate";
  case MirInstructionKind::Assembly: return "assembly";
  case MirInstructionKind::Trap: return "trap";
  }
  return "unknown";
}

const char *mir_terminator_kind_name(MirTerminatorKind kind) {
  switch (kind) {
  case MirTerminatorKind::Invalid: return "invalid";
  case MirTerminatorKind::Return: return "return";
  case MirTerminatorKind::Branch: return "branch";
  case MirTerminatorKind::ConditionalBranch: return "conditional_branch";
  case MirTerminatorKind::Switch: return "switch";
  case MirTerminatorKind::Unreachable: return "unreachable";
  }
  return "unknown";
}

} // namespace draft
