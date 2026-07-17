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
    switch (instruction.kind) {
    case MirInstructionKind::Invalid:
      error(instruction.range, "invalid instruction reached verification");
      break;
    case MirInstructionKind::Constant:
    case MirInstructionKind::Zero:
    case MirInstructionKind::Context:
    case MirInstructionKind::GlobalAddress:
    case MirInstructionKind::ProcedureReference:
      if (arity != 0) error(instruction.range, "leaf instruction has operands");
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
    case MirInstructionKind::ExtractMember:
    case MirInstructionKind::MemberAddress:
      if (arity != 1) error(instruction.range, "unary instruction has wrong arity");
      break;
    case MirInstructionKind::Store:
    case MirInstructionKind::Binary:
    case MirInstructionKind::IndexAddress:
    case MirInstructionKind::BoundsCheck:
      if (arity != 2) error(instruction.range, "binary instruction has wrong arity");
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
