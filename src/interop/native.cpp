// C ABI boundary checks independent from LLVM's permissive type system.

#include "interop/native.h"

#include "interop/c_abi.h"
#include "syntax/literal.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] bool has_body(
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    SymbolId symbol) {
  for (std::size_t index : selected_indices) {
    if (index >= procedures.size()) continue;
    for (const HirProcedure &procedure :
         procedures[index].program.procedures()) {
      if (procedure.symbol == symbol && procedure.valid) return true;
    }
  }
  return false;
}

[[nodiscard]] bool valid_c_signature(
    const TypeStore &types,
    TypeId id,
    const CAbiTable &abi) {
  const Type &procedure = types.type(id);
  if (procedure.kind != TypeKind::Procedure || !procedure.c_calling_convention ||
      procedure.members.empty()) {
    return false;
  }
  for (std::size_t index = 0; index + 1 < procedure.members.size(); ++index) {
    const CAbiType *parameter = abi.find(procedure.members[index]);
    if (parameter == nullptr ||
        parameter->classification == CAbiClass::Illegal) {
      return false;
    }
  }
  const TypeId result = procedure.members.back();
  if (result == types.builtins().void_type) return true;
  const CAbiType *classified_result = abi.find(result);
  return classified_result != nullptr &&
      classified_result->classification != CAbiClass::Illegal;
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
      !procedure.c_calling_convention || procedure.c_variadic ||
      procedure.members.size() != 1 ||
      procedure.members.back() != semantic.runtime_context_type) {
    return false;
  }
  const Type &context = semantic.types.type(semantic.runtime_context_type);
  return context.kind == TypeKind::Struct && context.c_representation &&
      context.layout.known && context.layout.size == 96 &&
      context.layout.alignment == 8;
}

[[nodiscard]] bool contains_type(
    const std::vector<TypeId> &types, TypeId candidate) {
  return std::find(types.begin(), types.end(), candidate) != types.end();
}

// The compiler-owned default-context entry is the sole intentionally private
// C signature in Draft 1. Consumer packages receive it through the canonical
// core/runtime interface, where it must retain the same narrow exemption as
// the defining package's native binding. Matching provider and linker identity
// prevents an unrelated package member named default_context from borrowing it.
[[nodiscard]] bool imported_default_context_bridge(
    const SemanticPackage &semantic, SymbolId symbol) {
  for (const ImportedSymbol &imported : semantic.imported_symbols_for_read()) {
    if (imported.proxy != symbol || imported.public_name != "default_context" ||
        imported.root_relative_path != "runtime" ||
        imported.native_provider != "draft_runtime") {
      continue;
    }
    const std::optional<std::string> linker_name =
        decode_linker_name(imported.native_linker_name_spelling);
    return linker_name.has_value() &&
        *linker_name == "__draft.runtime.default_context";
  }
  return false;
}

[[nodiscard]] bool local_default_context_bridge(
    const SemanticPackage &semantic, SymbolId symbol) {
  const Symbol &candidate = semantic.symbols.symbol(symbol);
  for (const NativeBinding &binding : semantic.native_bindings) {
    if (binding.symbol != symbol) continue;
    const std::optional<std::string> linker_name =
        decode_linker_name(binding.linker_name_spelling);
    return valid_default_context_bridge(
        semantic, binding, candidate, linker_name);
  }
  return false;
}

