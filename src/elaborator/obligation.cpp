// Canonical obligation construction for the provider-independent elaborator.

#include "elaborator/obligation.h"

#include "base/sha256.h"
#include "sema/denial.h"
#include "sema/interface.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - 1 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(bytes);
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

[[nodiscard]] bool is_obligation_kind(AgentConstructKind kind) {
  return kind != AgentConstructKind::Documentation;
}

[[nodiscard]] std::string source_relative_path(
    const LoadedPackage &loaded, FileId file) {
  for (const LoadedPackageFile &entry : loaded.files) {
    if (entry.source == file) return entry.relative_name;
  }
  return {};
}

[[nodiscard]] const SyntaxTree *find_tree(
    const LoadedPackage &loaded, FileId file) {
  for (const LoadedPackageFile &entry : loaded.files) {
    if (entry.source == file && entry.syntax.has_value()) return &*entry.syntax;
  }
  return nullptr;
}

[[nodiscard]] std::string anchor_name(
    const SemanticPackage &package, SymbolId anchor) {
  return anchor.is_valid() ? package.symbols.symbol(anchor).name : std::string();
}

[[nodiscard]] bool already_seen(
    const std::vector<std::string> &names, std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

[[nodiscard]] bool denies_symbol(
    std::span<const ResolvedDenialSelector> denials, SymbolId symbol) {
  return std::any_of(
      denials.begin(), denials.end(),
      [symbol](const ResolvedDenialSelector &denial) {
        return denial.kind == ResolvedDenialKind::Symbol &&
            denial.symbol == symbol;
      });
}

[[nodiscard]] bool denies_package(
    std::span<const ResolvedDenialSelector> denials,
    const ImportBinding &binding) {
  return std::any_of(
      denials.begin(), denials.end(),
      [&binding](const ResolvedDenialSelector &denial) {
        return denial.kind == ResolvedDenialKind::ImportedPackage &&
            denial.root_identity == binding.root_identity &&
            denial.root_relative_path == binding.root_relative_path;
      });
}

[[nodiscard]] bool denies_all_context(
    std::span<const ResolvedDenialSelector> denials) {
  return std::any_of(
      denials.begin(), denials.end(),
      [](const ResolvedDenialSelector &denial) {
        return denial.kind == ResolvedDenialKind::Context;
      });
}

[[nodiscard]] bool denies_context_field(
    std::span<const ResolvedDenialSelector> denials,
    std::string_view name) {
  return denies_all_context(denials) || std::any_of(
      denials.begin(), denials.end(),
      [name](const ResolvedDenialSelector &denial) {
        return denial.kind == ResolvedDenialKind::ContextField &&
            denial.field == name;
      });
}

// Produces one source-oriented type spelling from the same canonical TypeStore
// row used by checking. Nominal types stop at their visible name; structural
// types recursively expose their complete shape. This is deliberately kept
// here, beside obligation construction, so provider context cannot drift from
// the type graph whose digest protects the request.
[[nodiscard]] std::string type_text(
    const SemanticPackage &package, TypeId type_id) {
  const Type &type = package.types.type(type_id);
  switch (type.kind) {
  case TypeKind::Invalid: return "<invalid>";
  case TypeKind::Void: return "void";
  case TypeKind::UntypedInteger: return "untyped integer";
  case TypeKind::UntypedFloat: return "untyped float";
  case TypeKind::Bool:
  case TypeKind::BooleanStorage:
  case TypeKind::SignedInteger:
  case TypeKind::UnsignedInteger:
  case TypeKind::Float:
  case TypeKind::Rune:
  case TypeKind::EndianScalar:
  case TypeKind::RawPointer:
  case TypeKind::CString:
  case TypeKind::String:
  case TypeKind::Struct:
  case TypeKind::Enum:
  case TypeKind::TaggedUnion:
  case TypeKind::RawUnion:
  case TypeKind::Distinct:
  case TypeKind::TypeParameter:
    return type.name.empty()
        ? std::string(type_kind_name(type.kind))
        : type.name;
  case TypeKind::Pointer:
    return "^" + type_text(package, type.element);
  case TypeKind::MultiPointer:
    return "[^]" + type_text(package, type.element);
  case TypeKind::Slice:
    return "[]" + type_text(package, type.element);
  case TypeKind::Array:
  case TypeKind::Simd: {
    std::string count = std::to_string(type.element_count);
    if (type.owner_evaluated_element_count) {
      count = "owner-evaluated";
    } else if (type.element_count_expression.is_valid()) {
      count = integer_expression_identity(type.element_count_expression);
    }
    const std::string prefix =
        type.kind == TypeKind::Simd ? "#simd[" : "[";
    return prefix + count + "]" + type_text(package, type.element);
  }
  case TypeKind::Tuple: {
    std::string result = "(";
    for (std::size_t index = 0; index < type.members.size(); ++index) {
      if (index != 0) result += ", ";
      result += type_text(package, type.members[index]);
    }
    result += ")";
    return result;
  }
  case TypeKind::Procedure: {
    std::string result = type.c_calling_convention ? "c proc(" : "proc(";
    if (type.members.empty()) return result + ")";
    for (std::size_t index = 0; index + 1 < type.members.size(); ++index) {
      if (index != 0) result += ", ";
      result += type_text(package, type.members[index]);
    }
    result += ")";
    const TypeId return_type = type.members.back();
    if (package.types.type(return_type).kind != TypeKind::Void) {
      result += " -> " + type_text(package, return_type);
    }
    return result;
  }
  }
  return "<invalid>";
}

void append_context_u64(std::uint64_t value, std::string &output) {
  output += std::to_string(value);
  output.push_back('\n');
}

void append_context_field(
    std::string_view name,
    std::string_view value,
    std::string &output) {
  output += name;
  output.push_back(' ');
  append_context_u64(static_cast<std::uint64_t>(value.size()), output);
  output.append(value);
  output.push_back('\n');
}

[[nodiscard]] std::string interface_id_text(InterfaceTypeId id) {
  return id.is_valid() ? std::to_string(id.value) : "invalid";
}

// Constant payloads appear in generic value arguments. Every field is emitted
// even when the current kind does not use it; that makes this representation a
// direct, future-auditable reflection of the canonical interface row rather
// than a second semantic interpretation of constants.
void append_constant_context(
    const ConstantValue &value,
    std::string &output) {
  append_context_field(
      "CONSTANT_KIND",
      std::to_string(static_cast<std::uint32_t>(value.kind)),
      output);
  append_context_field(
      "CONSTANT_BOOL", value.boolean ? "true" : "false", output);
  append_context_field(
      "CONSTANT_INTEGER", value.integer.to_decimal(), output);
  append_context_field(
      "CONSTANT_FLOAT", value.floating.to_fraction(), output);
  append_context_field(
      "CONSTANT_FLOAT_BITS", std::to_string(value.float_bit_width), output);
  append_context_field(
      "CONSTANT_FLOAT_PAYLOAD", std::to_string(value.float_bits), output);
  append_context_field("CONSTANT_TEXT", value.text, output);
  append_context_field(
      "CONSTANT_SYMBOL_INDEX", std::to_string(value.symbol_index), output);
  append_context_field(
      "CONSTANT_ROOT_IDENTITY", value.root_identity, output);
  append_context_field(
      "CONSTANT_ROOT_RELATIVE_PATH", value.root_relative_path, output);
  append_context_field(
      "CONSTANT_VARIANT", std::to_string(value.variant_index), output);
  output += "CONSTANT_ELEMENTS ";
  append_context_u64(
      static_cast<std::uint64_t>(value.elements.size()), output);
  for (const ConstantValue &element : value.elements) {
    append_constant_context(element, output);
  }
}

// Procedure constants contain a process-local SymbolId while semantic checking
// is in progress. Canonical provider context uses the same package-qualified
// identity rule as public interfaces and clears that local index recursively.
[[nodiscard]] ConstantValue canonical_constant(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    ConstantValue value) {
  for (ConstantValue &element : value.elements) {
    element = canonical_constant(identity, package, std::move(element));
  }
  if (value.kind != ConstantKind::Procedure) return value;
  if (value.root_identity.empty() &&
      value.symbol_index != std::numeric_limits<std::uint32_t>::max() &&
      value.symbol_index < package.symbols.symbol_count()) {
    const SymbolId referenced{value.symbol_index};
    bool imported_identity = false;
    for (const ImportedSymbol &imported : package.imported_symbols) {
      if (imported.proxy != referenced) continue;
      value.root_identity = imported.root_identity;
      value.root_relative_path = imported.root_relative_path;
      value.text = imported.public_name;
      imported_identity = true;
      break;
    }
    if (!imported_identity) {
      value.root_identity = identity.root_identity;
      value.root_relative_path = identity.root_relative_path;
      value.text = package.symbols.symbol(referenced).name;
    }
  }
  value.symbol_index = std::numeric_limits<std::uint32_t>::max();
  return value;
}

// This is deliberately a labeled, length-prefixed text format rather than a
// JSON dependency. It is readable in a provider prompt, unambiguous for strings
// containing whitespace, and simple enough to keep beside the type graph whose
// fields it mirrors.
[[nodiscard]] std::string render_type_context(
    const InterfaceTypeGraph &graph) {
  std::string output;
  append_context_field(
      "TYPE_GRAPH_ROOT", interface_id_text(graph.root), output);
  output += "TYPE_ROWS ";
  append_context_u64(
      static_cast<std::uint64_t>(graph.types.size()), output);
  for (std::size_t index = 0; index < graph.types.size(); ++index) {
    const InterfaceType &type = graph.types[index];
    append_context_field("TYPE_ROW", std::to_string(index), output);
    append_context_field(
        "TYPE_KIND", std::string(type_kind_name(type.kind)), output);
    append_context_field("TYPE_NAME", type.name, output);
    append_context_field(
        "TYPE_NOMINAL_ROOT_IDENTITY", type.nominal_root_identity, output);
    append_context_field(
        "TYPE_NOMINAL_ROOT_RELATIVE_PATH",
        type.nominal_root_relative_path,
        output);
    append_context_field(
        "TYPE_NOMINAL_PUBLIC_NAME", type.nominal_public_name, output);
    append_context_field(
        "TYPE_LAYOUT_KNOWN", type.layout.known ? "true" : "false", output);
    append_context_field(
        "TYPE_SIZE", std::to_string(type.layout.size), output);
    append_context_field(
        "TYPE_ALIGNMENT", std::to_string(type.layout.alignment), output);
    append_context_field(
        "TYPE_BIT_WIDTH", std::to_string(type.bit_width), output);
    append_context_field(
        "TYPE_ELEMENT", interface_id_text(type.element), output);
    append_context_field(
        "TYPE_ELEMENT_COUNT", std::to_string(type.element_count), output);
    append_context_field(
        "TYPE_ELEMENT_COUNT_EXPRESSION",
        type.owner_evaluated_element_count
            ? "owner-evaluated"
            : type.element_count_expression.is_valid()
            ? integer_expression_identity(type.element_count_expression)
            : "none",
        output);
    append_context_field(
        "TYPE_OWNER_EVALUATED_ELEMENT_COUNT",
        type.owner_evaluated_element_count ? "true" : "false",
        output);
    append_context_field(
        "TYPE_C_CALLING_CONVENTION",
        type.c_calling_convention ? "true" : "false",
        output);
    append_context_field(
        "TYPE_C_REPRESENTATION",
        type.c_representation ? "true" : "false",
        output);
    append_context_field(
        "TYPE_REQUESTED_ALIGNMENT",
        std::to_string(type.requested_alignment),
        output);
    output += "TYPE_MEMBERS ";
    append_context_u64(
        static_cast<std::uint64_t>(type.members.size()), output);
    for (InterfaceTypeId member : type.members) {
      append_context_field("TYPE_MEMBER", interface_id_text(member), output);
    }
    output += "TYPE_MEMBER_OFFSETS ";
    append_context_u64(
        static_cast<std::uint64_t>(type.member_offsets.size()), output);
    for (std::uint64_t offset : type.member_offsets) {
      append_context_field("TYPE_MEMBER_OFFSET", std::to_string(offset), output);
    }
    output += "TYPE_NOMINAL_MEMBERS ";
    append_context_u64(
        static_cast<std::uint64_t>(type.nominal_members.size()), output);
    for (const InterfaceMember &member : type.nominal_members) {
      append_context_field("MEMBER_NAME", member.name, output);
      append_context_field(
          "MEMBER_KIND", std::string(symbol_kind_name(member.kind)), output);
      append_context_field("MEMBER_TYPE", interface_id_text(member.type), output);
      append_context_field(
          "MEMBER_OFFSET", std::to_string(member.offset), output);
      append_context_field(
          "MEMBER_HAS_ENUM_VALUE",
          member.has_enum_value ? "true" : "false",
          output);
      append_context_field(
          "MEMBER_ENUM_VALUE", member.enum_value.to_decimal(), output);
    }
    output += "TYPE_NOMINAL_ARGUMENTS ";
    append_context_u64(
        static_cast<std::uint64_t>(type.nominal_arguments.size()), output);
    for (const InterfaceNominalArgument &argument : type.nominal_arguments) {
      append_context_field(
          "ARGUMENT_KIND", argument.is_type ? "type" : "value", output);
      append_context_field(
          "ARGUMENT_TYPE", interface_id_text(argument.type), output);
      append_context_field(
          "ARGUMENT_VALUE_TYPE",
          interface_id_text(argument.value_type),
          output);
      append_context_field(
          "ARGUMENT_VALUE_EXPRESSION",
          argument.owner_evaluated_value
              ? "owner-evaluated"
              : (argument.value_expression.is_valid()
                     ? integer_expression_identity(argument.value_expression)
                     : "none"),
          output);
      append_constant_context(argument.value, output);
    }
  }
  return output;
}

void add_type_context(
    const InterfaceTypeGraph &graph,
    std::vector<AgentTypeContext> &contexts) {
  AgentTypeContext context;
  context.type_digest = hash_interface_type_graph(graph);
  context.definition = render_type_context(graph);
  context.definition_digest = sha256(context.definition);
  for (const AgentTypeContext &existing : contexts) {
    if (existing.type_digest == context.type_digest &&
        existing.definition_digest == context.definition_digest) {
      return;
    }
  }
  contexts.push_back(std::move(context));
}

[[nodiscard]] std::string_view constraint_name(TypeConstraintKind kind);

void append_path_context(
    std::string_view count_name,
    std::string_view element_name,
    const std::vector<std::string> &path,
    std::string &output) {
  output += count_name;
  output.push_back(' ');
  append_context_u64(static_cast<std::uint64_t>(path.size()), output);
  for (const std::string &field : path) {
    append_context_field(element_name, field, output);
  }
}

void append_imported_effect_context(
    const ImportedEffect &effect,
    std::string &output);

void append_imported_flow_value_context(
    const ImportedFlowValue &value,
    std::string &output) {
  append_context_field(
      "FLOW_UNKNOWN", value.unknown ? "true" : "false", output);
  output += "FLOW_SLOTS ";
  append_context_u64(
      static_cast<std::uint64_t>(value.flow_slots.size()), output);
  for (const ImportedReturnFlowSlot &slot : value.flow_slots) {
    append_context_field(
        "FLOW_PARAMETER", std::to_string(slot.parameter), output);
    append_context_field(
        "FLOW_CONTEXT", slot.context ? "true" : "false", output);
    append_path_context("FLOW_PATH", "FLOW_PATH_FIELD", slot.path, output);
  }
  output += "FLOW_CONTRACT_EFFECTS ";
  append_context_u64(
      static_cast<std::uint64_t>(value.contract_effects.size()), output);
  for (const ImportedEffect &effect : value.contract_effects) {
    append_imported_effect_context(effect, output);
  }
}

void append_imported_effect_context(
    const ImportedEffect &effect,
    std::string &output) {
  append_context_field(
      "EFFECT_KIND", std::string(effect_kind_name(effect.kind)), output);
  append_context_field(
      "EFFECT_ROOT_IDENTITY", effect.root_identity, output);
  append_context_field(
      "EFFECT_ROOT_RELATIVE_PATH", effect.root_relative_path, output);
  append_context_field("EFFECT_DECLARATION", effect.declaration, output);
  append_context_field("EFFECT_DETAIL", effect.detail, output);
  append_context_field(
      "EFFECT_FLOW_PARAMETER", std::to_string(effect.flow_parameter), output);
  append_context_field(
      "EFFECT_FLOW_CONTEXT", effect.flow_context ? "true" : "false", output);
  append_path_context(
      "EFFECT_FLOW_PATH", "EFFECT_FLOW_PATH_FIELD", effect.flow_path, output);
  output += "EFFECT_FLOW_ARGUMENTS ";
  append_context_u64(
      static_cast<std::uint64_t>(effect.flow_arguments.size()), output);
  for (const ImportedFlowArgument &argument : effect.flow_arguments) {
    output += "FLOW_ARGUMENT_FIELDS ";
    append_context_u64(
        static_cast<std::uint64_t>(argument.fields.size()), output);
    for (const ImportedFlowField &field : argument.fields) {
      append_path_context(
          "FLOW_FIELD_PATH", "FLOW_FIELD_PATH_ELEMENT", field.path, output);
      append_imported_flow_value_context(field.value, output);
    }
  }
}

[[nodiscard]] const ImportBinding *find_import_binding(
    const SemanticPackage &package,
    SymbolId symbol) {
  for (const ImportBinding &binding : package.imports) {
    if (binding.symbol == symbol) return &binding;
  }
  return nullptr;
}

[[nodiscard]] bool is_concrete_imported_instance(
    const SemanticPackage &package,
    SymbolId proxy) {
  for (const ImportedProcedureInstance &instance :
       package.imported_procedure_instances) {
    if (instance.instance_proxy == proxy) return true;
  }
  return false;
}

[[nodiscard]] std::optional<SymbolId> imported_public_symbol(
    const SemanticPackage &package,
    SymbolId import_symbol,
    std::string_view public_name) {
  for (const ImportedSymbol &imported : package.imported_symbols) {
    if (imported.import_symbol == import_symbol &&
        imported.public_name == public_name &&
        !is_concrete_imported_instance(package, imported.proxy)) {
      return imported.proxy;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string imported_package_definition(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ImportBinding &binding,
    std::span<const ResolvedDenialSelector> denials,
    std::vector<AgentTypeContext> &type_contexts,
    DiagnosticSink &diagnostics) {
  std::string output;
  std::size_t declaration_count = 0;
  for (const ImportedSymbol &imported : package.imported_symbols) {
    if (imported.import_symbol == binding.symbol &&
        !denies_symbol(denials, imported.proxy) &&
        !is_concrete_imported_instance(package, imported.proxy)) {
      ++declaration_count;
    }
  }
  output += "IMPORTED_DECLARATIONS ";
  append_context_u64(
      static_cast<std::uint64_t>(declaration_count), output);
  for (const ImportedSymbol &imported : package.imported_symbols) {
    if (imported.import_symbol != binding.symbol ||
        denies_symbol(denials, imported.proxy) ||
        is_concrete_imported_instance(package, imported.proxy)) {
      continue;
    }
    const Symbol &symbol = package.symbols.symbol(imported.proxy);
    append_context_field("DECLARATION_NAME", imported.public_name, output);
    append_context_field(
        "DECLARATION_KIND", std::string(symbol_kind_name(symbol.kind)), output);
    append_context_field(
        "DECLARATION_PARAMETRIC",
        symbol.flags.parametric ? "true" : "false",
        output);
    append_context_field(
        "DECLARATION_FOREIGN", symbol.flags.foreign ? "true" : "false", output);
    append_context_field(
        "DECLARATION_EXPORTED", symbol.flags.exported ? "true" : "false", output);
    append_context_field(
        "DECLARATION_THREAD_LOCAL",
        symbol.flags.is_thread_local ? "true" : "false",
        output);
    if (!symbol.type.is_valid() ||
        package.types.type(symbol.type).kind == TypeKind::Invalid) {
      diagnostics.error(
          symbol.name_range, "imported agent declaration has no complete type");
      append_context_field("DECLARATION_TYPE_TEXT", "<invalid>", output);
      append_context_field("DECLARATION_TYPE_SHA256", {}, output);
    } else {
      const InterfaceTypeGraph type = export_interface_type(
          identity, package, symbol.type, diagnostics);
      add_type_context(type, type_contexts);
      append_context_field(
          "DECLARATION_TYPE_TEXT", type_text(package, symbol.type), output);
      append_context_field(
          "DECLARATION_TYPE_SHA256",
          hash_interface_type_graph(type).hex(),
          output);
    }
    std::size_t parameter_count = 0;
    for (const ParametricParameterRecord &parameter :
         package.parametric_parameters) {
      if (parameter.owner == imported.proxy) ++parameter_count;
    }
    output += "DECLARATION_PARAMETERS ";
    append_context_u64(static_cast<std::uint64_t>(parameter_count), output);
    for (const ParametricParameterRecord &parameter :
         package.parametric_parameters) {
      if (parameter.owner != imported.proxy) continue;
      const Symbol &parameter_symbol =
          package.symbols.symbol(parameter.parameter);
      append_context_field("DECLARATION_PARAMETER", parameter_symbol.name, output);
      append_context_field(
          "DECLARATION_PARAMETER_KIND",
          std::string(symbol_kind_name(parameter_symbol.kind)),
          output);
      append_context_field(
          "DECLARATION_PARAMETER_CONSTRAINT",
          std::string(constraint_name(parameter.constraint)),
          output);
      if (parameter_symbol.type.is_valid()) {
        const InterfaceTypeGraph parameter_type = export_interface_type(
            identity, package, parameter_symbol.type, diagnostics);
        add_type_context(parameter_type, type_contexts);
        append_context_field(
            "DECLARATION_PARAMETER_TYPE_TEXT",
            type_text(package, parameter_symbol.type),
            output);
        append_context_field(
            "DECLARATION_PARAMETER_TYPE_SHA256",
            hash_interface_type_graph(parameter_type).hex(),
            output);
      }
    }
    append_context_field(
        "DECLARATION_HAS_CONSTANT",
        imported.has_constant ? "true" : "false",
        output);
    if (imported.has_constant) {
      append_constant_context(imported.constant, output);
    }
    append_context_field(
        "DECLARATION_NATIVE_PROVIDER", imported.native_provider, output);
    append_context_field(
        "DECLARATION_NATIVE_LINKER_NAME",
        imported.native_linker_name_spelling,
        output);
    append_context_field(
        "DECLARATION_HAS_EFFECT_SUMMARY",
        imported.has_effect_summary ? "true" : "false",
        output);
    std::size_t effect_count = 0;
    for (const ImportedEffect &effect : package.imported_effects) {
      if (effect.procedure_proxy == imported.proxy) ++effect_count;
    }
    output += "DECLARATION_EFFECTS ";
    append_context_u64(static_cast<std::uint64_t>(effect_count), output);
    for (const ImportedEffect &effect : package.imported_effects) {
      if (effect.procedure_proxy == imported.proxy) {
        append_imported_effect_context(effect, output);
      }
    }
    std::size_t return_count = 0;
    for (const ImportedProcedureReturn &returned : package.imported_returns) {
      if (returned.procedure_proxy == imported.proxy) ++return_count;
    }
    output += "DECLARATION_RETURNS ";
    append_context_u64(static_cast<std::uint64_t>(return_count), output);
    for (const ImportedProcedureReturn &returned : package.imported_returns) {
      if (returned.procedure_proxy != imported.proxy) continue;
      append_path_context(
          "RETURN_PATH", "RETURN_PATH_FIELD", returned.path, output);
      ImportedFlowValue value;
      value.flow_slots = returned.flow_slots;
      value.contract_effects = returned.contract_effects;
      value.unknown = returned.unknown;
      append_imported_flow_value_context(value, output);
    }
    std::size_t write_count = 0;
    for (const ImportedProcedureWrite &write : package.imported_writes) {
      if (write.procedure_proxy == imported.proxy) ++write_count;
    }
    output += "DECLARATION_WRITES ";
    append_context_u64(static_cast<std::uint64_t>(write_count), output);
    for (const ImportedProcedureWrite &write : package.imported_writes) {
      if (write.procedure_proxy != imported.proxy) continue;
      append_context_field(
          "WRITE_PARAMETER", std::to_string(write.parameter), output);
      append_context_field(
          "WRITE_INDIRECTION", std::to_string(write.indirection), output);
      append_path_context("WRITE_PATH", "WRITE_PATH_FIELD", write.path, output);
      ImportedFlowValue value;
      value.flow_slots = write.value_flow_slots;
      value.contract_effects = write.value_contract_effects;
      value.unknown = write.value_unknown;
      append_imported_flow_value_context(value, output);
    }
  }
  return output;
}

[[nodiscard]] std::vector<AgentImportedPackageContext>
imported_package_context(
    const PackageIdentity &identity,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AgentRecord &record,
    std::span<const ResolvedDenialSelector> denials,
    std::vector<AgentTypeContext> &type_contexts,
    DiagnosticSink &diagnostics) {
  std::vector<AgentImportedPackageContext> result;
  const SyntaxTree *tree = find_tree(loaded, record.syntax.file);
  const SourceRange site_range = tree == nullptr
      ? SourceRange::invalid()
      : tree->node(record.syntax.node).range;
  std::vector<std::string> names;
  ScopeId scope = record.scope;
  while (scope.is_valid()) {
    const Scope current = package.symbols.scope(scope);
    for (SymbolId symbol_id : current.symbols) {
      const Symbol &symbol = package.symbols.symbol(symbol_id);
      if (already_seen(names, symbol.name)) continue;
      if (site_range.is_valid() && symbol.name_range.is_valid() &&
          symbol.name_range.begin.file == site_range.begin.file &&
          symbol.name_range.begin.offset > site_range.begin.offset) {
        continue;
      }
      names.push_back(symbol.name);
      if (symbol.kind != SymbolKind::Import) continue;
      const ImportBinding *binding = find_import_binding(package, symbol_id);
      if (binding == nullptr) {
        diagnostics.error(
            symbol.name_range, "visible import has no semantic binding");
        continue;
      }
      if (denies_package(denials, *binding)) continue;
      AgentImportedPackageContext context;
      context.alias = symbol.name;
      context.root_identity = binding->root_identity;
      context.root_relative_path = binding->root_relative_path;
      context.definition = imported_package_definition(
          identity,
          package,
          *binding,
          denials,
          type_contexts,
          diagnostics);
      context.definition_digest = sha256(context.definition);
      for (const ImportedDocumentation &documentation :
           package.imported_documentation) {
        if (documentation.import_symbol != binding->symbol) continue;
        if (!documentation.declaration.empty()) {
          const std::optional<SymbolId> anchor = imported_public_symbol(
              package, binding->symbol, documentation.declaration);
          if (!anchor.has_value()) {
            diagnostics.error(
                symbol.name_range,
                "imported documentation has no public declaration anchor");
            continue;
          }
          if (denies_symbol(denials, *anchor)) continue;
        }
        if (documentation.files.size() !=
            documentation.file_contents.size()) {
          diagnostics.error(
              symbol.name_range,
              "imported documentation attachment identities are inconsistent");
          continue;
        }
        AgentDocumentationContext imported_documentation;
        imported_documentation.anchor_name = documentation.declaration;
        imported_documentation.text = documentation.text;
        for (const ImportedDocumentationFile &file : documentation.files) {
          imported_documentation.files.push_back(
              {file.relative_path, file.size, file.digest});
        }
        imported_documentation.file_contents = documentation.file_contents;
        imported_documentation.record_digest = documentation.record_digest;
        context.documentation.push_back(std::move(imported_documentation));
      }
      result.push_back(std::move(context));
    }
    scope = current.parent;
  }
  return result;
}

[[nodiscard]] AgentTargetContext target_context(const TargetProfile &target) {
  AgentTargetContext result;
  result.identity = target.facts.identity;
  result.arch = target.facts.arch;
  result.os = target.facts.os;
  result.abi = target.facts.abi;
  result.byte_order = target.facts.byte_order;
  result.object_format = target.facts.object_format;
  result.file_tag = target.facts.file_tag;
  result.pointer_bits = target.facts.pointer_bits;
  result.page_size = target.facts.page_size;
  result.features = target.facts.features;
  result.simd_shapes = target.facts.simd_shapes;
  result.assembly_architecture = target.parsed_assembly_architecture;
  result.assembly_dialect = target.parsed_assembly_dialect;
  result.assembly_instructions = target.parsed_assembly_instructions;
  return result;
}

// Package documentation is universal context. Documentation anchored to the
// enclosing declaration is also relevant to every site inside that declaration.
// Wider declaration-dependency closure is a separate expansion of this format;
// this rule is positional, deterministic, and already required by Draft 1.
[[nodiscard]] std::vector<AgentDocumentationContext> documentation_context(
    const SemanticPackage &package,
    const AgentMetadataResult &metadata,
    const AgentRecord &site) {
  std::vector<AgentDocumentationContext> result;
  for (const AgentRecord &record : metadata.records) {
    if (record.kind != AgentConstructKind::Documentation) continue;
    if (record.anchor.is_valid() && record.anchor != site.anchor) continue;
    result.push_back({
        anchor_name(package, record.anchor),
        record.text,
        record.files,
        record.file_contents,
        record.record_digest,
    });
  }
  return result;
}

// Rebuilds readable declaration source from nontrivia tokens. Source gaps are
// normalized to one space or one newline, which removes comments without
// joining two tokens that were separated in the authored program. Inserted
// semicolons are omitted because their following source gap already preserves
// the line boundary that caused insertion.
[[nodiscard]] std::string canonical_token_source(
    const SourceManager &sources,
    const SyntaxTree &tree,
    const SyntaxNode &node) {
  std::string result;
  std::uint32_t previous_end = 0;
  bool have_token = false;
  for (std::uint32_t index = node.token_begin;
       index < node.token_end; ++index) {
    const Token &token = tree.token(index);
    if (token.kind == TokenKind::EndOfFile || token.inserted) continue;
    if (have_token && token.range.begin.offset >= previous_end) {
      const SourceRange gap{
          {tree.file(), previous_end}, token.range.begin};
      const std::string_view bytes = sources.text(gap);
      if (!bytes.empty()) {
        result.push_back(
            bytes.find('\n') == std::string_view::npos ? ' ' : '\n');
      }
    }
    result.append(sources.text(token.range));
    previous_end = token.range.end.offset;
    have_token = true;
  }
  return result;
}

[[nodiscard]] AgentBranchRefinementKind branch_refinement_kind(
    SemanticBranchRefinementKind kind) {
  switch (kind) {
  case SemanticBranchRefinementKind::ConditionTrue:
    return AgentBranchRefinementKind::ConditionTrue;
  case SemanticBranchRefinementKind::ConditionFalse:
    return AgentBranchRefinementKind::ConditionFalse;
  case SemanticBranchRefinementKind::SwitchCase:
    return AgentBranchRefinementKind::SwitchCase;
  case SemanticBranchRefinementKind::SwitchDefault:
    return AgentBranchRefinementKind::SwitchDefault;
  }
  return AgentBranchRefinementKind::ConditionTrue;
}

// Converts body-checker path facts into the same portable source/type language
// as the rest of an obligation. Order remains outer-to-inner; switch label order
// remains authored case/label order. No HIR or process-local syntax ID crosses
// this boundary.
[[nodiscard]] std::vector<AgentBranchRefinement> branch_refinement_context(
    const PackageIdentity &identity,
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AgentRecord &record,
    std::vector<AgentTypeContext> &type_contexts,
    DiagnosticSink &diagnostics) {
  std::vector<AgentBranchRefinement> result;
  for (const SemanticBranchRefinement &semantic :
       record.branch_refinements) {
    const SyntaxTree *subject_tree = find_tree(
        loaded, semantic.subject.file);
    if (subject_tree == nullptr || !semantic.subject.node.is_valid()) {
      diagnostics.error(
          SourceRange::invalid(),
          "agent branch refinement has no subject syntax");
      continue;
    }
    if (!semantic.subject_type.is_valid() ||
        package.types.type(semantic.subject_type).kind == TypeKind::Invalid) {
      diagnostics.error(
          subject_tree->node(semantic.subject.node).range,
          "agent branch refinement has no complete subject type");
      continue;
    }

    const InterfaceTypeGraph type = export_interface_type(
        identity, package, semantic.subject_type, diagnostics);
    add_type_context(type, type_contexts);
    AgentBranchRefinement refinement;
    refinement.kind = branch_refinement_kind(semantic.kind);
    refinement.subject = canonical_token_source(
        sources,
        *subject_tree,
        subject_tree->node(semantic.subject.node));
    refinement.subject_digest = sha256(refinement.subject);
    refinement.type_digest = hash_interface_type_graph(type);
    refinement.type_text = type_text(package, semantic.subject_type);
    for (SyntaxReference value : semantic.values) {
      const SyntaxTree *value_tree = find_tree(loaded, value.file);
      if (value_tree == nullptr || !value.node.is_valid()) {
        diagnostics.error(
            SourceRange::invalid(),
            "agent branch refinement has no switch-label syntax");
        continue;
      }
      refinement.values.push_back(canonical_token_source(
          sources, *value_tree, value_tree->node(value.node)));
      refinement.value_digests.push_back(
          sha256(refinement.values.back()));
    }
    result.push_back(std::move(refinement));
  }
  return result;
}

[[nodiscard]] std::string_view validation_context_kind(
    std::string_view relative_name) {
  const std::size_t dot = relative_name.rfind('.');
  if (dot == std::string_view::npos ||
      relative_name.substr(dot) != ".draft") {
    return {};
  }
  std::string_view stem = relative_name.substr(0, dot);
  const std::size_t qualifier = stem.rfind('@');
  if (qualifier != std::string_view::npos) stem = stem.substr(0, qualifier);
  if (stem.ends_with("_test")) return "test";
  if (stem.ends_with("_bench")) return "benchmark";
  return {};
}

[[nodiscard]] AgentEnclosingDeclarationContext enclosing_declaration_context(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AgentRecord &record,
    DiagnosticSink &diagnostics) {
  AgentEnclosingDeclarationContext result;
  if (!record.anchor.is_valid()) return result;

  const Symbol &anchor = package.symbols.symbol(record.anchor);
  const SyntaxTree *tree = find_tree(loaded, anchor.syntax.file);
  if (tree == nullptr || !anchor.syntax.node.is_valid()) {
    diagnostics.error(
        SourceRange::invalid(),
        "agent obligation anchor has no enclosing declaration syntax");
    return result;
  }
  const SyntaxNode &declaration = tree->node(anchor.syntax.node);
  if (record.syntax.file != anchor.syntax.file ||
      !record.syntax.node.is_valid()) {
    diagnostics.error(
        SourceRange::invalid(),
        "agent obligation is not in its enclosing declaration file");
    return result;
  }
  const SourceRange site = tree->node(record.syntax.node).range;
  if (site.begin.offset < declaration.range.begin.offset ||
      site.end.offset > declaration.range.end.offset) {
    diagnostics.error(
        site, "agent obligation is outside its enclosing declaration");
    return result;
  }

  result.present = true;
  result.name = anchor.name;
  result.kind = anchor.kind;
  result.source = canonical_token_source(sources, *tree, declaration);
  result.source_digest = sha256(result.source);

  // The token rendering above preserves exact structural surroundings. This
  // parallel skeleton is deliberately smaller and semantic: every type and
  // offset comes from checked tables, and no body statement or local arena ID
  // appears. Providers can read the contract without reconstructing it from
  // syntax, while the full source remains available when structure matters.
  append_context_field("DECLARATION_NAME", anchor.name, result.semantic_skeleton);
  append_context_field(
      "DECLARATION_KIND",
      std::string(symbol_kind_name(anchor.kind)),
      result.semantic_skeleton);
  append_context_field(
      "DECLARATION_VISIBILITY",
      anchor.visibility == Visibility::Public ? "public" : "private",
      result.semantic_skeleton);
  append_context_field(
      "DECLARATION_PARAMETRIC",
      anchor.flags.parametric ? "true" : "false",
      result.semantic_skeleton);
  append_context_field(
      "DECLARATION_FOREIGN",
      anchor.flags.foreign ? "true" : "false",
      result.semantic_skeleton);
  append_context_field(
      "DECLARATION_EXPORTED",
      anchor.flags.exported ? "true" : "false",
      result.semantic_skeleton);
  append_context_field(
      "DECLARATION_THREAD_LOCAL",
      anchor.flags.is_thread_local ? "true" : "false",
      result.semantic_skeleton);
  if (anchor.type.is_valid()) {
    const Type &type = package.types.type(anchor.type);
    append_context_field(
        "DECLARATION_TYPE", type_text(package, anchor.type),
        result.semantic_skeleton);
    append_context_field(
        "DECLARATION_LAYOUT_KNOWN",
        type.layout.known ? "true" : "false",
        result.semantic_skeleton);
    append_context_field(
        "DECLARATION_LAYOUT_SIZE",
        std::to_string(type.layout.size),
        result.semantic_skeleton);
    append_context_field(
        "DECLARATION_LAYOUT_ALIGNMENT",
        std::to_string(type.layout.alignment),
        result.semantic_skeleton);
    if (type.kind == TypeKind::Procedure && !type.members.empty()) {
      append_context_field(
          "DECLARATION_RESULT_TYPE",
          type_text(package, type.members.back()),
          result.semantic_skeleton);
    }
  }

  std::vector<SymbolId> parameters;
  for (const OwnedSemanticScope &owned : package.owned_scopes) {
    if (owned.owner == record.anchor &&
        package.symbols.scope(owned.scope).kind == ScopeKind::Procedure) {
      parameters = package.symbols.scope(owned.scope).symbols;
      break;
    }
  }
  result.semantic_skeleton += "DECLARATION_PARAMETERS ";
  append_context_u64(
      static_cast<std::uint64_t>(parameters.size()),
      result.semantic_skeleton);
  for (SymbolId parameter_id : parameters) {
    const Symbol &parameter = package.symbols.symbol(parameter_id);
    append_context_field(
        "PARAMETER_NAME", parameter.name, result.semantic_skeleton);
    append_context_field(
        "PARAMETER_KIND",
        std::string(symbol_kind_name(parameter.kind)),
        result.semantic_skeleton);
    append_context_field(
        "PARAMETER_TYPE",
        parameter.type.is_valid()
            ? type_text(package, parameter.type)
            : std::string("<invalid>"),
        result.semantic_skeleton);
  }

  std::vector<AggregateMember> members;
  for (const AggregateMember &member : package.aggregate_members) {
    if (member.owner == record.anchor) members.push_back(member);
  }
  result.semantic_skeleton += "DECLARATION_MEMBERS ";
  append_context_u64(
      static_cast<std::uint64_t>(members.size()),
      result.semantic_skeleton);
  for (const AggregateMember &member : members) {
    const Symbol &field = package.symbols.symbol(member.member);
    append_context_field("MEMBER_NAME", field.name, result.semantic_skeleton);
    append_context_field(
        "MEMBER_KIND",
        std::string(symbol_kind_name(field.kind)),
        result.semantic_skeleton);
    append_context_field(
        "MEMBER_TYPE",
        field.type.is_valid()
            ? type_text(package, field.type)
            : std::string("<invalid>"),
        result.semantic_skeleton);
    append_context_field(
        "MEMBER_OFFSET", std::to_string(member.offset),
        result.semantic_skeleton);
  }
  result.semantic_skeleton_digest = sha256(result.semantic_skeleton);
  return result;
}

[[nodiscard]] bool is_denial_node(NodeKind kind) {
  return kind == NodeKind::DenyDeclaration ||
      kind == NodeKind::DenyMember ||
      kind == NodeKind::DenyStatement ||
      kind == NodeKind::DenyExpression;
}

[[nodiscard]] bool is_denial_site(SemanticSiteKind kind) {
  return kind == SemanticSiteKind::DenialDeclaration ||
      kind == SemanticSiteKind::DenialMember ||
      kind == SemanticSiteKind::DenialStatement ||
      kind == SemanticSiteKind::DenialExpression;
}

// Semantic analysis records the scope outside each governed deny region. This
// exact scope matters when the region later declares a same-named local: the
// selector keeps naming the outer entity and context pruning must do the same.
[[nodiscard]] ScopeId denial_scope(
    const SemanticPackage &package,
    SyntaxReference denial,
    ScopeId fallback) {
  for (const SemanticSite &site : package.sites) {
    if (is_denial_site(site.kind) && site.syntax == denial) return site.scope;
  }
  return fallback;
}

struct ActiveDenialContext {
  std::vector<AgentActiveDenial> descriptions;
  std::vector<ResolvedDenialSelector> resolved;
};

// Finds lexical denial ancestors without relying on NodeId allocation order.
// SyntaxTree intentionally has no parent links, but its half-open ranges make
// containment unambiguous. Sorting by start and then widest end produces a
// stable outer-to-inner order even though parser nodes are appended bottom-up.
[[nodiscard]] ActiveDenialContext active_denial_context(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AgentRecord &record,
    DiagnosticSink &diagnostics) {
  ActiveDenialContext result;
  const SyntaxTree *tree = find_tree(loaded, record.syntax.file);
  if (tree == nullptr || !record.syntax.node.is_valid()) {
    diagnostics.error(
        SourceRange::invalid(), "agent obligation has no syntax for denials");
    return result;
  }
  const SourceRange site = tree->node(record.syntax.node).range;
  std::vector<NodeId> ancestors;
  for (std::size_t index = 0; index < tree->nodes().size(); ++index) {
    const SyntaxNode &candidate = tree->nodes()[index];
    if (!is_denial_node(candidate.kind) ||
        candidate.range.begin.offset > site.begin.offset ||
        candidate.range.end.offset < site.end.offset) {
      continue;
    }
    ancestors.push_back(NodeId{static_cast<std::uint32_t>(index)});
  }
  std::sort(
      ancestors.begin(), ancestors.end(),
      [tree](NodeId left, NodeId right) {
        const SourceRange left_range = tree->node(left).range;
        const SourceRange right_range = tree->node(right).range;
        if (left_range.begin.offset != right_range.begin.offset) {
          return left_range.begin.offset < right_range.begin.offset;
        }
        if (left_range.end.offset != right_range.end.offset) {
          return left_range.end.offset > right_range.end.offset;
        }
        return left.value < right.value;
      });

  for (NodeId ancestor : ancestors) {
    const SyntaxNode &denial = tree->node(ancestor);
    // Every denial grammar stores selectors first and its governed declaration
    // list, member list, block, or expression as the final child.
    if (denial.children.size() < 2) {
      diagnostics.error(denial.range, "deny region has no governed syntax");
      continue;
    }
    for (std::size_t index = 0; index + 1 < denial.children.size(); ++index) {
      AgentActiveDenial active;
      active.selector = canonical_token_source(
          sources, *tree, tree->node(denial.children[index]));
      active.selector_digest = sha256(active.selector);
      result.descriptions.push_back(std::move(active));
    }
    std::vector<ResolvedDenialSelector> resolved = resolve_denial_selectors(
        sources,
        loaded,
        package,
        {record.syntax.file, ancestor},
        denial_scope(package, {record.syntax.file, ancestor}, record.scope),
        diagnostics);
    result.resolved.insert(
        result.resolved.end(),
        std::make_move_iterator(resolved.begin()),
        std::make_move_iterator(resolved.end()));
  }
  return result;
}

[[nodiscard]] std::string_view constraint_name(TypeConstraintKind kind) {
  switch (kind) {
  case TypeConstraintKind::AnyType: return "type";
  case TypeConstraintKind::Integer: return "integer";
  case TypeConstraintKind::Float: return "float";
  case TypeConstraintKind::Number: return "number";
  case TypeConstraintKind::CompileTimeValue: return "value";
  }
  return "invalid";
}

[[nodiscard]] std::vector<AgentParametricParameter> parametric_context(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const AgentRecord &record,
    DiagnosticSink &diagnostics) {
  std::vector<AgentParametricParameter> result;
  if (!record.anchor.is_valid()) return result;

  // Concrete generic bodies retain their source declaration's parameter
  // contract. The instance symbol has concrete substitutions elsewhere, but
  // the provider still needs to understand the template syntax it is filling.
  SymbolId owner = record.anchor;
  for (const ParametricInstanceRecord &instance :
       package.parametric_instances) {
    if (instance.instance == owner) {
      owner = instance.source;
      break;
    }
  }
  for (const ParametricParameterRecord &parameter :
       package.parametric_parameters) {
    if (parameter.owner != owner) continue;
    const Symbol &symbol = package.symbols.symbol(parameter.parameter);
    if (!symbol.type.is_valid() ||
        package.types.type(symbol.type).kind == TypeKind::Invalid) {
      diagnostics.error(
          symbol.name_range,
          "agent parametric parameter has no complete type");
      continue;
    }
    const InterfaceTypeGraph type = export_interface_type(
        identity, package, symbol.type, diagnostics);
    result.push_back({
        symbol.name,
        symbol.kind,
        std::string(constraint_name(parameter.constraint)),
        type_text(package, symbol.type),
        hash_interface_type_graph(type),
    });
  }
  return result;
}

// Returns true when earlier is a direct item of a list of list_kind and site is
// inside a later direct item of that same list. This is the structured-syntax
// dominance rule needed here: leaving a branch/list prevents its claims from
// leaking to siblings, while a claim before an if/loop/switch dominates sites
// nested inside that later statement.
[[nodiscard]] bool precedes_in_enclosing_list(
    const SyntaxTree &tree,
    NodeId earlier,
    NodeId site,
    NodeKind list_kind) {
  std::vector<NodeId> parents(tree.nodes().size());
  for (std::size_t parent_index = 0;
       parent_index < tree.nodes().size(); ++parent_index) {
    for (NodeId child : tree.nodes()[parent_index].children) {
      if (child.is_valid() &&
          static_cast<std::size_t>(child.value) < parents.size()) {
        parents[child.value] =
            NodeId{static_cast<std::uint32_t>(parent_index)};
      }
    }
  }

  NodeId list;
  NodeId earlier_item = earlier;
  NodeId current = earlier;
  while (current.is_valid() &&
         static_cast<std::size_t>(current.value) < parents.size()) {
    const NodeId parent = parents[current.value];
    if (!parent.is_valid()) break;
    if (tree.node(parent).kind == list_kind) {
      list = parent;
      earlier_item = current;
      break;
    }
    current = parent;
  }
  if (!list.is_valid()) return false;

  NodeId site_item = site;
  current = site;
  while (current.is_valid() && current != list &&
         static_cast<std::size_t>(current.value) < parents.size()) {
    site_item = current;
    current = parents[current.value];
  }
  if (current != list) return false;

  const std::vector<NodeId> &items = tree.node(list).children;
  const auto earlier_position = std::find(
      items.begin(), items.end(), earlier_item);
  const auto site_position = std::find(items.begin(), items.end(), site_item);
  return earlier_position != items.end() && site_position != items.end() &&
      earlier_position < site_position;
}

[[nodiscard]] std::vector<AgentJudgmentContext> guiding_judgment_context(
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AgentMetadataResult &metadata,
    const AgentRecord &site) {
  std::vector<AgentJudgmentContext> result;
  const SyntaxTree *site_tree = find_tree(loaded, site.syntax.file);
  for (const AgentRecord &record : metadata.records) {
    if (record.kind != AgentConstructKind::Judgment ||
        record.syntax == site.syntax) {
      continue;
    }
    bool available = !record.anchor.is_valid();
    if (!available && record.anchor == site.anchor && site_tree != nullptr &&
        record.syntax.file == site.syntax.file) {
      const SymbolKind anchor_kind =
          package.symbols.symbol(site.anchor).kind;
      if (anchor_kind == SymbolKind::Type) {
        available = precedes_in_enclosing_list(
            *site_tree,
            record.syntax.node,
            site.syntax.node,
            NodeKind::MemberList);
      } else if (anchor_kind == SymbolKind::Procedure) {
        available = precedes_in_enclosing_list(
            *site_tree,
            record.syntax.node,
            site.syntax.node,
            NodeKind::StatementList);
      }
    }
    if (!available) continue;
    result.push_back({
        anchor_name(package, record.anchor),
        record.text,
        record.files,
        record.file_contents,
        record.record_digest,
    });
  }
  return result;
}

// Walks lexical scopes from inner to outer. The first declaration of a name is
// the visible one; later declarations in the same block are excluded by source
// position. Sorting happens only after shadowing, so it cannot change meaning.
[[nodiscard]] std::vector<AgentVisibleBinding> visible_bindings(
    const PackageIdentity &identity,
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentRecord &record,
    std::span<const ResolvedDenialSelector> denials,
    std::vector<AgentTypeContext> &type_contexts,
    DiagnosticSink &diagnostics) {
  std::vector<AgentVisibleBinding> result;
  std::vector<std::string> names;
  const SyntaxTree *tree = find_tree(loaded, record.syntax.file);
  const SourceRange site_range = tree == nullptr
      ? SourceRange::invalid()
      : tree->node(record.syntax.node).range;
  ScopeId scope = record.scope;
  while (scope.is_valid()) {
    const Scope current = package.symbols.scope(scope);
    for (SymbolId symbol_id : current.symbols) {
      const Symbol &symbol = package.symbols.symbol(symbol_id);
      if (already_seen(names, symbol.name)) continue;
      if (site_range.is_valid() && symbol.name_range.is_valid() &&
          symbol.name_range.begin.file == site_range.begin.file &&
          symbol.name_range.begin.offset > site_range.begin.offset) {
        continue;
      }
      names.push_back(symbol.name);
      if (denies_symbol(denials, symbol_id)) continue;
      if (!symbol.type.is_valid() ||
          package.types.type(symbol.type).kind == TypeKind::Invalid ||
          symbol.kind == SymbolKind::Import) {
        continue;
      }
      const InterfaceTypeGraph type = export_interface_type(
          identity, package, symbol.type, diagnostics);
      add_type_context(type, type_contexts);
      AgentVisibleBinding binding;
      binding.name = symbol.name;
      binding.kind = symbol.kind;
      binding.type_digest = hash_interface_type_graph(type);
      binding.type_text = type_text(package, symbol.type);
      if (const ConstantValue *constant = constants.find(symbol_id)) {
        binding.has_constant = true;
        append_constant_context(
            canonical_constant(identity, package, *constant),
            binding.constant_definition);
        binding.constant_digest = sha256(binding.constant_definition);
      }

      // Parameters already have exact names and types, fields live in their
      // canonical type graph, and imported declarations live in their compact
      // package interface. The remaining source-declared binding kinds are the
      // bounded private/local definition closure that public interfaces cannot
      // provide. Reusing the symbol's declaration node handles grouped local
      // bindings without inventing a compiler-specific source representation.
      const bool source_definition_kind =
          symbol.kind == SymbolKind::Type ||
          symbol.kind == SymbolKind::Constant ||
          symbol.kind == SymbolKind::Variable ||
          symbol.kind == SymbolKind::Procedure ||
          symbol.kind == SymbolKind::Local;
      if (source_definition_kind && symbol_id != record.anchor) {
        const SyntaxTree *definition_tree = find_tree(loaded, symbol.syntax.file);
        if (definition_tree != nullptr && symbol.syntax.node.is_valid()) {
          binding.source_definition = canonical_token_source(
              sources,
              *definition_tree,
              definition_tree->node(symbol.syntax.node));
          binding.has_source_definition = !binding.source_definition.empty();
          if (binding.has_source_definition) {
            binding.source_definition_digest =
                sha256(binding.source_definition);
          }
        }
      }
      result.push_back(std::move(binding));
    }
    scope = current.parent;
  }
  std::sort(
      result.begin(), result.end(),
      [](const AgentVisibleBinding &left, const AgentVisibleBinding &right) {
        if (left.name != right.name) return left.name < right.name;
        return static_cast<std::uint32_t>(left.kind) <
            static_cast<std::uint32_t>(right.kind);
      });
  return result;
}

// Context fields are part of the usable symbol set, not merely a type graph.
// Find the owner by its exact TypeId so imported core/runtime.Context and the
// compiler's private ABI-identical Context follow the same path.
[[nodiscard]] std::vector<AgentContextField> context_fields(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    std::span<const ResolvedDenialSelector> denials,
    std::vector<AgentTypeContext> &type_contexts,
    DiagnosticSink &diagnostics) {
  std::vector<AgentContextField> result;
  if (!package.runtime_context_type.is_valid() ||
      denies_all_context(denials)) {
    return result;
  }
  for (const AggregateMember &member : package.aggregate_members) {
    const Symbol &owner = package.symbols.symbol(member.owner);
    if (owner.type != package.runtime_context_type) continue;
    const Symbol &field = package.symbols.symbol(member.member);
    if (denies_context_field(denials, field.name)) continue;
    if (!field.type.is_valid() ||
        package.types.type(field.type).kind == TypeKind::Invalid) {
      diagnostics.error(
          field.name_range, "runtime Context field has no complete type");
      continue;
    }
    const InterfaceTypeGraph type = export_interface_type(
        identity, package, field.type, diagnostics);
    add_type_context(type, type_contexts);
    result.push_back({
        field.name,
        member.offset,
        hash_interface_type_graph(type),
        type_text(package, field.type),
    });
  }
  std::sort(
      result.begin(), result.end(),
      [](const AgentContextField &left, const AgentContextField &right) {
        if (left.offset != right.offset) return left.offset < right.offset;
        return left.name < right.name;
      });
  return result;
}

[[nodiscard]] std::uint64_t occurrence_for(
    const AgentMetadataResult &metadata,
    std::size_t current_index,
    const AgentRecord &current) {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < current_index; ++index) {
    const AgentRecord &candidate = metadata.records[index];
    if (candidate.kind == current.kind &&
        candidate.syntax.file == current.syntax.file &&
        candidate.anchor == current.anchor) {
      ++result;
    }
  }
  return result;
}

[[nodiscard]] Sha256Digest site_identity_digest(
    const AgentObligation &obligation) {
  Sha256 hash;
  hash_field(hash, "draft-agent-site-v1");
  hash_field(hash, obligation.root_identity);
  hash_field(hash, obligation.root_relative_path);
  hash_field(hash, obligation.source_relative_path);
  hash_field(hash, obligation.anchor_name);
  hash_u64(hash, static_cast<std::uint64_t>(obligation.kind));
  hash_u64(hash, obligation.occurrence);
  return hash.finalize();
}

[[nodiscard]] Sha256Digest input_digest(
    const AgentObligation &obligation,
    const TargetProfile &target) {
  Sha256 hash;
  hash_field(hash, "draft-agent-obligation-v15");
  hash_field(hash, obligation.site_identity);
  hash.update(obligation.record_digest.bytes);
  hash.update(obligation.expected_type_digest.bytes);
  hash_field(hash, obligation.expected_type_text);
  hash_field(hash, obligation.target.identity);
  hash_field(hash, obligation.target.arch);
  hash_field(hash, obligation.target.os);
  hash_field(hash, obligation.target.abi);
  hash_field(hash, obligation.target.byte_order);
  hash_field(hash, obligation.target.object_format);
  hash_field(hash, obligation.target.file_tag);
  hash_u64(hash, obligation.target.pointer_bits);
  hash_u64(hash, obligation.target.page_size);
  hash_u64(
      hash, static_cast<std::uint64_t>(obligation.target.features.size()));
  for (const std::string &feature : obligation.target.features) {
    hash_field(hash, feature);
  }
  hash_u64(
      hash, static_cast<std::uint64_t>(obligation.target.simd_shapes.size()));
  for (const TargetSimdShape &shape : obligation.target.simd_shapes) {
    hash_field(hash, shape.element);
    hash_u64(hash, shape.lanes);
  }
  hash_field(hash, obligation.target.assembly_architecture);
  hash_field(hash, obligation.target.assembly_dialect);
  hash_u64(
      hash,
      static_cast<std::uint64_t>(
          obligation.target.assembly_instructions.size()));
  for (const std::string &instruction :
       obligation.target.assembly_instructions) {
    hash_field(hash, instruction);
  }
  hash_u64(hash, obligation.enclosing_declaration.present ? 1 : 0);
  if (obligation.enclosing_declaration.present) {
    hash_field(hash, obligation.enclosing_declaration.name);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(obligation.enclosing_declaration.kind));
    hash_field(hash, obligation.enclosing_declaration.source);
    hash.update(obligation.enclosing_declaration.source_digest.bytes);
    hash_field(hash, obligation.enclosing_declaration.semantic_skeleton);
    hash.update(
        obligation.enclosing_declaration.semantic_skeleton_digest.bytes);
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(obligation.branch_refinements.size()));
  for (const AgentBranchRefinement &refinement :
       obligation.branch_refinements) {
    hash_field(
        hash, agent_branch_refinement_kind_name(refinement.kind));
    hash_field(hash, refinement.subject);
    hash.update(refinement.subject_digest.bytes);
    hash.update(refinement.type_digest.bytes);
    hash_field(hash, refinement.type_text);
    hash_u64(
        hash, static_cast<std::uint64_t>(refinement.values.size()));
    for (const std::string &value : refinement.values) {
      hash_field(hash, value);
    }
    hash_u64(
        hash,
        static_cast<std::uint64_t>(refinement.value_digests.size()));
    for (const Sha256Digest &digest : refinement.value_digests) {
      hash.update(digest.bytes);
    }
  }
  hash_u64(hash, static_cast<std::uint64_t>(obligation.active_denials.size()));
  for (const AgentActiveDenial &denial : obligation.active_denials) {
    hash_field(hash, denial.selector);
    hash.update(denial.selector_digest.bytes);
  }
  hash_u64(hash, static_cast<std::uint64_t>(obligation.context_fields.size()));
  for (const AgentContextField &field : obligation.context_fields) {
    hash_field(hash, field.name);
    hash_u64(hash, field.offset);
    hash.update(field.type_digest.bytes);
    hash_field(hash, field.type_text);
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(obligation.parametric_parameters.size()));
  for (const AgentParametricParameter &parameter :
       obligation.parametric_parameters) {
    hash_field(hash, parameter.name);
    hash_u64(hash, static_cast<std::uint64_t>(parameter.kind));
    hash_field(hash, parameter.constraint);
    hash_field(hash, parameter.type_text);
    hash.update(parameter.type_digest.bytes);
  }
  hash_u64(hash, static_cast<std::uint64_t>(obligation.type_contexts.size()));
  for (const AgentTypeContext &type : obligation.type_contexts) {
    hash.update(type.type_digest.bytes);
    hash.update(type.definition_digest.bytes);
    hash_field(hash, type.definition);
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(obligation.imported_packages.size()));
  for (const AgentImportedPackageContext &package :
       obligation.imported_packages) {
    hash_field(hash, package.alias);
    hash_field(hash, package.root_identity);
    hash_field(hash, package.root_relative_path);
    hash.update(package.definition_digest.bytes);
    hash_field(hash, package.definition);
    hash_u64(
        hash, static_cast<std::uint64_t>(package.documentation.size()));
    for (const AgentDocumentationContext &documentation :
         package.documentation) {
      hash_field(hash, documentation.anchor_name);
      hash_field(hash, documentation.text);
      hash.update(documentation.record_digest.bytes);
      hash_u64(
          hash, static_cast<std::uint64_t>(documentation.files.size()));
      for (const AttachedFile &file : documentation.files) {
        hash_field(hash, file.relative_path);
        hash_u64(hash, file.size);
        hash.update(file.digest.bytes);
      }
    }
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(obligation.guiding_judgments.size()));
  for (const AgentJudgmentContext &judgment :
       obligation.guiding_judgments) {
    hash_field(hash, judgment.anchor_name);
    hash_field(hash, judgment.claim);
    hash.update(judgment.record_digest.bytes);
    hash_u64(hash, static_cast<std::uint64_t>(judgment.files.size()));
    for (const AttachedFile &file : judgment.files) {
      hash_field(hash, file.relative_path);
      hash_u64(hash, file.size);
      hash.update(file.digest.bytes);
    }
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(obligation.documentation.size()));
  for (const AgentDocumentationContext &documentation :
       obligation.documentation) {
    hash_field(hash, documentation.anchor_name);
    hash_field(hash, documentation.text);
    hash.update(documentation.record_digest.bytes);
    hash_u64(
        hash, static_cast<std::uint64_t>(documentation.files.size()));
    for (const AttachedFile &file : documentation.files) {
      hash_field(hash, file.relative_path);
      hash_u64(hash, file.size);
      hash.update(file.digest.bytes);
    }
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(obligation.validation_context.size()));
  for (const AgentValidationContext &validation :
       obligation.validation_context) {
    hash_field(hash, validation.kind);
    hash_field(hash, validation.source_relative_path);
    hash.update(validation.source_digest.bytes);
    hash_field(hash, validation.source);
  }
  hash_field(hash, target.facts.identity);
  hash_u64(hash, static_cast<std::uint64_t>(target.facts.simd_shapes.size()));
  for (const TargetSimdShape &shape : target.facts.simd_shapes) {
    hash_field(hash, shape.element);
    hash_u64(hash, shape.lanes);
  }
  hash_field(hash, target.llvm_triple);
  hash_field(hash, target.llvm_data_layout);
  hash_field(hash, target.llvm_cpu);
  hash_field(hash, target.llvm_feature_string);
  hash_field(hash, target.parsed_assembly_architecture);
  hash_field(hash, target.parsed_assembly_dialect);
  hash_u64(
      hash,
      static_cast<std::uint64_t>(target.parsed_assembly_instructions.size()));
  for (const std::string &instruction : target.parsed_assembly_instructions) {
    hash_field(hash, instruction);
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(target.system_link_providers.size()));
  for (const std::string &provider : target.system_link_providers) {
    hash_field(hash, provider);
  }
  hash_field(hash, target.system_link_library);
  hash_u64(
      hash,
      static_cast<std::uint64_t>(target.system_foreign_summaries.size()));
  for (const SystemForeignSummary &summary :
       target.system_foreign_summaries) {
    hash_field(hash, summary.provider);
    hash_field(hash, summary.linker_name);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(summary.callback_parameters.size()));
    for (std::uint32_t parameter : summary.callback_parameters) {
      hash_u64(hash, parameter);
    }
  }
  hash_u64(hash, static_cast<std::uint64_t>(obligation.visible_bindings.size()));
  for (const AgentVisibleBinding &binding : obligation.visible_bindings) {
    hash_field(hash, binding.name);
    hash_u64(hash, static_cast<std::uint64_t>(binding.kind));
    hash.update(binding.type_digest.bytes);
    hash_field(hash, binding.type_text);
    hash_u64(hash, binding.has_constant ? 1 : 0);
    if (binding.has_constant) {
      hash.update(binding.constant_digest.bytes);
      hash_field(hash, binding.constant_definition);
    }
    hash_u64(hash, binding.has_source_definition ? 1 : 0);
    if (binding.has_source_definition) {
      hash.update(binding.source_definition_digest.bytes);
      hash_field(hash, binding.source_definition);
    }
  }
  return hash.finalize();
}

} // namespace

