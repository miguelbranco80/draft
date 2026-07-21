// Defensive verifier for the target-independent MIR boundary.

#include "mir/verify.h"

#include <cstddef>
#include <string>
#include <vector>

namespace draft {
namespace {

class Verifier {
public:
  Verifier(const TypeStore &types, DiagnosticSink &diagnostics)
      : types_(types), diagnostics_(diagnostics) {}

  [[nodiscard]] bool run(const MirProgram &program) {
    const std::size_t initial_errors = diagnostics_.error_count();
    for (const MirProcedure &procedure : program.procedures()) {
      verify_procedure(procedure);
    }
    return diagnostics_.error_count() == initial_errors;
  }

private:
  [[nodiscard]] bool valid_type(TypeId type) const {
    return type.is_valid() && static_cast<std::size_t>(type.value) < types_.size();
  }

  [[nodiscard]] bool valid_value(
      const MirProcedure &procedure, MirValueId value) const {
    return value.is_valid() &&
        static_cast<std::size_t>(value.value) < procedure.values.size();
  }

  [[nodiscard]] bool valid_block(
      const MirProcedure &procedure, MirBlockId block) const {
    return block.is_valid() &&
        static_cast<std::size_t>(block.value) < procedure.blocks.size();
  }

  void error(SourceRange range, const std::string &message) {
    diagnostics_.error(range, "invalid MIR: " + message);
  }

  // Returns the semantic value addressed by one MIR pointer. Source pointers
  // carry an ordinary Pointer TypeId. Compiler-created local/member/index
  // addresses instead carry rawptr plus addressed_type on their defining
  // instruction, so verification does not require lowering to mutate the
  // semantic TypeStore. Invalid hand-built MIR returns no type and remains a
  // diagnostic rather than reaching an unchecked table access.
  [[nodiscard]] TypeId addressed_type(
      const MirProcedure &procedure, MirValueId value) const {
    if (!valid_value(procedure, value)) return {};
    const MirValue &row = procedure.value(value);
    if (!valid_type(row.type)) return {};
    const Type &type = types_.type(row.type);
    if (type.kind == TypeKind::Pointer) return type.element;
    if (type.kind != TypeKind::RawPointer || !row.definition.is_valid() ||
        static_cast<std::size_t>(row.definition.value) >=
            procedure.instructions.size()) {
      return {};
    }
    const TypeId addressed =
        procedure.instruction(row.definition).addressed_type;
    return valid_type(addressed) ? addressed : TypeId{};
  }

  // Enum fields can still contain an invalid underlying value in hand-built
  // MIR. Verify the closed semantic vocabulary before the backend maps names.
  [[nodiscard]] bool valid_atomic_order(AtomicMemoryOrder order) const {
    switch (order) {
    case AtomicMemoryOrder::Relaxed:
    case AtomicMemoryOrder::Acquire:
    case AtomicMemoryOrder::Release:
    case AtomicMemoryOrder::AcquireRelease:
    case AtomicMemoryOrder::SequentiallyConsistent:
      return true;
    }
    return false;
  }

  // Mirrors semantic checking at the phase boundary. The duplicate check is
  // intentional: MIR may eventually be deserialized or constructed by tools
  // that do not run the source body checker.
  [[nodiscard]] bool valid_compare_exchange_orders(
      AtomicMemoryOrder success, AtomicMemoryOrder failure) const {
    if (failure == AtomicMemoryOrder::Release ||
        failure == AtomicMemoryOrder::AcquireRelease) {
      return false;
    }
    return failure == AtomicMemoryOrder::Relaxed ||
        (failure == AtomicMemoryOrder::Acquire &&
         (success == AtomicMemoryOrder::Acquire ||
          success == AtomicMemoryOrder::AcquireRelease ||
          success == AtomicMemoryOrder::SequentiallyConsistent)) ||
        (failure == AtomicMemoryOrder::SequentiallyConsistent &&
         success == AtomicMemoryOrder::SequentiallyConsistent);
  }

