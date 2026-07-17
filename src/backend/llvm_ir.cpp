// LLVM IR text emission for the first AArch64 macOS backend.
//
// This is a deliberately small printer over Draft MIR. LLVM performs target
// instruction selection and object emission, but it does not decide Draft
// evaluation order, bounds behavior, defer behavior, or source control flow;
// those choices are already explicit in MIR. Unsupported semantic forms produce
// diagnostics instead of silently selecting an ABI or representation.

#include "backend/llvm_ir.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

struct StringConstant {
  std::size_t procedure = 0;
  std::size_t instruction = 0;
  std::string value;
};

[[nodiscard]] std::string encoded_name(std::string_view text) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (const char character : text) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) != 0) {
      result.push_back(static_cast<char>(byte));
    } else {
      result.push_back('_');
      result.push_back(digits[(byte >> 4U) & 0x0fU]);
      result.push_back(digits[byte & 0x0fU]);
    }
  }
  return result;
}

[[nodiscard]] std::string llvm_bytes(std::string_view bytes) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (const char character : bytes) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= 0x20U && byte <= 0x7eU && byte != '"' && byte != '\\') {
      result.push_back(static_cast<char>(byte));
    } else {
      result.push_back('\\');
      result.push_back(digits[(byte >> 4U) & 0x0fU]);
      result.push_back(digits[byte & 0x0fU]);
    }
  }
  return result;
}

class Emitter {
public:
  Emitter(
      const TargetProfile &target,
      const LlvmIrOptions &options,
      const SemanticPackage &semantic,
      const ConstantTable &constants,
      const MirProgram &mir,
      DiagnosticSink &diagnostics)
      : target_(target), options_(options), semantic_(semantic),
        constants_(constants), mir_(mir), diagnostics_(diagnostics) {}

  [[nodiscard]] LlvmIrResult run() {
    LlvmIrResult result;
    initial_errors_ = diagnostics_.error_count();
    collect_strings();

    output_ << "; Draft bootstrap LLVM module\n"
            << "source_filename = \"draft:"
            << encoded_name(options_.package.root_identity) << '/'
            << encoded_name(options_.package.root_relative_path) << "\"\n"
            << "target datalayout = \"" << target_.llvm_data_layout << "\"\n"
            << "target triple = \"" << target_.llvm_triple << "\"\n\n";
    emit_nominal_types();
    emit_strings();
    emit_runtime_declarations();
    emit_globals();
    emit_external_declarations();
    emit_procedures();
    if (options_.emit_program_entry) emit_entry();

    result.ok = diagnostics_.error_count() == initial_errors_;
    result.text = output_.str();
    return result;
  }

private:
  void error(SourceRange range, const std::string &message) {
    diagnostics_.error(range, "LLVM emission: " + message);
  }

  [[nodiscard]] const Type &type(TypeId id) const {
    return semantic_.types.type(id);
  }

  [[nodiscard]] bool integer_kind(TypeKind kind) const {
    return kind == TypeKind::Bool || kind == TypeKind::BooleanStorage ||
        kind == TypeKind::SignedInteger || kind == TypeKind::UnsignedInteger ||
        kind == TypeKind::Rune || kind == TypeKind::EndianScalar ||
        kind == TypeKind::Enum;
  }

  [[nodiscard]] bool signed_integer(TypeId id) const {
    const Type &value = type(id);
    if (value.kind == TypeKind::Distinct) return signed_integer(value.element);
    return value.kind == TypeKind::SignedInteger || value.kind == TypeKind::Rune;
  }

  [[nodiscard]] std::uint32_t integer_bits(TypeId id) const {
    const Type &value = type(id);
    if (value.kind == TypeKind::Bool) return 1;
    if (value.kind == TypeKind::Enum) {
      return static_cast<std::uint32_t>(value.layout.size * 8U);
    }
    if (value.kind == TypeKind::Distinct) return integer_bits(value.element);
    return value.bit_width;
  }