std::vector<AgentValidationContext> collect_agent_validation_context(
    const SourceManager &sources,
    const LoadedPackage &loaded) {
  std::vector<AgentValidationContext> result;
  for (const LoadedPackageFile &file : loaded.files) {
    if (!file.syntax.has_value() || !file.syntax->root().is_valid()) continue;
    const std::string_view kind = validation_context_kind(file.relative_name);
    if (kind.empty()) continue;
    AgentValidationContext context;
    context.kind = kind;
    context.source_relative_path = file.relative_name;
    context.source = canonical_token_source(
        sources, *file.syntax, file.syntax->node(file.syntax->root()));
    context.source_digest = sha256(context.source);
    result.push_back(std::move(context));
  }
  return result;
}

std::string_view agent_construct_kind_name(AgentConstructKind kind) {
  switch (kind) {
  case AgentConstructKind::Documentation: return "documentation";
  case AgentConstructKind::Judgment: return "judgment";
  case AgentConstructKind::SynthesisDeclaration: return "declaration";
  case AgentConstructKind::SynthesisMember: return "member";
  case AgentConstructKind::SynthesisStatement: return "statement";
  case AgentConstructKind::SynthesisExpression: return "expression";
  case AgentConstructKind::SynthesisAssembly: return "assembly";
  }
  return "invalid";
}