  // Checks invariants shared by the six atomic instruction kinds after generic
  // operand-table validation. It does not rediscover core/atomic nominal
  // identity; that language-level proof belongs to semantic checking.
  void verify_atomic_shape(
      const MirProcedure &procedure, const MirInstruction &instruction) {
    const std::size_t arity = instruction.operands.size();
    if (!valid_atomic_order(instruction.atomic_order)) {
      error(instruction.range, "atomic operation has an unknown memory order");
    }
    if (instruction.kind != MirInstructionKind::AtomicFence && arity != 0 &&
        !addressed_type(procedure, instruction.operands.front()).is_valid()) {
      error(instruction.range, "atomic object operand is not a pointer");
    }
    switch (instruction.kind) {
    case MirInstructionKind::AtomicLoad:
      if (instruction.atomic_order == AtomicMemoryOrder::Release ||
          instruction.atomic_order == AtomicMemoryOrder::AcquireRelease) {
        error(instruction.range, "atomic load has an invalid memory order");
      }
      break;
    case MirInstructionKind::AtomicStore:
      if (instruction.type != types_.builtins().void_type) {
        error(instruction.range, "atomic store produces a result");
      }
      if (instruction.atomic_order == AtomicMemoryOrder::Acquire ||
          instruction.atomic_order == AtomicMemoryOrder::AcquireRelease) {
        error(instruction.range, "atomic store has an invalid memory order");
      }
      break;
    case MirInstructionKind::AtomicExchange:
    case MirInstructionKind::AtomicReadModifyWrite:
      if (arity == 2 && valid_value(procedure, instruction.operands[1]) &&
          procedure.value(instruction.operands[1]).type != instruction.type) {
        error(instruction.range, "atomic update value differs from result type");
      }
      if (instruction.kind == MirInstructionKind::AtomicReadModifyWrite &&
          instruction.operation != HirOperation::Add &&
          instruction.operation != HirOperation::Subtract &&
          instruction.operation != HirOperation::BitwiseAnd &&
          instruction.operation != HirOperation::BitwiseOr &&
          instruction.operation != HirOperation::BitwiseXor) {
        error(instruction.range, "atomic read/modify/write operation is invalid");
      }
      break;
    case MirInstructionKind::AtomicCompareExchange:
      if (instruction.type != types_.builtins().bool_type) {
        error(instruction.range, "compare-exchange result is not bool");
      }
      if (arity == 3 && valid_value(procedure, instruction.operands[1]) &&
          valid_value(procedure, instruction.operands[2])) {
        const TypeId expected_value =
            addressed_type(procedure, instruction.operands[1]);
        const TypeId desired = procedure.value(instruction.operands[2]).type;
        if (!expected_value.is_valid() || !valid_type(desired) ||
            expected_value != desired) {
          error(
              instruction.range,
              "compare-exchange expected pointer and desired value disagree");
        }
      }
      if (!valid_atomic_order(instruction.atomic_failure_order)) {
        error(
            instruction.range,
            "compare-exchange has an unknown failure memory order");
      }
      if (!valid_compare_exchange_orders(
              instruction.atomic_order, instruction.atomic_failure_order)) {
        error(instruction.range, "compare-exchange memory orders are invalid");
      }
      break;
    case MirInstructionKind::AtomicFence:
      if (instruction.type != types_.builtins().void_type) {
        error(instruction.range, "atomic fence produces a result");
      }
      break;
    default:
      break;
    }
  }

