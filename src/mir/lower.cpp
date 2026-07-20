// Direct structured-HIR to CFG/MIR lowering.
//
// The implementation intentionally mirrors Draft evaluation order. It first
// computes lvalue addresses, then right-hand values, and finally stores;
// short-circuit and conditional expressions become branches; a defer captures
// call operands at its source position and emits calls in reverse order on each
// lexical exit. There is no optimization in this pass.

#include "mir/lower.h"

#include "mir/verify.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

struct LocalBinding {
  SymbolId symbol;
  MirLocalId local;
};

struct CapturedCall {
  SourceRange range;
  TypeId result_type;
  std::vector<MirValueId> operands;
  bool passes_context = false;
};

struct DeferScope {
  std::vector<CapturedCall> calls;
};

struct ControlTarget {
  MirBlockId break_target;
  MirBlockId continue_target;
  std::size_t defer_depth = 0;
  bool is_loop = false;
};

class ProcedureLowerer {
public:
  ProcedureLowerer(
      SemanticPackage &semantic,
      const HirProgram &hir,
      const HirProcedure &source,
      const AssemblyProgram *assembly,
      RuntimeAssertionMode runtime_assertions,
      DiagnosticSink &diagnostics)
      : semantic_(semantic), hir_(hir), source_(source),
        assembly_(assembly), runtime_assertions_(runtime_assertions),
        diagnostics_(diagnostics) {}

  [[nodiscard]] MirProcedure run() {
    procedure_.symbol = source_.symbol;
    procedure_.type = source_.type;
    procedure_.range = semantic_.symbols.symbol(source_.symbol).name_range;
    procedure_.entry = procedure_.add_block(procedure_.range);
    current_ = procedure_.entry;

    if (!source_.valid) {
      MirTerminator terminator;
      terminator.kind = MirTerminatorKind::Unreachable;
      terminator.range = procedure_.range;
      procedure_.set_terminator(current_, std::move(terminator));
      return std::move(procedure_);
    }

    add_parameters();
    const Type signature = semantic_.types.type(source_.type);
    if (!signature.c_calling_convention) {
      MirInstruction context;
      context.kind = MirInstructionKind::Context;
      context.range = procedure_.range;
      context.type = semantic_.types.builtins().rawptr_type;
      context_ = emit_value(std::move(context));
    }

    lower_block(source_.body);
    if (!terminated()) {
      const TypeId result = procedure_result_type();
      if (result != semantic_.types.builtins().void_type) {
        diagnostics_.error(
            procedure_.range,
            "non-void procedure reaches MIR fallthrough without a return");
        MirTerminator terminator;
        terminator.kind = MirTerminatorKind::Unreachable;
        terminator.range = procedure_.range;
        procedure_.set_terminator(current_, std::move(terminator));
      } else {
        MirTerminator terminator;
        terminator.kind = MirTerminatorKind::Return;
        terminator.range = procedure_.range;
        procedure_.set_terminator(current_, std::move(terminator));
      }
    }
    procedure_.valid = diagnostics_.error_count() == initial_errors_;
    return std::move(procedure_);
  }

private:
  [[nodiscard]] TypeId procedure_result_type() const {
    const Type signature = semantic_.types.type(source_.type);
    return signature.members.empty()
        ? semantic_.types.builtins().void_type
        : signature.members.back();
  }

  void add_parameters() {
    std::optional<ScopeId> parameter_scope;
    for (const OwnedSemanticScope &owned : semantic_.owned_scopes) {
      if (owned.owner == source_.symbol &&
          semantic_.symbols.scope(owned.scope).kind == ScopeKind::Procedure) {
        parameter_scope = owned.scope;
        break;
      }
    }
    if (!parameter_scope.has_value()) {
      diagnostics_.error(procedure_.range, "procedure has no semantic parameter scope");
      return;
    }
    std::uint32_t parameter_index = 0;
    for (SymbolId symbol_id : semantic_.symbols.scope(*parameter_scope).symbols) {
      const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
      if (symbol.kind != SymbolKind::Parameter) continue;
      MirLocal local;
      local.kind = MirLocalKind::Parameter;
      local.symbol = symbol_id;
      local.type = symbol.type;
      local.range = symbol.name_range;
      local.parameter_index = parameter_index;
      const MirLocalId local_id = procedure_.add_local(std::move(local));
      locals_.push_back({symbol_id, local_id});
      ++parameter_index;
    }
  }

  [[nodiscard]] bool terminated() const {
    return procedure_.blocks[current_.value].terminator.kind !=
        MirTerminatorKind::Invalid;
  }

  [[nodiscard]] MirValueId emit_value(MirInstruction instruction) {
    const MirInstructionId id =
        procedure_.add_instruction(current_, std::move(instruction), true);
    return procedure_.instructions[id.value].result;
  }

  void emit_void(MirInstruction instruction) {
    (void)procedure_.add_instruction(current_, std::move(instruction), false);
  }

  void branch(MirBlockId target, SourceRange range) {
    if (terminated()) return;
    MirTerminator terminator;
    terminator.kind = MirTerminatorKind::Branch;
    terminator.range = range;
    terminator.targets.push_back(target);
    procedure_.set_terminator(current_, std::move(terminator));
  }

  void conditional_branch(
      MirValueId condition,
      MirBlockId true_target,
      MirBlockId false_target,
      SourceRange range) {
    MirTerminator terminator;
    terminator.kind = MirTerminatorKind::ConditionalBranch;
    terminator.range = range;
    terminator.value = condition;
    terminator.targets = {true_target, false_target};
    procedure_.set_terminator(current_, std::move(terminator));
  }

  [[nodiscard]] MirLocalId find_local(SymbolId symbol) const {
    for (const LocalBinding &binding : locals_) {
      if (binding.symbol == symbol) return binding.local;
    }
    return {};
  }

  [[nodiscard]] MirLocalId ensure_local(SymbolId symbol_id) {
    const MirLocalId existing = find_local(symbol_id);
    if (existing.is_valid()) return existing;
    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    MirLocal local;
    local.kind = MirLocalKind::Automatic;
    local.symbol = symbol_id;
    local.type = symbol.type;
    local.range = symbol.name_range;
    const MirLocalId id = procedure_.add_local(std::move(local));
    locals_.push_back({symbol_id, id});
    return id;
  }

  [[nodiscard]] MirLocalId add_temporary(TypeId type, SourceRange range) {
    MirLocal local;
    local.kind = MirLocalKind::Temporary;
    local.type = type;
    local.range = range;
    return procedure_.add_local(std::move(local));
  }

