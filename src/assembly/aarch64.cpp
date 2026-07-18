// Validator and lowering metadata for draft-aarch64-apple-v1 assembly.

#include "assembly/aarch64.h"

#include "sema/big_integer.h"
#include "syntax/token.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

enum class RegisterClass {
  General,
  Vector,
};

struct Register {
  bool valid = false;
  bool zero = false;
  std::uint32_t index = 0;
  std::uint32_t bits = 0;
  RegisterClass register_class = RegisterClass::General;
  std::string name;
};

struct DeclaredRegister {
  Register reg;
  SourceRange range;
  TypeId type;
};

struct ParsedInstruction {
  std::vector<Register> reads;
  std::vector<Register> writes;
  std::vector<std::pair<Register, std::uint32_t>> memory_accesses;
  bool writes_flags = false;
};

[[nodiscard]] bool same_physical(Register left, Register right) {
  return left.valid && right.valid && left.index == right.index &&
      left.zero == right.zero &&
      left.register_class == right.register_class;
}

[[nodiscard]] bool all_decimal(std::string_view text) {
  return !text.empty() && std::all_of(
      text.begin(), text.end(), [](char character) {
        return character >= '0' && character <= '9';
      });
}

[[nodiscard]] Register parse_register(std::string_view text) {
  Register result;
  result.name = std::string(text);
  if (text == "xzr" || text == "wzr") {
    result.valid = true;
    result.zero = true;
    result.index = 31;
    result.bits = text.front() == 'x' ? 64U : 32U;
    return result;
  }
  if (text.size() < 2 || !all_decimal(text.substr(1))) {
    return result;
  }
  const char prefix = text.front();
  const bool general = prefix == 'x' || prefix == 'w';
  const bool vector = prefix == 'b' || prefix == 'h' || prefix == 's' ||
      prefix == 'd' || prefix == 'q' || prefix == 'v';
  if (!general && !vector) return result;
  const std::optional<BigInteger> number = BigInteger::parse_literal(text.substr(1));
  const std::optional<std::uint64_t> index =
      number.has_value() ? number->to_u64() : std::nullopt;
  if (!index.has_value() || *index > (general ? 30U : 31U)) return result;
  result.valid = true;
  result.index = static_cast<std::uint32_t>(*index);
  result.register_class = general
      ? RegisterClass::General
      : RegisterClass::Vector;
  if (prefix == 'x') result.bits = 64;
  if (prefix == 'w') result.bits = 32;
  if (prefix == 'b') result.bits = 8;
  if (prefix == 'h') result.bits = 16;
  if (prefix == 's') result.bits = 32;
  if (prefix == 'd') result.bits = 64;
  if (prefix == 'q' || prefix == 'v') result.bits = 128;
  return result;
}

[[nodiscard]] std::string trimmed_source(
    const SourceManager &sources, SourceRange range) {
  std::string text(sources.text(range));
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  if (!text.empty() && text.back() == ';') text.pop_back();
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  return text;
}

class Analyzer {
public:
  Analyzer(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      const TargetProfile &target,
      const SemanticPackage &semantic,
      const HirProgram &hir,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), target_(target),
        semantic_(semantic), hir_(hir), diagnostics_(diagnostics) {}