  void verify_instruction(
      const MirProcedure &procedure,
      MirInstructionId id,
      const MirInstruction &instruction) {
    if (!valid_type(instruction.type)) {
      error(instruction.range, "instruction has an invalid result type");
    }
    for (MirValueId operand : instruction.operands) {
      if (!valid_value(procedure, operand)) {
        error(instruction.range, "instruction references an invalid operand");
      }
    }
    if (instruction.result.is_valid()) {
      if (!valid_value(procedure, instruction.result)) {
        error(instruction.range, "instruction result is outside the value table");
      } else {
        const MirValue &value = procedure.values[instruction.result.value];
        if (value.definition != id || value.type != instruction.type) {
          error(instruction.range, "instruction result and value definition disagree");
        }
      }
    }

    const std::size_t arity = instruction.operands.size();
    const bool address_instruction =
        instruction.kind == MirInstructionKind::LocalAddress ||
        instruction.kind == MirInstructionKind::GlobalAddress ||
        instruction.kind == MirInstructionKind::MemberAddress ||
        instruction.kind == MirInstructionKind::IndexAddress;
    if (address_instruction) {
      if (!valid_type(instruction.addressed_type)) {
        error(instruction.range, "address instruction has no addressed type");
      }
      if (valid_type(instruction.type)) {
        const Type &result_type = types_.type(instruction.type);
        if (result_type.kind != TypeKind::RawPointer &&
            (result_type.kind != TypeKind::Pointer ||
             result_type.element != instruction.addressed_type)) {
          error(
              instruction.range,
              "address instruction result disagrees with its addressed type");
        }
      }
    } else if (instruction.addressed_type.is_valid()) {
      error(instruction.range,
            "non-address instruction carries an addressed type");
    }
    switch (instruction.kind) {
    case MirInstructionKind::Invalid:
      error(instruction.range, "invalid instruction reached verification");
      break;
    case MirInstructionKind::Constant:
    case MirInstructionKind::Zero:
    case MirInstructionKind::Context:
    case MirInstructionKind::GlobalAddress:
    case MirInstructionKind::ProcedureReference:
    case MirInstructionKind::AtomicFence:
      if (arity != 0) error(instruction.range, "leaf instruction has operands");
      if (instruction.kind == MirInstructionKind::AtomicFence) {
        verify_atomic_shape(procedure, instruction);
      }
      break;
    case MirInstructionKind::LocalAddress:
      if (!instruction.local.is_valid() ||
          static_cast<std::size_t>(instruction.local.value) >= procedure.locals.size()) {
        error(instruction.range, "local address references an invalid local");
      }
      if (arity != 0) error(instruction.range, "local address has operands");
      break;
    case MirInstructionKind::Load:
    case MirInstructionKind::Unary:
    case MirInstructionKind::Convert:
    case MirInstructionKind::Length:
    case MirInstructionKind::RawData:
    case MirInstructionKind::ExtractMember:
    case MirInstructionKind::MemberAddress:
    case MirInstructionKind::AtomicLoad:
      if (arity != 1) error(instruction.range, "unary instruction has wrong arity");
      if (instruction.kind == MirInstructionKind::AtomicLoad) {
        verify_atomic_shape(procedure, instruction);
      }
      if (instruction.kind == MirInstructionKind::RawData && arity == 1 &&
          valid_value(procedure, instruction.operands.front())) {
        const TypeId source =
            procedure.value(instruction.operands.front()).type;
        const bool source_is_string =
            valid_type(source) && types_.type(source).kind == TypeKind::String;
        const bool result_is_bytes = valid_type(instruction.type) &&
            types_.type(instruction.type).kind == TypeKind::MultiPointer &&
            types_.type(instruction.type).element ==
                types_.builtins().u8_type;
        if (!source_is_string || !result_is_bytes) {
          error(
              instruction.range,
              "raw_data must extract [^]u8 from a string");
        }
      }
      break;
    case MirInstructionKind::Store:
    case MirInstructionKind::Binary:
    case MirInstructionKind::PointerOffset:
    case MirInstructionKind::PointerSubtract:
    case MirInstructionKind::IndexAddress:
    case MirInstructionKind::BoundsCheck:
    case MirInstructionKind::AtomicStore:
    case MirInstructionKind::AtomicExchange:
    case MirInstructionKind::AtomicReadModifyWrite:
      if (arity != 2) error(instruction.range, "binary instruction has wrong arity");
      if (instruction.kind == MirInstructionKind::AtomicStore ||
          instruction.kind == MirInstructionKind::AtomicExchange ||
          instruction.kind == MirInstructionKind::AtomicReadModifyWrite) {
        verify_atomic_shape(procedure, instruction);
      }
      break;
    case MirInstructionKind::AtomicCompareExchange:
      if (arity != 3) {
        error(instruction.range, "compare-exchange requires object, expected, desired");
      }
      verify_atomic_shape(procedure, instruction);
      break;
    case MirInstructionKind::SliceBoundsCheck:
    case MirInstructionKind::Slice:
      if (arity != 3) error(instruction.range, "slice requires base, low, and high");
      break;
    case MirInstructionKind::Call:
      if (arity < 1) error(instruction.range, "call has no callee");
      break;
    case MirInstructionKind::Assert:
      if (arity < 1 || arity > 2) {
        error(instruction.range, "assert requires a condition and optional message");
      }
      break;
    case MirInstructionKind::Aggregate:
    case MirInstructionKind::Assembly:
      break;
    case MirInstructionKind::Trap:
      if (arity != 0) error(instruction.range, "trap has operands");
      break;
    }
  }