  [[nodiscard]] MirValueId local_address(
      MirLocalId local_id, SourceRange range) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::LocalAddress;
    instruction.range = range;
    instruction.local = local_id;
    instruction.type = semantic_.types.pointer(procedure_.local(local_id).type);
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] MirValueId global_address(
      SymbolId symbol_id, SourceRange range) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::GlobalAddress;
    instruction.range = range;
    instruction.symbol = symbol_id;
    instruction.type =
        semantic_.types.pointer(semantic_.symbols.symbol(symbol_id).type);
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] MirValueId load(
      MirValueId address, TypeId type, SourceRange range) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Load;
    instruction.range = range;
    instruction.type = type;
    instruction.operands.push_back(address);
    return emit_value(std::move(instruction));
  }

  void store(MirValueId address, MirValueId value, SourceRange range) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Store;
    instruction.range = range;
    instruction.type = semantic_.types.builtins().void_type;
    instruction.operands = {address, value};
    emit_void(std::move(instruction));
  }

  [[nodiscard]] MirValueId constant(
      ConstantValue value,
      TypeId type,
      SourceRange range,
      SymbolId symbol = {}) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Constant;
    instruction.range = range;
    instruction.type = type;
    instruction.constant = std::move(value);
    instruction.symbol = symbol;
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] MirValueId usize_constant(
      std::uint64_t value, SourceRange range) {
    return constant(
        ConstantValue::make_integer(BigInteger::from_u64(value)),
        semantic_.types.builtins().usize_type,
        range);
  }

  [[nodiscard]] MirValueId zero(TypeId type, SourceRange range) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Zero;
    instruction.range = range;
    instruction.type = type;
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] MirValueId binary(
      HirOperation operation,
      MirValueId left,
      MirValueId right,
      TypeId type,
      SourceRange range) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Binary;
    instruction.range = range;
    instruction.type = type;
    instruction.operation = operation;
    instruction.operands = {left, right};
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] Type runtime_scalar_type(TypeId type_id) const {
    Type result = semantic_.types.type(type_id);
    while (result.kind == TypeKind::Distinct) {
      result = semantic_.types.type(result.element);
    }
    return result;
  }

  [[nodiscard]] bool signed_integer_type(TypeId type_id) const {
    const Type type = runtime_scalar_type(type_id);
    if (type.kind == TypeKind::Enum && type.element.is_valid()) {
      return signed_integer_type(type.element);
    }
    return type.kind == TypeKind::SignedInteger || type.kind == TypeKind::Rune;
  }

  [[nodiscard]] std::uint32_t integer_bit_width(TypeId type_id) const {
    const Type type = runtime_scalar_type(type_id);
    if (type.kind == TypeKind::Enum) {
      return static_cast<std::uint32_t>(type.layout.size * 8U);
    }
    return type.bit_width;
  }

  // Emits an explicit failure edge rather than relying on LLVM's undefined or
  // poison behavior. The safe block becomes the new insertion point.
  void trap_unless(MirValueId condition, SourceRange range) {
    const MirBlockId safe = procedure_.add_block(range);
    const MirBlockId failure = procedure_.add_block(range);
    conditional_branch(condition, safe, failure, range);
    current_ = failure;
    MirInstruction trap;
    trap.kind = MirInstructionKind::Trap;
    trap.range = range;
    trap.type = semantic_.types.builtins().void_type;
    emit_void(std::move(trap));
    MirTerminator unreachable;
    unreachable.kind = MirTerminatorKind::Unreachable;
    unreachable.range = range;
    procedure_.set_terminator(current_, std::move(unreachable));
    current_ = safe;
  }

  // Selects between two already-evaluated scalar values without exposing a
  // target-specific select instruction in MIR. The temporary also keeps both
  // predecessor paths explicit for the verifier and for future backends.
  // Callers must evaluate true_value and false_value before entering here;
  // Draft expression evaluation order is therefore unchanged.
  [[nodiscard]] MirValueId select_value(
      MirValueId condition,
      MirValueId true_value,
      MirValueId false_value,
      TypeId type,
      SourceRange range) {
    const MirLocalId result_local = add_temporary(type, range);
    const MirValueId result_address = local_address(result_local, range);
    const MirBlockId true_block = procedure_.add_block(range);
    const MirBlockId false_block = procedure_.add_block(range);
    const MirBlockId join_block = procedure_.add_block(range);
    conditional_branch(condition, true_block, false_block, range);

    current_ = true_block;
    store(result_address, true_value, range);
    branch(join_block, range);
    current_ = false_block;
    store(result_address, false_value, range);
    branch(join_block, range);
    current_ = join_block;
    return load(result_address, type, range);
  }

  [[nodiscard]] MirValueId convert(
      MirValueId source, TypeId target, SourceRange range) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Convert;
    instruction.range = range;
    instruction.type = target;
    instruction.operands.push_back(source);
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] std::uint32_t floating_precision(TypeId type_id) const {
    const Type type = runtime_scalar_type(type_id);
    if (type.kind != TypeKind::Float) return 0;
    if (type.bit_width == 16) return 11;
    if (type.bit_width == 32) return 24;
    if (type.bit_width == 64) return 53;
    return 0;
  }

  // LLVM's fptosi/fptoui produce poison for NaN and out-of-range input. Draft
  // requires a trap, so the conversion is reached only through ordered bounds
  // checks. For wide signed targets the source format cannot represent min-1;
  // comparing inclusively with the exactly representable minimum is equivalent
  // and avoids rounding the lower threshold onto a valid value.
  [[nodiscard]] MirValueId checked_conversion(
      MirValueId source, TypeId target, SourceRange range) {
    const Type source_type = runtime_scalar_type(procedure_.value(source).type);
    const Type target_type = runtime_scalar_type(target);
    const bool integer_target =
        target_type.kind == TypeKind::SignedInteger ||
        target_type.kind == TypeKind::UnsignedInteger ||
        target_type.kind == TypeKind::Rune ||
        target_type.kind == TypeKind::Enum;
    if (source_type.kind == TypeKind::Float && integer_target) {
      const std::uint32_t bits = integer_bit_width(target);
      if (bits != 0) {
        const TypeId comparison_type = procedure_.value(source).type;
        const bool signed_target = signed_integer_type(target);
        BigInteger lower;
        HirOperation lower_operation = HirOperation::Greater;
        if (signed_target) {
          const BigInteger minimum = BigInteger::from_u64(1)
              .shifted_left(static_cast<std::size_t>(bits - 1U))
              .negated();
          if (bits > floating_precision(comparison_type)) {
            lower = minimum;
            lower_operation = HirOperation::GreaterEqual;
          } else {
            lower = minimum.subtracted(BigInteger::from_u64(1));
          }
        } else {
          lower = BigInteger::from_i64(-1);
        }
        const BigInteger upper = BigInteger::from_u64(1).shifted_left(
            static_cast<std::size_t>(signed_target ? bits - 1U : bits));
        const MirValueId lower_value = constant(
            ConstantValue::make_float(ExactRational(lower)), comparison_type, range);
        const MirValueId upper_value = constant(
            ConstantValue::make_float(ExactRational(upper)), comparison_type, range);
        const MirValueId above_lower = binary(
            lower_operation,
            source,
            lower_value,
            semantic_.types.builtins().bool_type,
            range);
        const MirValueId below_upper = binary(
            HirOperation::Less,
            source,
            upper_value,
            semantic_.types.builtins().bool_type,
            range);
        const MirValueId in_range = binary(
            HirOperation::BitwiseAnd,
            above_lower,
            below_upper,
            semantic_.types.builtins().bool_type,
            range);
        trap_unless(in_range, range);
      }
    }

    const MirValueId converted = convert(source, target, range);
    if (target_type.kind == TypeKind::Rune) {
      const MirValueId zero = constant(
          ConstantValue::make_integer(0), target, range);
      const MirValueId maximum = constant(
          ConstantValue::make_integer(0x10ffff), target, range);
      const MirValueId surrogate_begin = constant(
          ConstantValue::make_integer(0xd800), target, range);
      const MirValueId surrogate_end = constant(
          ConstantValue::make_integer(0xdfff), target, range);
      const MirValueId nonnegative = binary(
          HirOperation::GreaterEqual,
          converted,
          zero,
          semantic_.types.builtins().bool_type,
          range);
      const MirValueId below_maximum = binary(
          HirOperation::LessEqual,
          converted,
          maximum,
          semantic_.types.builtins().bool_type,
          range);
      const MirValueId before_surrogates = binary(
          HirOperation::Less,
          converted,
          surrogate_begin,
          semantic_.types.builtins().bool_type,
          range);
      const MirValueId after_surrogates = binary(
          HirOperation::Greater,
          converted,
          surrogate_end,
          semantic_.types.builtins().bool_type,
          range);
      const MirValueId outside_surrogates = binary(
          HirOperation::BitwiseOr,
          before_surrogates,
          after_surrogates,
          semantic_.types.builtins().bool_type,
          range);
      const MirValueId scalar_range = binary(
          HirOperation::BitwiseAnd,
          nonnegative,
          below_maximum,
          semantic_.types.builtins().bool_type,
          range);
      const MirValueId valid = binary(
          HirOperation::BitwiseAnd,
          scalar_range,
          outside_surrogates,
          semantic_.types.builtins().bool_type,
          range);
      trap_unless(valid, range);
    } else if (semantic_.types.type(target).kind == TypeKind::Enum) {
      // Validation belongs to the backing-value -> enum conversion. A cast
      // from an already valid enum into a distinct wrapper has enum-shaped
      // runtime storage, but it does not create a new enum value and the
      // wrapper owns no separate alternative table.
      MirValueId valid;
      for (const AggregateMember &member : semantic_.aggregate_members) {
        const Symbol &owner = semantic_.symbols.symbol(member.owner);
        if (owner.type != target) continue;
        const EnumMemberValue *enum_value = nullptr;
        for (const EnumMemberValue &candidate :
             semantic_.enum_member_values) {
          if (candidate.member == member.member) {
            enum_value = &candidate;
            break;
          }
        }
        if (enum_value == nullptr) continue;
        const MirValueId candidate = constant(
            ConstantValue::make_integer(enum_value->value),
            target,
            range);
        const MirValueId matches = binary(
            HirOperation::Equal,
            converted,
            candidate,
            semantic_.types.builtins().bool_type,
            range);
        if (!valid.is_valid()) {
          valid = matches;
        } else {
          valid = binary(
              HirOperation::BitwiseOr,
              valid,
              matches,
              semantic_.types.builtins().bool_type,
              range);
        }
      }
      if (!valid.is_valid()) {
        valid = constant(
            ConstantValue::make_bool(false),
            semantic_.types.builtins().bool_type,
            range);
      }
      trap_unless(valid, range);
    }
    return converted;
  }

  [[nodiscard]] MirValueId checked_binary(
      HirOperation operation,
      MirValueId left,
      MirValueId right,
      TypeId result_type,
      SourceRange range) {
    const Type left_type = runtime_scalar_type(procedure_.value(left).type);
    const bool integer = left_type.kind == TypeKind::SignedInteger ||
        left_type.kind == TypeKind::UnsignedInteger ||
        left_type.kind == TypeKind::Rune || left_type.kind == TypeKind::Enum ||
        left_type.kind == TypeKind::BooleanStorage ||
        left_type.kind == TypeKind::EndianScalar;
    if (integer &&
        (operation == HirOperation::Divide || operation == HirOperation::Remainder)) {
      const TypeId operand_type = procedure_.value(right).type;
      const MirValueId zero_value = constant(
          ConstantValue::make_integer(0), operand_type, range);
      MirValueId safe = binary(
          HirOperation::NotEqual,
          right,
          zero_value,
          semantic_.types.builtins().bool_type,
          range);
      if (signed_integer_type(procedure_.value(left).type)) {
        const std::uint32_t bits = integer_bit_width(procedure_.value(left).type);
        if (bits != 0) {
          const BigInteger minimum = BigInteger::from_u64(1)
              .shifted_left(static_cast<std::size_t>(bits - 1U))
              .negated();
          const MirValueId minimum_value = constant(
              ConstantValue::make_integer(minimum),
              procedure_.value(left).type,
              range);
          const MirValueId negative_one = constant(
              ConstantValue::make_integer(-1), operand_type, range);
          const MirValueId left_is_minimum = binary(
              HirOperation::Equal,
              left,
              minimum_value,
              semantic_.types.builtins().bool_type,
              range);
          const MirValueId right_is_negative_one = binary(
              HirOperation::Equal,
              right,
              negative_one,
              semantic_.types.builtins().bool_type,
              range);
          const MirValueId overflow = binary(
              HirOperation::BitwiseAnd,
              left_is_minimum,
              right_is_negative_one,
              semantic_.types.builtins().bool_type,
              range);
          if (operation == HirOperation::Divide) {
            const MirValueId false_value = constant(
                ConstantValue::make_bool(false),
                semantic_.types.builtins().bool_type,
                range);
            const MirValueId no_overflow = binary(
                HirOperation::Equal,
                overflow,
                false_value,
                semantic_.types.builtins().bool_type,
                range);
            safe = binary(
                HirOperation::BitwiseAnd,
                safe,
                no_overflow,
                semantic_.types.builtins().bool_type,
                range);
          } else {
            // LLVM defines signed minimum `srem -1` as poison even though
            // Draft defines the mathematical remainder as zero. Feed `srem`
            // a divisor of one on precisely that edge; every other operation
            // retains the already-evaluated source divisor.
            const MirValueId one_value = constant(
                ConstantValue::make_integer(1), operand_type, range);
            right = select_value(
                overflow, one_value, right, operand_type, range);
          }
        }
      }
      trap_unless(safe, range);
    } else if (integer &&
               (operation == HirOperation::ShiftLeft ||
                operation == HirOperation::ShiftRight)) {
      const TypeId count_type = procedure_.value(right).type;

      // Draft permits the shift count to have any concrete integer type.  Do
      // not encode the operand width in that type: a u128 operand has width
      // 128, which an i8 count would reinterpret as -128 before comparison.
      //
      // u128 is a simple common validation domain for every Draft 1 integer
      // count and every scalar operand width.  Extending a negative signed
      // count to u128 preserves its two's-complement sign, producing a large
      // unsigned value that fails the single `< width` test.  Once that test
      // succeeds, the original count can be narrowed to the operand type
      // without losing a bit used by the shift itself.
      const TypeId validation_type =
          semantic_.types.find_builtin("u128").value();
      MirValueId validated_count = right;
      if (count_type != validation_type) {
        validated_count = convert(right, validation_type, range);
      }
      const MirValueId width = constant(
          ConstantValue::make_integer(
              static_cast<std::int64_t>(integer_bit_width(procedure_.value(left).type))),
          validation_type,
          range);
      const MirValueId safe = binary(
          HirOperation::Less,
          validated_count,
          width,
          semantic_.types.builtins().bool_type,
          range);
      trap_unless(safe, range);

      const TypeId operand_type = procedure_.value(left).type;
      if (operand_type == validation_type) {
        right = validated_count;
      } else if (count_type != operand_type) {
        right = convert(right, operand_type, range);
      }
    }
    return binary(operation, left, right, result_type, range);
  }

  [[nodiscard]] std::uint64_t member_offset(
      const HirExpression &expression) const {
    if (expression.symbol.is_valid()) {
      for (const AggregateMember &member : semantic_.aggregate_members) {
        if (member.member == expression.symbol) return member.offset;
      }
    }
    if (!expression.operands.empty()) {
      const Type base_type = runtime_scalar_type(
          hir_.expression(expression.operands.front()).type);
      const std::optional<std::uint64_t> index = expression.constant.integer.to_u64();
      if (index.has_value() && *index < base_type.member_offsets.size()) {
        return base_type.member_offsets[static_cast<std::size_t>(*index)];
      }
    }
    return 0;
  }

  [[nodiscard]] MirValueId materialize(
      MirValueId value, TypeId type, SourceRange range) {
    const MirLocalId local_id = add_temporary(type, range);
    const MirValueId address = local_address(local_id, range);
    store(address, value, range);
    return address;
  }

  [[nodiscard]] MirValueId lower_address(HirExpressionId expression_id) {
    const HirExpression &expression = hir_.expression(expression_id);
    switch (expression.kind) {
    case HirExpressionKind::Context:
      if (context_.is_valid()) return context_;
      diagnostics_.error(
          expression.range, "a C-convention procedure has no active Draft context");
      return {};
    case HirExpressionKind::Symbol: {
      const Symbol &symbol = semantic_.symbols.symbol(expression.symbol);
      if (symbol.kind == SymbolKind::Local || symbol.kind == SymbolKind::Parameter) {
        return local_address(ensure_local(expression.symbol), expression.range);
      }
      if (symbol.kind == SymbolKind::Variable) {
        return global_address(expression.symbol, expression.range);
      }
      diagnostics_.error(expression.range, "MIR address requested for a non-storage symbol");
      return {};
    }
    case HirExpressionKind::Dereference:
      if (!expression.operands.empty()) {
        return lower_expression(expression.operands.front());
      }
      break;
    case HirExpressionKind::Member: {
      if (expression.operands.empty()) break;
      const HirExpression &base = hir_.expression(expression.operands.front());
      MirValueId base_address;
      if (base.addressable) {
        base_address = lower_address(expression.operands.front());
      } else {
        base_address = materialize(
            lower_expression(expression.operands.front()),
            base.type,
            base.range);
      }
      MirInstruction instruction;
      instruction.kind = MirInstructionKind::MemberAddress;
      instruction.range = expression.range;
      instruction.type = semantic_.types.pointer(expression.type);
      instruction.symbol = expression.symbol;
      instruction.offset = member_offset(expression);
      instruction.operands.push_back(base_address);
      return emit_value(std::move(instruction));
    }
    case HirExpressionKind::Index:
      return lower_index_address(expression);
    case HirExpressionKind::Denial:
      if (!expression.operands.empty()) {
        return lower_address(expression.operands.front());
      }
      break;
    default:
      break;
    }
    diagnostics_.error(expression.range, "expression has no lowerable MIR address");
    return {};
  }

  [[nodiscard]] MirValueId length(
      MirValueId base, TypeId base_type, SourceRange range) {
    const Type type = runtime_scalar_type(base_type);
    if (type.kind == TypeKind::Array) {
      return usize_constant(type.element_count, range);
    }
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Length;
    instruction.range = range;
    instruction.type = semantic_.types.builtins().usize_type;
    instruction.operands.push_back(base);
    return emit_value(std::move(instruction));
  }

  void bounds_check(
      MirValueId index,
      MirValueId length_value,
      SourceRange range) {
    if (unchecked_depth_ != 0) return;
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::BoundsCheck;
    instruction.range = range;
    instruction.type = semantic_.types.builtins().void_type;
    instruction.operands = {index, length_value};
    emit_void(std::move(instruction));
  }

  [[nodiscard]] MirValueId lower_index_address(
      const HirExpression &expression) {
    if (expression.operands.size() != 2) {
      diagnostics_.error(expression.range, "indexed HIR expression has wrong arity");
      return {};
    }
    const HirExpressionId base_id = expression.operands[0];
    const HirExpression &base_expression = hir_.expression(base_id);
    const Type base_type = runtime_scalar_type(base_expression.type);
    MirValueId base;
    if (base_type.kind == TypeKind::Array) {
      base = base_expression.addressable
          ? lower_address(base_id)
          : materialize(
                lower_expression(base_id), base_expression.type, base_expression.range);
    } else {
      base = lower_expression(base_id);
    }
    // Draft requires the base expression to be evaluated before the index.
    // Keep these as separate statements: C++ function-argument evaluation order
    // is not the language contract of the compiler being implemented.
    const MirValueId index = lower_expression(expression.operands[1]);
    if (base_type.kind == TypeKind::Array && !expression.bounds_proven) {
      bounds_check(
          index,
          usize_constant(base_type.element_count, expression.range),
          expression.range);
    } else if ((base_type.kind == TypeKind::Slice ||
                base_type.kind == TypeKind::String) &&
               !expression.bounds_proven) {
      bounds_check(
          index, length(base, base_expression.type, expression.range), expression.range);
    }
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::IndexAddress;
    instruction.range = expression.range;
    instruction.type = semantic_.types.pointer(expression.type);
    instruction.operands = {base, index};
    instruction.offset = semantic_.types.type(expression.type).layout.size;
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] MirValueId lower_short_circuit(
      const HirExpression &expression) {
    const MirValueId left = lower_expression(expression.operands[0]);
    const MirLocalId result_local = add_temporary(expression.type, expression.range);
    const MirValueId result_address = local_address(result_local, expression.range);
    store(result_address, left, expression.range);

    const MirBlockId right_block = procedure_.add_block(expression.range);
    const MirBlockId join_block = procedure_.add_block(expression.range);
    MirValueId condition = left;
    if (procedure_.value(left).type != semantic_.types.builtins().bool_type) {
      // Source-level distinct bool remains the expression's result type. MIR
      // branches, however, deliberately accept only canonical bool. Expose the
      // identical underlying computation representation at this internal
      // boundary without changing the stored/result value.
      condition = convert(
          left, semantic_.types.builtins().bool_type, expression.range);
    }
    if (expression.operation == HirOperation::LogicalAnd) {
      conditional_branch(condition, right_block, join_block, expression.range);
    } else {
      conditional_branch(condition, join_block, right_block, expression.range);
    }
    current_ = right_block;
    const MirValueId right = lower_expression(expression.operands[1]);
    store(result_address, right, expression.range);
    branch(join_block, expression.range);
    current_ = join_block;
    return load(result_address, expression.type, expression.range);
  }

  [[nodiscard]] MirValueId lower_conditional_expression(
      const HirExpression &expression) {
    const MirValueId condition = lower_expression(expression.operands[0]);
    const MirLocalId result_local = add_temporary(expression.type, expression.range);
    const MirValueId result_address = local_address(result_local, expression.range);
    const MirBlockId true_block = procedure_.add_block(expression.range);
    const MirBlockId false_block = procedure_.add_block(expression.range);
    const MirBlockId join_block = procedure_.add_block(expression.range);
    conditional_branch(condition, true_block, false_block, expression.range);

    current_ = true_block;
    store(
        result_address,
        lower_expression(expression.operands[1]),
        expression.range);
    branch(join_block, expression.range);
    current_ = false_block;
    store(
        result_address,
        lower_expression(expression.operands[2]),
        expression.range);
    branch(join_block, expression.range);
    current_ = join_block;
    return load(result_address, expression.type, expression.range);
  }

  [[nodiscard]] CapturedCall capture_call(const HirExpression &expression) {
    CapturedCall captured;
    captured.range = expression.range;
    captured.result_type = expression.type;
    if (expression.operands.empty()) {
      diagnostics_.error(expression.range, "call HIR has no callee");
      return captured;
    }
    const HirExpression &callee_expression =
        hir_.expression(expression.operands.front());
    captured.operands.push_back(lower_expression(expression.operands.front()));
    const Type signature = runtime_scalar_type(callee_expression.type);
    if (!signature.c_calling_convention) {
      captured.passes_context = true;
      if (!context_.is_valid()) {
        diagnostics_.error(
            expression.range,
            "a C-convention procedure cannot call an ordinary Draft procedure "
            "without establishing context");
      } else {
        captured.operands.push_back(context_);
      }
    }
    for (std::size_t index = 1; index < expression.operands.size(); ++index) {
      captured.operands.push_back(lower_expression(expression.operands[index]));
    }
    return captured;
  }

  [[nodiscard]] MirValueId emit_call(const CapturedCall &captured) {
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Call;
    instruction.range = captured.range;
    instruction.type = captured.result_type;
    instruction.operands = captured.operands;
    instruction.passes_context = captured.passes_context;
    if (captured.result_type == semantic_.types.builtins().void_type) {
      emit_void(std::move(instruction));
      return {};
    }
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] std::optional<std::uint64_t> union_discriminator(
      SymbolId alternative) const {
    if (!alternative.is_valid()) return std::nullopt;
    const Symbol &member_symbol = semantic_.symbols.symbol(alternative);
    std::uint64_t discriminator = 0;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      const Symbol &candidate = semantic_.symbols.symbol(member.member);
      if (candidate.scope != member_symbol.scope) continue;
      if (member.member == alternative) return discriminator;
      ++discriminator;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::uint64_t> aggregate_member_offset(
      SymbolId member_id) const {
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.member == member_id) return member.offset;
    }
    return std::nullopt;
  }

  [[nodiscard]] MirValueId extract_member(
      MirValueId aggregate,
      TypeId member_type,
      std::uint64_t offset,
      SourceRange range) {
    MirInstruction extract;
    extract.kind = MirInstructionKind::ExtractMember;
    extract.range = range;
    extract.type = member_type;
    extract.offset = offset;
    extract.operands.push_back(aggregate);
    return emit_value(std::move(extract));
  }

  [[nodiscard]] std::uint64_t aggregate_operand_offset(
      const HirExpression &expression,
      std::size_t index) const {
    const Type type = runtime_scalar_type(expression.type);
    if (index < expression.operand_members.size() &&
        expression.operand_members[index].is_valid()) {
      for (const AggregateMember &member : semantic_.aggregate_members) {
        if (member.member == expression.operand_members[index]) return member.offset;
      }
    }
    if (type.kind == TypeKind::Array) {
      return type.element_count == 0
          ? 0
          : semantic_.types.type(type.element).layout.size * index;
    }
    if (index < type.member_offsets.size()) return type.member_offsets[index];
    return 0;
  }

  [[nodiscard]] MirValueId lower_slice(const HirExpression &expression) {
    const HirExpressionId base_id = expression.operands.front();
    const HirExpression &base_expression = hir_.expression(base_id);
    const Type base_type = runtime_scalar_type(base_expression.type);
    MirValueId base;
    if (base_type.kind == TypeKind::Array) {
      base = base_expression.addressable
          ? lower_address(base_id)
          : materialize(
                lower_expression(base_id), base_expression.type, base_expression.range);
    } else {
      base = lower_expression(base_id);
    }
    MirValueId base_length;
    if (base_type.kind != TypeKind::MultiPointer) {
      base_length = length(base, base_expression.type, expression.range);
    }
    std::size_t bound_index = 1;
    const MirValueId low = expression.slice_has_low
        ? lower_expression(expression.operands[bound_index++])
        : usize_constant(0, expression.range);
    MirValueId high;
    if (expression.slice_has_high) {
      high = lower_expression(expression.operands[bound_index]);
    } else if (base_type.kind == TypeKind::MultiPointer) {
      // Body checking already rejects this source form. Keep recovery explicit
      // so malformed HIR cannot make Length extract field 1 from a raw pointer.
      diagnostics_.error(
          expression.range,
          "multi-pointer slicing reached MIR without an explicit length");
      high = low;
    } else {
      high = base_length;
    }
    if (base_type.kind != TypeKind::MultiPointer && !expression.bounds_proven &&
        unchecked_depth_ == 0) {
      MirInstruction check;
      check.kind = MirInstructionKind::SliceBoundsCheck;
      check.range = expression.range;
      check.type = semantic_.types.builtins().void_type;
      check.operands = {low, high, base_length};
      emit_void(std::move(check));
    }
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Slice;
    instruction.range = expression.range;
    instruction.type = expression.type;
    instruction.operands = {base, low, high};
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] MirValueId lower_assembly(
      SyntaxReference syntax,
      TypeId result_type,
      const std::vector<HirExpressionId> &inputs,
      SourceRange range) {
    if (assembly_ == nullptr) {
      diagnostics_.error(
          range,
          "parsed assembly requires the AArch64 validation phase before MIR lowering");
      return {};
    }
    const AssemblyRegion *region = assembly_->find(syntax);
    if (region == nullptr) {
      diagnostics_.error(range, "validated assembly region is missing");
      return {};
    }
    if (region->input_count != inputs.size() || region->result_type != result_type) {
      diagnostics_.error(range, "validated assembly metadata differs from typed HIR");
      return {};
    }
    MirInstruction instruction;
    instruction.kind = MirInstructionKind::Assembly;
    instruction.range = range;
    instruction.type = result_type;
    instruction.assembly_text = region->instruction_text;
    instruction.assembly_constraints = region->llvm_constraints;
    for (HirExpressionId input : inputs) {
      instruction.operands.push_back(lower_expression(input));
    }
    if (result_type == semantic_.types.builtins().void_type) {
      emit_void(std::move(instruction));
      return {};
    }
    return emit_value(std::move(instruction));
  }

  [[nodiscard]] MirValueId lower_expression(HirExpressionId expression_id) {
    const HirExpression &expression = hir_.expression(expression_id);
    switch (expression.kind) {
    case HirExpressionKind::Discard:
      // Discards are consumed by assignment lowering and have no MIR value.
      return {};
    case HirExpressionKind::Constant:
      return constant(
          expression.constant,
          expression.type,
          expression.range,
          expression.symbol);
    case HirExpressionKind::Context:
      if (!context_.is_valid()) {
        diagnostics_.error(
            expression.range, "a C-convention procedure has no active Draft context");
        return {};
      }
      return load(context_, expression.type, expression.range);
    case HirExpressionKind::Symbol: {
      const Symbol &symbol = semantic_.symbols.symbol(expression.symbol);
      if (symbol.kind == SymbolKind::Procedure) {
        MirInstruction instruction;
        instruction.kind = MirInstructionKind::ProcedureReference;
        instruction.range = expression.range;
        instruction.type = expression.type;
        instruction.symbol = expression.symbol;
        return emit_value(std::move(instruction));
      }
      const MirValueId address = lower_address(expression_id);
      return load(address, expression.type, expression.range);
    }
    case HirExpressionKind::Address:
      return expression.operands.empty() ? MirValueId{}
                                         : lower_address(expression.operands.front());
    case HirExpressionKind::Dereference:
    case HirExpressionKind::Member:
    case HirExpressionKind::Index:
      return load(lower_address(expression_id), expression.type, expression.range);
    case HirExpressionKind::Unary: {
      MirInstruction instruction;
      instruction.kind = MirInstructionKind::Unary;
      instruction.range = expression.range;
      instruction.type = expression.type;
      instruction.operation = expression.operation;
      instruction.operands.push_back(lower_expression(expression.operands.front()));
      return emit_value(std::move(instruction));
    }
    case HirExpressionKind::Binary:
      if (expression.operation == HirOperation::LogicalAnd ||
          expression.operation == HirOperation::LogicalOr) {
        return lower_short_circuit(expression);
      }
      {
        const MirValueId left = lower_expression(expression.operands[0]);
        const MirValueId right = lower_expression(expression.operands[1]);
        return checked_binary(
            expression.operation,
            left,
            right,
            expression.type,
            expression.range);
      }
    case HirExpressionKind::Call:
      return emit_call(capture_call(expression));
    case HirExpressionKind::Intrinsic: {
      if (expression.constant.text == "call_with_context") {
        if (expression.operands.size() < 2) {
          diagnostics_.error(
              expression.range,
              "call_with_context HIR is missing its Context or callback");
          return {};
        }
        MirInstruction instruction;
        instruction.kind = MirInstructionKind::Call;
        instruction.range = expression.range;
        instruction.type = expression.type;
        // HIR keeps source order (Context, callback, arguments). MIR Call keeps
        // physical order (callback, hidden Context, arguments), exactly like an
        // ordinary call whose implicit Context has been made explicit.
        instruction.operands.push_back(
            lower_expression(expression.operands[1]));
        instruction.operands.push_back(
            lower_expression(expression.operands[0]));
        for (std::size_t index = 2; index < expression.operands.size(); ++index) {
          instruction.operands.push_back(
              lower_expression(expression.operands[index]));
        }
        instruction.passes_context = true;
        instruction.establishes_thread_context = true;
        if (expression.type == semantic_.types.builtins().void_type) {
          emit_void(std::move(instruction));
          return {};
        }
        return emit_value(std::move(instruction));
      }
      if (expression.constant.text.rfind("atomic.", 0) == 0) {
        const std::string_view operation(expression.constant.text);
        MirInstruction instruction;
        instruction.range = expression.range;
        instruction.type = expression.type;
        instruction.atomic_order = expression.atomic_order;
        instruction.atomic_failure_order = expression.atomic_failure_order;
        if (operation == "atomic.load") {
          instruction.kind = MirInstructionKind::AtomicLoad;
        } else if (operation == "atomic.store") {
          instruction.kind = MirInstructionKind::AtomicStore;
        } else if (operation == "atomic.exchange") {
          instruction.kind = MirInstructionKind::AtomicExchange;
        } else if (operation == "atomic.compare_exchange") {
          instruction.kind = MirInstructionKind::AtomicCompareExchange;
        } else if (operation == "atomic.fence") {
          instruction.kind = MirInstructionKind::AtomicFence;
        } else {
          instruction.kind = MirInstructionKind::AtomicReadModifyWrite;
          if (operation == "atomic.fetch_add") {
            instruction.operation = HirOperation::Add;
          } else if (operation == "atomic.fetch_sub") {
            instruction.operation = HirOperation::Subtract;
          } else if (operation == "atomic.fetch_and") {
            instruction.operation = HirOperation::BitwiseAnd;
          } else if (operation == "atomic.fetch_or") {
            instruction.operation = HirOperation::BitwiseOr;
          } else if (operation == "atomic.fetch_xor") {
            instruction.operation = HirOperation::BitwiseXor;
          } else {
            diagnostics_.error(
                expression.range, "unknown atomic intrinsic reached MIR lowering");
            return {};
          }
        }
        for (HirExpressionId operand : expression.operands) {
          instruction.operands.push_back(lower_expression(operand));
        }
        if (expression.type == semantic_.types.builtins().void_type) {
          emit_void(std::move(instruction));
          return {};
        }
        return emit_value(std::move(instruction));
      }
      if (expression.constant.text == "len") {
        const HirExpression &argument = hir_.expression(expression.operands.front());
        return length(
            lower_expression(expression.operands.front()),
            argument.type,
            expression.range);
      }
      if (expression.constant.text == "raw_data") {
        // Semantic checking has established the exact string-to-[^]u8
        // contract. MIR retains that operation instead of recasting the
        // string as an ordinary aggregate and coupling every backend to the
        // current physical member numbering.
        MirInstruction instruction;
        instruction.kind = MirInstructionKind::RawData;
        instruction.range = expression.range;
        instruction.type = expression.type;
        instruction.operands.push_back(
            lower_expression(expression.operands.front()));
        return emit_value(std::move(instruction));
      }
      if (expression.constant.text == "assert") {
        // Do not lower either operand when assertions are disabled. In
        // particular, lowering the condition and then discarding the Assert
        // instruction would retain calls, traps, or stores used only by the
        // assertion and violate the language's no-evaluation rule.
        if (runtime_assertions_ == RuntimeAssertionMode::Off) return {};
        MirInstruction instruction;
        instruction.kind = MirInstructionKind::Assert;
        instruction.range = expression.range;
        instruction.type = semantic_.types.builtins().void_type;
        for (HirExpressionId operand : expression.operands) {
          instruction.operands.push_back(lower_expression(operand));
        }
        emit_void(std::move(instruction));
        return {};
      }
      if (expression.constant.text == "static_assert") {
        // The body checker has already evaluated and diagnosed this compile-time
        // invariant. It has no runtime value, effect, or control-flow edge.
        return {};
      }
      if (expression.constant.text == "ptr_offset" ||
          expression.constant.text == "ptr_sub") {
        MirInstruction instruction;
        instruction.kind = expression.constant.text == "ptr_offset"
            ? MirInstructionKind::PointerOffset
            : MirInstructionKind::PointerSubtract;
        instruction.range = expression.range;
        instruction.type = expression.type;
        for (HirExpressionId operand : expression.operands) {
          instruction.operands.push_back(lower_expression(operand));
        }
        const TypeId pointer_type =
            hir_.expression(expression.operands.front()).type;
        const Type pointer = runtime_scalar_type(pointer_type);
        instruction.offset = semantic_.types.type(pointer.element).layout.size;
        return emit_value(std::move(instruction));
      }
      if (expression.constant.text == "cast") {
        return checked_conversion(
            lower_expression(expression.operands.front()),
            expression.type,
            expression.range);
      }
      diagnostics_.error(expression.range, "unknown intrinsic reached MIR lowering");
      return {};
    }
    case HirExpressionKind::Tuple:
    case HirExpressionKind::Composite: {
      MirInstruction instruction;
      instruction.kind = MirInstructionKind::Aggregate;
      instruction.range = expression.range;
      instruction.type = expression.type;
      instruction.symbol = expression.symbol;
      const Type aggregate_type = runtime_scalar_type(expression.type);
      if (aggregate_type.kind == TypeKind::TaggedUnion) {
        const std::optional<std::uint64_t> discriminator =
            union_discriminator(expression.symbol);
        if (!discriminator.has_value()) {
          diagnostics_.error(
              expression.range,
              "tagged-union construction has no alternative discriminator");
          return {};
        }
        instruction.operands.push_back(constant(
            ConstantValue::make_integer(
                BigInteger::from_u64(*discriminator)),
            aggregate_type.element,
            expression.range));
        instruction.offsets.push_back(0);
      }
      for (std::size_t index = 0; index < expression.operands.size(); ++index) {
        instruction.operands.push_back(lower_expression(expression.operands[index]));
        if (aggregate_type.kind == TypeKind::TaggedUnion) {
          instruction.offsets.push_back(
              aggregate_member_offset(expression.symbol).value_or(0));
        } else {
          instruction.offsets.push_back(aggregate_operand_offset(expression, index));
        }
      }
      return emit_value(std::move(instruction));
    }
    case HirExpressionKind::Slice:
      return lower_slice(expression);
    case HirExpressionKind::Conditional:
      return lower_conditional_expression(expression);
    case HirExpressionKind::Denial:
      return expression.operands.empty() ? MirValueId{}
                                         : lower_expression(expression.operands.front());
    case HirExpressionKind::Synthesis:
      diagnostics_.error(
          expression.range,
          "unresolved synthesis expression prevents native lowering");
      return {};
    case HirExpressionKind::Assembly:
      return lower_assembly(
          expression.syntax,
          expression.type,
          expression.operands,
          expression.range);
    case HirExpressionKind::Invalid:
      diagnostics_.error(expression.range, "invalid HIR expression reached MIR lowering");
      return {};
    }
    diagnostics_.error(expression.range, "unhandled HIR expression reached MIR lowering");
    return {};
  }

  void emit_deferred_call(const CapturedCall &call) {
    (void)emit_call(call);
  }

  void emit_defers_to(std::size_t retained_depth) {
    for (std::size_t scope_index = defer_scopes_.size();
         scope_index > retained_depth;
         --scope_index) {
      const DeferScope &scope = defer_scopes_[scope_index - 1];
      for (std::size_t call_index = scope.calls.size(); call_index > 0; --call_index) {
        emit_deferred_call(scope.calls[call_index - 1]);
      }
    }
  }

  [[nodiscard]] bool rooted_in_context(HirExpressionId id) const {
    if (!id.is_valid()) return false;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Context) return true;
    if ((expression.kind == HirExpressionKind::Member ||
         expression.kind == HirExpressionKind::Denial) &&
        !expression.operands.empty()) {
      return rooted_in_context(expression.operands.front());
    }
    return false;
  }

  [[nodiscard]] bool takes_context_address(HirExpressionId id) const {
    if (!id.is_valid()) return false;
    const HirExpression &expression = hir_.expression(id);
    if (expression.kind == HirExpressionKind::Address &&
        !expression.operands.empty() &&
        rooted_in_context(expression.operands.front())) {
      return true;
    }
    for (HirExpressionId operand : expression.operands) {
      if (takes_context_address(operand)) return true;
    }
    return false;
  }

  // A lexical context override is created before executing any statement in
  // the scope.  Direct field stores obviously require one; taking a field's
  // address also requires one because a later indirect call or dereference may
  // write through that pointer. Nested source blocks make their own decision.
  [[nodiscard]] bool statement_requires_context_copy(
      const HirStatement &statement) const {
    if (statement.kind == HirStatementKind::Assignment) {
      const std::size_t left_count = std::min(
          statement.assignment_target_count, statement.expressions.size());
      for (std::size_t index = 0; index < left_count; ++index) {
        if (rooted_in_context(statement.expressions[index])) return true;
      }
    }
    for (HirExpressionId expression : statement.expressions) {
      if (takes_context_address(expression)) return true;
    }
    for (HirStatementId header : statement.header_statements) {
      if (statement_requires_context_copy(hir_.statement(header))) return true;
    }
    return false;
  }

  [[nodiscard]] bool block_requires_context_copy(const HirBlock &block) const {
    for (HirStatementId statement : block.statements) {
      if (statement_requires_context_copy(hir_.statement(statement))) return true;
    }
    return false;
  }

  void lower_block(HirBlockId block_id) {
    const HirBlock &block = hir_.block(block_id);
    const MirValueId surrounding_context = context_;
    if (context_.is_valid() &&
        semantic_.runtime_context_type.is_valid() &&
        block_requires_context_copy(block)) {
      const MirLocalId local = add_temporary(
          semantic_.runtime_context_type, block.range);
      const MirValueId address = local_address(local, block.range);
      store(
          address,
          load(context_, semantic_.runtime_context_type, block.range),
          block.range);
      context_ = address;
    }
    defer_scopes_.push_back({});
    for (HirStatementId statement : block.statements) {
      if (terminated()) break;
      lower_statement(statement);
    }
    if (!terminated()) {
      emit_defers_to(defer_scopes_.size() - 1);
    }
    defer_scopes_.pop_back();
    context_ = surrounding_context;
  }

  void lower_local_declaration(const HirStatement &statement) {
    std::vector<MirLocalId> locals;
    for (SymbolId binding : statement.bindings) {
      locals.push_back(ensure_local(binding));
    }
    if (statement.local_is_uninitialized) return;
    MirValueId initializer;
    TypeId initializer_type;
    if (!statement.expressions.empty()) {
      const HirExpression &source = hir_.expression(statement.expressions.front());
      initializer_type = source.type;
      initializer = lower_expression(statement.expressions.front());
    } else if (statement.local_destructures_tuple) {
      // The zero value of a tuple is the product of its member zero values. We
      // can initialize the selected locals directly without constructing a
      // temporary aggregate, and discarded members require no storage at all.
      for (MirLocalId local_id : locals) {
        store(
            local_address(local_id, statement.range),
            zero(procedure_.local(local_id).type, statement.range),
            statement.range);
      }
      return;
    }

    if (statement.local_destructures_tuple && initializer.is_valid()) {
      const Type tuple = semantic_.types.type(initializer_type);
      if (tuple.kind != TypeKind::Tuple ||
          statement.binding_member_indices.size() != locals.size()) {
        diagnostics_.error(statement.range, "tuple destructuring HIR is inconsistent");
        return;
      }
      for (std::size_t index = 0; index < locals.size(); ++index) {
        const std::size_t member_index = statement.binding_member_indices[index];
        if (member_index >= tuple.members.size() ||
            member_index >= tuple.member_offsets.size()) {
          diagnostics_.error(statement.range, "tuple binding index is out of range");
          continue;
        }
        MirInstruction extract;
        extract.kind = MirInstructionKind::ExtractMember;
        extract.range = statement.range;
        extract.type = tuple.members[member_index];
        extract.offset = tuple.member_offsets[member_index];
        extract.operands.push_back(initializer);
        const MirValueId member = emit_value(std::move(extract));
        store(local_address(locals[index], statement.range), member, statement.range);
      }
      return;
    }

    if (initializer.is_valid()) {
      for (MirLocalId local_id : locals) {
        store(local_address(local_id, statement.range), initializer, statement.range);
      }
      return;
    }
    for (MirLocalId local_id : locals) {
      store(
          local_address(local_id, statement.range),
          zero(procedure_.local(local_id).type, statement.range),
          statement.range);
    }
  }

  void lower_assignment(const HirStatement &statement) {
    const std::size_t left_count = std::min(
        statement.assignment_target_count, statement.expressions.size());
    std::vector<MirValueId> addresses;
    std::vector<MirValueId> left_values;
    std::vector<MirValueId> right_values;
    addresses.reserve(left_count);
    for (std::size_t index = 0; index < left_count; ++index) {
      const HirExpression &target = hir_.expression(statement.expressions[index]);
      addresses.push_back(target.kind == HirExpressionKind::Discard
          ? MirValueId{}
          : lower_address(statement.expressions[index]));
    }
    if (statement.assignment_destructures_tuple) {
      if (statement.expressions.size() <= left_count ||
          statement.assignment_member_indices.size() != left_count) {
        diagnostics_.error(
            statement.range, "tuple assignment HIR is inconsistent");
        return;
      }
      const MirValueId tuple_value =
          lower_expression(statement.expressions[left_count]);
      const Type tuple = semantic_.types.type(
          hir_.expression(statement.expressions[left_count]).type);
      if (tuple.kind != TypeKind::Tuple) {
        diagnostics_.error(
            statement.range, "tuple assignment HIR has a non-tuple value");
        return;
      }
      for (std::size_t index = 0; index < left_count; ++index) {
        const std::size_t member = statement.assignment_member_indices[index];
        if (member >= tuple.members.size() || member >= tuple.member_offsets.size()) {
          diagnostics_.error(
              statement.range, "tuple assignment member index is invalid");
          return;
        }
        MirInstruction extract;
        extract.kind = MirInstructionKind::ExtractMember;
        extract.range = statement.range;
        extract.type = tuple.members[member];
        extract.offset = tuple.member_offsets[member];
        extract.operands.push_back(tuple_value);
        store(
            addresses[index],
            emit_value(std::move(extract)),
            statement.range);
      }
      return;
    }
    if (statement.operation != HirOperation::Assign) {
      for (std::size_t index = 0; index < left_count; ++index) {
        const HirExpression &left = hir_.expression(statement.expressions[index]);
        if (left.kind == HirExpressionKind::Discard) continue;
        left_values.push_back(load(addresses[index], left.type, left.range));
      }
    }
    const std::size_t right_count = statement.expressions.size() - left_count;
    for (std::size_t index = 0; index < right_count; ++index) {
      right_values.push_back(
          lower_expression(statement.expressions[left_count + index]));
    }
    const std::size_t paired = std::min(left_count, right_values.size());
    std::size_t loaded_index = 0;
    for (std::size_t index = 0; index < paired; ++index) {
      const HirExpression &left = hir_.expression(statement.expressions[index]);
      if (left.kind == HirExpressionKind::Discard) continue;
      MirValueId value = right_values[index];
      if (statement.operation != HirOperation::Assign) {
        value = checked_binary(
            statement.operation,
            left_values[loaded_index++],
            value,
            left.type,
            statement.range);
      }
      store(addresses[index], value, statement.range);
    }
  }

  void lower_if(const HirStatement &statement) {
    if (statement.expressions.empty() || statement.blocks.empty()) {
      diagnostics_.error(statement.range, "if HIR is missing its condition or body");
      return;
    }
    const MirValueId condition = lower_expression(statement.expressions.front());
    const MirBlockId true_block = procedure_.add_block(statement.range);
    const MirBlockId join_block = procedure_.add_block(statement.range);
    const MirBlockId false_block = statement.blocks.size() > 1
        ? procedure_.add_block(statement.range)
        : join_block;
    conditional_branch(condition, true_block, false_block, statement.range);

    current_ = true_block;
    lower_block(statement.blocks[0]);
    branch(join_block, statement.range);
    if (statement.blocks.size() > 1) {
      current_ = false_block;
      lower_block(statement.blocks[1]);
      branch(join_block, statement.range);
    }
    current_ = join_block;
  }

  void lower_return(const HirStatement &statement) {
    MirValueId result;
    if (!statement.expressions.empty()) {
      result = lower_expression(statement.expressions.front());
    }
    emit_defers_to(0);
    MirTerminator terminator;
    terminator.kind = MirTerminatorKind::Return;
    terminator.range = statement.range;
    terminator.value = result;
    procedure_.set_terminator(current_, std::move(terminator));
  }

  void lower_control_exit(const HirStatement &statement, bool is_continue) {
    for (std::size_t index = controls_.size(); index > 0; --index) {
      const ControlTarget &target = controls_[index - 1];
      if (is_continue && !target.is_loop) continue;
      emit_defers_to(target.defer_depth);
      branch(
          is_continue ? target.continue_target : target.break_target,
          statement.range);
      return;
    }
    diagnostics_.error(statement.range, "control exit has no MIR target");
  }

  void lower_defer(const HirStatement &statement) {
    if (defer_scopes_.empty() || statement.expressions.empty()) {
      diagnostics_.error(statement.range, "defer HIR is missing a lexical scope or call");
      return;
    }
    const HirExpression &call = hir_.expression(statement.expressions.front());
    defer_scopes_.back().calls.push_back(capture_call(call));
  }

  void lower_clause_statements(
      const HirStatement &statement, std::size_t begin, std::size_t end) {
    for (std::size_t index = begin; index < end && !terminated(); ++index) {
      lower_statement(statement.header_statements[index]);
    }
  }

  void lower_regular_loop(const HirStatement &statement) {
    const std::size_t initialization_count =
        std::min(statement.for_initialization_count,
                 statement.header_statements.size());
    if (statement.for_kind == HirForKind::Clause) {
      lower_clause_statements(statement, 0, initialization_count);
    }
    const MirBlockId condition_block = procedure_.add_block(statement.range);
    const MirBlockId body_block = procedure_.add_block(statement.range);
    const MirBlockId post_block = procedure_.add_block(statement.range);
    const MirBlockId exit_block = procedure_.add_block(statement.range);
    branch(condition_block, statement.range);

    current_ = condition_block;
    if (statement.for_kind == HirForKind::Infinite || statement.expressions.empty()) {
      branch(body_block, statement.range);
    } else {
      conditional_branch(
          lower_expression(statement.expressions.front()),
          body_block,
          exit_block,
          statement.range);
    }

    controls_.push_back(
        {exit_block, post_block, defer_scopes_.size(), true});
    current_ = body_block;
    if (!statement.blocks.empty()) lower_block(statement.blocks.front());
    branch(post_block, statement.range);
    controls_.pop_back();

    current_ = post_block;
    if (statement.for_kind == HirForKind::Clause) {
      lower_clause_statements(
          statement, initialization_count, statement.header_statements.size());
    }
    branch(condition_block, statement.range);
    current_ = exit_block;
  }

  [[nodiscard]] MirValueId iteration_element(
      MirLocalId iterable_local,
      TypeId iterable_type,
      MirValueId index,
      SourceRange range) {
    const Type type = runtime_scalar_type(iterable_type);
    MirValueId base;
    if (type.kind == TypeKind::Array) {
      base = local_address(iterable_local, range);
    } else {
      base = load(local_address(iterable_local, range), iterable_type, range);
    }
    MirInstruction address;
    address.kind = MirInstructionKind::IndexAddress;
    address.range = range;
    address.type = semantic_.types.pointer(type.element);
    address.operands = {base, index};
    address.offset = semantic_.types.type(type.element).layout.size;
    return load(emit_value(std::move(address)), type.element, range);
  }

  void lower_iteration_loop(const HirStatement &statement) {
    if (statement.expressions.empty() || statement.blocks.empty()) {
      diagnostics_.error(statement.range, "iteration loop HIR is incomplete");
      return;
    }
    const HirExpression &iterable_expression =
        hir_.expression(statement.expressions.front());
    const Type iterable_type = runtime_scalar_type(iterable_expression.type);
    const MirLocalId iterable_local =
        add_temporary(iterable_expression.type, statement.range);
    const MirValueId iterable_value =
        lower_expression(statement.expressions.front());
    store(
        local_address(iterable_local, statement.range),
        iterable_value,
        statement.range);
    const MirLocalId index_local =
        add_temporary(semantic_.types.builtins().usize_type, statement.range);
    store(
        local_address(index_local, statement.range),
        usize_constant(0, statement.range),
        statement.range);

    const MirBlockId condition_block = procedure_.add_block(statement.range);
    const MirBlockId body_block = procedure_.add_block(statement.range);
    const MirBlockId post_block = procedure_.add_block(statement.range);
    const MirBlockId exit_block = procedure_.add_block(statement.range);
    branch(condition_block, statement.range);

    current_ = condition_block;
    const MirValueId index = load(
        local_address(index_local, statement.range),
        semantic_.types.builtins().usize_type,
        statement.range);
    MirValueId iterable_for_length = iterable_value;
    if (iterable_type.kind == TypeKind::Slice) {
      iterable_for_length = load(
          local_address(iterable_local, statement.range),
          iterable_expression.type,
          statement.range);
    }
    const MirValueId count = length(
        iterable_for_length, iterable_expression.type, statement.range);
    const MirValueId condition = binary(
        HirOperation::Less,
        index,
        count,
        semantic_.types.builtins().bool_type,
        statement.range);
    conditional_branch(condition, body_block, exit_block, statement.range);

    controls_.push_back(
        {exit_block, post_block, defer_scopes_.size(), true});
    current_ = body_block;
    if (!statement.bindings.empty()) {
      const MirLocalId value_local = ensure_local(statement.bindings[0]);
      store(
          local_address(value_local, statement.range),
          iteration_element(
              iterable_local, iterable_expression.type, index, statement.range),
          statement.range);
    }
    if (statement.bindings.size() > 1) {
      const MirLocalId source_index = ensure_local(statement.bindings[1]);
      store(local_address(source_index, statement.range), index, statement.range);
    }
    lower_block(statement.blocks.front());
    branch(post_block, statement.range);
    controls_.pop_back();

    current_ = post_block;
    const MirValueId old_index = load(
        local_address(index_local, statement.range),
        semantic_.types.builtins().usize_type,
        statement.range);
    const MirValueId next_index = binary(
        HirOperation::Add,
        old_index,
        usize_constant(1, statement.range),
        semantic_.types.builtins().usize_type,
        statement.range);
    store(local_address(index_local, statement.range), next_index, statement.range);
    branch(condition_block, statement.range);
    current_ = exit_block;
  }

  void lower_loop(const HirStatement &statement) {
    if (statement.for_kind == HirForKind::Iteration) {
      lower_iteration_loop(statement);
    } else {
      lower_regular_loop(statement);
    }
  }

  // LLVM's switch terminator accepts integer conditions only. Draft value
  // switches also permit the remaining scalar equality types, which are
  // lowered below as ordered compare-and-branch chains.
  [[nodiscard]] bool llvm_switch_subject(TypeId type_id) const {
    const TypeKind kind = runtime_scalar_type(type_id).kind;
    return kind == TypeKind::Bool || kind == TypeKind::BooleanStorage ||
        kind == TypeKind::SignedInteger || kind == TypeKind::UnsignedInteger ||
        kind == TypeKind::Rune || kind == TypeKind::EndianScalar ||
        kind == TypeKind::Enum;
  }

  void lower_switch(const HirStatement &statement) {
    if (statement.expressions.empty()) {
      diagnostics_.error(statement.range, "switch HIR has no subject");
      return;
    }
    const HirExpression &subject_expression =
        hir_.expression(statement.expressions.front());
    const Type subject_type = runtime_scalar_type(subject_expression.type);
    const MirValueId subject = lower_expression(statement.expressions.front());
    const MirValueId switch_subject = subject_type.kind == TypeKind::TaggedUnion
        ? extract_member(
              subject,
              subject_type.element,
              0,
              subject_expression.range)
        : subject;
    const MirBlockId join_block = procedure_.add_block(statement.range);
    std::vector<MirBlockId> case_blocks;
    case_blocks.reserve(statement.switch_cases.size());
    for (const HirSwitchCase &source_case : statement.switch_cases) {
      case_blocks.push_back(
          procedure_.add_block(hir_.block(source_case.body).range));
    }

    struct DispatchLabel {
      HirExpressionId expression;
      MirBlockId target;
    };
    std::vector<DispatchLabel> labels;
    MirBlockId default_target = join_block;
    bool has_default = false;
    for (std::size_t case_index = 0;
         case_index < statement.switch_cases.size();
         ++case_index) {
      const HirSwitchCase &source_case = statement.switch_cases[case_index];
      if (source_case.is_default) {
        has_default = true;
        default_target = case_blocks[case_index];
      }
      for (std::size_t label_index = 0;
           label_index < source_case.label_count;
           ++label_index) {
        const std::size_t expression_index =
            source_case.first_label + label_index;
        labels.push_back(
            {statement.expressions[expression_index], case_blocks[case_index]});
      }
    }
    MirBlockId impossible_default;
    if (statement.switch_is_exhaustive && !has_default) {
      // A complete enum/tagged-union switch has no semantic default value, but
      // LLVM's switch terminator requires a default edge. Route impossible
      // representations to an explicit unreachable block rather than making
      // the ordinary join appear reachable.
      impossible_default = procedure_.add_block(statement.range);
      default_target = impossible_default;
    }

    const TypeId dispatch_type = subject_type.kind == TypeKind::TaggedUnion
        ? subject_type.element
        : subject_expression.type;
    if (llvm_switch_subject(dispatch_type)) {
      MirTerminator terminator;
      terminator.kind = MirTerminatorKind::Switch;
      terminator.range = statement.range;
      terminator.value = switch_subject;
      for (const DispatchLabel &label : labels) {
        terminator.switch_arms.push_back(
            {lower_expression(label.expression), label.target});
      }
      terminator.targets.push_back(default_target);
      procedure_.set_terminator(current_, std::move(terminator));
    } else if (labels.empty()) {
      branch(default_target, statement.range);
    } else {
      // The subject was evaluated once above. Each label is a checked constant,
      // so this chain preserves source-order dispatch without introducing a
      // second evaluation or asking LLVM to accept a non-integer switch.
      for (std::size_t index = 0; index < labels.size(); ++index) {
        const DispatchLabel &label = labels[index];
        const HirExpression &label_expression = hir_.expression(label.expression);
        const MirValueId label_value = lower_expression(label.expression);
        const MirValueId matches = binary(
            HirOperation::Equal,
            switch_subject,
            label_value,
            semantic_.types.builtins().bool_type,
            label_expression.range);
        const MirBlockId miss = index + 1 == labels.size()
            ? default_target
            : procedure_.add_block(label_expression.range);
        conditional_branch(
            matches, label.target, miss, label_expression.range);
        if (index + 1 != labels.size()) current_ = miss;
      }
    }

    if (impossible_default.is_valid()) {
      current_ = impossible_default;
      MirTerminator unreachable;
      unreachable.kind = MirTerminatorKind::Unreachable;
      unreachable.range = statement.range;
      procedure_.set_terminator(current_, std::move(unreachable));
    }

    controls_.push_back({join_block, {}, defer_scopes_.size(), false});
    for (std::size_t case_index = 0; case_index < case_blocks.size(); ++case_index) {
      current_ = case_blocks[case_index];
      const HirSwitchCase &source_case = statement.switch_cases[case_index];
      if (source_case.payload_binding.is_valid() &&
          source_case.payload_alternative.is_valid()) {
        const Symbol &payload =
            semantic_.symbols.symbol(source_case.payload_alternative);
        const std::optional<std::uint64_t> offset =
            aggregate_member_offset(source_case.payload_alternative);
        if (!offset.has_value()) {
          diagnostics_.error(
              statement.range, "tagged-union payload has no physical offset");
        } else {
          const MirValueId value = extract_member(
              subject, payload.type, *offset, statement.range);
          const MirLocalId binding = ensure_local(source_case.payload_binding);
          store(local_address(binding, statement.range), value, statement.range);
        }
      }
      lower_block(source_case.body);
      branch(join_block, statement.range);
    }
    controls_.pop_back();
    current_ = join_block;
    if (statement.switch_definitely_returns) {
      MirTerminator unreachable;
      unreachable.kind = MirTerminatorKind::Unreachable;
      unreachable.range = statement.range;
      procedure_.set_terminator(current_, std::move(unreachable));
    }
  }

  void lower_statement(HirStatementId statement_id) {
    const HirStatement &statement = hir_.statement(statement_id);
    switch (statement.kind) {
    case HirStatementKind::LocalDeclaration:
      lower_local_declaration(statement);
      break;
    case HirStatementKind::CompileTimeDeclaration:
      // The value was substituted into HIR uses during body checking.
      break;
    case HirStatementKind::TypeDeclaration:
      // The semantic TypeId and any member scope were completed before MIR.
      break;
    case HirStatementKind::NestedProcedure:
      // Its HirProcedure row is lowered independently as a static function.
      // The lexical declaration itself has no runtime initialization.
      break;
    case HirStatementKind::Expression:
      for (HirExpressionId expression : statement.expressions) {
        (void)lower_expression(expression);
      }
      break;
    case HirStatementKind::Assignment:
      lower_assignment(statement);
      break;
    case HirStatementKind::Return:
      lower_return(statement);
      break;
    case HirStatementKind::Break:
      lower_control_exit(statement, false);
      break;
    case HirStatementKind::Continue:
      lower_control_exit(statement, true);
      break;
    case HirStatementKind::Defer:
      lower_defer(statement);
      break;
    case HirStatementKind::If:
      lower_if(statement);
      break;
    case HirStatementKind::For:
      lower_loop(statement);
      break;
    case HirStatementKind::Switch:
      lower_switch(statement);
      break;
    case HirStatementKind::Block:
    case HirStatementKind::Denial:
    case HirStatementKind::CompileTimeSelection:
      for (HirBlockId block : statement.blocks) lower_block(block);
      break;
    case HirStatementKind::Unchecked:
      ++unchecked_depth_;
      for (HirBlockId block : statement.blocks) lower_block(block);
      --unchecked_depth_;
      break;
    case HirStatementKind::Judgment:
      // A judgment is compile-time evidence and intentionally emits no MIR.
      break;
    case HirStatementKind::Synthesis:
      diagnostics_.error(
          statement.range,
          "unresolved synthesis statement prevents native lowering");
      break;
    case HirStatementKind::Assembly:
      (void)lower_assembly(
          statement.syntax,
          semantic_.types.builtins().void_type,
          statement.expressions,
          statement.range);
      break;
    case HirStatementKind::Invalid:
      diagnostics_.error(statement.range, "invalid HIR statement reached MIR lowering");
      break;
    }
  }

  SemanticPackage &semantic_;
  const HirProgram &hir_;
  const HirProcedure &source_;
  const AssemblyProgram *assembly_ = nullptr;
  RuntimeAssertionMode runtime_assertions_ = RuntimeAssertionMode::On;
  DiagnosticSink &diagnostics_;
  MirProcedure procedure_;
  MirBlockId current_;
  MirValueId context_;
  std::vector<LocalBinding> locals_;
  std::vector<DeferScope> defer_scopes_;
  std::vector<ControlTarget> controls_;
  std::size_t unchecked_depth_ = 0;
  std::size_t initial_errors_ = diagnostics_.error_count();
};

} // namespace

MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    const AssemblyProgram *assembly,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics) {
  MirLoweringResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  for (const HirProcedure &procedure : hir.procedures()) {
    if (procedure.parametric_template || procedure.compile_time_only) continue;
    MirProcedure lowered =
        ProcedureLowerer(
            semantic,
            hir,
            procedure,
            assembly,
            runtime_assertions,
            diagnostics).run();
    if (lowered.valid) ++result.lowered_procedures;
    result.program.add_procedure(std::move(lowered));
  }
  if (diagnostics.error_count() == initial_errors) {
    result.ok = verify_mir_program(result.program, semantic.types, diagnostics);
  }
  return result;
}

MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics) {
  return lower_package_to_mir(
      semantic, hir, nullptr, RuntimeAssertionMode::On, diagnostics);
}

MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics) {
  return lower_package_to_mir(
      semantic, hir, nullptr, runtime_assertions, diagnostics);
}

MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    const AssemblyProgram &assembly,
    DiagnosticSink &diagnostics) {
  return lower_package_to_mir(
      semantic, hir, &assembly, RuntimeAssertionMode::On, diagnostics);
}

MirLoweringResult lower_package_to_mir(
    SemanticPackage &semantic,
    const HirProgram &hir,
    const AssemblyProgram &assembly,
    RuntimeAssertionMode runtime_assertions,
    DiagnosticSink &diagnostics) {
  return lower_package_to_mir(
      semantic, hir, &assembly, runtime_assertions, diagnostics);
}

} // namespace draft
