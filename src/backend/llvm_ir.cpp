// LLVM IR text emission for the first AArch64 macOS backend.
//
// This is a deliberately small printer over Draft MIR. LLVM performs target
// instruction selection and object emission, but it does not decide Draft
// evaluation order, bounds behavior, defer behavior, or source control flow;
// those choices are already explicit in MIR. Unsupported semantic forms produce
// diagnostics instead of silently selecting an ABI or representation.

#include "backend/llvm_ir.h"

#include "interop/aarch64_abi.h"
#include "sema/ieee_float.h"
#include "syntax/literal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
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
  SymbolId global;
  // One MIR/global constant may contain several strings nested inside arrays,
  // tuples, structs, or union payloads.  The logical aggregate path gives each
  // leaf a stable module identity without teaching ConstantValue about LLVM.
  std::vector<std::size_t> path;
};

struct ConstantSite {
  std::size_t procedure = std::numeric_limits<std::size_t>::max();
  std::size_t instruction = std::numeric_limits<std::size_t>::max();
  SymbolId global;
  std::vector<std::size_t> path;
};

struct TypedConstantOperand {
  std::string type;
  std::string value;
};

// Runtime assertions need source spellings that are not ordinary MIR string
// values. The site table connects those module constants back to the Assert
// instruction without adding target/runtime details to target-independent MIR.
struct AssertionSite {
  std::size_t procedure = 0;
  std::size_t instruction = 0;
  std::size_t condition_string = 0;
  std::size_t file_string = 0;
  LineColumn location;
};

struct BoundsSite {
  std::size_t procedure = 0;
  std::size_t instruction = 0;
  std::size_t file_string = 0;
  LineColumn location;
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

[[nodiscard]] std::string llvm_inline_assembly(std::string_view text) {
  std::string result;
  for (char character : text) {
    if (character == '\n') {
      result += "\\0A\\09";
    } else if (character == '\\' || character == '"') {
      result.push_back('\\');
      result.push_back(character);
    } else if (character == '$') {
      // LLVM uses $N for operand substitution. Draft's fixed-register syntax
      // has no placeholders, so a literal dollar must remain literal.
      result += "$$";
    } else {
      result.push_back(character);
    }
  }
  return result;
}

class Emitter {
public:
  Emitter(
      const TargetProfile &target,
      const SourceManager &sources,
      const LlvmIrOptions &options,
      const SemanticPackage &semantic,
      const ConstantTable &global_initializers,
      const MirProgram &mir,
      DiagnosticSink &diagnostics)
      : target_(target), sources_(sources), options_(options), semantic_(semantic),
        global_initializers_(global_initializers), mir_(mir),
        diagnostics_(diagnostics) {}

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
    if (value.kind == TypeKind::Enum && value.element.is_valid()) {
      return signed_integer(value.element);
    }
    return value.kind == TypeKind::SignedInteger || value.kind == TypeKind::Rune;
  }

  [[nodiscard]] TypeKind runtime_scalar_kind(TypeId id) const {
    const Type &value = type(id);
    return value.kind == TypeKind::Distinct
        ? runtime_scalar_kind(value.element)
        : value.kind;
  }

  [[nodiscard]] TypeId runtime_scalar_id(TypeId id) const {
    while (type(id).kind == TypeKind::Distinct) id = type(id).element;
    return id;
  }