  [[nodiscard]] AssemblyProgram run() {
    AssemblyProgram result;
    const std::size_t initial_errors = diagnostics_.error_count();
    for (std::size_t index = 0; index < hir_.expression_count(); ++index) {
      const HirExpression &expression =
          hir_.expression(HirExpressionId{static_cast<std::uint32_t>(index)});
      if (expression.kind != HirExpressionKind::Assembly) continue;
      std::vector<TypeId> inputs;
      for (HirExpressionId operand : expression.operands) {
        inputs.push_back(hir_.expression(operand).type);
      }
      result.regions.push_back(analyze_region(
          expression.syntax, expression.type, inputs, true));
    }
    for (std::size_t index = 0; index < hir_.statement_count(); ++index) {
      const HirStatement &statement =
          hir_.statement(HirStatementId{static_cast<std::uint32_t>(index)});
      if (statement.kind != HirStatementKind::Assembly) continue;
      std::vector<TypeId> inputs;
      for (HirExpressionId operand : statement.expressions) {
        inputs.push_back(hir_.expression(operand).type);
      }
      result.regions.push_back(analyze_region(
          statement.syntax,
          semantic_.types.builtins().void_type,
          inputs,
          false));
    }
    result.ok = diagnostics_.error_count() == initial_errors;
    return result;
  }

private:
  [[nodiscard]] const SyntaxTree *tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) return &*entry.syntax;
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<std::uint32_t> row_tokens(
      const SyntaxTree &syntax, const SyntaxNode &node) const {
    std::vector<std::uint32_t> tokens;
    for (std::uint32_t index = node.token_begin; index < node.token_end; ++index) {
      if (syntax.token(index).kind != TokenKind::Semicolon) tokens.push_back(index);
    }
    return tokens;
  }

  [[nodiscard]] std::string token_text(
      const SyntaxTree &syntax, std::uint32_t token) const {
    return std::string(sources_.text(syntax.token(token).range));
  }

  [[nodiscard]] bool runtime_register_type(TypeId type_id, Register reg) const {
    if (!type_id.is_valid() || !reg.valid) return false;
    Type value = semantic_.types.type(type_id);
    while (value.kind == TypeKind::Distinct) {
      value = semantic_.types.type(value.element);
    }
    if (value.kind == TypeKind::Pointer || value.kind == TypeKind::MultiPointer ||
        value.kind == TypeKind::RawPointer || value.kind == TypeKind::CString ||
        value.kind == TypeKind::Procedure) {
      return reg.register_class == RegisterClass::General && reg.bits == 64;
    }
    if (value.kind == TypeKind::Enum) {
      if (!value.layout.known || value.layout.size > 8) return false;
      const std::uint32_t register_bits = value.layout.size <= 4 ? 32U : 64U;
      return reg.register_class == RegisterClass::General &&
          reg.bits == register_bits;
    }
    if (value.kind == TypeKind::Bool) {
      return reg.register_class == RegisterClass::General && reg.bits == 32;
    }
    if (value.kind == TypeKind::SignedInteger ||
        value.kind == TypeKind::UnsignedInteger || value.kind == TypeKind::Rune ||
        value.kind == TypeKind::BooleanStorage ||
        value.kind == TypeKind::EndianScalar) {
      if (value.bit_width == 0 || value.bit_width > 64) return false;
      const std::uint32_t register_bits = value.bit_width <= 32 ? 32U : 64U;
      return reg.register_class == RegisterClass::General &&
          reg.bits == register_bits;
    }
    if (value.kind == TypeKind::Float) {
      return reg.register_class == RegisterClass::Vector &&
          value.bit_width == reg.bits && reg.bits <= 64;
    }
    if (value.kind == TypeKind::Simd && value.layout.known) {
      const std::uint64_t bits = value.layout.size * 8U;
      return reg.register_class == RegisterClass::Vector &&
          (bits == 64 || bits == 128) && bits == reg.bits;
    }
    return false;
  }

  [[nodiscard]] const DeclaredRegister *declared_input(
      const std::vector<DeclaredRegister> &inputs, Register reg) const {
    for (const DeclaredRegister &input : inputs) {
      if (same_physical(input.reg, reg)) return &input;
    }
    return nullptr;
  }

  [[nodiscard]] bool memory_value_type(TypeId type_id, std::uint32_t bits) const {
    Type value = semantic_.types.type(type_id);
    while (value.kind == TypeKind::Distinct) {
      value = semantic_.types.type(value.element);
    }
    const bool scalar = value.kind == TypeKind::Bool ||
        value.kind == TypeKind::BooleanStorage ||
        value.kind == TypeKind::SignedInteger ||
        value.kind == TypeKind::UnsignedInteger ||
        value.kind == TypeKind::Float || value.kind == TypeKind::Rune ||
        value.kind == TypeKind::EndianScalar || value.kind == TypeKind::Enum;
    const bool vector = value.kind == TypeKind::Simd &&
        (bits == 64 || bits == 128);
    return (scalar || vector) && value.layout.known &&
        value.layout.size * 8U == bits;
  }

  [[nodiscard]] bool typed_memory_access(
      const DeclaredRegister &address, std::uint32_t bits) const {
    Type pointer = semantic_.types.type(address.type);
    while (pointer.kind == TypeKind::Distinct) {
      pointer = semantic_.types.type(pointer.element);
    }
    if (pointer.kind == TypeKind::CString) {
      return bits == 8;
    }
    if (pointer.kind != TypeKind::Pointer &&
        pointer.kind != TypeKind::MultiPointer) {
      return false;
    }
    return memory_value_type(pointer.element, bits);
  }

  [[nodiscard]] bool contains_register(
      const std::vector<DeclaredRegister> &registers, Register needle) const {
    return std::any_of(
        registers.begin(), registers.end(), [needle](const DeclaredRegister &entry) {
          return same_physical(entry.reg, needle);
        });
  }

  [[nodiscard]] bool contains_register(
      const std::vector<Register> &registers, Register needle) const {
    return std::any_of(
        registers.begin(), registers.end(), [needle](Register entry) {
          return same_physical(entry, needle);
        });
  }

  [[nodiscard]] std::optional<Register> operand_register(
      const SyntaxTree &syntax,
      const std::vector<std::uint32_t> &operand) const {
    if (operand.size() != 1 || syntax.token(operand.front()).kind != TokenKind::Identifier) {
      return std::nullopt;
    }
    const Register reg = parse_register(token_text(syntax, operand.front()));
    return reg.valid ? std::optional<Register>(reg) : std::nullopt;
  }

  [[nodiscard]] std::optional<Register> memory_base_register(
      const SyntaxTree &syntax,
      const std::vector<std::uint32_t> &operand) const {
    if (operand.size() != 3 ||
        syntax.token(operand[0]).kind != TokenKind::LeftBracket ||
        syntax.token(operand[2]).kind != TokenKind::RightBracket ||
        syntax.token(operand[1]).kind != TokenKind::Identifier) {
      return std::nullopt;
    }
    const Register base = parse_register(token_text(syntax, operand[1]));
    if (!base.valid || base.zero ||
        base.register_class != RegisterClass::General || base.bits != 64) {
      return std::nullopt;
    }
    return base;
  }

  [[nodiscard]] bool immediate_operand(
      const SyntaxTree &syntax,
      const std::vector<std::uint32_t> &operand,
      std::uint64_t maximum) const {
    if (operand.size() != 2 || syntax.token(operand[0]).kind != TokenKind::Hash ||
        syntax.token(operand[1]).kind != TokenKind::IntegerLiteral) {
      return false;
    }
    const std::optional<BigInteger> value =
        BigInteger::parse_literal(token_text(syntax, operand[1]));
    const std::optional<std::uint64_t> narrowed =
        value.has_value() ? value->to_u64() : std::nullopt;
    return narrowed.has_value() && *narrowed <= maximum;
  }

  [[nodiscard]] std::vector<std::vector<std::uint32_t>> instruction_operands(
      const SyntaxTree &syntax,
      const std::vector<std::uint32_t> &tokens) const {
    std::vector<std::vector<std::uint32_t>> operands;
    if (tokens.size() <= 1) return operands;
    operands.emplace_back();
    for (std::size_t index = 1; index < tokens.size(); ++index) {
      if (syntax.token(tokens[index]).kind == TokenKind::Comma) {
        operands.emplace_back();
      } else {
        operands.back().push_back(tokens[index]);
      }
    }
    return operands;
  }

  [[nodiscard]] ParsedInstruction parse_instruction(
      const SyntaxTree &syntax, const SyntaxNode &node) {
    ParsedInstruction result;
    const std::vector<std::uint32_t> tokens = row_tokens(syntax, node);
    if (tokens.empty()) {
      diagnostics_.error(node.range, "empty assembly instruction");
      return result;
    }
    const std::string mnemonic = token_text(syntax, tokens.front());
    const std::vector<std::vector<std::uint32_t>> operands =
        instruction_operands(syntax, tokens);
    auto require_register = [&](std::size_t index) -> std::optional<Register> {
      if (index >= operands.size()) return std::nullopt;
      return operand_register(syntax, operands[index]);
    };
    auto wrong_shape = [&]() {
      diagnostics_.error(
          node.range, "invalid operands for AArch64 instruction '" + mnemonic + "'");
    };

    if (mnemonic == "add" || mnemonic == "sub" || mnemonic == "and" ||
        mnemonic == "orr" || mnemonic == "eor" || mnemonic == "mul" ||
        mnemonic == "lsl" || mnemonic == "lsr" || mnemonic == "asr") {
      const std::optional<Register> destination = require_register(0);
      const std::optional<Register> left = require_register(1);
      const std::optional<Register> right = require_register(2);
      bool third_valid = right.has_value();
      if ((mnemonic == "add" || mnemonic == "sub") && operands.size() == 3) {
        third_valid = third_valid || immediate_operand(syntax, operands[2], 4095);
      }
      if ((mnemonic == "lsl" || mnemonic == "lsr" || mnemonic == "asr") &&
          operands.size() == 3) {
        const std::uint64_t maximum = destination.has_value() && destination->bits == 32
            ? 31
            : 63;
        third_valid = third_valid || immediate_operand(syntax, operands[2], maximum);
      }
      if (operands.size() != 3 || !destination.has_value() || !left.has_value() ||
          !third_valid || destination->bits != left->bits ||
          destination->register_class != RegisterClass::General ||
          left->register_class != RegisterClass::General ||
          (right.has_value() && right->bits != destination->bits)) {
        wrong_shape();
        return result;
      }
      result.writes.push_back(*destination);
      result.reads.push_back(*left);
      if (right.has_value()) result.reads.push_back(*right);
      return result;
    }
    if (mnemonic == "mov") {
      const std::optional<Register> destination = require_register(0);
      const std::optional<Register> source = require_register(1);
      const bool source_valid = source.has_value() ||
          (operands.size() == 2 && immediate_operand(syntax, operands[1], 65535));
      if (operands.size() != 2 || !destination.has_value() || !source_valid ||
          destination->register_class != RegisterClass::General ||
          (source.has_value() &&
           (source->register_class != RegisterClass::General ||
            source->bits != destination->bits))) {
        wrong_shape();
        return result;
      }
      result.writes.push_back(*destination);
      if (source.has_value()) result.reads.push_back(*source);
      return result;
    }
    if (mnemonic == "cmp") {
      const std::optional<Register> left = require_register(0);
      const std::optional<Register> right = require_register(1);
      const bool right_valid = right.has_value() ||
          (operands.size() == 2 && immediate_operand(syntax, operands[1], 4095));
      if (operands.size() != 2 || !left.has_value() || !right_valid ||
          left->register_class != RegisterClass::General ||
          (right.has_value() &&
           (right->register_class != RegisterClass::General ||
            right->bits != left->bits))) {
        wrong_shape();
        return result;
      }
      result.reads.push_back(*left);
      if (right.has_value()) result.reads.push_back(*right);
      result.writes_flags = true;
      return result;
    }
    if (mnemonic == "fadd" || mnemonic == "fsub" || mnemonic == "fmul" ||
        mnemonic == "fdiv") {
      const std::optional<Register> destination = require_register(0);
      const std::optional<Register> left = require_register(1);
      const std::optional<Register> right = require_register(2);
      if (operands.size() != 3 || !destination.has_value() ||
          !left.has_value() || !right.has_value() ||
          destination->register_class != RegisterClass::Vector ||
          destination->bits > 64 || destination->bits != left->bits ||
          destination->bits != right->bits) {
        wrong_shape();
        return result;
      }
      result.writes.push_back(*destination);
      result.reads.push_back(*left);
      result.reads.push_back(*right);
      return result;
    }
    if (mnemonic == "fmov" || mnemonic == "fneg" || mnemonic == "fabs" ||
        mnemonic == "fsqrt") {
      const std::optional<Register> destination = require_register(0);
      const std::optional<Register> source = require_register(1);
      if (operands.size() != 2 || !destination.has_value() ||
          !source.has_value() ||
          destination->register_class != RegisterClass::Vector ||
          source->register_class != RegisterClass::Vector ||
          destination->bits > 64 || destination->bits != source->bits) {
        wrong_shape();
        return result;
      }
      result.writes.push_back(*destination);
      result.reads.push_back(*source);
      return result;
    }
    if (mnemonic == "fcmp") {
      const std::optional<Register> left = require_register(0);
      const std::optional<Register> right = require_register(1);
      if (operands.size() != 2 || !left.has_value() || !right.has_value() ||
          left->register_class != RegisterClass::Vector ||
          right->register_class != RegisterClass::Vector ||
          left->bits > 64 || left->bits != right->bits) {
        wrong_shape();
        return result;
      }
      result.reads.push_back(*left);
      result.reads.push_back(*right);
      result.writes_flags = true;
      return result;
    }
    if (mnemonic == "ldr" || mnemonic == "str" || mnemonic == "ldar" ||
        mnemonic == "stlr" || mnemonic == "ldrb" || mnemonic == "strb" ||
        mnemonic == "ldrh" || mnemonic == "strh") {
      const bool load = mnemonic == "ldr" || mnemonic == "ldar" ||
          mnemonic == "ldrb" || mnemonic == "ldrh";
      const std::optional<Register> value = require_register(0);
      const std::optional<Register> base = operands.size() == 2
          ? memory_base_register(syntax, operands[1])
          : std::nullopt;
      const bool byte_access = mnemonic == "ldrb" || mnemonic == "strb";
      const bool half_access = mnemonic == "ldrh" || mnemonic == "strh";
      const bool narrow_access = byte_access || half_access;
      const bool atomic = mnemonic == "ldar" || mnemonic == "stlr";
      if (operands.size() != 2 || !value.has_value() || !base.has_value() ||
          (narrow_access &&
           (value->register_class != RegisterClass::General ||
            value->bits != 32)) ||
          (atomic && value->register_class != RegisterClass::General)) {
        wrong_shape();
        return result;
      }
      const std::uint32_t access_bits = byte_access
          ? 8U
          : (half_access ? 16U : value->bits);
      result.reads.push_back(*base);
      if (load) {
        result.writes.push_back(*value);
      } else {
        result.reads.push_back(*value);
      }
      result.memory_accesses.push_back({*base, access_bits});
      return result;
    }
    if (mnemonic == "dmb" || mnemonic == "dsb") {
      if (operands.size() != 1 || operands[0].size() != 1) {
        wrong_shape();
        return result;
      }
      const std::string domain = token_text(syntax, operands[0][0]);
      if (domain != "sy" && domain != "ish" && domain != "ishld" &&
          domain != "ishst") {
        wrong_shape();
      }
      return result;
    }
    if (mnemonic == "isb") {
      if (!operands.empty() &&
          (operands.size() != 1 || operands[0].size() != 1 ||
           token_text(syntax, operands[0][0]) != "sy")) {
        wrong_shape();
      }
      return result;
    }
    if (mnemonic == "nop") {
      if (!operands.empty()) wrong_shape();
      return result;
    }
    diagnostics_.error(
        node.range,
        "unsupported instruction '" + mnemonic +
            "' in parsed AArch64 assembly");
    return result;
  }

  [[nodiscard]] std::vector<TypeId> output_types(
      TypeId result_type, std::size_t count, SourceRange range, bool expression) {
    std::vector<TypeId> result;
    if (!expression) {
      if (count != 0) {
        diagnostics_.error(range, "assembly statement cannot declare value outputs");
      }
      return result;
    }
    const Type &type = semantic_.types.type(result_type);
    if (type.kind == TypeKind::Tuple) {
      result = type.members;
    } else if (type.kind != TypeKind::Void && type.kind != TypeKind::Invalid) {
      result.push_back(result_type);
    }
    if (result.size() != count) {
      diagnostics_.error(
          range,
          "assembly output count does not match the declared expression result type");
    }
    return result;
  }

  [[nodiscard]] AssemblyRegion analyze_region(
      SyntaxReference reference,
      TypeId result_type,
      const std::vector<TypeId> &input_types,
      bool expression) {
    AssemblyRegion result;
    result.syntax = reference;
    result.result_type = result_type;
    const SyntaxTree *syntax = tree(reference.file);
    if (syntax == nullptr || !reference.node.is_valid()) {
      diagnostics_.error(SourceRange::invalid(), "assembly site has no syntax tree");
      return result;
    }
    const SyntaxNode &node = syntax->node(reference.node);
    if (node.token_begin + 1 >= node.token_end) {
      diagnostics_.error(node.range, "assembly site has no architecture");
      return result;
    }
    const std::string architecture = token_text(*syntax, node.token_begin + 1);
    if (architecture != target_.parsed_assembly_architecture) {
      diagnostics_.error(
          syntax->token(node.token_begin + 1).range,
          "assembly architecture '" + architecture + "' does not match target '" +
              target_.parsed_assembly_architecture + "'");
    }

    std::vector<DeclaredRegister> inputs;
    std::vector<DeclaredRegister> outputs;
    std::vector<DeclaredRegister> clobbers;
    bool clobber_flags = false;
    bool clobber_memory = false;
    std::vector<const SyntaxNode *> instructions;
    std::size_t input_index = 0;
    for (NodeId child_id : node.children) {
      const SyntaxNode &child = syntax->node(child_id);
      const std::vector<std::uint32_t> tokens = row_tokens(*syntax, child);
      if (child.kind == NodeKind::AsmInput || child.kind == NodeKind::AsmOutput) {
        if (tokens.size() < 2) continue;
        const Register reg = parse_register(token_text(*syntax, tokens[1]));
        if (!reg.valid || reg.zero) {
          diagnostics_.error(
              syntax->token(tokens[1]).range,
              "assembly directive requires an x0-x30 or w0-w30 register");
          continue;
        }
        const bool duplicate = child.kind == NodeKind::AsmInput
            ? contains_register(inputs, reg) || contains_register(clobbers, reg)
            : contains_register(outputs, reg) || contains_register(clobbers, reg);
        if (duplicate) {
          diagnostics_.error(
              syntax->token(tokens[1]).range,
              "physical assembly register is declared more than once");
          continue;
        }
        if (child.kind == NodeKind::AsmInput) {
          if (input_index >= input_types.size()) {
            diagnostics_.error(child.range, "assembly input has no checked HIR value");
          } else {
            if (!runtime_register_type(input_types[input_index], reg)) {
              diagnostics_.error(
                  child.range,
                  "assembly input type does not match fixed register width");
            }
            inputs.push_back(
                {reg, syntax->token(tokens[1]).range, input_types[input_index]});
          }
          ++input_index;
        } else {
          outputs.push_back({reg, syntax->token(tokens[1]).range, {}});
        }
      } else if (child.kind == NodeKind::AsmClobber) {
        if (tokens.size() != 2) {
          diagnostics_.error(child.range, "clobber requires one register, flags, or memory");
          continue;
        }
        const TokenKind kind = syntax->token(tokens[1]).kind;
        if (kind == TokenKind::KeywordFlags) {
          if (clobber_flags) diagnostics_.error(child.range, "duplicate flags clobber");
          clobber_flags = true;
        } else if (kind == TokenKind::KeywordMemory) {
          if (clobber_memory) diagnostics_.error(child.range, "duplicate memory clobber");
          clobber_memory = true;
        } else {
          const Register reg = parse_register(token_text(*syntax, tokens[1]));
          if (!reg.valid || reg.zero || contains_register(inputs, reg) ||
              contains_register(outputs, reg) || contains_register(clobbers, reg)) {
            diagnostics_.error(
                child.range,
                "invalid, conflicting, or duplicate register clobber");
          } else {
            clobbers.push_back({reg, syntax->token(tokens[1]).range, {}});
          }
        }
      } else if (child.kind == NodeKind::AsmInstruction) {
        instructions.push_back(&child);
      } else if (child.kind == NodeKind::SynthesisAssembly) {
        // The provider-independent front end keeps a typed, anchored assembly
        // obligation, but native emission must never silently drop it.  A
        // resolver will replace this node with checked instruction rows before
        // the same analyzer is run again.
        diagnostics_.error(
            child.range,
            "unresolved assembly synthesis prevents native lowering");
      }
    }
    result.input_count = inputs.size();
    result.output_count = outputs.size();
    if (input_index != input_types.size()) {
      diagnostics_.error(node.range, "assembly HIR input count differs from directives");
    }

    const std::vector<TypeId> expected_outputs =
        output_types(result_type, outputs.size(), node.range, expression);
    for (std::size_t index = 0;
         index < outputs.size() && index < expected_outputs.size();
         ++index) {
      outputs[index].type = expected_outputs[index];
      if (!runtime_register_type(expected_outputs[index], outputs[index].reg)) {
        diagnostics_.error(
            outputs[index].range,
            "assembly output type does not match fixed register width");
      }
      for (const DeclaredRegister &input : inputs) {
        if (same_physical(input.reg, outputs[index].reg) &&
            (input.reg.bits != outputs[index].reg.bits ||
             input.type != expected_outputs[index])) {
          diagnostics_.error(
              outputs[index].range,
              "tied assembly input and output must use one register view and type");
        }
      }
    }

    std::vector<Register> initialized;
    std::vector<Register> written;
    bool typed_memory_dependency = false;
    for (const DeclaredRegister &input : inputs) initialized.push_back(input.reg);
    for (const SyntaxNode *instruction_node : instructions) {
      const ParsedInstruction instruction =
          parse_instruction(*syntax, *instruction_node);
      for (Register reg : instruction.reads) {
        if (!reg.zero && !contains_register(initialized, reg)) {
          diagnostics_.error(
              instruction_node->range,
              "assembly instruction reads register '" + reg.name +
                  "' before a declared input or prior write");
        }
      }
      for (Register reg : instruction.writes) {
        if (reg.zero) continue;
        if (!contains_register(outputs, reg) && !contains_register(clobbers, reg)) {
          diagnostics_.error(
              instruction_node->range,
              "written register '" + reg.name +
                  "' must be declared as an output or clobber");
        }
        if (!contains_register(initialized, reg)) initialized.push_back(reg);
        if (!contains_register(written, reg)) written.push_back(reg);
      }
      if (instruction.writes_flags && !clobber_flags) {
        diagnostics_.error(
            instruction_node->range,
            "instruction writes condition flags without 'clobber flags'");
      }
      for (const auto &[base, bits] : instruction.memory_accesses) {
        const DeclaredRegister *input = declared_input(inputs, base);
        if (input != nullptr && typed_memory_access(*input, bits)) {
          typed_memory_dependency = true;
        } else if (!clobber_memory) {
          diagnostics_.error(
              instruction_node->range,
              "assembly memory access is not typed by a matching pointer input; "
              "declare 'clobber memory' for an untyped access");
        }
      }
      if (!result.instruction_text.empty()) result.instruction_text += "\n\t";
      result.instruction_text += trimmed_source(sources_, instruction_node->range);
    }
    for (const DeclaredRegister &output : outputs) {
      if (!contains_register(written, output.reg)) {
        diagnostics_.error(
            output.range,
            "assembly output register is never written by the instruction list");
      }
    }

    bool first_constraint = true;
    auto append_constraint = [&](const std::string &constraint) {
      if (!first_constraint) result.llvm_constraints += ',';
      result.llvm_constraints += constraint;
      first_constraint = false;
    };
    for (const DeclaredRegister &output : outputs) {
      append_constraint("={" + output.reg.name + "}");
    }
    for (const DeclaredRegister &input : inputs) {
      std::optional<std::size_t> tied_output;
      for (std::size_t index = 0; index < outputs.size(); ++index) {
        if (same_physical(outputs[index].reg, input.reg)) {
          tied_output = index;
          break;
        }
      }
      append_constraint(tied_output.has_value()
          ? std::to_string(*tied_output)
          : "{" + input.reg.name + "}");
    }
    for (const DeclaredRegister &clobber : clobbers) {
      append_constraint("~{" + clobber.reg.name + "}");
    }
    if (clobber_flags) append_constraint("~{cc}");
    if (clobber_memory || typed_memory_dependency) {
      append_constraint("~{memory}");
    }
    return result;
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const TargetProfile &target_;
  const SemanticPackage &semantic_;
  const HirProgram &hir_;
  DiagnosticSink &diagnostics_;
};

} // namespace

const AssemblyRegion *AssemblyProgram::find(SyntaxReference syntax) const {
  for (const AssemblyRegion &region : regions) {
    if (region.syntax == syntax) return &region;
  }
  return nullptr;
}

AssemblyProgram analyze_aarch64_assembly(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetProfile &target,
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics) {
  return Analyzer(sources, loaded, target, semantic, hir, diagnostics).run();
}

} // namespace draft
