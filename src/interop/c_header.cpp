// C header generation from checked semantic C-ABI declarations.

#include "interop/c_header.h"

#include "interop/aarch64_abi.h"
#include "syntax/literal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool contains_type(
    const std::vector<TypeId> &types, TypeId needle) {
  return std::find(types.begin(), types.end(), needle) != types.end();
}

[[nodiscard]] bool c_identifier_character(char byte, bool first) {
  const unsigned char character = static_cast<unsigned char>(byte);
  return std::isalpha(character) != 0 || byte == '_' ||
      (!first && std::isdigit(character) != 0);
}

[[nodiscard]] bool c_keyword(std::string_view text) {
  static constexpr std::string_view keywords[] = {
      "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
      "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "alignas",
      "alignof", "auto", "bool", "break", "case", "char", "const", "constexpr",
      "continue", "default", "do", "double", "else", "enum", "extern", "false",
      "float", "for", "goto", "if", "inline", "int", "long", "nullptr",
      "register", "restrict", "return", "short", "signed", "sizeof", "static",
      "static_assert", "struct", "switch", "thread_local", "true", "typedef",
      "typeof", "typeof_unqual", "union", "unsigned", "void", "volatile", "while",
  };
  return std::find(std::begin(keywords), std::end(keywords), text) !=
      std::end(keywords);
}

[[nodiscard]] bool valid_c_identifier(std::string_view text) {
  if (text.empty() || c_keyword(text)) return false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (!c_identifier_character(text[index], index == 0)) return false;
  }
  return true;
}

[[nodiscard]] std::string safe_identifier(std::string_view text) {
  std::string result;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char byte = text[index];
    if (c_identifier_character(byte, result.empty())) {
      result += byte;
    } else {
      result += '_';
    }
  }
  if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
    result.insert(result.begin(), '_');
  }
  if (c_keyword(result)) result += '_';
  return result;
}

[[nodiscard]] std::string upper_identifier(std::string_view text) {
  std::string result = safe_identifier(text);
  for (char &byte : result) {
    byte = static_cast<char>(
        std::toupper(static_cast<unsigned char>(byte)));
  }
  return result;
}

[[nodiscard]] std::string c_string_bytes(std::string_view text) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const char raw_byte : text) {
    const unsigned char byte = static_cast<unsigned char>(raw_byte);
    if (byte == '\\' || byte == '"') {
      result += '\\';
      result += static_cast<char>(byte);
    } else if (byte >= 0x20U && byte <= 0x7eU) {
      result += static_cast<char>(byte);
    } else {
      result += "\\x";
      result += digits[(byte >> 4U) & 0x0fU];
      result += digits[byte & 0x0fU];
    }
  }
  return result;
}

class Emitter {
public:
  Emitter(
      const SemanticPackage &semantic,
      const TargetProfile &target,
      const CHeaderOptions &options,
      DiagnosticSink &diagnostics)
      : semantic_(semantic), target_(target), options_(options),
        diagnostics_(diagnostics) {}

  [[nodiscard]] CHeaderResult run() {
    CHeaderResult result;
    const std::size_t initial_errors = diagnostics_.error_count();
    collect_exports();
    emit_preamble();
    emit_forward_declarations();
    emit_enum_definitions();
    emit_procedure_typedefs();
    emit_aggregate_definitions();
    emit_exports();
    emit_postamble();
    result.ok = diagnostics_.error_count() == initial_errors;
    result.text = output_.str();
    result.export_count = exports_.size();
    return result;
  }

private:
  [[nodiscard]] const Type &type(TypeId id) const {
    return semantic_.types.type(id);
  }

  [[nodiscard]] std::string package_prefix() const {
    return "draft_" + safe_identifier(semantic_.short_name);
  }

  [[nodiscard]] std::string nominal_name(TypeId id) const {
    const Type &value = type(id);
    const std::string local = value.name.empty()
        ? "type_" + std::to_string(id.value)
        : safe_identifier(value.name);
    return package_prefix() + "_" + local;
  }

  [[nodiscard]] std::string procedure_name(TypeId id) const {
    return package_prefix() + "_proc_" + std::to_string(id.value);
  }

