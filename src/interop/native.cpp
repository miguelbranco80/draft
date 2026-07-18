// C ABI boundary checks independent from LLVM's permissive type system.

#include "interop/native.h"

#include "interop/aarch64_abi.h"
#include "syntax/literal.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace draft {
namespace {

[[nodiscard]] bool has_body(const HirProgram &hir, SymbolId symbol) {
  return std::any_of(
      hir.procedures().begin(),
      hir.procedures().end(),
      [symbol](const HirProcedure &procedure) {
        return procedure.symbol == symbol && procedure.valid;
      });
}

[[nodiscard]] bool valid_c_signature(const TypeStore &types, TypeId id) {
  const Type &procedure = types.type(id);
  if (procedure.kind != TypeKind::Procedure || !procedure.c_calling_convention ||
      procedure.members.empty()) {
    return false;
  }
  for (std::size_t index = 0; index + 1 < procedure.members.size(); ++index) {
    if (classify_aarch64_darwin_c_type(types, procedure.members[index])
            .classification == Aarch64CAbiClass::Illegal) {
      return false;
    }
  }
  const TypeId result = procedure.members.back();
  return result == types.builtins().void_type ||
      classify_aarch64_darwin_c_type(types, result).classification !=
          Aarch64CAbiClass::Illegal;
}

[[nodiscard]] std::optional<std::string> decode_linker_name(
    std::string_view spelling) {
  if (spelling.empty()) return std::nullopt;
  if (spelling.front() == '"') {
    return decode_string_literal(spelling, TokenKind::StringLiteral);
  }
  return std::string(spelling);
}

// core/runtime.default_context is the one deliberate exception to ordinary
// user C-signature legality. Context contains ordinary Draft procedure pointers
// and is therefore not a user-portable C aggregate, but the compiler/runtime
// pair owns both sides of this versioned bridge and lowers the 96-byte result
// through the target's indirect aggregate-return rule.
[[nodiscard]] bool valid_default_context_bridge(
    const SemanticPackage &semantic,
    const NativeBinding &binding,
    const Symbol &symbol,
    const std::optional<std::string> &linker_name) {
  if (binding.kind != NativeBindingKind::ForeignImport ||
      binding.provider != "draft_runtime" ||
      !linker_name.has_value() ||
      *linker_name != "__draft.runtime.default_context" ||
      !semantic.runtime_context_type.is_valid()) {
    return false;
  }
  const Type &procedure = semantic.types.type(symbol.type);
  if (procedure.kind != TypeKind::Procedure ||
      !procedure.c_calling_convention || procedure.members.size() != 1 ||
      procedure.members.back() != semantic.runtime_context_type) {
    return false;
  }
  const Type &context = semantic.types.type(semantic.runtime_context_type);
  return context.kind == TypeKind::Struct && context.c_representation &&
      context.layout.known && context.layout.size == 96 &&
      context.layout.alignment == 8;
}

} // namespace

NativeInteropResult validate_native_interop(
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics) {
  NativeInteropResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  for (const NativeBinding &binding : semantic.native_bindings) {
    const Symbol &symbol = semantic.symbols.symbol(binding.symbol);
    if (symbol.kind != SymbolKind::Procedure || !symbol.type.is_valid() ||
        semantic.types.type(symbol.type).kind != TypeKind::Procedure) {
      diagnostics.error(
          symbol.name_range, "native import or export must declare a procedure");
      continue;
    }
    const std::optional<std::string> linker_name =
        decode_linker_name(binding.linker_name_spelling);
    if (!valid_c_signature(semantic.types, symbol.type) &&
        !valid_default_context_bridge(
            semantic, binding, symbol, linker_name)) {
      diagnostics.error(
          symbol.name_range,
          "native import or export requires a Draft 1 C-ABI-legal 'c proc' signature");
    }
    const bool body = has_body(hir, binding.symbol);
    if (binding.kind == NativeBindingKind::ForeignImport && body) {
      diagnostics.error(symbol.name_range, "foreign procedure cannot define a body");
    }
    if (binding.kind == NativeBindingKind::CExport && !body) {
      diagnostics.error(symbol.name_range, "exported procedure requires a body");
    }
    if (!linker_name.has_value() || linker_name->empty() ||
        linker_name->find('\0') != std::string::npos) {
      diagnostics.error(symbol.name_range, "native linker name is empty or invalid");
    }
    if (binding.kind == NativeBindingKind::ForeignImport &&
        !binding.provider.empty() &&
        std::find(result.providers.begin(), result.providers.end(), binding.provider) ==
            result.providers.end()) {
      result.providers.push_back(binding.provider);
    }
  }
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