std::string_view agent_branch_refinement_kind_name(
    AgentBranchRefinementKind kind) {
  switch (kind) {
  case AgentBranchRefinementKind::ConditionTrue: return "condition-true";
  case AgentBranchRefinementKind::ConditionFalse: return "condition-false";
  case AgentBranchRefinementKind::SwitchCase: return "switch-case";
  case AgentBranchRefinementKind::SwitchDefault: return "switch-default";
  }
  return "invalid";
}

AgentObligationResult build_agent_obligations(
    const PackageIdentity &identity,
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentMetadataResult &metadata,
    const TargetProfile &target,
    DiagnosticSink &diagnostics,
    std::span<const AgentValidationContext> validation_context) {
  AgentObligationResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  for (std::size_t index = 0; index < metadata.records.size(); ++index) {
    const AgentRecord &record = metadata.records[index];
    if (!is_obligation_kind(record.kind)) continue;
    AgentObligation obligation;
    obligation.kind = record.kind;
    obligation.syntax = record.syntax;
    obligation.root_identity = identity.root_identity;
    obligation.root_relative_path = identity.root_relative_path;
    obligation.source_relative_path =
        source_relative_path(loaded, record.syntax.file);
    obligation.anchor_name = anchor_name(package, record.anchor);
    obligation.occurrence = occurrence_for(metadata, index, record);
    obligation.record_digest = record.record_digest;
    if (obligation.source_relative_path.empty()) {
      diagnostics.error(
          SourceRange::invalid(), "agent obligation has no package-relative source");
      continue;
    }
    if (record.expected_type.is_valid()) {
      const InterfaceTypeGraph expected = export_interface_type(
          identity, package, record.expected_type, diagnostics);
      obligation.expected_type_digest = hash_interface_type_graph(expected);
      obligation.expected_type_text = type_text(package, record.expected_type);
      add_type_context(expected, obligation.type_contexts);
    }
    ActiveDenialContext denials = active_denial_context(
        sources, loaded, package, record, diagnostics);
    obligation.active_denials = std::move(denials.descriptions);
    obligation.context_fields = context_fields(
        identity,
        package,
        denials.resolved,
        obligation.type_contexts,
        diagnostics);
    obligation.visible_bindings = visible_bindings(
        identity,
        sources,
        loaded,
        package,
        constants,
        record,
        denials.resolved,
        obligation.type_contexts,
        diagnostics);
    obligation.imported_packages = imported_package_context(
        identity,
        loaded,
        package,
        record,
        denials.resolved,
        obligation.type_contexts,
        diagnostics);
    obligation.target = target_context(target);
    obligation.enclosing_declaration = enclosing_declaration_context(
        sources, loaded, package, record, diagnostics);
    obligation.branch_refinements = branch_refinement_context(
        identity,
        sources,
        loaded,
        package,
        record,
        obligation.type_contexts,
        diagnostics);
    obligation.parametric_parameters = parametric_context(
        identity, package, record, diagnostics);
    obligation.guiding_judgments = guiding_judgment_context(
        loaded, package, metadata, record);
    obligation.documentation = documentation_context(package, metadata, record);
    obligation.validation_context.assign(
        validation_context.begin(), validation_context.end());
    obligation.site_identity = "site-" + site_identity_digest(obligation).hex();
    obligation.input_digest = input_digest(obligation, target);
    result.obligations.push_back(std::move(obligation));
  }
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