// Finds concrete C procedure types even when they are nested behind pointers,
// arrays, records, or other procedure signatures. A pointer can make its
// pointee opaque at one C data boundary, but source code can later dereference
// and call a c proc value, so that callable's own ABI must still be valid.
void validate_c_procedure_type_graph(
    const SemanticPackage &semantic,
    TypeId root,
    SourceRange diagnostic_range,
    bool exempt_root,
    const CAbiTable &abi,
    std::vector<TypeId> &diagnosed,
    DiagnosticSink &diagnostics) {
  std::vector<TypeId> visited;
  const auto visit = [&](const auto &self, TypeId type_id, bool is_root) -> void {
    if (!type_id.is_valid() || contains_type(visited, type_id)) return;
    visited.push_back(type_id);
    const Type &type = semantic.types.type(type_id);
    if (type.kind == TypeKind::Procedure && type.c_calling_convention) {
      const CAbiType &classification = *abi.find(type_id);
      const bool legal =
          classification.classification != CAbiClass::Illegal;
      if (!legal) {
        if (!(is_root && exempt_root) && !contains_type(diagnosed, type_id)) {
          diagnostics.error(
              diagnostic_range,
              "c proc parameter and result types must be Draft 1 C-ABI-legal");
          diagnosed.push_back(type_id);
        }
        return;
      }
    }
    if (type.kind == TypeKind::Pointer ||
        type.kind == TypeKind::MultiPointer || type.kind == TypeKind::Slice ||
        type.kind == TypeKind::Array || type.kind == TypeKind::Simd ||
        type.kind == TypeKind::Distinct) {
      self(self, type.element, false);
    }
    for (TypeId member : type.members) self(self, member, false);
  };
  visit(visit, root, true);
}

} // namespace

NativeInteropResult validate_native_interop(
    const SemanticPackage &semantic,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const CAbiTable &abi,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  NativeInteropResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  std::optional<std::size_t> previous;
  for (std::size_t index : selected_indices) {
    if (index >= procedures.size() ||
        (previous.has_value() && index <= *previous)) {
      diagnostics.error(
          SourceRange::invalid(),
          "native interop received an invalid selected body product order");
      return result;
    }
    previous = index;
  }
  // Native validation is a consumer of the explicit ABI facet, not another
  // hidden classifier pass. A missing row or mismatched target is an internal
  // orchestration failure: continuing would make the accepted C boundary
  // depend on which consumer happened to run first.
  if (!abi.complete_for(semantic.types, target)) {
    diagnostics.error(
        SourceRange::invalid(),
        "native interop requires a complete ABI classification table");
    return result;
  }
  std::vector<TypeId> diagnosed_c_procedures;
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
    const Type &procedure = semantic.types.type(symbol.type);
    if (!valid_c_signature(semantic.types, symbol.type, abi) &&
        !valid_default_context_bridge(
            semantic, binding, symbol, linker_name)) {
      diagnostics.error(
          symbol.name_range,
          "native import or export requires a Draft 1 C-ABI-legal 'c proc' signature");
      diagnosed_c_procedures.push_back(symbol.type);
    }
    // Draft can call a foreign C variadic procedure because every call site
    // supplies and promotes the complete unnamed tail. Defining or exporting
    // one would require a source-level va_list/va_start consumption model,
    // which is intentionally not part of this first interoperation slice.
    if (procedure.c_variadic &&
        binding.kind != NativeBindingKind::ForeignImport) {
      diagnostics.error(
          symbol.name_range,
          "Draft cannot export a C variadic procedure");
    }
    const bool body = has_body(procedures, selected_indices, binding.symbol);
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

  // A local c proc, callback type, or callback nested in a record uses the same
  // ABI as an import/export and therefore needs the same legality check. Native
  // bindings above keep their more specific diagnostic; this walk covers every
  // remaining symbol/type use and reports one error for each interned signature.
  for (std::size_t index = 0; index < semantic.symbols.symbol_count(); ++index) {
    const SymbolId symbol_id{static_cast<std::uint32_t>(index)};
    const Symbol &symbol = semantic.symbols.symbol(symbol_id);
    if (!symbol.type.is_valid()) continue;
    const bool bridge = imported_default_context_bridge(semantic, symbol_id) ||
        local_default_context_bridge(semantic, symbol_id);
    validate_c_procedure_type_graph(
        semantic,
        symbol.type,
        symbol.name_range,
        bridge,
        abi,
        diagnosed_c_procedures,
        diagnostics);
  }
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

NativeInteropResult validate_native_interop(
    const SemanticPackage &semantic,
    std::span<const ProcedureBodyHirResult> procedures,
    const CAbiTable &abi,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  std::vector<std::size_t> selected;
  selected.reserve(procedures.size());
  for (std::size_t index = 0; index < procedures.size(); ++index) {
    selected.push_back(index);
  }
  return validate_native_interop(
      semantic, procedures, selected, abi, target, diagnostics);
}

} // namespace draft