  void verify_terminator(
      const MirProcedure &procedure,
      const MirBlock &block,
      TypeId result_type) {
    const MirTerminator &terminator = block.terminator;
    if (terminator.kind == MirTerminatorKind::Invalid) {
      error(block.range, "basic block has no terminator");
      return;
    }
    if (terminator.value.is_valid() &&
        !valid_value(procedure, terminator.value)) {
      error(terminator.range, "terminator references an invalid value");
    }
    for (MirBlockId target : terminator.targets) {
      if (!valid_block(procedure, target)) {
        error(terminator.range, "terminator references an invalid block");
      }
    }
    for (const MirSwitchArm &arm : terminator.switch_arms) {
      if (!valid_value(procedure, arm.label) || !valid_block(procedure, arm.target)) {
        error(terminator.range, "switch arm references an invalid value or block");
      }
    }

    switch (terminator.kind) {
    case MirTerminatorKind::Invalid:
      break;
    case MirTerminatorKind::Return:
      if (terminator.targets.size() != 0) {
        error(terminator.range, "return has branch targets");
      }
      if (result_type == types_.builtins().void_type && terminator.value.is_valid()) {
        error(terminator.range, "void procedure returns a value");
      } else if (result_type != types_.builtins().void_type &&
                 !terminator.value.is_valid()) {
        error(terminator.range, "non-void procedure returns no value");
      } else if (terminator.value.is_valid() &&
                 procedure.value(terminator.value).type != result_type) {
        error(terminator.range, "return value type differs from procedure result");
      }
      break;
    case MirTerminatorKind::Branch:
      if (terminator.targets.size() != 1 || terminator.value.is_valid()) {
        error(terminator.range, "branch shape is invalid");
      }
      break;
    case MirTerminatorKind::ConditionalBranch:
      if (terminator.targets.size() != 2 || !terminator.value.is_valid()) {
        error(terminator.range, "conditional branch shape is invalid");
      } else if (procedure.value(terminator.value).type !=
                 types_.builtins().bool_type) {
        error(terminator.range, "conditional branch condition is not bool");
      }
      break;
    case MirTerminatorKind::Switch:
      if (terminator.targets.size() != 1 || !terminator.value.is_valid()) {
        error(terminator.range, "switch shape is invalid");
      } else {
        const TypeId subject_type = procedure.value(terminator.value).type;
        for (const MirSwitchArm &arm : terminator.switch_arms) {
          if (valid_value(procedure, arm.label) &&
              procedure.value(arm.label).type != subject_type) {
            error(
                terminator.range,
                "switch label type differs from subject type");
          }
        }
      }
      break;
    case MirTerminatorKind::Unreachable:
      if (terminator.value.is_valid() || !terminator.targets.empty()) {
        error(terminator.range, "unreachable terminator carries data");
      }
      break;
    }
  }

  void verify_procedure(const MirProcedure &procedure) {
    if (!valid_type(procedure.type) ||
        types_.type(procedure.type).kind != TypeKind::Procedure) {
      error(procedure.range, "procedure row has a non-procedure type");
      return;
    }
    if (!valid_block(procedure, procedure.entry)) {
      error(procedure.range, "procedure has no valid entry block");
    }
    const Type &signature = types_.type(procedure.type);
    const TypeId result_type = signature.members.empty()
        ? types_.builtins().void_type
        : signature.members.back();

    std::vector<std::size_t> instruction_owners(
        procedure.instructions.size(), 0);
    for (const MirBlock &block : procedure.blocks) {
      for (MirInstructionId instruction_id : block.instructions) {
        if (!instruction_id.is_valid() ||
            static_cast<std::size_t>(instruction_id.value) >=
                procedure.instructions.size()) {
          error(block.range, "block references an invalid instruction");
          continue;
        }
        ++instruction_owners[instruction_id.value];
        verify_instruction(
            procedure,
            instruction_id,
            procedure.instructions[instruction_id.value]);
      }
      verify_terminator(procedure, block, result_type);
    }
    for (std::size_t count : instruction_owners) {
      if (count != 1) {
        error(procedure.range, "instruction does not belong to exactly one block");
      }
    }
  }

  const TypeStore &types_;
  DiagnosticSink &diagnostics_;
};

} // namespace

bool verify_mir_program(
    const MirProgram &program,
    const TypeStore &types,
    DiagnosticSink &diagnostics) {
  return Verifier(types, diagnostics).run(program);
}

} // namespace draft