  [[nodiscard]] std::string llvm_type(TypeId id) const {
    const Type &value = type(id);
    switch (value.kind) {
    case TypeKind::Invalid: return "<invalid>";
    case TypeKind::Void: return "void";
    case TypeKind::UntypedInteger:
    case TypeKind::UntypedFloat:
      return "<untyped>";
    case TypeKind::Bool: return "i1";
    case TypeKind::BooleanStorage:
    case TypeKind::SignedInteger:
    case TypeKind::UnsignedInteger:
    case TypeKind::Rune:
    case TypeKind::EndianScalar:
      return "i" + std::to_string(value.bit_width);
    case TypeKind::Float:
      if (value.bit_width == 16) return "half";
      if (value.bit_width == 32) return "float";
      if (value.bit_width == 64) return "double";
      if (value.bit_width == 128) return "fp128";
      return "<invalid-float>";
    case TypeKind::RawPointer:
    case TypeKind::CString:
    case TypeKind::Pointer:
    case TypeKind::MultiPointer:
    case TypeKind::Procedure:
      return "ptr";
    case TypeKind::String:
    case TypeKind::Slice:
      return "{ ptr, i64 }";
    case TypeKind::Array:
      return "[" + std::to_string(value.element_count) + " x " +
          llvm_type(value.element) + "]";
    case TypeKind::Tuple: {
      std::string result = "{ ";
      for (std::size_t index = 0; index < value.members.size(); ++index) {
        if (index != 0) result += ", ";
        result += llvm_type(value.members[index]);
      }
      result += " }";
      return result;
    }
    case TypeKind::Simd:
      return "<" + std::to_string(value.element_count) + " x " +
          llvm_type(value.element) + ">";
    case TypeKind::Struct:
      return "%draft.type." + std::to_string(id.value);
    case TypeKind::Enum:
      return "i" + std::to_string(value.layout.size * 8U);
    case TypeKind::TaggedUnion:
    case TypeKind::RawUnion:
      return "%draft.type." + std::to_string(id.value);
    case TypeKind::Distinct:
      return llvm_type(value.element);
    case TypeKind::TypeParameter:
      return "<type-parameter>";
    }
    return "<invalid>";
  }

  void emit_nominal_types() {
    for (std::size_t index = 0; index < semantic_.types.size(); ++index) {
      const TypeId id{static_cast<std::uint32_t>(index)};
      const Type &value = type(id);
      if (value.kind == TypeKind::Struct) {
        output_ << "%draft.type." << index << " = type { ";
        for (std::size_t member = 0; member < value.members.size(); ++member) {
          if (member != 0) output_ << ", ";
          output_ << llvm_type(value.members[member]);
        }
        output_ << " }\n";
      } else if (value.kind == TypeKind::TaggedUnion ||
                 value.kind == TypeKind::RawUnion) {
        if (!value.layout.known) {
          error(value.declaration, "union has no complete physical layout");
          continue;
        }
        output_ << "%draft.type." << index << " = type ["
                << value.layout.size << " x i8]\n";
      }
    }
    output_ << '\n';
  }

  void collect_strings() {
    const std::vector<MirProcedure> &procedures = mir_.procedures();
    for (std::size_t procedure_index = 0;
         procedure_index < procedures.size();
         ++procedure_index) {
      const MirProcedure &procedure = procedures[procedure_index];
      for (std::size_t instruction_index = 0;
           instruction_index < procedure.instructions.size();
           ++instruction_index) {
        const MirInstruction &instruction =
            procedure.instructions[instruction_index];
        if (instruction.kind == MirInstructionKind::Constant &&
            instruction.constant.kind == ConstantKind::String) {
          strings_.push_back(
              {procedure_index, instruction_index, instruction.constant.text});
        }
      }
    }
  }

  void emit_strings() {
    for (std::size_t index = 0; index < strings_.size(); ++index) {
      output_ << "@.draft.string." << index
              << " = private unnamed_addr constant ["
              << strings_[index].value.size() << " x i8] c\""
              << llvm_bytes(strings_[index].value) << "\", align 1\n";
    }
    if (!strings_.empty()) output_ << '\n';
  }

  void emit_runtime_declarations() {
    output_ << "declare void @llvm.trap() cold noreturn nounwind\n\n"
            << "define internal void @__draft.assert(i1 %condition) {\n"
            << "entry:\n"
            << "  br i1 %condition, label %ok, label %fail\n"
            << "fail:\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  ret void\n"
            << "}\n\n"
            << "define internal void @__draft.bounds(i64 %index, i64 %length) {\n"
            << "entry:\n"
            << "  %inside = icmp ult i64 %index, %length\n"
            << "  br i1 %inside, label %ok, label %fail\n"
            << "fail:\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  ret void\n"
            << "}\n\n"
            << "define internal void @__draft.slice_bounds(i64 %low, i64 %high, i64 %length) {\n"
            << "entry:\n"
            << "  %ordered = icmp ule i64 %low, %high\n"
            << "  %inside = icmp ule i64 %high, %length\n"
            << "  %valid = and i1 %ordered, %inside\n"
            << "  br i1 %valid, label %ok, label %fail\n"
            << "fail:\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  ret void\n"
            << "}\n\n";
  }

  [[nodiscard]] std::string package_symbol_name(
      const PackageIdentity &identity, std::string_view name) const {
    return "@\"draft." + encoded_name(identity.root_identity) + "." +
        encoded_name(identity.root_relative_path) + "." + encoded_name(name) + "\"";
  }