  [[nodiscard]] std::optional<SymbolId> owner_symbol(TypeId id) const {
    for (std::size_t index = 0;
         index < semantic_.symbols.symbol_count();
         ++index) {
      const SymbolId symbol{static_cast<std::uint32_t>(index)};
      const Symbol &candidate = semantic_.symbols.symbol(symbol);
      if (candidate.kind == SymbolKind::Type && candidate.type == id) {
        return symbol;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<SymbolId> aggregate_fields(TypeId id) const {
    std::vector<SymbolId> result;
    const std::optional<SymbolId> owner = owner_symbol(id);
    if (!owner.has_value()) return result;
    for (const AggregateMember &member : semantic_.aggregate_members) {
      if (member.owner == *owner) result.push_back(member.member);
    }
    return result;
  }

  [[nodiscard]] std::vector<SymbolId> enum_members(TypeId id) const {
    std::vector<SymbolId> result;
    const std::optional<SymbolId> owner = owner_symbol(id);
    if (!owner.has_value()) return result;
    for (const OwnedSemanticScope &scope : semantic_.owned_scopes) {
      if (scope.owner != *owner ||
          semantic_.symbols.scope(scope.scope).kind != ScopeKind::Type) {
        continue;
      }
      for (SymbolId symbol : semantic_.symbols.scope(scope.scope).symbols) {
        if (semantic_.symbols.symbol(symbol).kind == SymbolKind::EnumMember) {
          result.push_back(symbol);
        }
      }
      break;
    }
    return result;
  }

  [[nodiscard]] std::optional<BigInteger> enum_value(SymbolId member) const {
    for (const EnumMemberValue &value : semantic_.enum_member_values) {
      if (value.member == member) return value.value;
    }
    return std::nullopt;
  }

  void collect_type(TypeId id) {
    if (!id.is_valid()) return;
    const Type &value = type(id);
    if (value.kind == TypeKind::Procedure) {
      if (contains_type(procedure_seen_, id)) return;
      procedure_seen_.push_back(id);
      for (TypeId member : value.members) collect_type(member);
      procedure_types_.push_back(id);
      return;
    }
    if (value.kind == TypeKind::Pointer || value.kind == TypeKind::MultiPointer) {
      // A pointer may hide an arbitrary Draft pointee. Only collect a complete
      // C definition when the pointee can actually be named by the generated
      // header; otherwise declaration() deliberately renders the edge as
      // void *. This prevents pointer-only APIs from leaking zero-length
      // arrays, slices, or other non-C fields into an invalid C definition.
      // For a representable `^^C_Record`, the recursive walk still reaches the
      // nominal declaration; the seen sets below break record/callback cycles.
      if (typed_pointer_element(value.element)) {
        collect_type(value.element);
      }
      return;
    }
    if (value.kind == TypeKind::Array) {
      if (value.element_count != 0) collect_type(value.element);
      return;
    }
    if (value.kind != TypeKind::Struct && value.kind != TypeKind::RawUnion &&
        value.kind != TypeKind::Enum) {
      return;
    }
    if (!value.c_representation || contains_type(nominal_seen_, id) ||
        classify_aarch64_c_type(semantic_.types, id, target_.facts)
                .classification ==
            Aarch64CAbiClass::Illegal) {
      return;
    }
    nominal_seen_.push_back(id);
    for (TypeId member : value.members) collect_type(member);
    nominal_types_.push_back(id);
  }

  void collect_exports() {
    for (const NativeBinding &binding : semantic_.native_bindings) {
      if (binding.kind != NativeBindingKind::CExport) continue;
      exports_.push_back(&binding);
      const Symbol &symbol = semantic_.symbols.symbol(binding.symbol);
      if (!symbol.type.is_valid()) continue;
      const Type &signature = type(symbol.type);
      if (signature.kind != TypeKind::Procedure || signature.members.empty()) {
        continue;
      }
      for (TypeId member : signature.members) collect_type(member);
    }
  }

  [[nodiscard]] std::string integer_name(const Type &value) const {
    const bool signed_value = value.kind == TypeKind::SignedInteger;
    if (value.bit_width == 8) return signed_value ? "int8_t" : "uint8_t";
    if (value.bit_width == 16) return signed_value ? "int16_t" : "uint16_t";
    if (value.bit_width == 32) return signed_value ? "int32_t" : "uint32_t";
    if (value.bit_width == 64) return signed_value ? "int64_t" : "uint64_t";
    // The selected Clang target defines the AArch64 C ABI for its standard
    // `__int128` extension. There is no C `<stdint.h>` typedef for 128 bits, so
    // spelling the extension is the only header form matching Draft i128/u128.
    if (value.bit_width == 128) {
      return signed_value ? "__int128" : "unsigned __int128";
    }
    return "void";
  }

  [[nodiscard]] std::string base_type(TypeId id) const {
    const Type &value = type(id);
    switch (value.kind) {
    case TypeKind::Void: return "void";
    case TypeKind::SignedInteger:
    case TypeKind::UnsignedInteger: return integer_name(value);
    case TypeKind::Rune:
      // rune is a distinct i32 in the language. Its valid values happen to be
      // nonnegative Unicode scalars, but the C spelling must still preserve
      // the declared signed machine type rather than silently advertise u32.
      return "int32_t";
    case TypeKind::BooleanStorage:
    case TypeKind::EndianScalar:
      if (value.bit_width == 8) return "uint8_t";
      if (value.bit_width == 16) return "uint16_t";
      if (value.bit_width == 32) return "uint32_t";
      if (value.bit_width == 64) return "uint64_t";
      return "void";
    case TypeKind::Float:
      if (value.bit_width == 16) return "_Float16";
      if (value.bit_width == 32) return "float";
      if (value.bit_width == 64) return "double";
      return "void";
    case TypeKind::RawPointer: return "void *";
    case TypeKind::CString: return "char *";
    case TypeKind::Procedure: return procedure_name(id);
    case TypeKind::Struct:
    case TypeKind::RawUnion:
    case TypeKind::Enum:
      return value.c_representation ? nominal_name(id) : "void";
    default: return "void";
    }
  }

  [[nodiscard]] bool typed_pointer_element(TypeId id) const {
    const Type &value = type(id);
    if (value.kind == TypeKind::Array) {
      return value.element_count != 0 && typed_pointer_element(value.element);
    }
    if (value.kind == TypeKind::Pointer || value.kind == TypeKind::MultiPointer) {
      return typed_pointer_element(value.element);
    }
    if (value.kind == TypeKind::SignedInteger ||
        value.kind == TypeKind::UnsignedInteger ||
        value.kind == TypeKind::BooleanStorage || value.kind == TypeKind::Rune ||
        value.kind == TypeKind::EndianScalar || value.kind == TypeKind::Float ||
        value.kind == TypeKind::RawPointer || value.kind == TypeKind::CString) {
      return true;
    }
    // Only a C procedure has a C function-pointer declarator. An ordinary
    // Draft procedure carries the hidden Context ABI and is deliberately not
    // representable in a generated C header. A data pointer may still point to
    // storage containing that value under the general opaque-pointee rule, so
    // render ^proc as void * (and preserve any additional pointer layers).
    if (value.kind == TypeKind::Procedure) {
      return value.c_calling_convention;
    }
    return (value.kind == TypeKind::Struct || value.kind == TypeKind::RawUnion ||
            value.kind == TypeKind::Enum) &&
        value.c_representation &&
        classify_aarch64_c_type(semantic_.types, id, target_.facts)
                .classification !=
            Aarch64CAbiClass::Illegal;
  }

  [[nodiscard]] std::string declaration(TypeId id, std::string name) const {
    const Type &value = type(id);
    if (value.kind == TypeKind::Array) {
      return declaration(
          value.element,
          std::move(name) + "[" + std::to_string(value.element_count) + "]");
    }
    if (value.kind == TypeKind::Pointer || value.kind == TypeKind::MultiPointer) {
      const Type &element = type(value.element);
      if (!typed_pointer_element(value.element) &&
          element.kind != TypeKind::Pointer &&
          element.kind != TypeKind::MultiPointer) {
        // One pointer may intentionally hide any Draft pointee behind void.
        // Preserve outer pointer layers, though: ^^Opaque is void ** rather
        // than void *, because C code can observe and update the inner pointer
        // value even though it cannot name the ultimate Draft object.
        return "void *" + name;
      }

      // C pointer and array declarators nest around the identifier rather than
      // composing as prefix type strings. Recursing with the identifier makes
      // `^^u8` become `uint8_t **name` and `^[4]u8` become
      // `uint8_t (*name)[4]` without a general declarator AST.
      std::string pointer_name = "*" + name;
      if (element.kind == TypeKind::Array) {
        pointer_name = "(" + pointer_name + ")";
      }
      return declaration(value.element, std::move(pointer_name));
    }
    return base_type(id) + " " + name;
  }

  [[nodiscard]] std::string parameter_list(const Type &signature) const {
    if (signature.members.size() <= 1) return "void";
    std::string result;
    for (std::size_t index = 0; index + 1 < signature.members.size(); ++index) {
      if (index != 0) result += ", ";
      result += declaration(
          signature.members[index], "arg" + std::to_string(index));
    }
    return result;
  }

  void emit_preamble() {
    const std::string guard = options_.include_guard.empty()
        ? "DRAFT_" + upper_identifier(semantic_.short_name) + "_H"
        : upper_identifier(options_.include_guard);
    guard_ = guard;
    output_ << "// Generated by the Draft compiler. Do not edit.\n"
            << "#ifndef " << guard << "\n"
            << "#define " << guard << "\n\n"
            << "#include <stddef.h>\n"
            << "#include <stdint.h>\n\n"
            << "#if defined(__cplusplus)\n"
            << "#define DRAFT_STATIC_ASSERT static_assert\n"
            << "#define DRAFT_ALIGNOF alignof\n"
            << "#else\n"
            << "#define DRAFT_STATIC_ASSERT _Static_assert\n"
            << "#define DRAFT_ALIGNOF _Alignof\n"
            << "#endif\n\n";
  }

  void emit_forward_declarations() {
    for (TypeId id : nominal_types_) {
      const Type &value = type(id);
      if (value.kind == TypeKind::Struct) {
        output_ << "typedef struct " << nominal_name(id) << ' '
                << nominal_name(id) << ";\n";
      } else if (value.kind == TypeKind::RawUnion) {
        output_ << "typedef union " << nominal_name(id) << ' '
                << nominal_name(id) << ";\n";
      }
    }
    if (!nominal_types_.empty()) output_ << '\n';
  }

  void emit_procedure_typedefs() {
    for (TypeId id : procedure_types_) {
      const Type &signature = type(id);
      if (!signature.c_calling_convention || signature.members.empty()) continue;
      // Feed the entire function-pointer declarator through the result type.
      // This keeps nested pointers and pointer-to-array returns in their C
      // binding positions instead of flattening them into an approximate base
      // spelling.
      const std::string declarator =
          "(*" + procedure_name(id) + ")(" + parameter_list(signature) + ")";
      output_ << "typedef "
              << declaration(signature.members.back(), declarator) << ";\n";
    }
    if (!procedure_types_.empty()) output_ << '\n';
  }

  // Renders an exact, warning-free C integer constant for an enum macro. Plain
  // decimal text is insufficient at the boundaries: 2^64-1 has no signed C
  // literal type, and spelling -2^63 first forms an out-of-range positive
  // token. The stdint constant macros make every <=64-bit case explicit.
  //
  // C has no INT128_C macro. For a fixed 128-bit backing, assemble the value
  // from two UINT64_C halves in unsigned __int128 and cast only after the bit
  // pattern is complete. A negative signed minimum uses -(magnitude - 1) - 1,
  // so no unrepresentable positive signed intermediate is ever formed.
  [[nodiscard]] std::optional<std::string> enum_integer_expression(
      const BigInteger &integer, TypeId backing) const {
    const Type &storage = type(backing);
    const bool signed_value = storage.kind == TypeKind::SignedInteger;
    const std::uint32_t bits = storage.bit_width;
    const BigInteger magnitude = integer.absolute();

    if (bits <= 64 && (signed_value ||
                       storage.kind == TypeKind::UnsignedInteger)) {
      const std::optional<std::uint64_t> value = magnitude.to_u64();
      if (!value.has_value()) return std::nullopt;
      const std::string macro =
          std::string(signed_value ? "INT" : "UINT") +
          std::to_string(bits) + "_C";
      if (!integer.is_negative()) {
        return macro + "(" + std::to_string(*value) + ")";
      }
      const BigInteger signed_minimum_magnitude =
          BigInteger::from_u64(1).shifted_left(bits - 1U);
      if (magnitude == signed_minimum_magnitude) {
        return "(-" + macro + "(" + std::to_string(*value - 1U) +
            ") - " + macro + "(1))";
      }
      return "(-" + macro + "(" + std::to_string(*value) + "))";
    }

    if (bits == 128 && (signed_value ||
                        storage.kind == TypeKind::UnsignedInteger)) {
      const auto unsigned_128 = [](const BigInteger &value)
          -> std::optional<std::string> {
        const BigInteger base = BigInteger::from_u64(1).shifted_left(64);
        BigInteger high;
        BigInteger low;
        if (!value.divide(base, high, low)) return std::nullopt;
        const std::optional<std::uint64_t> high_u64 = high.to_u64();
        const std::optional<std::uint64_t> low_u64 = low.to_u64();
        if (!high_u64.has_value() || !low_u64.has_value()) {
          return std::nullopt;
        }
        return "(((unsigned __int128)UINT64_C(" +
            std::to_string(*high_u64) + ") << 64) | " +
            "(unsigned __int128)UINT64_C(" +
            std::to_string(*low_u64) + "))";
      };
      if (!signed_value) return unsigned_128(magnitude);
      if (!integer.is_negative()) {
        const std::optional<std::string> positive = unsigned_128(magnitude);
        return positive.has_value()
            ? std::optional<std::string>("((__int128)" + *positive + ")")
            : std::nullopt;
      }
      const BigInteger one = BigInteger::from_u64(1);
      const std::optional<std::string> lowered =
          unsigned_128(magnitude.subtracted(one));
      return lowered.has_value()
          ? std::optional<std::string>(
                "(-((__int128)" + *lowered + ") - 1)")
          : std::nullopt;
    }
    return std::nullopt;
  }

  void emit_enum(TypeId id) {
    const Type &value = type(id);
    output_ << "typedef " << base_type(value.element) << ' ' << nominal_name(id)
            << ";\n";
    for (SymbolId member : enum_members(id)) {
      const Symbol &symbol = semantic_.symbols.symbol(member);
      const std::optional<BigInteger> integer = enum_value(member);
      if (!integer.has_value()) continue;
      const std::optional<std::string> expression =
          enum_integer_expression(*integer, value.element);
      if (!expression.has_value()) {
        diagnostics_.error(
            symbol.name_range,
            "cannot render enum value in the generated C header");
        continue;
      }
      output_ << "#define " << upper_identifier(nominal_name(id)) << '_'
              << upper_identifier(symbol.name) << " ((" << nominal_name(id)
              << ")" << *expression
              << ")\n";
    }
    output_ << "DRAFT_STATIC_ASSERT(sizeof(" << nominal_name(id) << ") == "
            << value.layout.size << ", \"Draft enum size mismatch\");\n\n";
  }

  void emit_aggregate(TypeId id) {
    const Type &value = type(id);
    const bool structure = value.kind == TypeKind::Struct;
    const std::vector<SymbolId> fields = aggregate_fields(id);
    output_ << (structure ? "struct " : "union ");
    if (value.requested_alignment != 0) {
      // Clang accepts a type attribute between the struct/union keyword and
      // its tag. Placing it after the tag but before the opening brace looks
      // plausible, but is not valid C and used to leave only an incomplete
      // forward declaration in generated headers.
      output_ << "__attribute__((aligned(" << value.requested_alignment << "))) ";
    }
    output_ << nominal_name(id) << " {\n";
    for (std::size_t index = 0; index < value.members.size(); ++index) {
      const std::string field = index < fields.size()
          ? safe_identifier(semantic_.symbols.symbol(fields[index]).name)
          : "field_" + std::to_string(index);
      output_ << "    " << declaration(value.members[index], field) << ";\n";
    }
    output_ << "};\n"
            << "DRAFT_STATIC_ASSERT(sizeof(" << nominal_name(id) << ") == "
            << value.layout.size << ", \"Draft aggregate size mismatch\");\n"
            << "DRAFT_STATIC_ASSERT(DRAFT_ALIGNOF(" << nominal_name(id) << ") == "
            << value.layout.alignment << ", \"Draft aggregate alignment mismatch\");\n";
    if (structure) {
      for (std::size_t index = 0;
           index < value.members.size() && index < value.member_offsets.size();
           ++index) {
        const std::string field = index < fields.size()
            ? safe_identifier(semantic_.symbols.symbol(fields[index]).name)
            : "field_" + std::to_string(index);
        output_ << "DRAFT_STATIC_ASSERT(offsetof(" << nominal_name(id) << ", "
                << field << ") == " << value.member_offsets[index]
                << ", \"Draft field offset mismatch\");\n";
      }
    }
    output_ << '\n';
  }

  void emit_enum_definitions() {
    for (TypeId id : nominal_types_) {
      if (type(id).kind == TypeKind::Enum) emit_enum(id);
    }
  }

  void emit_aggregate_definitions() {
    for (TypeId id : nominal_types_) {
      if (type(id).kind != TypeKind::Enum) emit_aggregate(id);
    }
  }

  [[nodiscard]] std::optional<std::string> linker_name(
      const NativeBinding &binding) const {
    if (binding.linker_name_spelling.empty()) return std::nullopt;
    if (binding.linker_name_spelling.front() == '"') {
      return decode_string_literal(
          binding.linker_name_spelling, TokenKind::StringLiteral);
    }
    return binding.linker_name_spelling;
  }

  void emit_exports() {
    if (exports_.empty()) {
      output_ << "// This package declares no C exports.\n\n";
      return;
    }
    output_ << "#if defined(__cplusplus)\nextern \"C\" {\n#endif\n\n";
    for (const NativeBinding *binding : exports_) {
      const Symbol &symbol = semantic_.symbols.symbol(binding->symbol);
      const Type &signature = type(symbol.type);
      const std::optional<std::string> exact = linker_name(*binding);
      if (!exact.has_value() || signature.kind != TypeKind::Procedure ||
          signature.members.empty()) {
        diagnostics_.error(
            symbol.name_range, "cannot emit malformed C export in header");
        continue;
      }
      const bool direct_name = valid_c_identifier(*exact);
      const std::string declaration_name = direct_name
          ? *exact
          : package_prefix() + "_" + safe_identifier(symbol.name);
      const std::string declarator =
          declaration_name + "(" + parameter_list(signature) + ")";
      output_ << "extern "
              << declaration(signature.members.back(), declarator);
      if (!direct_name) {
        // An asm label names the final object symbol. Mach-O C identifiers gain
        // one platform underscore; ELF identifiers do not. The quoted Draft
        // linker name itself remains target-independent source text.
        const std::string_view prefix =
            target_.facts.object_format == "macho" ? "_" : "";
        output_ << " __asm__(\"" << prefix << c_string_bytes(*exact) << "\")";
      }
      output_ << ";\n";
    }
    output_ << "\n#if defined(__cplusplus)\n}\n#endif\n\n";
  }

  void emit_postamble() {
    output_ << "#undef DRAFT_ALIGNOF\n"
            << "#undef DRAFT_STATIC_ASSERT\n\n"
            << "#endif // " << guard_ << '\n';
  }

  const SemanticPackage &semantic_;
  const TargetProfile &target_;
  const CHeaderOptions &options_;
  DiagnosticSink &diagnostics_;
  std::vector<const NativeBinding *> exports_;
  std::vector<TypeId> nominal_seen_;
  std::vector<TypeId> nominal_types_;
  std::vector<TypeId> procedure_seen_;
  std::vector<TypeId> procedure_types_;
  std::string guard_;
  std::ostringstream output_;
};

} // namespace

CHeaderResult emit_c_header(
    const SemanticPackage &semantic,
    const TargetProfile &target,
    const CHeaderOptions &options,
    DiagnosticSink &diagnostics) {
  return Emitter(semantic, target, options, diagnostics).run();
}

} // namespace draft