  [[nodiscard]] bool endian_requires_swap(TypeId id) const {
    const Type &storage = type(runtime_scalar_id(id));
    if (storage.kind != TypeKind::EndianScalar) return false;
    const bool target_is_little = target_.facts.byte_order == "little";
    return (storage.scalar_byte_order == ScalarByteOrder::Little &&
            !target_is_little) ||
        (storage.scalar_byte_order == ScalarByteOrder::Big && target_is_little);
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

  [[nodiscard]] bool is_parametric_template_type(TypeId id) const {
    for (std::size_t index = 0; index < semantic_.symbols.symbol_count(); ++index) {
      const Symbol &symbol = semantic_.symbols.symbol(
          SymbolId{static_cast<std::uint32_t>(index)});
      if (symbol.kind == SymbolKind::Type && symbol.flags.parametric &&
          symbol.type == id) {
        return true;
      }
    }
    return false;
  }

  void emit_nominal_types() {
    for (std::size_t index = 0; index < semantic_.types.size(); ++index) {
      const TypeId id{static_cast<std::uint32_t>(index)};
      const Type &value = type(id);
      if (is_parametric_template_type(id)) continue;
      if (value.kind == TypeKind::Struct) {
        // A packed LLVM body plus explicit byte fields reproduces the semantic
        // offsets exactly. This is required for @align tail stride and for a
        // nested over-aligned member; LLVM's implicit aggregate alignment does
        // not know Draft's requested_alignment metadata.
        output_ << "%draft.type." << index << " = type <{ ";
        std::uint64_t cursor = 0;
        bool emitted = false;
        for (std::size_t member = 0; member < value.members.size(); ++member) {
          const std::uint64_t offset = member < value.member_offsets.size()
              ? value.member_offsets[member]
              : cursor;
          if (offset > cursor) {
            if (emitted) output_ << ", ";
            output_ << '[' << offset - cursor << " x i8]";
            emitted = true;
          }
          if (emitted) output_ << ", ";
          output_ << llvm_type(value.members[member]);
          emitted = true;
          cursor = offset + type(value.members[member]).layout.size;
        }
        if (value.layout.known && value.layout.size > cursor) {
          if (emitted) output_ << ", ";
          output_ << '[' << value.layout.size - cursor << " x i8]";
        }
        output_ << " }>\n";
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

  void collect_constant_strings(
      const ConstantValue &value,
      std::size_t procedure,
      std::size_t instruction,
      SymbolId global,
      std::vector<std::size_t> &path) {
    if (value.kind == ConstantKind::String) {
      strings_.push_back(
          {procedure, instruction, value.text, global, path});
      return;
    }
    if (value.kind != ConstantKind::Aggregate &&
        value.kind != ConstantKind::EnumLabel) {
      return;
    }
    for (std::size_t index = 0; index < value.elements.size(); ++index) {
      path.push_back(index);
      collect_constant_strings(
          value.elements[index], procedure, instruction, global, path);
      path.pop_back();
    }
  }

  void collect_strings() {
    for (const ConstantBinding &binding : global_initializers_.bindings) {
      const Symbol &symbol = semantic_.symbols.symbol(binding.symbol);
      if (symbol.kind != SymbolKind::Variable || symbol.flags.foreign) continue;
      std::vector<std::size_t> path;
      collect_constant_strings(
          binding.value,
          std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max(),
          binding.symbol,
          path);
    }
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
        if (instruction.kind == MirInstructionKind::Constant) {
          std::vector<std::size_t> path;
          collect_constant_strings(
              instruction.constant,
              procedure_index,
              instruction_index,
              {},
              path);
        }
        if (instruction.kind == MirInstructionKind::Assert &&
            !instruction.operands.empty()) {
          SourceRange condition_range = instruction.range;
          const MirValueId condition = instruction.operands.front();
          if (condition.is_valid() &&
              static_cast<std::size_t>(condition.value) < procedure.values.size()) {
            const MirInstructionId definition =
                procedure.value(condition).definition;
            if (definition.is_valid() &&
                static_cast<std::size_t>(definition.value) <
                    procedure.instructions.size()) {
              condition_range = procedure.instruction(definition).range;
            }
          }

          // Assertion MIR always originates in parsed source. Keeping a small
          // defensive fallback makes malformed hand-built MIR diagnosable
          // without dereferencing an invalid FileId in the backend.
          std::string condition_text;
          std::string file_path;
          LineColumn location;
          if (condition_range.is_valid() && instruction.range.is_valid()) {
            condition_text = std::string(sources_.text(condition_range));
            file_path = sources_.file(instruction.range.begin.file).display_path;
            location = sources_.line_column(instruction.range.begin);
          }
          const std::size_t condition_string = strings_.size();
          strings_.push_back(
              {std::numeric_limits<std::size_t>::max(),
               std::numeric_limits<std::size_t>::max(),
               std::move(condition_text),
               {},
               {}});
          const std::size_t file_string = strings_.size();
          strings_.push_back(
              {std::numeric_limits<std::size_t>::max(),
               std::numeric_limits<std::size_t>::max(),
               std::move(file_path),
               {},
               {}});
          assertion_sites_.push_back(
              {procedure_index,
               instruction_index,
               condition_string,
               file_string,
               location});
        }
        if (instruction.kind == MirInstructionKind::BoundsCheck ||
            instruction.kind == MirInstructionKind::SliceBoundsCheck) {
          std::string file_path;
          LineColumn location;
          if (instruction.range.is_valid()) {
            file_path = sources_.file(instruction.range.begin.file).display_path;
            location = sources_.line_column(instruction.range.begin);
          }
          const std::size_t file_string = strings_.size();
          strings_.push_back(
              {std::numeric_limits<std::size_t>::max(),
               std::numeric_limits<std::size_t>::max(),
               std::move(file_path),
               {},
               {}});
          bounds_sites_.push_back(
              {procedure_index, instruction_index, file_string, location});
        }
      }
    }
  }

  void emit_strings() {
    for (std::size_t index = 0; index < strings_.size(); ++index) {
      output_ << "@.draft.string." << index
              << " = private unnamed_addr constant ["
              << strings_[index].value.size() + 1 << " x i8] c\""
              << llvm_bytes(strings_[index].value) << "\\00\", align 1\n";
    }
    if (!strings_.empty()) output_ << '\n';
  }

  void emit_runtime_declarations() {
    // LLVM intrinsics are declared in every package module that may use them.
    // The Draft runtime helpers below are different: dependency modules only
    // declare them, while the executable root owns their single definitions
    // and the one process-wide root context.
    output_ << "declare void @llvm.trap() cold noreturn nounwind\n"
            << "declare i16 @llvm.bswap.i16(i16)\n"
            << "declare i32 @llvm.bswap.i32(i32)\n"
            << "declare i64 @llvm.bswap.i64(i64)\n"
            << "declare i128 @llvm.bswap.i128(i128)\n\n";
    if (!options_.emit_program_entry) {
      output_ << "declare hidden void @__draft.assert(ptr, i1, { ptr, i64 }, "
                 "{ ptr, i64 }, { ptr, i64 }, i64, i64)\n"
              << "declare hidden void @__draft.bounds(i64, i64, ptr, i64, i64)\n"
              << "declare hidden void @__draft.slice_bounds("
                 "i64, i64, i64, ptr, i64, i64)\n"
              << "declare hidden void @\"__draft.runtime.attach_thread\"()\n\n";
      return;
    }

    // These layout-only handle records are the physical ABI shared with the
    // compiler-distributed core/runtime Context declaration.  Each provider is
    // a procedure pointer paired with provider-owned state.  Keeping the full
    // Context shape here, even before all providers are populated, prevents an
    // early one-field bootstrap layout from becoming an accidental ABI.
    output_ << "%draft.runtime.Allocator = type { ptr, ptr }\n"
            << "%draft.runtime.Logger = type { ptr, ptr }\n"
            << "%draft.runtime.RandomGenerator = type { ptr, ptr }\n"
            << "%draft.runtime.Context = type { "
               "%draft.runtime.Allocator, %draft.runtime.Allocator, ptr, "
               "%draft.runtime.Logger, %draft.runtime.RandomGenerator, "
               "ptr, i64, ptr }\n"
            << "@__draft.process_argc = internal global i32 0, align 4\n"
            << "@__draft.process_argv = internal global ptr null, align 8\n"
            << "@__draft.root_context = internal global %draft.runtime.Context "
               "{ %draft.runtime.Allocator { ptr @__draft.default_allocator, "
               "ptr null }, "
               "%draft.runtime.Allocator { ptr @__draft.default_allocator, "
               "ptr null }, "
               "ptr @__draft.default_assertion_failure, "
               "%draft.runtime.Logger { ptr @__draft.default_logger, ptr null }, "
               "%draft.runtime.RandomGenerator { ptr @__draft.default_random, "
               "ptr null }, ptr null, i64 0, ptr null }, align 8\n"
            << "@__draft.thread_context = internal thread_local global "
               "%draft.runtime.Context zeroinitializer, align 8\n"
            << "@__draft.thread_context_initialized = internal thread_local "
               "global i1 false, align 1\n\n";

    const std::string assertion_prefix = "Draft assertion failed: ";
    const std::string bounds_prefix = "Draft bounds check failed";
    const std::string newline = "\n";
    output_ << "@.draft.runtime.assertion_prefix = private unnamed_addr "
               "constant [" << assertion_prefix.size() << " x i8] c\""
            << llvm_bytes(assertion_prefix) << "\", align 1\n"
            << "@.draft.runtime.bounds_prefix = private unnamed_addr constant ["
            << bounds_prefix.size() << " x i8] c\""
            << llvm_bytes(bounds_prefix) << "\", align 1\n"
            << "@.draft.runtime.newline = private unnamed_addr constant [1 x i8] "
               "c\"" << llvm_bytes(newline) << "\", align 1\n\n"
            << "declare i64 @write(i32, ptr, i64)\n"
            << "declare ptr @calloc(i64, i64)\n"
            << "declare ptr @realloc(ptr, i64)\n"
            << "declare void @free(ptr)\n"
            << "declare i32 @posix_memalign(ptr, i64, i64)\n"
            << "declare void @arc4random_buf(ptr, i64)\n"
            << "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n"
            << "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n\n"
            // The runtime allocator is an ordinary Draft procedure pointer:
            // its first physical argument is the hidden Context, followed by
            // provider state and the source-visible allocator ABI. Allocation
            // returns cleared storage so typed `memory.new` observes Draft's
            // zero value. Large alignments use posix_memalign; aligned resize
            // preserves the old allocation on failure and copies only the
            // common live prefix before releasing it.
            << "define internal ptr @__draft.allocate_zeroed("
               "i64 %size, i64 %alignment) {\n"
            << "entry:\n"
            << "  %is.empty = icmp eq i64 %size, 0\n"
            << "  br i1 %is.empty, label %empty, label %choose\n"
            << "choose:\n"
            << "  %ordinary = icmp ule i64 %alignment, 16\n"
            << "  br i1 %ordinary, label %calloc, label %aligned\n"
            << "calloc:\n"
            << "  %calloc.memory = call ptr @calloc(i64 1, i64 %size)\n"
            << "  ret ptr %calloc.memory\n"
            << "aligned:\n"
            << "  %slot = alloca ptr, align 8\n"
            << "  store ptr null, ptr %slot, align 8\n"
            << "  %status = call i32 @posix_memalign("
               "ptr %slot, i64 %alignment, i64 %size)\n"
            << "  %aligned.ok = icmp eq i32 %status, 0\n"
            << "  br i1 %aligned.ok, label %clear, label %failed\n"
            << "clear:\n"
            << "  %aligned.memory = load ptr, ptr %slot, align 8\n"
            << "  call void @llvm.memset.p0.i64("
               "ptr %aligned.memory, i8 0, i64 %size, i1 false)\n"
            << "  ret ptr %aligned.memory\n"
            << "failed:\n"
            << "  ret ptr null\n"
            << "empty:\n"
            << "  ret ptr null\n"
            << "}\n\n"
            // The default logger deliberately owns no formatting policy beyond
            // a trailing newline. Applications can replace the context field
            // with any ordinary Draft provider record.
            << "define internal void @__draft.default_logger("
               "ptr %context, ptr %user, i8 %level, "
               "{ ptr, i64 } %message) {\n"
            << "entry:\n"
            << "  %message.pointer = extractvalue { ptr, i64 } %message, 0\n"
            << "  %message.length = extractvalue { ptr, i64 } %message, 1\n"
            << "  %write.message = call i64 @write("
               "i32 2, ptr %message.pointer, i64 %message.length)\n"
            << "  %write.newline = call i64 @write("
               "i32 2, ptr @.draft.runtime.newline, i64 1)\n"
            << "  ret void\n"
            << "}\n\n"
            // arc4random_buf is supplied by the pinned macOS runtime. It has
            // no failure result; the Draft provider therefore returns true
            // after filling the requested byte range, including an empty one.
            << "define internal i1 @__draft.default_random("
               "ptr %context, ptr %user, ptr %output, i64 %count) {\n"
            << "entry:\n"
            << "  call void @arc4random_buf(ptr %output, i64 %count)\n"
            << "  ret i1 true\n"
            << "}\n\n"
            << "define internal ptr @__draft.default_allocator("
               "ptr %context, ptr %user, i8 %operation, ptr %old_memory, "
               "i64 %old_size, i64 %new_size, i64 %alignment) {\n"
            << "entry:\n"
            << "  switch i8 %operation, label %invalid [\n"
            << "    i8 0, label %allocate\n"
            << "    i8 1, label %resize\n"
            << "    i8 2, label %release\n"
            << "  ]\n"
            << "allocate:\n"
            << "  %allocated = call ptr @__draft.allocate_zeroed("
               "i64 %new_size, i64 %alignment)\n"
            << "  ret ptr %allocated\n"
            << "resize:\n"
            << "  %resize.empty = icmp eq i64 %new_size, 0\n"
            << "  br i1 %resize.empty, label %release, label %resize.choose\n"
            << "resize.choose:\n"
            << "  %resize.ordinary = icmp ule i64 %alignment, 16\n"
            << "  br i1 %resize.ordinary, label %resize.realloc, "
               "label %resize.aligned\n"
            << "resize.realloc:\n"
            << "  %resized = call ptr @realloc("
               "ptr %old_memory, i64 %new_size)\n"
            << "  ret ptr %resized\n"
            << "resize.aligned:\n"
            << "  %replacement = call ptr @__draft.allocate_zeroed("
               "i64 %new_size, i64 %alignment)\n"
            << "  %replacement.ok = icmp ne ptr %replacement, null\n"
            << "  br i1 %replacement.ok, label %resize.copy.check, "
               "label %resize.failed\n"
            << "resize.copy.check:\n"
            << "  %old.exists = icmp ne ptr %old_memory, null\n"
            << "  %old.smaller = icmp ult i64 %old_size, %new_size\n"
            << "  %copy.size = select i1 %old.smaller, "
               "i64 %old_size, i64 %new_size\n"
            << "  %copy.nonempty = icmp ne i64 %copy.size, 0\n"
            << "  %copy.required = and i1 %old.exists, %copy.nonempty\n"
            << "  br i1 %copy.required, label %resize.copy, "
               "label %resize.finish\n"
            << "resize.copy:\n"
            << "  call void @llvm.memcpy.p0.p0.i64("
               "ptr %replacement, ptr %old_memory, i64 %copy.size, i1 false)\n"
            << "  br label %resize.finish\n"
            << "resize.finish:\n"
            << "  call void @free(ptr %old_memory)\n"
            << "  ret ptr %replacement\n"
            << "resize.failed:\n"
            << "  ret ptr null\n"
            << "release:\n"
            << "  call void @free(ptr %old_memory)\n"
            << "  ret ptr null\n"
            << "invalid:\n"
            << "  ret ptr null\n"
            << "}\n\n"
            << "define internal void @__draft.default_assertion_failure("
               "ptr %context, { ptr, i64 } %condition_text, "
               "{ ptr, i64 } %message, { ptr, i64 } %file, "
               "i64 %line, i64 %column) {\n"
            << "entry:\n"
            << "  %condition.pointer = extractvalue { ptr, i64 } "
               "%condition_text, 0\n"
            << "  %condition.length = extractvalue { ptr, i64 } "
               "%condition_text, 1\n"
            << "  %write.prefix = call i64 @write(i32 2, ptr "
               "@.draft.runtime.assertion_prefix, i64 "
            << assertion_prefix.size() << ")\n"
            << "  %write.condition = call i64 @write(i32 2, "
               "ptr %condition.pointer, i64 %condition.length)\n"
            << "  %write.newline = call i64 @write(i32 2, ptr "
               "@.draft.runtime.newline, i64 1)\n"
            << "  ret void\n"
            << "}\n\n"
            << "define hidden void @__draft.assert(ptr %context, i1 %condition, "
               "{ ptr, i64 } %condition_text, { ptr, i64 } %message, "
               "{ ptr, i64 } %file, i64 %line, i64 %column) {\n"
            << "entry:\n"
            << "  br i1 %condition, label %ok, label %fail\n"
            << "fail:\n"
            << "  %has.context = icmp ne ptr %context, null\n"
            << "  br i1 %has.context, label %load.handler, label %trap\n"
            << "load.handler:\n"
            << "  %handler.slot = getelementptr %draft.runtime.Context, "
               "ptr %context, i32 0, i32 2\n"
            << "  %handler = load ptr, ptr %handler.slot, align 8\n"
            << "  %has.handler = icmp ne ptr %handler, null\n"
            << "  br i1 %has.handler, label %call.handler, label %trap\n"
            << "call.handler:\n"
            << "  call void %handler(ptr %context, "
               "{ ptr, i64 } %condition_text, { ptr, i64 } %message, "
               "{ ptr, i64 } %file, i64 %line, i64 %column)\n"
            << "  br label %trap\n"
            << "trap:\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  ret void\n"
            << "}\n\n"
            << "define internal void @__draft.bounds_failure(i8 %kind, "
               "i64 %first, i64 %second, i64 %length, ptr %file, "
               "i64 %line, i64 %column) {\n"
            << "entry:\n"
            << "  %write.bounds = call i64 @write(i32 2, ptr "
               "@.draft.runtime.bounds_prefix, i64 "
            << bounds_prefix.size() << ")\n"
            << "  %write.bounds.newline = call i64 @write(i32 2, ptr "
               "@.draft.runtime.newline, i64 1)\n"
            << "  ret void\n"
            << "}\n\n"
            << "define hidden void @__draft.bounds(i64 %index, i64 %length, "
               "ptr %file, i64 %line, i64 %column) {\n"
            << "entry:\n"
            << "  %inside = icmp ult i64 %index, %length\n"
            << "  br i1 %inside, label %ok, label %fail\n"
            << "fail:\n"
            << "  call void @__draft.bounds_failure(i8 0, i64 %index, i64 0, "
               "i64 %length, ptr %file, i64 %line, i64 %column)\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  ret void\n"
            << "}\n\n"
            << "define hidden void @__draft.slice_bounds("
               "i64 %low, i64 %high, "
               "i64 %length, ptr %file, i64 %line, i64 %column) {\n"
            << "entry:\n"
            << "  %ordered = icmp ule i64 %low, %high\n"
            << "  %inside = icmp ule i64 %high, %length\n"
            << "  %valid = and i1 %ordered, %inside\n"
            << "  br i1 %valid, label %ok, label %fail\n"
            << "fail:\n"
            << "  call void @__draft.bounds_failure(i8 1, i64 %low, i64 %high, "
               "i64 %length, ptr %file, i64 %line, i64 %column)\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  ret void\n"
            << "}\n\n";
    if (!semantic_.runtime_context_type.is_valid()) {
      error(
          SourceRange::invalid(),
          "hosted runtime has no semantic Context type");
      return;
    }
    const std::string context_type =
        llvm_type(semantic_.runtime_context_type);
    // Foreign-created threads lazily acquire a private Context snapshot. The
    // bridge's explicit Context pointer remains dynamic-call state; this TLS
    // copy is the persistent thread attachment used by later default-context
    // requests and future thread-owned runtime facilities.
    output_ << "define internal ptr @__draft.ensure_thread_context() {\n"
            << "entry:\n"
            << "  %initialized = load i1, ptr "
               "@__draft.thread_context_initialized, align 1\n"
            << "  br i1 %initialized, label %ready, label %initialize\n"
            << "initialize:\n"
            << "  %root = load %draft.runtime.Context, "
               "ptr @__draft.root_context, align 8\n"
            << "  store %draft.runtime.Context %root, "
               "ptr @__draft.thread_context, align 8\n"
            << "  store i1 true, ptr @__draft.thread_context_initialized, "
               "align 1\n"
            << "  br label %ready\n"
            << "ready:\n"
            << "  ret ptr @__draft.thread_context\n"
            << "}\n\n"
            << "define hidden void @\"__draft.runtime.attach_thread\"() {\n"
            << "entry:\n"
            << "  %thread.context = call ptr @__draft.ensure_thread_context()\n"
            << "  ret void\n"
            << "}\n\n"
            << "define hidden void @\"__draft.runtime.default_context\"("
               "ptr sret(" << context_type << ") align 8 %result) {\n"
            << "entry:\n"
            << "  %thread.context = call ptr @__draft.ensure_thread_context()\n"
            << "  %snapshot = load " << context_type
            << ", ptr %thread.context, align 8\n"
            << "  store " << context_type
            << " %snapshot, ptr %result, align 8\n"
            << "  ret void\n"
            << "}\n\n";
  }

  [[nodiscard]] std::string package_symbol_name(
      const PackageIdentity &identity, std::string_view name) const {
    return "@\"draft." + encoded_name(identity.root_identity) + "." +
        encoded_name(identity.root_relative_path) + "." + encoded_name(name) + "\"";
  }

  [[nodiscard]] std::string exact_linker_symbol(std::string_view name) const {
    return "@\"" + llvm_bytes(name) + "\"";
  }

  [[nodiscard]] std::string decoded_linker_name(
      std::string_view spelling) const {
    if (spelling.size() >= 2 && spelling.front() == '"' && spelling.back() == '"') {
      const std::optional<std::string> decoded =
          decode_string_literal(spelling, TokenKind::StringLiteral);
      return decoded.value_or(std::string());
    }
    return std::string(spelling);
  }

  [[nodiscard]] std::optional<std::string> native_symbol_name(
      SymbolId symbol_id) const {
    for (const NativeBinding &binding : semantic_.native_bindings) {
      if (binding.symbol == symbol_id) {
        const std::string decoded =
            decoded_linker_name(binding.linker_name_spelling);
        if (!decoded.empty()) return exact_linker_symbol(decoded);
      }
    }
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy == symbol_id &&
          !imported.native_linker_name_spelling.empty()) {
        const std::string decoded =
            decoded_linker_name(imported.native_linker_name_spelling);
        if (!decoded.empty()) return exact_linker_symbol(decoded);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool is_imported_symbol(SymbolId symbol_id) const {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.proxy == symbol_id) return true;
    }
    return false;
  }

  [[nodiscard]] bool root_runtime_defines(SymbolId symbol_id) const {
    if (!options_.emit_program_entry) return false;
    const std::optional<std::string> native = native_symbol_name(symbol_id);
    return native.has_value() &&
        *native == "@\"__draft.runtime.default_context\"";
  }

  [[nodiscard]] std::string symbol_name(SymbolId symbol_id) const {
    if (const std::optional<std::string> native = native_symbol_name(symbol_id)) {
      return *native;
    }
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

  [[nodiscard]] std::optional<std::string> procedure_constant_name(
      const ConstantValue &value) const {
    if (value.symbol_index != std::numeric_limits<std::uint32_t>::max() &&
        value.symbol_index < semantic_.symbols.symbol_count()) {
      const SymbolId symbol{value.symbol_index};
      if (semantic_.symbols.symbol(symbol).kind == SymbolKind::Procedure) {
        return symbol_name(symbol);
      }
    }
    if (value.root_identity.empty() || value.text.empty()) {
      return std::nullopt;
    }
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      if (imported.root_identity == value.root_identity &&
          imported.root_relative_path == value.root_relative_path &&
          imported.public_name == value.text &&
          semantic_.symbols.symbol(imported.proxy).kind == SymbolKind::Procedure) {
        return symbol_name(imported.proxy);
      }
    }
    if (value.root_identity == options_.package.root_identity &&
        value.root_relative_path == options_.package.root_relative_path) {
      const std::optional<SymbolId> local = semantic_.symbols.lookup_direct(
          semantic_.package_scope, value.text);
      if (local.has_value() &&
          semantic_.symbols.symbol(*local).kind == SymbolKind::Procedure) {
        return symbol_name(*local);
      }
    }
    return package_symbol_name(
        {value.root_identity, value.root_relative_path}, value.text);
  }

  [[nodiscard]] std::string homogeneous_llvm_type(
      const Aarch64CAbiType &abi) const {
    std::string element = "<invalid-hfa>";
    if (abi.homogeneous_element_bits == 16) element = "half";
    if (abi.homogeneous_element_bits == 32) element = "float";
    if (abi.homogeneous_element_bits == 64) element = "double";
    return "[" + std::to_string(abi.homogeneous_element_count) + " x " +
        element + "]";
  }

  [[nodiscard]] std::string integer_container_type(
      std::uint32_t bits, std::uint32_t count) const {
    if (count == 1) return "i" + std::to_string(bits);
    return "[" + std::to_string(count) + " x i" +
        std::to_string(bits) + "]";
  }

  [[nodiscard]] std::string c_parameter_type(TypeId type_id) const {
    const Aarch64CAbiType abi = c_abi_type(type_id);
    switch (abi.classification) {
    case Aarch64CAbiClass::Direct:
      return llvm_type(type_id);
    case Aarch64CAbiClass::HomogeneousFloatAggregate:
      return homogeneous_llvm_type(abi);
    case Aarch64CAbiClass::SmallAggregate:
      return integer_container_type(
          abi.argument_integer_bits, abi.argument_integer_count);
    case Aarch64CAbiClass::Indirect:
      return "ptr";
    case Aarch64CAbiClass::Illegal:
      return "<illegal-c-abi>";
    }
    return "<illegal-c-abi>";
  }

  // Darwin arm64 requires C integer values narrower than 32 bits to carry an
  // explicit extension contract. These LLVM attributes affect register bits
  // and therefore belong on declarations, definitions, and every call site.
  [[nodiscard]] std::string c_integer_extension(TypeId type_id) const {
    const Type &value = type(type_id);
    if (value.bit_width >= 32) return {};
    if (value.kind == TypeKind::SignedInteger) return "signext";
    if (value.kind == TypeKind::UnsignedInteger ||
        value.kind == TypeKind::BooleanStorage ||
        value.kind == TypeKind::EndianScalar) {
      return "zeroext";
    }
    return {};
  }

  [[nodiscard]] std::string c_result_type(TypeId type_id) const {
    if (type_id == semantic_.types.builtins().void_type) return "void";
    const Aarch64CAbiType abi = c_abi_type(type_id);
    switch (abi.classification) {
    case Aarch64CAbiClass::Direct:
      return llvm_type(type_id);
    case Aarch64CAbiClass::HomogeneousFloatAggregate:
      // Using the same explicit lane array for both directions avoids relying
      // on LLVM to rediscover HFA shape through Draft's opaque raw-union
      // storage or through nested source aggregate names.
      return homogeneous_llvm_type(abi);
    case Aarch64CAbiClass::SmallAggregate:
      return integer_container_type(
          abi.result_integer_bits, abi.result_integer_count);
    case Aarch64CAbiClass::Indirect:
      return "void";
    case Aarch64CAbiClass::Illegal:
      return "<illegal-c-abi>";
    }
    return "<illegal-c-abi>";
  }

  [[nodiscard]] Aarch64CAbiType function_result_abi(TypeId type_id) const {
    const TypeId result = function_result(type_id);
    if (result == semantic_.types.builtins().void_type) return {};
    return c_abi_type(result);
  }

  [[nodiscard]] Aarch64CAbiType c_abi_type(TypeId type_id) const {
    if (type_id == semantic_.runtime_context_type) {
      const Type &context = type(type_id);
      Aarch64CAbiType abi;
      abi.classification = Aarch64CAbiClass::Indirect;
      abi.size = context.layout.size;
      abi.alignment = context.layout.alignment;
      return abi;
    }
    return classify_aarch64_darwin_c_type(semantic_.types, type_id);
  }

  [[nodiscard]] std::string llvm_function_result(TypeId type_id) const {
    const Type &signature = type(type_id);
    const TypeId result = function_result(type_id);
    if (!signature.c_calling_convention) return llvm_type(result);
    const std::string extension = c_integer_extension(result);
    return extension.empty()
        ? c_result_type(result)
        : extension + " " + c_result_type(result);
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
    if (signature.c_calling_convention) {
      const TypeId logical_result = function_result(type_id);
      const Aarch64CAbiType result_abi = function_result_abi(type_id);
      if (result_abi.classification == Aarch64CAbiClass::Indirect) {
        result += "ptr sret(" + llvm_type(logical_result) + ") align " +
            std::to_string(result_abi.alignment);
        if (with_names) result += " %sret";
        emitted = true;
      }
    } else {
      result += "ptr";
      if (with_names) result += " %context";
      emitted = true;
    }
    for (std::size_t index = 0; index < parameter_count; ++index) {
      if (emitted) result += ", ";
      if (signature.c_calling_convention) {
        result += c_parameter_type(signature.members[index]);
        const std::string extension =
            c_integer_extension(signature.members[index]);
        if (!extension.empty()) result += " " + extension;
      } else {
        result += llvm_type(signature.members[index]);
      }
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
      if (symbol.kind != SymbolKind::Variable || symbol.flags.foreign ||
          !symbol.type.is_valid()) {
        continue;
      }
      output_ << symbol_name(symbol_id) << " = ";
      if (symbol.flags.is_thread_local) output_ << "thread_local ";
      output_ << "global ";
      const ConstantValue *initializer = global_initializers_.find(symbol_id);
      if (initializer != nullptr) {
        ConstantSite site;
        site.global = symbol_id;
        const std::optional<TypedConstantOperand> relocatable =
            relocatable_union_constant(
                *initializer, symbol.type, site, symbol.name_range);
        if (relocatable.has_value()) {
          // Opaque LLVM pointers let the allocation use an initializer-specific
          // packed storage type while every Draft load still uses the canonical
          // union value type.  The packed field keeps pointer/string relocations
          // intact at the exact language-defined payload offset.
          output_ << relocatable->type << ' ' << relocatable->value;
        } else {
          output_ << llvm_type(symbol.type) << ' ' << constant_operand(
              *initializer, symbol.type, std::move(site), symbol.name_range);
        }
      } else {
        output_ << llvm_type(symbol.type) << " zeroinitializer";
      }
      output_ << ", align " << type(symbol.type).layout.alignment << "\n";
    }
    output_ << '\n';
  }

  void emit_external_declarations() {
    for (const ImportedSymbol &imported : semantic_.imported_symbols) {
      const Symbol &symbol = semantic_.symbols.symbol(imported.proxy);
      if (root_runtime_defines(imported.proxy)) continue;
      if (symbol.kind == SymbolKind::Procedure) {
        // The imported public template is symbolic and can contain
        // TypeParameter pseudo-types. Calls always reference one of the
        // concrete imported instance proxies created by body checking.
        if (symbol.flags.parametric) continue;
        output_ << "declare " << llvm_function_result(symbol.type) << ' '
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
      // Parametric source declarations are symbolic templates. Concrete private
      // instance symbols carry every executable body and are emitted below;
      // declaring the template would leak TypeParameter pseudo-types into LLVM.
      if (symbol.kind == SymbolKind::Procedure &&
          !symbol.flags.parametric &&
          !has_body(symbol_id) &&
          !is_imported_symbol(symbol_id) &&
          !root_runtime_defines(symbol_id)) {
        output_ << "declare " << llvm_function_result(symbol.type) << ' '
                << symbol_name(symbol_id)
                << function_signature(symbol.type, false) << "\n";
      } else if (symbol.kind == SymbolKind::Variable && symbol.flags.foreign) {
        output_ << symbol_name(symbol_id) << " = external ";
        if (symbol.flags.is_thread_local) output_ << "thread_local ";
        output_ << "global " << llvm_type(symbol.type) << "\n";
      }
    }
    output_ << '\n';
  }

  [[nodiscard]] std::optional<std::size_t> string_index(
      std::size_t procedure,
      std::size_t instruction,
      const std::vector<std::size_t> &path = {}) const {
    for (std::size_t index = 0; index < strings_.size(); ++index) {
      if (strings_[index].procedure == procedure &&
          strings_[index].instruction == instruction &&
          strings_[index].path == path) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> global_string_index(
      SymbolId symbol,
      const std::vector<std::size_t> &path = {}) const {
    for (std::size_t index = 0; index < strings_.size(); ++index) {
      if (strings_[index].global == symbol && strings_[index].path == path) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> constant_string_index(
      const ConstantSite &site) const {
    return site.global.is_valid()
        ? global_string_index(site.global, site.path)
        : string_index(site.procedure, site.instruction, site.path);
  }

  [[nodiscard]] const AssertionSite *assertion_site(
      std::size_t procedure, std::size_t instruction) const {
    for (const AssertionSite &site : assertion_sites_) {
      if (site.procedure == procedure && site.instruction == instruction) {
        return &site;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const BoundsSite *bounds_site(
      std::size_t procedure, std::size_t instruction) const {
    for (const BoundsSite &site : bounds_sites_) {
      if (site.procedure == procedure && site.instruction == instruction) {
        return &site;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::string string_constant_operand(std::size_t index) const {
    return "{ ptr @.draft.string." + std::to_string(index) + ", i64 " +
        std::to_string(strings_[index].value.size()) + " }";
  }

  [[nodiscard]] bool target_uses_little_endian() const {
    return target_.facts.byte_order == "little";
  }

  [[nodiscard]] bool scalar_uses_little_endian(TypeId type_id) const {
    const Type &storage = type(runtime_scalar_id(type_id));
    if (storage.kind == TypeKind::EndianScalar) {
      return storage.scalar_byte_order != ScalarByteOrder::Big;
    }
    return target_uses_little_endian();
  }

  [[nodiscard]] std::optional<IeeeBinaryFormat> ieee_format(
      TypeId type_id) const {
    Type value = type(runtime_scalar_id(type_id));
    if (value.kind == TypeKind::EndianScalar && value.element.is_valid()) {
      value = type(value.element);
    }
    return value.kind == TypeKind::Float
        ? ieee_format_for_width(value.bit_width)
        : std::nullopt;
  }

  [[nodiscard]] bool write_integer_bytes(
      const BigInteger &value,
      TypeId type_id,
      std::vector<std::uint8_t> &bytes,
      std::uint64_t offset,
      SourceRange range) {
    const Type &storage = type(runtime_scalar_id(type_id));
    const std::uint32_t bits = integer_bits(type_id);
    const std::uint64_t size = storage.layout.size;
    if (bits == 0 || size == 0 || offset > bytes.size() ||
        size > bytes.size() - offset) {
      error(range, "integer constant does not fit union storage");
      return false;
    }

    // Convert negative mathematical integers to the finite-width two's-
    // complement bit pattern before extracting bytes.  All values have already
    // passed semantic range/conversion checks, so masking is defensive rather
    // than a second language conversion.
    const BigInteger modulus = BigInteger::from_u64(1).shifted_left(bits);
    BigInteger encoded = value;
    if (encoded.is_negative()) encoded = encoded.added(modulus);
    BigInteger quotient;
    BigInteger remainder;
    if (!encoded.divide(modulus, quotient, remainder)) {
      error(range, "could not encode integer constant bytes");
      return false;
    }
    encoded = std::move(remainder);

    const bool little = scalar_uses_little_endian(type_id);
    for (std::uint64_t byte_index = 0; byte_index < size; ++byte_index) {
      const std::optional<std::uint64_t> byte = encoded
          .shifted_right(static_cast<std::size_t>(byte_index * 8U))
          .bitwise_and(BigInteger::from_u64(0xffU))
          .to_u64();
      if (!byte.has_value()) {
        error(range, "could not encode integer constant byte");
        return false;
      }
      const std::uint64_t destination = little
          ? offset + byte_index
          : offset + size - byte_index - 1U;
      bytes[static_cast<std::size_t>(destination)] =
          static_cast<std::uint8_t>(*byte);
    }
    return true;
  }

  [[nodiscard]] bool write_float_bytes(
      const ConstantValue &value,
      TypeId type_id,
      std::vector<std::uint8_t> &bytes,
      std::uint64_t offset,
      SourceRange range) {
    const Type &storage = type(runtime_scalar_id(type_id));
    const std::optional<IeeeBinaryFormat> format = ieee_format(type_id);
    const std::uint32_t bit_width = format.has_value()
        ? 1U + format->exponent_bits + format->fraction_bits
        : 0;
    const std::optional<std::uint64_t> bits =
        value.float_bit_width != 0 && value.float_bit_width == bit_width
        ? std::optional<std::uint64_t>(value.float_bits)
        : (format.has_value()
               ? round_ieee_bits(value.floating, *format)
               : std::nullopt);
    if (!bits.has_value()) {
      error(range, "floating constant has no union-storage encoding");
      return false;
    }
    const std::uint64_t size = storage.layout.size;
    if (offset > bytes.size() || size > bytes.size() - offset) {
      error(range, "floating constant does not fit union storage");
      return false;
    }
    const bool little = scalar_uses_little_endian(type_id);
    for (std::uint64_t byte_index = 0; byte_index < size; ++byte_index) {
      const std::uint64_t destination = little
          ? offset + byte_index
          : offset + size - byte_index - 1U;
      bytes[static_cast<std::size_t>(destination)] =
          static_cast<std::uint8_t>((*bits >> (byte_index * 8U)) & 0xffU);
    }
    return true;
  }

  [[nodiscard]] bool write_constant_bytes(
      const ConstantValue &value,
      TypeId type_id,
      std::vector<std::uint8_t> &bytes,
      std::uint64_t offset,
      ConstantSite site,
      SourceRange range) {
    while (type(type_id).kind == TypeKind::Distinct) {
      type_id = type(type_id).element;
    }
    const Type &storage = type(type_id);
    if (!storage.layout.known || offset > bytes.size() ||
        storage.layout.size > bytes.size() - offset) {
      error(range, "constant does not fit aggregate byte storage");
      return false;
    }

    if (value.kind == ConstantKind::Nil) {
      return true;
    }
    if (value.kind == ConstantKind::Bool && storage.kind == TypeKind::Bool) {
      bytes[static_cast<std::size_t>(offset)] = value.boolean ? 1U : 0U;
      return true;
    }
    if (value.kind == ConstantKind::Integer && integer_kind(storage.kind)) {
      return write_integer_bytes(value.integer, type_id, bytes, offset, range);
    }
    if (value.kind == ConstantKind::Float &&
        (storage.kind == TypeKind::Float ||
         storage.kind == TypeKind::EndianScalar)) {
      return write_float_bytes(value, type_id, bytes, offset, range);
    }
    if (value.kind == ConstantKind::String) {
      // A relocatable pointer cannot be represented faithfully as independent
      // i8 constants in the backend's opaque union storage.  Ordinary nested
      // strings are emitted directly; only type-punned union payloads take this
      // byte path.
      error(range, "relocatable string payload is unsupported in a union constant");
      return false;
    }
    if (value.kind == ConstantKind::Procedure) {
      error(
          range,
          "relocatable procedure identity is unsupported in opaque union storage");
      return false;
    }
    if (value.kind != ConstantKind::Aggregate) {
      error(range, "constant kind has no aggregate byte encoding");
      return false;
    }

    if (storage.kind == TypeKind::Array || storage.kind == TypeKind::Simd) {
      if (value.elements.size() != storage.element_count) {
        error(range, "constant array has the wrong element count");
        return false;
      }
      const std::uint64_t stride = type(storage.element).layout.size;
      for (std::size_t index = 0; index < value.elements.size(); ++index) {
        ConstantSite child = site;
        child.path.push_back(index);
        if (!write_constant_bytes(
                value.elements[index],
                storage.element,
                bytes,
                offset + static_cast<std::uint64_t>(index) * stride,
                std::move(child),
                range)) {
          return false;
        }
      }
      return true;
    }
    if (storage.kind == TypeKind::Tuple || storage.kind == TypeKind::Struct) {
      if (value.elements.size() != storage.members.size()) {
        error(range, "constant aggregate has the wrong member count");
        return false;
      }
      for (std::size_t index = 0; index < value.elements.size(); ++index) {
        const std::uint64_t member_offset =
            index < storage.member_offsets.size()
            ? storage.member_offsets[index]
            : 0;
        ConstantSite child = site;
        child.path.push_back(index);
        if (!write_constant_bytes(
                value.elements[index],
                storage.members[index],
                bytes,
                offset + member_offset,
                std::move(child),
                range)) {
          return false;
        }
      }
      return true;
    }
    if (storage.kind == TypeKind::RawUnion ||
        storage.kind == TypeKind::TaggedUnion) {
      if (value.variant_index >= storage.members.size()) {
        error(range, "constant union has an invalid selected member");
        return false;
      }
      if (storage.kind == TypeKind::TaggedUnion) {
        if (!write_integer_bytes(
                BigInteger::from_u64(value.variant_index),
                storage.element,
                bytes,
                offset,
                range)) {
          return false;
        }
      }
      if (value.elements.empty()) return true;
      if (value.elements.size() != 1) {
        error(range, "constant union has more than one payload");
        return false;
      }
      const std::uint64_t payload_offset =
          value.variant_index < storage.member_offsets.size()
          ? storage.member_offsets[value.variant_index]
          : 0;
      ConstantSite child = std::move(site);
      child.path.push_back(0);
      return write_constant_bytes(
          value.elements.front(),
          storage.members[value.variant_index],
          bytes,
          offset + payload_offset,
          std::move(child),
          range);
    }

    error(range, "constant type has no aggregate byte encoding");
    return false;
  }

  [[nodiscard]] bool contains_relocation(
      const ConstantValue &value) const {
    if (value.kind == ConstantKind::String ||
        value.kind == ConstantKind::Procedure) {
      return true;
    }
    for (const ConstantValue &element : value.elements) {
      if (contains_relocation(element)) return true;
    }
    return false;
  }

  [[nodiscard]] std::string byte_array_constant(
      const std::vector<std::uint8_t> &bytes) const {
    std::string result = "[";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index != 0) result += ", ";
      result += "i8 " + std::to_string(bytes[index]);
    }
    result += "]";
    return result;
  }

  [[nodiscard]] std::optional<TypedConstantOperand> relocatable_union_constant(
      const ConstantValue &value,
      TypeId type_id,
      ConstantSite site,
      SourceRange range) {
    while (type(type_id).kind == TypeKind::Distinct) {
      type_id = type(type_id).element;
    }
    const Type &storage = type(type_id);
    if ((storage.kind != TypeKind::TaggedUnion &&
         storage.kind != TypeKind::RawUnion) ||
        !contains_relocation(value)) {
      return std::nullopt;
    }
    if (value.kind != ConstantKind::Aggregate ||
        value.variant_index >= storage.members.size() ||
        value.elements.size() != 1) {
      error(range, "relocatable union constant has an invalid payload");
      return TypedConstantOperand{llvm_type(type_id), "zeroinitializer"};
    }

    const TypeId payload_type = storage.members[value.variant_index];
    const std::uint64_t payload_offset =
        value.variant_index < storage.member_offsets.size()
        ? storage.member_offsets[value.variant_index]
        : 0;
    const std::uint64_t payload_size = type(payload_type).layout.size;
    if (!storage.layout.known || !type(payload_type).layout.known ||
        payload_offset > storage.layout.size ||
        payload_size > storage.layout.size - payload_offset) {
      error(range, "relocatable union payload does not fit its storage");
      return TypedConstantOperand{llvm_type(type_id), "zeroinitializer"};
    }

    std::vector<std::uint8_t> prefix(
        static_cast<std::size_t>(payload_offset), 0);
    if (storage.kind == TypeKind::TaggedUnion &&
        !write_integer_bytes(
            BigInteger::from_u64(value.variant_index),
            storage.element,
            prefix,
            0,
            range)) {
      return TypedConstantOperand{llvm_type(type_id), "zeroinitializer"};
    }
    std::vector<std::uint8_t> suffix(
        static_cast<std::size_t>(
            storage.layout.size - payload_offset - payload_size),
        0);

    ConstantSite child = site;
    child.path.push_back(0);
    const std::optional<TypedConstantOperand> nested =
        relocatable_union_constant(
            value.elements.front(), payload_type, child, range);
    const std::string payload_llvm_type = nested.has_value()
        ? nested->type
        : llvm_type(payload_type);
    const std::string payload = nested.has_value()
        ? nested->value
        : constant_operand(
              value.elements.front(),
              payload_type,
              std::move(child),
              range);
    const std::string prefix_type =
        "[" + std::to_string(prefix.size()) + " x i8]";
    const std::string suffix_type =
        "[" + std::to_string(suffix.size()) + " x i8]";
    return TypedConstantOperand{
        "<{ " + prefix_type + ", " + payload_llvm_type + ", " +
            suffix_type + " }>",
        "<{ " + prefix_type + " " + byte_array_constant(prefix) + ", " +
            payload_llvm_type + " " + payload + ", " + suffix_type + " " +
            byte_array_constant(suffix) + " }>",
    };
  }

  [[nodiscard]] std::string union_constant(
      const ConstantValue &value,
      TypeId type_id,
      ConstantSite site,
      SourceRange range) {
    const Type &storage = type(type_id);
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(storage.layout.size), 0);
    if (!write_constant_bytes(
            value, type_id, bytes, 0, std::move(site), range)) {
      return "zeroinitializer";
    }
    std::string result = "[";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index != 0) result += ", ";
      result += "i8 " + std::to_string(bytes[index]);
    }
    result += "]";
    return result;
  }

  [[nodiscard]] std::string constant_operand(
      const ConstantValue &value,
      TypeId type_id,
      ConstantSite site,
      SourceRange range) {
    if (value.kind == ConstantKind::String) {
      const std::optional<std::size_t> index = constant_string_index(site);
      if (!index.has_value()) {
        error(range, "string constant was not interned");
        return "zeroinitializer";
      }
      return string_constant_operand(*index);
    }
    if (value.kind != ConstantKind::Aggregate) {
      return scalar_constant(value, type_id, range);
    }

    while (type(type_id).kind == TypeKind::Distinct) {
      type_id = type(type_id).element;
    }
    const Type &aggregate = type(type_id);
    if (aggregate.kind == TypeKind::TaggedUnion ||
        aggregate.kind == TypeKind::RawUnion) {
      return union_constant(value, type_id, std::move(site), range);
    }

    const bool homogeneous = aggregate.kind == TypeKind::Array ||
        aggregate.kind == TypeKind::Simd;
    const bool product = aggregate.kind == TypeKind::Tuple ||
        aggregate.kind == TypeKind::Struct;
    const std::size_t expected = homogeneous
        ? static_cast<std::size_t>(aggregate.element_count)
        : aggregate.members.size();
    if ((!homogeneous && !product) || value.elements.size() != expected) {
      error(range, "aggregate constant does not match its runtime type");
      return "zeroinitializer";
    }

    std::string result;
    if (aggregate.kind == TypeKind::Array) result = "[";
    if (aggregate.kind == TypeKind::Simd) result = "<";
    if (aggregate.kind == TypeKind::Tuple) result = "{ ";
    if (aggregate.kind == TypeKind::Struct) result = "<{ ";
    std::uint64_t cursor = 0;
    bool emitted = false;
    for (std::size_t index = 0; index < value.elements.size(); ++index) {
      const TypeId element_type = homogeneous
          ? aggregate.element
          : aggregate.members[index];
      if (aggregate.kind == TypeKind::Struct) {
        const std::uint64_t member_offset =
            index < aggregate.member_offsets.size()
            ? aggregate.member_offsets[index]
            : cursor;
        if (member_offset > cursor) {
          if (emitted) result += ", ";
          result += "[" + std::to_string(member_offset - cursor) +
              " x i8] zeroinitializer";
          emitted = true;
        }
        cursor = member_offset + type(element_type).layout.size;
      }
      if (emitted) result += ", ";
      ConstantSite child = site;
      child.path.push_back(index);
      result += llvm_type(element_type) + " " + constant_operand(
          value.elements[index],
          element_type,
          std::move(child),
          range);
      emitted = true;
    }
    if (aggregate.kind == TypeKind::Struct &&
        aggregate.layout.size > cursor) {
      if (emitted) result += ", ";
      result += "[" + std::to_string(aggregate.layout.size - cursor) +
          " x i8] zeroinitializer";
    }
    if (aggregate.kind == TypeKind::Array) result += "]";
    if (aggregate.kind == TypeKind::Simd) result += ">";
    if (aggregate.kind == TypeKind::Tuple) result += " }";
    if (aggregate.kind == TypeKind::Struct) result += " }>";
    return result;
  }

  [[nodiscard]] std::string scalar_constant(
      const ConstantValue &value, TypeId type_id, SourceRange range) {
    switch (value.kind) {
    case ConstantKind::Unavailable:
      if (llvm_type(type_id) == "ptr") return "null";
      return "zeroinitializer";
    case ConstantKind::Nil:
      return "null";
    case ConstantKind::Bool:
      return value.boolean ? "true" : "false";
    case ConstantKind::Integer:
      return value.integer.to_decimal();
    case ConstantKind::EnumLabel:
      // Contextual alternatives retain their SymbolId on MIR Constant rows;
      // instruction_constant handles the declaration-order discriminator.
      return "0";
    case ConstantKind::Float: {
      Type storage_type = type(type_id);
      while (storage_type.kind == TypeKind::Distinct) {
        storage_type = type(storage_type.element);
      }
      Type float_type = storage_type;
      if (storage_type.kind == TypeKind::EndianScalar &&
          storage_type.element.is_valid()) {
        float_type = type(storage_type.element);
      }
      const std::optional<IeeeBinaryFormat> format =
          float_type.kind == TypeKind::Float
          ? ieee_format_for_width(float_type.bit_width)
          : std::nullopt;
      if (!format.has_value()) {
        error(range, "floating constant has no supported IEEE format");
        return "zeroinitializer";
      }
      const std::optional<std::uint64_t> bits =
          value.float_bit_width != 0 &&
              value.float_bit_width == float_type.bit_width
          ? std::optional<std::uint64_t>(value.float_bits)
          : round_ieee_bits(value.floating, *format);
      if (!bits.has_value()) {
        error(range, "floating constant could not be rounded for the target");
        return "zeroinitializer";
      }
      if (storage_type.kind == TypeKind::EndianScalar) {
        std::uint64_t storage_bits = *bits;
        if (endian_requires_swap(type_id)) {
          std::uint64_t swapped = 0;
          const std::uint32_t byte_count = float_type.bit_width / 8U;
          for (std::uint32_t index = 0; index < byte_count; ++index) {
            swapped = (swapped << 8U) | ((storage_bits >> (index * 8U)) & 0xffU);
          }
          storage_bits = swapped;
        }
        return std::to_string(storage_bits);
      }
      return "bitcast (i" + std::to_string(float_type.bit_width) + " " +
          std::to_string(*bits) + " to " + llvm_type(type_id) + ")";
    }
    case ConstantKind::String:
      error(range, "string constant requires module string identity");
      return "zeroinitializer";
    case ConstantKind::Aggregate:
      error(range, "aggregate constant requires aggregate emission");
      return "zeroinitializer";
    case ConstantKind::Procedure: {
      const std::optional<std::string> name = procedure_constant_name(value);
      if (!name.has_value()) {
        error(range, "procedure constant has no resolvable identity");
        return "null";
      }
      return *name;
    }
    case ConstantKind::Target:
      error(range, "target pseudo-value reached runtime emission");
      return "zeroinitializer";
    }
    return "zeroinitializer";
  }

  [[nodiscard]] std::string enum_value(SymbolId member) const {
    for (const EnumMemberValue &entry : semantic_.enum_member_values) {
      if (entry.member == member) return entry.value.to_decimal();
    }
    return "0";
  }

  [[nodiscard]] std::string instruction_constant(
      std::size_t procedure_index,
      std::size_t instruction_index,
      const MirInstruction &instruction) {
    if (instruction.constant.kind == ConstantKind::EnumLabel) {
      return enum_value(instruction.symbol);
    }
    ConstantSite site;
    site.procedure = procedure_index;
    site.instruction = instruction_index;
    return constant_operand(
        instruction.constant,
        instruction.type,
        std::move(site),
        instruction.range);
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

  [[nodiscard]] std::string abi_call_argument_scratch(
      std::size_t instruction, std::size_t argument) const {
    return "%abi.call." + std::to_string(instruction) + ".arg." +
        std::to_string(argument);
  }

  [[nodiscard]] std::string abi_call_result_scratch(
      std::size_t instruction) const {
    return "%abi.call." + std::to_string(instruction) + ".result";
  }

  [[nodiscard]] std::uint64_t abi_argument_storage_size(
      const Aarch64CAbiType &abi) const {
    if (abi.classification == Aarch64CAbiClass::SmallAggregate) {
      return static_cast<std::uint64_t>(abi.argument_integer_bits / 8U) *
          abi.argument_integer_count;
    }
    return abi.size;
  }

  [[nodiscard]] std::uint64_t abi_result_storage_size(
      const Aarch64CAbiType &abi) const {
    if (abi.classification == Aarch64CAbiClass::SmallAggregate) {
      return static_cast<std::uint64_t>(abi.result_integer_bits / 8U) *
          abi.result_integer_count;
    }
    return abi.size;
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
      if (runtime_scalar_kind(instruction.type) == TypeKind::Float) {
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
    const TypeKind operand_kind = runtime_scalar_kind(operand_type);
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
    const TypeKind source_kind = runtime_scalar_kind(source_type);
    const TypeKind target_kind = runtime_scalar_kind(instruction.type);
    const std::string source = value_operand(operands, source_id, instruction.range);

    // `bool` is an i1 computation value, while b8/b16/b32/b64 retain every
    // stored bit. Converting storage back to bool must therefore test for zero;
    // truncation would incorrectly make a nonzero even value false.
    if (source_kind == TypeKind::Bool &&
        target_kind == TypeKind::BooleanStorage) {
      const std::string result = "%v" +
          std::to_string(instruction.result.value);
      output_ << "  " << result << " = zext i1 " << source << " to "
              << llvm_type(instruction.type) << '\n';
      assign_alias(operands, instruction, result);
      return;
    }
    if (source_kind == TypeKind::BooleanStorage &&
        target_kind == TypeKind::Bool) {
      const std::string result = "%v" +
          std::to_string(instruction.result.value);
      output_ << "  " << result << " = icmp ne " << llvm_type(source_type)
              << ' ' << source << ", 0\n";
      assign_alias(operands, instruction, result);
      return;
    }

    // Endian scalars are integer-shaped storage in LLVM even when their native
    // counterpart is floating point. A conversion first exposes the IEEE bits,
    // swaps exactly when storage order differs from the target, and then restores
    // the native scalar representation in the opposite direction.
    const TypeId source_runtime = runtime_scalar_id(source_type);
    const TypeId target_runtime = runtime_scalar_id(instruction.type);
    if (target_kind == TypeKind::EndianScalar &&
        type(target_runtime).element == source_runtime) {
      const std::uint32_t bits = type(target_runtime).bit_width;
      const std::string integer_type = "i" + std::to_string(bits);
      std::string stored_bits = source;
      if (source_kind == TypeKind::Float) {
        const std::string bitcast = endian_requires_swap(target_runtime)
            ? auxiliary()
            : "%v" + std::to_string(instruction.result.value);
        output_ << "  " << bitcast << " = bitcast " << llvm_type(source_type)
                << ' ' << source << " to " << integer_type << '\n';
        stored_bits = bitcast;
      }
      if (endian_requires_swap(target_runtime)) {
        const std::string result = "%v" +
            std::to_string(instruction.result.value);
        output_ << "  " << result << " = call " << integer_type
                << " @llvm.bswap." << integer_type << '(' << integer_type
                << ' ' << stored_bits << ")\n";
        stored_bits = result;
      }
      assign_alias(operands, instruction, stored_bits);
      return;
    }
    if (source_kind == TypeKind::EndianScalar &&
        type(source_runtime).element == target_runtime) {
      const std::uint32_t bits = type(source_runtime).bit_width;
      const std::string integer_type = "i" + std::to_string(bits);
      std::string native_bits = source;
      if (endian_requires_swap(source_runtime)) {
        const std::string swapped = target_kind == TypeKind::Float
            ? auxiliary()
            : "%v" + std::to_string(instruction.result.value);
        output_ << "  " << swapped << " = call " << integer_type
                << " @llvm.bswap." << integer_type << '(' << integer_type
                << ' ' << source << ")\n";
        native_bits = swapped;
      }
      if (target_kind == TypeKind::Float) {
        const std::string result = "%v" +
            std::to_string(instruction.result.value);
        output_ << "  " << result << " = bitcast " << integer_type << ' '
                << native_bits << " to " << llvm_type(instruction.type) << '\n';
        native_bits = result;
      }
      assign_alias(operands, instruction, native_bits);
      return;
    }

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
    std::size_t physical_index = 0;
    std::uint64_t cursor = 0;
    for (std::size_t index = 0; index < aggregate.member_offsets.size(); ++index) {
      const std::uint64_t member_offset = aggregate.member_offsets[index];
      if (member_offset > cursor) ++physical_index;
      if (member_offset == offset) return physical_index;
      ++physical_index;
      if (index < aggregate.members.size()) {
        cursor = member_offset + type(aggregate.members[index]).layout.size;
      }
    }
    return 0;
  }

  [[nodiscard]] std::string union_scratch(std::size_t instruction_index) const {
    return "%union.scratch." + std::to_string(instruction_index);
  }

  [[nodiscard]] std::optional<TypeId> union_scratch_type(
      const MirProcedure &procedure,
      const MirInstruction &instruction) const {
    TypeId candidate;
    if (instruction.kind == MirInstructionKind::Aggregate) {
      candidate = instruction.type;
    } else if (instruction.kind == MirInstructionKind::ExtractMember &&
               !instruction.operands.empty()) {
      candidate = procedure.value(instruction.operands.front()).type;
    }
    if (!candidate.is_valid()) return std::nullopt;
    const TypeKind kind = type(candidate).kind;
    if (kind != TypeKind::TaggedUnion && kind != TypeKind::RawUnion) {
      return std::nullopt;
    }
    return candidate;
  }

  void emit_aggregate(
      std::size_t instruction_index,
      const MirProcedure &procedure,
      const MirInstruction &instruction,
      std::vector<std::string> &operands) {
    if (type(instruction.type).kind == TypeKind::TaggedUnion ||
        type(instruction.type).kind == TypeKind::RawUnion) {
      // Unions are represented as their exact byte-sized storage because LLVM
      // has no source-level tagged-union type. Build the value in temporary
      // memory: zeroing first gives deterministic padding and the Draft zero
      // value, then discriminator/payload stores write their typed fields.
      const Type &aggregate_type = type(instruction.type);
      const std::string storage = union_scratch(instruction_index);
      output_ << "  store " << llvm_type(instruction.type)
              << " zeroinitializer, ptr " << storage << ", align "
              << aggregate_type.layout.alignment << '\n';
      for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
        const MirValueId operand = instruction.operands[index];
        const TypeId operand_type = procedure.value(operand).type;
        const std::uint64_t offset = index < instruction.offsets.size()
            ? instruction.offsets[index]
            : 0;
        const std::string address = auxiliary();
        output_ << "  " << address << " = getelementptr i8, ptr "
                << storage << ", i64 " << offset << '\n';
        output_ << "  store "
                << typed_operand(procedure, operands, operand, instruction.range)
                << ", ptr " << address << ", align "
                << type(operand_type).layout.alignment << '\n';
      }
      const std::string result = "%v" + std::to_string(instruction.result.value);
      output_ << "  " << result << " = load " << llvm_type(instruction.type)
              << ", ptr " << storage << ", align "
              << aggregate_type.layout.alignment << '\n';
      assign_alias(operands, instruction, result);
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

  void emit_c_call(
      std::size_t instruction_index,
      const MirProcedure &procedure,
      const MirInstruction &instruction,
      std::vector<std::string> &operands) {
    const MirValueId callee = instruction.operands.front();
    const TypeId signature_id = procedure.value(callee).type;
    const Type &signature = type(signature_id);
    const std::size_t parameter_count = signature.members.size() - 1;
    std::vector<std::string> arguments;
    arguments.reserve(parameter_count + 1);

    const TypeId logical_result = signature.members.back();
    const bool returns_void =
        logical_result == semantic_.types.builtins().void_type;
    const Aarch64CAbiType result_abi = returns_void
        ? Aarch64CAbiType{}
        : c_abi_type(logical_result);
    if (result_abi.classification == Aarch64CAbiClass::Indirect) {
      arguments.push_back(
          "ptr sret(" + llvm_type(logical_result) + ") align " +
          std::to_string(result_abi.alignment) + " " +
          abi_call_result_scratch(instruction_index));
    }

    for (std::size_t index = 0; index < parameter_count; ++index) {
      const TypeId logical_type = signature.members[index];
      const MirValueId value = instruction.operands[index + 1];
      const Aarch64CAbiType abi = c_abi_type(logical_type);
      if (abi.classification == Aarch64CAbiClass::Direct) {
        std::string argument = llvm_type(logical_type);
        const std::string extension = c_integer_extension(logical_type);
        if (!extension.empty()) argument += " " + extension;
        argument += " " + value_operand(
            operands, value, instruction.range);
        arguments.push_back(std::move(argument));
        continue;
      }
      if (abi.classification == Aarch64CAbiClass::Illegal) {
        error(instruction.range, "illegal C ABI call argument reached emission");
        arguments.push_back("i8 poison");
        continue;
      }

      const std::string scratch =
          abi_call_argument_scratch(instruction_index, index);
      const std::uint64_t storage = abi_argument_storage_size(abi);
      if (abi.classification == Aarch64CAbiClass::SmallAggregate) {
        output_ << "  store [" << storage
                << " x i8] zeroinitializer, ptr " << scratch
                << ", align " << abi.alignment << '\n';
      }
      output_ << "  store "
              << typed_operand(procedure, operands, value, instruction.range)
              << ", ptr " << scratch << ", align " << abi.alignment << '\n';
      if (abi.classification == Aarch64CAbiClass::Indirect) {
        arguments.push_back("ptr " + scratch);
        continue;
      }
      const std::string physical = auxiliary();
      const std::string physical_type = c_parameter_type(logical_type);
      output_ << "  " << physical << " = load " << physical_type
              << ", ptr " << scratch << ", align " << abi.alignment << '\n';
      arguments.push_back(physical_type + " " + physical);
    }

    const std::string callee_operand =
        value_operand(operands, callee, instruction.range);
    const std::string result = instruction.result.is_valid()
        ? "%v" + std::to_string(instruction.result.value)
        : std::string();
    if (returns_void || result_abi.classification == Aarch64CAbiClass::Indirect) {
      output_ << "  call void " << callee_operand << '(';
    } else if (result_abi.classification == Aarch64CAbiClass::SmallAggregate ||
               result_abi.classification ==
                   Aarch64CAbiClass::HomogeneousFloatAggregate) {
      output_ << "  %abi.call." << instruction_index << ".physical = call "
              << c_result_type(logical_result) << ' ' << callee_operand << '(';
    } else {
      const std::string extension = c_integer_extension(logical_result);
      output_ << "  " << result << " = call ";
      if (!extension.empty()) output_ << extension << ' ';
      output_ << c_result_type(logical_result) << ' ' << callee_operand << '(';
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (index != 0) output_ << ", ";
      output_ << arguments[index];
    }
    output_ << ")\n";

    if (returns_void) return;
    if (result_abi.classification == Aarch64CAbiClass::SmallAggregate ||
        result_abi.classification ==
            Aarch64CAbiClass::HomogeneousFloatAggregate) {
      const std::string scratch = abi_call_result_scratch(instruction_index);
      const std::string physical =
          "%abi.call." + std::to_string(instruction_index) + ".physical";
      output_ << "  store " << c_result_type(logical_result) << ' ' << physical
              << ", ptr " << scratch << ", align " << result_abi.alignment
              << '\n'
              << "  " << result << " = load " << llvm_type(logical_result)
              << ", ptr " << scratch << ", align " << result_abi.alignment
              << '\n';
    } else if (result_abi.classification == Aarch64CAbiClass::Indirect) {
      output_ << "  " << result << " = load " << llvm_type(logical_result)
              << ", ptr " << abi_call_result_scratch(instruction_index)
              << ", align " << result_abi.alignment << '\n';
    } else if (result_abi.classification == Aarch64CAbiClass::Illegal) {
      error(instruction.range, "illegal C ABI call result reached emission");
    }
    if (instruction.result.is_valid()) {
      assign_alias(operands, instruction, result);
    }
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
    case MirInstructionKind::PointerOffset: {
      const std::string pointer = value_operand(
          operands, instruction.operands[0], instruction.range);
      std::string byte_count = value_operand(
          operands, instruction.operands[1], instruction.range);
      if (instruction.offset != 1) {
        const std::string scaled = auxiliary();
        output_ << "  " << scaled << " = mul "
                << llvm_type(procedure.value(instruction.operands[1]).type)
                << ' ' << byte_count << ", " << instruction.offset << '\n';
        byte_count = scaled;
      }
      output_ << "  " << result << " = getelementptr i8, ptr " << pointer
              << ", "
              << llvm_type(procedure.value(instruction.operands[1]).type)
              << ' ' << byte_count << '\n';
      assign_alias(operands, instruction, result);
      break;
    }
    case MirInstructionKind::PointerSubtract: {
      const std::string integer_type = llvm_type(instruction.type);
      const std::string left = auxiliary();
      const std::string right = auxiliary();
      output_ << "  " << left << " = ptrtoint ptr "
              << value_operand(
                     operands, instruction.operands[0], instruction.range)
              << " to " << integer_type << '\n';
      output_ << "  " << right << " = ptrtoint ptr "
              << value_operand(
                     operands, instruction.operands[1], instruction.range)
              << " to " << integer_type << '\n';
      const std::string difference = instruction.offset == 1
          ? result
          : auxiliary();
      // Pointer subtraction is specified only when the mathematical byte
      // difference fits isize. `nsw` and `exact` encode those source-level UB
      // preconditions without adding hidden runtime checks to a primitive op.
      output_ << "  " << difference << " = sub nsw " << integer_type << ' '
              << left << ", " << right << '\n';
      if (instruction.offset != 1) {
        output_ << "  " << result << " = sdiv exact " << integer_type << ' '
                << difference << ", " << instruction.offset << '\n';
      }
      assign_alias(operands, instruction, result);
      break;
    }
    case MirInstructionKind::Call: {
      const Type &callee_signature = type(
          procedure.value(instruction.operands.front()).type);
      if (callee_signature.kind == TypeKind::Procedure &&
          callee_signature.c_calling_convention) {
        emit_c_call(
            instruction_index, procedure, instruction, operands);
        break;
      }
      if (instruction.establishes_thread_context) {
        output_ << "  call void @\"__draft.runtime.attach_thread\"()\n";
      }
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
    case MirInstructionKind::Assert: {
      const AssertionSite *site =
          assertion_site(procedure_index, instruction_index);
      if (site == nullptr) {
        error(instruction.range, "assertion source metadata was not collected");
        break;
      }
      output_ << "  call void @__draft.assert(ptr %context, "
              << typed_operand(
                     procedure, operands, instruction.operands[0], instruction.range)
              << ", { ptr, i64 } "
              << string_constant_operand(site->condition_string) << ", ";
      if (instruction.operands.size() == 2) {
        output_ << typed_operand(
            procedure, operands, instruction.operands[1], instruction.range);
      } else {
        output_ << "{ ptr, i64 } zeroinitializer";
      }
      output_ << ", { ptr, i64 } "
              << string_constant_operand(site->file_string)
              << ", i64 " << site->location.line
              << ", i64 " << site->location.column << ")\n";
      break;
    }
    case MirInstructionKind::MemberAddress:
      output_ << "  " << result << " = getelementptr i8, ptr "
              << value_operand(operands, instruction.operands[0], instruction.range)
              << ", i64 " << instruction.offset << '\n';
      assign_alias(operands, instruction, result);
      break;
    case MirInstructionKind::ExtractMember:
      {
        const MirValueId aggregate_id = instruction.operands[0];
        const TypeId aggregate_type = procedure.value(aggregate_id).type;
        const TypeKind aggregate_kind = type(aggregate_type).kind;
        if (aggregate_kind == TypeKind::TaggedUnion ||
            aggregate_kind == TypeKind::RawUnion) {
          // The source is an opaque byte aggregate in LLVM. Materializing it
          // permits a typed load at the semantically checked payload offset.
          const std::string storage = union_scratch(instruction_index);
          output_ << "  store "
                  << typed_operand(
                         procedure, operands, aggregate_id, instruction.range)
                  << ", ptr " << storage << ", align "
                  << type(aggregate_type).layout.alignment << '\n';
          const std::string address = auxiliary();
          output_ << "  " << address << " = getelementptr i8, ptr "
                  << storage << ", i64 " << instruction.offset << '\n';
          output_ << "  " << result << " = load "
                  << llvm_type(instruction.type) << ", ptr " << address
                  << ", align " << type(instruction.type).layout.alignment << '\n';
        } else {
          const std::size_t member = aggregate_index(
              aggregate_type, instruction.offset);
          output_ << "  " << result << " = extractvalue "
                  << typed_operand(
                         procedure, operands, aggregate_id, instruction.range)
                  << ", " << member << '\n';
        }
        assign_alias(operands, instruction, result);
      }
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
    case MirInstructionKind::BoundsCheck: {
      const BoundsSite *site = bounds_site(procedure_index, instruction_index);
      if (site == nullptr) {
        error(instruction.range, "bounds-check source metadata was not collected");
        break;
      }
      output_ << "  call void @__draft.bounds("
              << typed_operand(
                     procedure, operands, instruction.operands[0], instruction.range)
              << ", "
              << typed_operand(
                     procedure, operands, instruction.operands[1], instruction.range)
              << ", ptr @.draft.string." << site->file_string
              << ", i64 " << site->location.line
              << ", i64 " << site->location.column << ")\n";
      break;
    }
    case MirInstructionKind::SliceBoundsCheck: {
      const BoundsSite *site = bounds_site(procedure_index, instruction_index);
      if (site == nullptr) {
        error(instruction.range, "slice-check source metadata was not collected");
        break;
      }
      output_ << "  call void @__draft.slice_bounds(";
      for (std::size_t index = 0; index < 3; ++index) {
        if (index != 0) output_ << ", ";
        output_ << typed_operand(
            procedure, operands, instruction.operands[index], instruction.range);
      }
      output_ << ", ptr @.draft.string." << site->file_string
              << ", i64 " << site->location.line
              << ", i64 " << site->location.column << ")\n";
      break;
    }
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
      // A Draft string is the same physical {data,len} view as []u8 but its
      // immutability is represented by TypeKind::String rather than an element
      // TypeId. Its byte stride is therefore explicit here.
      const std::uint64_t stride = slice_type.kind == TypeKind::String
          ? 1
          : type(slice_type.element).layout.size;
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
      emit_aggregate(instruction_index, procedure, instruction, operands);
      break;
    case MirInstructionKind::Assembly:
      if (instruction.result.is_valid()) output_ << "  " << result << " = ";
      else output_ << "  ";
      output_ << "call " << llvm_type(instruction.type)
              << " asm sideeffect \""
              << llvm_inline_assembly(instruction.assembly_text) << "\", \""
              << instruction.assembly_constraints << "\"(";
      for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
        if (index != 0) output_ << ", ";
        output_ << typed_operand(
            procedure, operands, instruction.operands[index], instruction.range);
      }
      output_ << ")\n";
      if (instruction.result.is_valid()) assign_alias(operands, instruction, result);
      break;
    case MirInstructionKind::Trap:
      output_ << "  call void @llvm.trap()\n";
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
        const Type &signature = type(procedure.type);
        const TypeId logical_result = procedure.value(terminator.value).type;
        const Aarch64CAbiType abi = signature.c_calling_convention
            ? c_abi_type(logical_result)
            : Aarch64CAbiType{};
        if (signature.c_calling_convention &&
            abi.classification == Aarch64CAbiClass::Indirect) {
          output_ << "  store "
                  << typed_operand(
                         procedure, operands, terminator.value, terminator.range)
                  << ", ptr %sret, align " << abi.alignment << '\n'
                  << "  ret void\n";
        } else if (signature.c_calling_convention &&
                   (abi.classification == Aarch64CAbiClass::SmallAggregate ||
                    abi.classification ==
                        Aarch64CAbiClass::HomogeneousFloatAggregate)) {
          const std::string physical = auxiliary();
          output_ << "  store "
                  << typed_operand(
                         procedure, operands, terminator.value, terminator.range)
                  << ", ptr %abi.return, align " << abi.alignment << '\n'
                  << "  " << physical << " = load "
                  << c_result_type(logical_result)
                  << ", ptr %abi.return, align " << abi.alignment << '\n'
                  << "  ret " << c_result_type(logical_result)
                  << ' ' << physical << '\n';
        } else {
          output_ << "  ret "
                  << typed_operand(
                         procedure, operands, terminator.value, terminator.range)
                  << '\n';
        }
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

  void emit_abi_scratch(
      const std::string &name,
      std::uint64_t size,
      std::uint32_t alignment) {
    output_ << "  " << name << " = alloca [" << size
            << " x i8], align " << alignment << '\n';
  }

  // All ABI conversion storage is reserved in the entry block. A source call
  // may execute repeatedly inside a loop; placing an alloca at the call site
  // would grow the stack once per iteration even though one reusable slot is
  // sufficient for each statically distinct MIR instruction.
  void emit_c_abi_scratch_allocas(const MirProcedure &procedure) {
    const Type &own_signature = type(procedure.type);
    if (own_signature.c_calling_convention) {
      const Aarch64CAbiType result_abi = function_result_abi(procedure.type);
      if (result_abi.classification == Aarch64CAbiClass::SmallAggregate ||
          result_abi.classification ==
              Aarch64CAbiClass::HomogeneousFloatAggregate) {
        emit_abi_scratch(
            "%abi.return",
            abi_result_storage_size(result_abi),
            result_abi.alignment);
      }
    }

    for (std::size_t instruction_index = 0;
         instruction_index < procedure.instructions.size();
         ++instruction_index) {
      const MirInstruction &instruction =
          procedure.instructions[instruction_index];
      if (instruction.kind != MirInstructionKind::Call ||
          instruction.operands.empty()) {
        continue;
      }
      const MirValueId callee = instruction.operands.front();
      if (!callee.is_valid() ||
          static_cast<std::size_t>(callee.value) >= procedure.values.size()) {
        continue;
      }
      const TypeId signature_id = procedure.value(callee).type;
      const Type &signature = type(signature_id);
      if (signature.kind != TypeKind::Procedure ||
          !signature.c_calling_convention || signature.members.empty()) {
        continue;
      }
      const std::size_t parameter_count = signature.members.size() - 1;
      for (std::size_t argument = 0;
           argument < parameter_count && argument + 1 < instruction.operands.size();
           ++argument) {
        const Aarch64CAbiType abi =
            c_abi_type(signature.members[argument]);
        if (abi.classification == Aarch64CAbiClass::Direct ||
            abi.classification == Aarch64CAbiClass::Illegal) {
          continue;
        }
        emit_abi_scratch(
            abi_call_argument_scratch(instruction_index, argument),
            abi_argument_storage_size(abi),
            abi.alignment);
      }
      const Aarch64CAbiType result_abi = function_result_abi(signature_id);
      if (result_abi.classification == Aarch64CAbiClass::SmallAggregate ||
          result_abi.classification ==
              Aarch64CAbiClass::HomogeneousFloatAggregate ||
          result_abi.classification == Aarch64CAbiClass::Indirect) {
        emit_abi_scratch(
            abi_call_result_scratch(instruction_index),
            abi_result_storage_size(result_abi),
            result_abi.alignment);
      }
    }
  }

  void emit_procedure(std::size_t procedure_index, const MirProcedure &procedure) {
    if (!procedure.valid) return;
    auxiliary_index_ = 0;
    output_ << "define " << llvm_function_result(procedure.type) << ' '
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
            const Type &signature = type(procedure.type);
            const std::string argument =
                "%arg" + std::to_string(local.parameter_index);
            if (!signature.c_calling_convention) {
              output_ << "  store " << llvm_type(local.type) << ' ' << argument
                      << ", ptr %l" << local_index << ", align "
                      << type(local.type).layout.alignment << '\n';
            } else {
              const Aarch64CAbiType abi = c_abi_type(local.type);
              if (abi.classification == Aarch64CAbiClass::Direct) {
                output_ << "  store " << llvm_type(local.type) << ' ' << argument
                        << ", ptr %l" << local_index << ", align "
                        << type(local.type).layout.alignment << '\n';
              } else if (abi.classification ==
                         Aarch64CAbiClass::HomogeneousFloatAggregate) {
                output_ << "  store " << homogeneous_llvm_type(abi) << ' '
                        << argument << ", ptr %l" << local_index << ", align "
                        << abi.alignment << '\n';
              } else if (abi.classification ==
                         Aarch64CAbiClass::SmallAggregate) {
                const std::string scratch =
                    "%abi.param." + std::to_string(local.parameter_index);
                const std::uint64_t storage = abi_argument_storage_size(abi);
                emit_abi_scratch(scratch, storage, abi.alignment);
                output_ << "  store [" << storage
                        << " x i8] zeroinitializer, ptr " << scratch
                        << ", align " << abi.alignment << '\n'
                        << "  store " << c_parameter_type(local.type) << ' '
                        << argument << ", ptr " << scratch << ", align "
                        << abi.alignment << '\n';
                const std::string logical =
                    "%abi.param." + std::to_string(local.parameter_index) +
                    ".logical";
                output_ << "  " << logical << " = load "
                        << llvm_type(local.type) << ", ptr " << scratch
                        << ", align " << abi.alignment << '\n'
                        << "  store " << llvm_type(local.type) << ' ' << logical
                        << ", ptr %l" << local_index << ", align "
                        << type(local.type).layout.alignment << '\n';
              } else if (abi.classification == Aarch64CAbiClass::Indirect) {
                const std::string logical =
                    "%abi.param." + std::to_string(local.parameter_index) +
                    ".logical";
                output_ << "  " << logical << " = load "
                        << llvm_type(local.type) << ", ptr " << argument
                        << ", align " << abi.alignment << '\n'
                        << "  store " << llvm_type(local.type) << ' ' << logical
                        << ", ptr %l" << local_index << ", align "
                        << type(local.type).layout.alignment << '\n';
              } else {
                error(procedure.range, "illegal C ABI parameter reached emission");
              }
            }
          }
        }
        emit_c_abi_scratch_allocas(procedure);
        // Union packing and extraction use one fixed scratch slot per MIR
        // instruction. Declaring these in the entry block avoids dynamic stack
        // growth when the source operation executes repeatedly inside a loop.
        for (std::size_t instruction_index = 0;
             instruction_index < procedure.instructions.size();
             ++instruction_index) {
          const std::optional<TypeId> scratch_type = union_scratch_type(
              procedure, procedure.instructions[instruction_index]);
          if (!scratch_type.has_value()) continue;
          output_ << "  " << union_scratch(instruction_index) << " = alloca "
                  << llvm_type(*scratch_type) << ", align "
                  << type(*scratch_type).layout.alignment << '\n';
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
    if (parameters != 0 || signature.c_calling_convention ||
        symbol.flags.parametric) {
      error(
          symbol.name_range,
          "Draft main must be a non-parametric ordinary zero-parameter procedure");
      return;
    }
    const TypeId result_type = function_result(symbol.type);
    output_ << "define i32 @main(i32 %argc, ptr %argv) {\n"
            << "entry:\n"
            << "  store i32 %argc, ptr @__draft.process_argc, align 4\n"
            << "  store ptr %argv, ptr @__draft.process_argv, align 8\n";
    if (result_type == semantic_.types.builtins().void_type) {
      output_ << "  call void " << symbol_name(*entry)
              << "(ptr @__draft.root_context)\n"
              << "  ret i32 0\n";
    } else if (result_type == semantic_.types.builtins().int_type) {
      output_ << "  %draft.result = call " << llvm_type(result_type) << ' '
              << symbol_name(*entry) << "(ptr @__draft.root_context)\n";
      output_ << "  %exit.result = trunc " << llvm_type(result_type)
              << " %draft.result to i32\n"
              << "  ret i32 %exit.result\n";
    } else {
      error(symbol.name_range, "Draft main result must be void or int");
      output_ << "  ret i32 1\n";
    }
    output_ << "}\n\n";
  }

  const TargetProfile &target_;
  const SourceManager &sources_;
  const LlvmIrOptions &options_;
  const SemanticPackage &semantic_;
  const ConstantTable &global_initializers_;
  const MirProgram &mir_;
  DiagnosticSink &diagnostics_;
  std::ostringstream output_;
  std::vector<StringConstant> strings_;
  std::vector<AssertionSite> assertion_sites_;
  std::vector<BoundsSite> bounds_sites_;
  std::size_t initial_errors_ = 0;
  std::size_t auxiliary_index_ = 0;
};

} // namespace

LlvmIrResult emit_llvm_ir(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const ConstantTable &global_initializers,
    const MirProgram &mir,
    DiagnosticSink &diagnostics) {
  return Emitter(
      target,
      sources,
      options,
      semantic,
      global_initializers,
      mir,
      diagnostics).run();
}

} // namespace draft