  [[nodiscard]] std::string symbol_name(SymbolId symbol_id) const {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy == symbol_id) {
        return package_symbol_name(
            {imported.root_identity, imported.root_relative_path},
            imported.public_name);
      }
    }
    return package_symbol_name(
        options_.package, semantic_.symbols.symbol(symbol_id).name);
  }

  [[nodiscard]] std::string function_signature(
      TypeId type_id,
      bool with_names) const {
    const Type &signature = type(type_id);
    const std::size_t parameter_count = signature.members.empty()
        ? 0
        : signature.members.size() - 1;
    std::string result = "(";
    bool emitted = false;
    if (!signature.c_calling_convention) {
      result += "ptr";
      if (with_names) result += " %context";
      emitted = true;
    }
    for (std::size_t index = 0; index < parameter_count; ++index) {
      if (emitted) result += ", ";
      result += llvm_type(signature.members[index]);
      if (with_names) result += " %arg" + std::to_string(index);
      emitted = true;
    }
    result += ")";
    return result;
  }

  [[nodiscard]] TypeId function_result(TypeId type_id) const {
    const Type &signature = type(type_id);
    return signature.members.empty()
        ? semantic_.types.builtins().void_type
        : signature.members.back();
  }

  [[nodiscard]] bool has_body(SymbolId symbol) const {
    for (const MirProcedure &procedure : mir_.procedures()) {
      if (procedure.symbol == symbol && procedure.valid) return true;
    }
    return false;
  }

  void emit_globals() {
    const Scope &package_scope =
        semantic_.symbols.scope(semantic_.package_scope);
    for (SymbolId symbol_id : package_scope.symbols) {
      const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
      if (symbol.kind != SymbolKind::Variable || !symbol.type.is_valid()) continue;
      output_ << symbol_name(symbol_id) << " = ";
      if (symbol.flags.is_thread_local) output_ << "thread_local ";
      output_ << "global " << llvm_type(symbol.type) << ' ';
      const ConstantValue *initializer = constants_.find(symbol_id);
      if (initializer != nullptr) {
        output_ << scalar_constant(*initializer, symbol.type, symbol.name_range);
      } else {
        output_ << "zeroinitializer";
      }
      output_ << ", align " << type(symbol.type).layout.alignment << "\n";
    }
    output_ << '\n';
  }

  void emit_external_declarations() {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      const Symbol &symbol = semantic_.symbols.symbol(imported.proxy);
      if (symbol.kind == SymbolKind::Procedure) {
        output_ << "declare " << llvm_type(function_result(symbol.type)) << ' '
                << symbol_name(imported.proxy)
                << function_signature(symbol.type, false) << "\n";
      } else if (symbol.kind == SymbolKind::Variable) {
        output_ << symbol_name(imported.proxy) << " = external global "
                << llvm_type(symbol.type) << "\n";
      }
    }
    const Scope &package_scope =
        semantic_.symbols.scope(semantic_.package_scope);
    for (SymbolId symbol_id : package_scope.symbols) {
      const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
      if (symbol.kind == SymbolKind::Procedure && !has_body(symbol_id)) {
        output_ << "declare " << llvm_type(function_result(symbol.type)) << ' '
                << symbol_name(symbol_id)
                << function_signature(symbol.type, false) << "\n";
      }
    }
    output_ << '\n';
  }

  [[nodiscard]] std::optional<std::size_t> string_index(
      std::size_t procedure, std::size_t instruction) const {
    for (std::size_t index = 0; index < strings_.size(); ++index) {
      if (strings_[index].procedure == procedure &&
          strings_[index].instruction == instruction) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string scalar_constant(
      const ConstantValue &value, TypeId type_id, SourceRange range) {
    switch (value.kind) {
    case ConstantKind::Unavailable:
      if (llvm_type(type_id) == "ptr") return "null";
      return "zeroinitializer";
    case ConstantKind::Bool:
      return value.boolean ? "true" : "false";
    case ConstantKind::Integer:
      return value.integer.to_decimal();
    case ConstantKind::EnumLabel:
      // Contextual alternatives retain their SymbolId on MIR Constant rows;
      // instruction_constant handles the declaration-order discriminator.
      return "0";
    case ConstantKind::Float:
      error(range, "exact rational floating constants are not lowered yet");
      return "0.0";
    case ConstantKind::String:
      error(range, "string constant requires module string identity");
      return "zeroinitializer";
    case ConstantKind::Target:
      error(range, "target pseudo-value reached runtime emission");
      return "zeroinitializer";
    }
    return "zeroinitializer";
  }

  [[nodiscard]] std::uint64_t enum_discriminator(SymbolId member) const {
    if (!member.is_valid()) return 0;
    const Symbol &member_symbol = semantic_.symbols.symbol(member);
    std::uint64_t discriminator = 0;
    for (const AggregateMember &entry : semantic_.aggregate_members) {
      if (entry.member == member) return discriminator;
      const Symbol &candidate = semantic_.symbols.symbol(entry.member);
      if (candidate.scope == member_symbol.scope) ++discriminator;
    }
    return 0;
  }

  [[nodiscard]] std::string instruction_constant(
      std::size_t procedure_index,
      std::size_t instruction_index,
      const MirInstruction &instruction) {
    if (instruction.constant.kind == ConstantKind::String) {
      const std::optional<std::size_t> index =
          string_index(procedure_index, instruction_index);
      if (!index.has_value()) {
        error(instruction.range, "string constant was not interned");
        return "zeroinitializer";
      }
      return "{ ptr @.draft.string." + std::to_string(*index) + ", i64 " +
          std::to_string(instruction.constant.text.size()) + " }";
    }
    if (instruction.constant.kind == ConstantKind::EnumLabel) {
      return std::to_string(enum_discriminator(instruction.symbol));
    }
    return scalar_constant(
        instruction.constant, instruction.type, instruction.range);
  }

  [[nodiscard]] std::string value_operand(
      const std::vector<std::string> &operands,
      MirValueId id,
      SourceRange range) {
    if (!id.is_valid() || static_cast<std::size_t>(id.value) >= operands.size() ||
        operands[id.value].empty()) {
      error(range, "MIR value has no emitted LLVM operand");
      return "undef";
    }
    return operands[id.value];
  }

  [[nodiscard]] std::string typed_operand(
      const MirProcedure &procedure,
      const std::vector<std::string> &operands,
      MirValueId id,
      SourceRange range) {
    return llvm_type(procedure.value(id).type) + " " +
        value_operand(operands, id, range);
  }

  [[nodiscard]] std::string auxiliary() {
    return "%a" + std::to_string(auxiliary_index_++);
  }

  void assign_alias(
      std::vector<std::string> &operands,
      const MirInstruction &instruction,
      const std::string &value) {
    if (instruction.result.is_valid()) operands[instruction.result.value] = value;
  }

  [[nodiscard]] std::string integer_binary_opcode(
      HirOperation operation, TypeId operand_type) const {
    switch (operation) {
    case HirOperation::Add: return "add";
    case HirOperation::Subtract: return "sub";
    case HirOperation::Multiply: return "mul";
    case HirOperation::Divide: return signed_integer(operand_type) ? "sdiv" : "udiv";
    case HirOperation::Remainder: return signed_integer(operand_type) ? "srem" : "urem";
    case HirOperation::BitwiseAnd: return "and";
    case HirOperation::BitwiseOr: return "or";
    case HirOperation::BitwiseXor: return "xor";
    case HirOperation::ShiftLeft: return "shl";
    case HirOperation::ShiftRight: return signed_integer(operand_type) ? "ashr" : "lshr";
    default: return {};
    }
  }

  [[nodiscard]] std::string comparison_predicate(
      HirOperation operation, TypeId operand_type) const {
    const bool is_signed = signed_integer(operand_type);
    switch (operation) {
    case HirOperation::Equal: return "eq";
    case HirOperation::NotEqual: return "ne";
    case HirOperation::Less: return is_signed ? "slt" : "ult";
    case HirOperation::LessEqual: return is_signed ? "sle" : "ule";
    case HirOperation::Greater: return is_signed ? "sgt" : "ugt";
    case HirOperation::GreaterEqual: return is_signed ? "sge" : "uge";
    default: return {};
    }
  }

  void emit_unary(
      const MirProcedure &procedure,
      const MirInstruction &instruction,
      std::vector<std::string> &operands) {
    const MirValueId source = instruction.operands.front();
    const std::string source_operand = value_operand(operands, source, instruction.range);
    const std::string result = "%v" + std::to_string(instruction.result.value);
    const std::string value_type = llvm_type(instruction.type);
    if (instruction.operation == HirOperation::Positive) {
      assign_alias(operands, instruction, source_operand);
      return;
    }
    output_ << "  " << result << " = ";
    if (instruction.operation == HirOperation::Negate) {
      if (type(instruction.type).kind == TypeKind::Float) {
        output_ << "fneg " << value_type << ' ' << source_operand;
      } else {
        output_ << "sub " << value_type << " 0, " << source_operand;
      }
    } else if (instruction.operation == HirOperation::LogicalNot) {
      output_ << "xor i1 " << source_operand << ", true";
    } else if (instruction.operation == HirOperation::BitwiseNot) {
      output_ << "xor " << value_type << ' ' << source_operand << ", -1";
    } else {
      error(instruction.range, "unsupported unary operation");
      output_ << "freeze " << value_type << " poison";
    }
    output_ << '\n';
    assign_alias(operands, instruction, result);
    (void)procedure;
  }

  void emit_binary(
      const MirProcedure &procedure,
      const MirInstruction &instruction,
      std::vector<std::string> &operands) {
    const MirValueId left_id = instruction.operands[0];
    const MirValueId right_id = instruction.operands[1];
    const TypeId operand_type = procedure.value(left_id).type;
    const TypeKind operand_kind = type(operand_type).kind;
    const std::string value_type = llvm_type(operand_type);
    const std::string left = value_operand(operands, left_id, instruction.range);
    const std::string right = value_operand(operands, right_id, instruction.range);
    const std::string result = "%v" + std::to_string(instruction.result.value);
    const std::string predicate =
        comparison_predicate(instruction.operation, operand_type);
    output_ << "  " << result << " = ";
    if (!predicate.empty()) {
      if (operand_kind == TypeKind::Float) {
        std::string float_predicate = predicate;
        if (instruction.operation == HirOperation::Equal) float_predicate = "oeq";
        if (instruction.operation == HirOperation::NotEqual) float_predicate = "une";
        if (instruction.operation == HirOperation::Less) float_predicate = "olt";
        if (instruction.operation == HirOperation::LessEqual) float_predicate = "ole";
        if (instruction.operation == HirOperation::Greater) float_predicate = "ogt";
        if (instruction.operation == HirOperation::GreaterEqual) float_predicate = "oge";
        output_ << "fcmp " << float_predicate << ' ' << value_type << ' '
                << left << ", " << right;
      } else {
        output_ << "icmp " << predicate << ' ' << value_type << ' '
                << left << ", " << right;
      }
    } else if (operand_kind == TypeKind::Float) {
      std::string opcode;
      if (instruction.operation == HirOperation::Add) opcode = "fadd";
      if (instruction.operation == HirOperation::Subtract) opcode = "fsub";
      if (instruction.operation == HirOperation::Multiply) opcode = "fmul";
      if (instruction.operation == HirOperation::Divide) opcode = "fdiv";
      if (instruction.operation == HirOperation::Remainder) opcode = "frem";
      if (opcode.empty()) {
        error(instruction.range, "unsupported floating binary operation");
        opcode = "fadd";
      }
      output_ << opcode << ' ' << value_type << ' ' << left << ", " << right;
    } else {
      std::string opcode = integer_binary_opcode(instruction.operation, operand_type);
      if (opcode.empty()) {
        error(instruction.range, "unsupported integer binary operation");
        opcode = "add";
      }
      output_ << opcode << ' ' << value_type << ' ' << left << ", " << right;
    }
    output_ << '\n';
    assign_alias(operands, instruction, result);
  }

  void emit_convert(
      const MirProcedure &procedure,
      const MirInstruction &instruction,
      std::vector<std::string> &operands) {
    const MirValueId source_id = instruction.operands.front();
    const TypeId source_type = procedure.value(source_id).type;
    const TypeKind source_kind = type(source_type).kind;
    const TypeKind target_kind = type(instruction.type).kind;
    const std::string source = value_operand(operands, source_id, instruction.range);
    if (llvm_type(source_type) == llvm_type(instruction.type)) {
      assign_alias(operands, instruction, source);
      return;
    }
    std::string opcode;
    if (integer_kind(source_kind) && integer_kind(target_kind)) {
      const std::uint32_t source_bits = integer_bits(source_type);
      const std::uint32_t target_bits = integer_bits(instruction.type);
      if (source_bits > target_bits) opcode = "trunc";
      else opcode = signed_integer(source_type) ? "sext" : "zext";
    } else if (integer_kind(source_kind) && target_kind == TypeKind::Float) {
      opcode = signed_integer(source_type) ? "sitofp" : "uitofp";
    } else if (source_kind == TypeKind::Float && integer_kind(target_kind)) {
      opcode = signed_integer(instruction.type) ? "fptosi" : "fptoui";
    } else if (source_kind == TypeKind::Float && target_kind == TypeKind::Float) {
      opcode = type(source_type).bit_width > type(instruction.type).bit_width
          ? "fptrunc"
          : "fpext";
    } else if (llvm_type(source_type) == "ptr" && integer_kind(target_kind)) {
      opcode = "ptrtoint";
    } else if (integer_kind(source_kind) && llvm_type(instruction.type) == "ptr") {
      opcode = "inttoptr";
    }
    if (opcode.empty()) {
      error(instruction.range, "unsupported cast in LLVM emission");
      assign_alias(operands, instruction, source);
      return;
    }
    const std::string result = "%v" + std::to_string(instruction.result.value);
    output_ << "  " << result << " = " << opcode << ' '
            << llvm_type(source_type) << ' ' << source << " to "
            << llvm_type(instruction.type) << '\n';
    assign_alias(operands, instruction, result);
  }

  [[nodiscard]] std::size_t aggregate_index(
      TypeId aggregate_type, std::uint64_t offset) const {
    const Type &aggregate = type(aggregate_type);
    if (aggregate.kind == TypeKind::Array) {
      const std::uint64_t stride = type(aggregate.element).layout.size;
      return stride == 0 ? 0 : static_cast<std::size_t>(offset / stride);
    }
    for (std::size_t index = 0; index < aggregate.member_offsets.size(); ++index) {
      if (aggregate.member_offsets[index] == offset) return index;
    }
    return 0;
  }

  void emit_aggregate(
      const MirProcedure &procedure,
      const MirInstruction &instruction,
      std::vector<std::string> &operands) {
    if (type(instruction.type).kind == TypeKind::TaggedUnion ||
        type(instruction.type).kind == TypeKind::RawUnion) {
      error(instruction.range, "union aggregate construction is not lowered yet");
      assign_alias(operands, instruction, "zeroinitializer");
      return;
    }
    if (instruction.operands.empty()) {
      assign_alias(operands, instruction, "zeroinitializer");
      return;
    }
    std::string aggregate = "zeroinitializer";
    for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
      const MirValueId value_id = instruction.operands[index];
      const std::string result = index + 1 == instruction.operands.size()
          ? "%v" + std::to_string(instruction.result.value)
          : auxiliary();
      const std::size_t member = aggregate_index(
          instruction.type,
          index < instruction.offsets.size() ? instruction.offsets[index] : 0);
      output_ << "  " << result << " = insertvalue "
              << llvm_type(instruction.type) << ' ' << aggregate << ", "
              << typed_operand(procedure, operands, value_id, instruction.range)
              << ", " << member << '\n';
      aggregate = result;
    }
    assign_alias(
        operands,
        instruction,
        "%v" + std::to_string(instruction.result.value));
  }

  void emit_instruction(
      std::size_t procedure_index,
      std::size_t instruction_index,
      const MirProcedure &procedure,
      const MirInstruction &instruction,
      std::vector<std::string> &operands) {
    const std::string result = instruction.result.is_valid()
        ? "%v" + std::to_string(instruction.result.value)
        : std::string();
    switch (instruction.kind) {
    case MirInstructionKind::Constant:
      assign_alias(
          operands,
          instruction,
          instruction_constant(procedure_index, instruction_index, instruction));
      break;
    case MirInstructionKind::Zero:
      assign_alias(operands, instruction, "zeroinitializer");
      break;
    case MirInstructionKind::Context:
      assign_alias(operands, instruction, "%context");
      break;
    case MirInstructionKind::LocalAddress:
      assign_alias(
          operands, instruction, "%l" + std::to_string(instruction.local.value));
      break;
    case MirInstructionKind::GlobalAddress:
    case MirInstructionKind::ProcedureReference:
      assign_alias(operands, instruction, symbol_name(instruction.symbol));
      break;
    case MirInstructionKind::Load:
      output_ << "  " << result << " = load " << llvm_type(instruction.type)
              << ", ptr "
              << value_operand(operands, instruction.operands[0], instruction.range)
              << ", align " << type(instruction.type).layout.alignment << '\n';
      assign_alias(operands, instruction, result);
      break;
    case MirInstructionKind::Store: {
      const MirValueId value_id = instruction.operands[1];
      output_ << "  store "
              << typed_operand(procedure, operands, value_id, instruction.range)
              << ", ptr "
              << value_operand(operands, instruction.operands[0], instruction.range)
              << ", align "
              << type(procedure.value(value_id).type).layout.alignment << '\n';
      break;
    }
    case MirInstructionKind::Unary:
      emit_unary(procedure, instruction, operands);
      break;
    case MirInstructionKind::Binary:
      emit_binary(procedure, instruction, operands);
      break;
    case MirInstructionKind::Convert:
      emit_convert(procedure, instruction, operands);
      break;
    case MirInstructionKind::Call: {
      if (instruction.result.is_valid()) output_ << "  " << result << " = ";
      output_ << "call " << llvm_type(instruction.type) << ' '
              << value_operand(operands, instruction.operands[0], instruction.range)
              << '(';
      for (std::size_t index = 1; index < instruction.operands.size(); ++index) {
        if (index != 1) output_ << ", ";
        output_ << typed_operand(
            procedure, operands, instruction.operands[index], instruction.range);
      }
      output_ << ")\n";
      if (instruction.result.is_valid()) assign_alias(operands, instruction, result);
      break;
    }
    case MirInstructionKind::Length: {
      const MirValueId base = instruction.operands.front();
      output_ << "  " << result << " = extractvalue "
              << typed_operand(procedure, operands, base, instruction.range)
              << ", 1\n";
      assign_alias(operands, instruction, result);
      break;
    }
    case MirInstructionKind::Assert:
      output_ << "  call void @__draft.assert("
              << typed_operand(
                     procedure, operands, instruction.operands[0], instruction.range)
              << ")\n";
      break;
    case MirInstructionKind::MemberAddress:
      output_ << "  " << result << " = getelementptr i8, ptr "
              << value_operand(operands, instruction.operands[0], instruction.range)
              << ", i64 " << instruction.offset << '\n';
      assign_alias(operands, instruction, result);
      break;
    case MirInstructionKind::ExtractMember:
      error(instruction.range, "extract-member MIR is not implemented");
      assign_alias(operands, instruction, "undef");
      break;
    case MirInstructionKind::IndexAddress: {
      const MirValueId base_id = instruction.operands[0];
      std::string base = value_operand(operands, base_id, instruction.range);
      const TypeKind base_kind = type(procedure.value(base_id).type).kind;
      if (base_kind == TypeKind::Slice || base_kind == TypeKind::String) {
        const std::string data = auxiliary();
        output_ << "  " << data << " = extractvalue "
                << typed_operand(procedure, operands, base_id, instruction.range)
                << ", 0\n";
        base = data;
      }
      output_ << "  " << result << " = getelementptr i8, ptr " << base
              << ", i64 ";
      const MirValueId index = instruction.operands[1];
      const std::string index_operand = value_operand(operands, index, instruction.range);
      if (instruction.offset == 1) {
        output_ << index_operand << '\n';
      } else {
        const std::string scaled = auxiliary();
        output_ << "0\n";
        // Replace the placeholder pointer with a second, typed byte offset GEP.
        // The first zero GEP keeps result naming simple while preserving direct
        // and easily audited arithmetic in the emitted text.
        output_ << "  " << scaled << " = mul i64 " << index_operand << ", "
                << instruction.offset << '\n';
        output_ << "  " << result << ".scaled = getelementptr i8, ptr " << base
                << ", i64 " << scaled << '\n';
        assign_alias(operands, instruction, result + ".scaled");
        break;
      }
      assign_alias(operands, instruction, result);
      break;
    }
    case MirInstructionKind::BoundsCheck:
      output_ << "  call void @__draft.bounds("
              << typed_operand(
                     procedure, operands, instruction.operands[0], instruction.range)
              << ", "
              << typed_operand(
                     procedure, operands, instruction.operands[1], instruction.range)
              << ")\n";
      break;
    case MirInstructionKind::SliceBoundsCheck:
      output_ << "  call void @__draft.slice_bounds(";
      for (std::size_t index = 0; index < 3; ++index) {
        if (index != 0) output_ << ", ";
        output_ << typed_operand(
            procedure, operands, instruction.operands[index], instruction.range);
      }
      output_ << ")\n";
      break;
    case MirInstructionKind::Slice: {
      const MirValueId base_id = instruction.operands[0];
      std::string data = value_operand(operands, base_id, instruction.range);
      const TypeKind base_kind = type(procedure.value(base_id).type).kind;
      if (base_kind == TypeKind::Slice || base_kind == TypeKind::String) {
        const std::string extracted = auxiliary();
        output_ << "  " << extracted << " = extractvalue "
                << typed_operand(procedure, operands, base_id, instruction.range)
                << ", 0\n";
        data = extracted;
      }
      const MirValueId low_id = instruction.operands[1];
      const MirValueId high_id = instruction.operands[2];
      const Type &slice_type = type(instruction.type);
      const std::uint64_t stride = type(slice_type.element).layout.size;
      std::string adjusted = data;
      if (stride != 0) {
        const std::string byte_offset = auxiliary();
        const std::string pointer = auxiliary();
        output_ << "  " << byte_offset << " = mul i64 "
                << value_operand(operands, low_id, instruction.range) << ", "
                << stride << '\n'
                << "  " << pointer << " = getelementptr i8, ptr " << data
                << ", i64 " << byte_offset << '\n';
        adjusted = pointer;
      }
      const std::string count = auxiliary();
      const std::string with_data = auxiliary();
      output_ << "  " << count << " = sub i64 "
              << value_operand(operands, high_id, instruction.range) << ", "
              << value_operand(operands, low_id, instruction.range) << '\n'
              << "  " << with_data << " = insertvalue "
              << llvm_type(instruction.type) << " zeroinitializer, ptr "
              << adjusted << ", 0\n"
              << "  " << result << " = insertvalue "
              << llvm_type(instruction.type) << ' ' << with_data << ", i64 "
              << count << ", 1\n";
      assign_alias(operands, instruction, result);
      break;
    }
    case MirInstructionKind::Aggregate:
      emit_aggregate(procedure, instruction, operands);
      break;
    case MirInstructionKind::Assembly:
      error(instruction.range, "assembly MIR requires the parsed AArch64 emitter");
      break;
    case MirInstructionKind::Invalid:
      error(instruction.range, "invalid MIR instruction reached emission");
      break;
    }
  }

  void emit_terminator(
      const MirProcedure &procedure,
      const MirTerminator &terminator,
      const std::vector<std::string> &operands) {
    switch (terminator.kind) {
    case MirTerminatorKind::Return:
      if (terminator.value.is_valid()) {
        output_ << "  ret "
                << typed_operand(
                       procedure, operands, terminator.value, terminator.range)
                << '\n';
      } else {
        output_ << "  ret void\n";
      }
      break;
    case MirTerminatorKind::Branch:
      output_ << "  br label %b" << terminator.targets[0].value << '\n';
      break;
    case MirTerminatorKind::ConditionalBranch:
      output_ << "  br "
              << typed_operand(
                     procedure, operands, terminator.value, terminator.range)
              << ", label %b" << terminator.targets[0].value
              << ", label %b" << terminator.targets[1].value << '\n';
      break;
    case MirTerminatorKind::Switch:
      output_ << "  switch "
              << typed_operand(
                     procedure, operands, terminator.value, terminator.range)
              << ", label %b" << terminator.targets[0].value << " [\n";
      for (const MirSwitchArm &arm : terminator.switch_arms) {
        output_ << "    "
                << typed_operand(procedure, operands, arm.label, terminator.range)
                << ", label %b" << arm.target.value << '\n';
      }
      output_ << "  ]\n";
      break;
    case MirTerminatorKind::Unreachable:
      output_ << "  unreachable\n";
      break;
    case MirTerminatorKind::Invalid:
      error(terminator.range, "unterminated MIR block reached emission");
      output_ << "  unreachable\n";
      break;
    }
  }

  void emit_procedure(std::size_t procedure_index, const MirProcedure &procedure) {
    if (!procedure.valid) return;
    auxiliary_index_ = 0;
    output_ << "define " << llvm_type(function_result(procedure.type)) << ' '
            << symbol_name(procedure.symbol)
            << function_signature(procedure.type, true) << " {\n";
    std::vector<std::string> operands(procedure.values.size());
    for (std::size_t block_index = 0;
         block_index < procedure.blocks.size();
         ++block_index) {
      const MirBlock &block = procedure.blocks[block_index];
      output_ << "b" << block_index << ":\n";
      if (block_index == procedure.entry.value) {
        for (std::size_t local_index = 0;
             local_index < procedure.locals.size();
             ++local_index) {
          const MirLocal &local = procedure.locals[local_index];
          output_ << "  %l" << local_index << " = alloca "
                  << llvm_type(local.type) << ", align "
                  << type(local.type).layout.alignment << '\n';
          if (local.kind == MirLocalKind::Parameter) {
            output_ << "  store " << llvm_type(local.type) << " %arg"
                    << local.parameter_index << ", ptr %l" << local_index
                    << ", align " << type(local.type).layout.alignment << '\n';
          }
        }
      }
      for (MirInstructionId instruction_id : block.instructions) {
        emit_instruction(
            procedure_index,
            instruction_id.value,
            procedure,
            procedure.instruction(instruction_id),
            operands);
      }
      emit_terminator(procedure, block.terminator, operands);
    }
    output_ << "}\n\n";
  }

  void emit_procedures() {
    for (std::size_t index = 0; index < mir_.procedures().size(); ++index) {
      emit_procedure(index, mir_.procedures()[index]);
    }
  }

  [[nodiscard]] std::optional<SymbolId> main_symbol() const {
    const std::optional<SymbolId> found =
        semantic_.symbols.lookup_direct(semantic_.package_scope, "main");
    if (!found.has_value()) return std::nullopt;
    const Symbol &symbol = semantic_.symbols.symbol(*found);
    if (symbol.kind != SymbolKind::Procedure || !has_body(*found)) {
      return std::nullopt;
    }
    return found;
  }

  void emit_entry() {
    const std::optional<SymbolId> entry = main_symbol();
    if (!entry.has_value()) {
      error(SourceRange::invalid(), "executable root package has no defined main procedure");
      return;
    }
    const Symbol &symbol = semantic_.symbols.symbol(*entry);
    const Type &signature = type(symbol.type);
    const std::size_t parameters = signature.members.empty()
        ? 0
        : signature.members.size() - 1;
    if (parameters != 0 || signature.c_calling_convention) {
      error(symbol.name_range, "Draft main must be an ordinary zero-parameter procedure");
      return;
    }
    const TypeId result_type = function_result(symbol.type);
    output_ << "define i32 @main(i32 %argc, ptr %argv) {\n"
            << "entry:\n";
    if (result_type == semantic_.types.builtins().void_type) {
      output_ << "  call void " << symbol_name(*entry) << "(ptr null)\n"
              << "  ret i32 0\n";
    } else if (integer_kind(type(result_type).kind)) {
      output_ << "  %draft.result = call " << llvm_type(result_type) << ' '
              << symbol_name(*entry) << "(ptr null)\n";
      const std::uint32_t bits = integer_bits(result_type);
      if (bits > 32) {
        output_ << "  %exit.result = trunc " << llvm_type(result_type)
                << " %draft.result to i32\n"
                << "  ret i32 %exit.result\n";
      } else if (bits < 32) {
        output_ << "  %exit.result = zext " << llvm_type(result_type)
                << " %draft.result to i32\n"
                << "  ret i32 %exit.result\n";
      } else {
        output_ << "  ret i32 %draft.result\n";
      }
    } else {
      error(symbol.name_range, "Draft main result must be void or an integer");
      output_ << "  ret i32 1\n";
    }
    output_ << "}\n\n";
  }

  const TargetProfile &target_;
  const LlvmIrOptions &options_;
  const SemanticPackage &semantic_;
  const ConstantTable &constants_;
  const MirProgram &mir_;
  DiagnosticSink &diagnostics_;
  std::ostringstream output_;
  std::vector<StringConstant> strings_;
  std::size_t initial_errors_ = 0;
  std::size_t auxiliary_index_ = 0;
};

} // namespace

LlvmIrResult emit_llvm_ir(
    const TargetProfile &target,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const ConstantTable &constants,
    const MirProgram &mir,
    DiagnosticSink &diagnostics) {
  return Emitter(target, options, semantic, constants, mir, diagnostics).run();
}

} // namespace draft
