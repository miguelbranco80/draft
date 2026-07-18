// Public interface extraction and import reconstruction.

#include "sema/interface.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace draft {
namespace {

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::uint8_t bytes[8];
  for (std::size_t index = 0; index < 8; ++index) {
    const std::size_t shift = (7 - index) * 8;
    bytes[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
  hash.update(std::span<const std::uint8_t>(bytes, 8));
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

void hash_constant(Sha256 &hash, const ConstantValue &value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.kind));
  hash_u64(hash, value.boolean ? 1 : 0);
  hash_field(hash, value.integer.to_decimal());
  hash_field(hash, value.floating.to_fraction());
  hash_u64(hash, value.float_bit_width);
  hash_u64(hash, value.float_bits);
  hash_field(hash, value.text);
  hash_u64(hash, value.symbol_index);
  hash_field(hash, value.root_identity);
  hash_field(hash, value.root_relative_path);
  hash_u64(hash, value.variant_index);
  hash_u64(hash, static_cast<std::uint64_t>(value.elements.size()));
  for (const ConstantValue &element : value.elements) {
    hash_constant(hash, element);
  }
}

void hash_type_id(Sha256 &hash, InterfaceTypeId id) {
  hash_u64(hash, id.is_valid() ? id.value : std::numeric_limits<std::uint32_t>::max());
}

void hash_nominal_argument(
    Sha256 &hash, const InterfaceNominalArgument &argument) {
  hash_u64(hash, argument.is_type ? 1 : 0);
  hash_type_id(hash, argument.type);
  hash_type_id(hash, argument.value_type);
  hash_constant(hash, argument.value);
}

void hash_interface_type(Sha256 &hash, const InterfaceType &type) {
  hash_u64(hash, static_cast<std::uint64_t>(type.kind));
  const bool nominal = !type.nominal_root_identity.empty() ||
      !type.nominal_root_relative_path.empty() ||
      !type.nominal_public_name.empty();
  // An imported nominal's local display name is qualified while its defining
  // package stores the short source name. Both spellings denote the same type;
  // the canonical nominal_* identity below is the only name that may affect a
  // cross-package instance hash. Builtins and structural named scalars still
  // use name to distinguish identities such as int and isize.
  hash_field(hash, nominal ? std::string_view() : std::string_view(type.name));
  hash_field(hash, type.nominal_root_identity);
  hash_field(hash, type.nominal_root_relative_path);
  hash_field(hash, type.nominal_public_name);
  hash_u64(hash, type.layout.known ? 1 : 0);
  hash_u64(hash, type.layout.size);
  hash_u64(hash, type.layout.alignment);
  hash_u64(hash, type.bit_width);
  hash_type_id(hash, type.element);
  hash_u64(hash, type.element_count);
  hash_u64(hash, type.element_count_parameter);
  hash_u64(hash, static_cast<std::uint64_t>(type.members.size()));
  for (InterfaceTypeId member : type.members) hash_type_id(hash, member);
  hash_u64(hash, static_cast<std::uint64_t>(type.member_offsets.size()));
  for (std::uint64_t offset : type.member_offsets) hash_u64(hash, offset);
  hash_u64(hash, type.c_calling_convention ? 1 : 0);
  hash_u64(hash, type.c_representation ? 1 : 0);
  hash_u64(hash, type.requested_alignment);
  // A concrete type packet imported solely for monomorphization does not bind
  // source-level member symbols in the destination package. Its Type row still
  // retains the exact layout, ordered member types, offsets, and nominal
  // identity. Hash only those semantic facts so the defining package and any
  // number of re-exporting consumers derive the same instance name.
  hash_u64(
      hash,
      nominal ? 0 : static_cast<std::uint64_t>(type.nominal_members.size()));
  if (!nominal) {
    for (const InterfaceMember &member : type.nominal_members) {
      hash_field(hash, member.name);
      hash_u64(hash, static_cast<std::uint64_t>(member.kind));
      hash_type_id(hash, member.type);
      hash_u64(hash, member.offset);
      hash_u64(hash, member.has_enum_value ? 1 : 0);
      hash_field(hash, member.enum_value.to_decimal());
    }
  }
  hash_u64(hash, static_cast<std::uint64_t>(type.nominal_arguments.size()));
  for (const InterfaceNominalArgument &argument : type.nominal_arguments) {
    hash_nominal_argument(hash, argument);
  }
}

// InterfaceBuilder translates source TypeIds lazily. Installing the mapping
// before following children permits recursive pointer graphs through a nominal
// type without recursion failure.
class InterfaceBuilder {
public:
  InterfaceBuilder(
      const PackageIdentity &identity,
      const SemanticPackage &package,
      const ConstantTable &constants,
      const AgentMetadataResult *metadata,
      const EffectSummaryResult *effects,
      DiagnosticSink &diagnostics)
      : identity_(identity), package_(package), constants_(constants),
        metadata_(metadata), effects_(effects), diagnostics_(diagnostics) {
    result_.identity = identity;
    result_.short_name = package.short_name;
    translated_.resize(package.types.size());
  }

  [[nodiscard]] PackageInterface run() {
    const Scope &scope = package_.symbols.scope(package_.package_scope);
    for (SymbolId id : scope.symbols) {
      const Symbol &symbol = package_.symbols.symbol(id);
      if (symbol.visibility != Visibility::Public) {
        continue;
      }
      if (!symbol.type.is_valid() ||
          package_.types.type(symbol.type).kind == TypeKind::Invalid) {
        diagnostics_.error(
            symbol.name_range,
            "public declaration '" + symbol.name + "' has no complete interface type");
        continue;
      }
      InterfaceDeclaration declaration;
      declaration.name = symbol.name;
      declaration.kind = symbol.kind;
      declaration.flags = symbol.flags;
      declaration.type = translate_type(symbol.type);
      for (const ParametricParameterRecord &parameter :
           package_.parametric_parameters) {
        if (parameter.owner != id) {
          continue;
        }
        const Symbol &parameter_symbol =
            package_.symbols.symbol(parameter.parameter);
        if (!parameter_symbol.type.is_valid()) {
          diagnostics_.error(
              parameter_symbol.name_range,
              "public parametric parameter '" + parameter_symbol.name +
                  "' has no complete interface type");
          continue;
        }
        declaration.parameters.push_back({
            parameter_symbol.name,
            parameter_symbol.kind,
            parameter.constraint,
            translate_type(parameter_symbol.type),
        });
      }
      for (const NativeBinding &binding : package_.native_bindings) {
        if (binding.symbol == id) {
          declaration.native_provider = binding.provider;
          declaration.native_linker_name_spelling = binding.linker_name_spelling;
          break;
        }
      }
      if (const ConstantValue *constant = constants_.find(id)) {
        declaration.has_constant = true;
        declaration.constant = canonical_constant(*constant);
      }
      if (effects_ != nullptr && symbol.kind == SymbolKind::Procedure) {
        if (const ProcedureEffectSummary *summary = effects_->find(id)) {
          declaration.has_effect_summary = true;
          for (const SemanticEffect &effect : summary->effects) {
            InterfaceDeclaration::Effect interface_effect;
            interface_effect.kind = effect.kind;
            interface_effect.detail = effect.text;
            if (!effect.root_identity.empty()) {
              interface_effect.root_identity = effect.root_identity;
              interface_effect.root_relative_path = effect.root_relative_path;
              interface_effect.declaration = effect.declaration;
            } else if (effect.symbol.is_valid()) {
              bool imported_origin = false;
              for (const ImportedSymbol &imported : package_.imported_symbols) {
                if (imported.proxy == effect.symbol) {
                  interface_effect.root_identity = imported.root_identity;
                  interface_effect.root_relative_path = imported.root_relative_path;
                  interface_effect.declaration = imported.public_name;
                  imported_origin = true;
                  break;
                }
              }
              if (!imported_origin) {
                interface_effect.root_identity = identity_.root_identity;
                interface_effect.root_relative_path = identity_.root_relative_path;
                interface_effect.declaration =
                    package_.symbols.symbol(effect.symbol).name;
              }
            }
            declaration.effects.push_back(std::move(interface_effect));
          }
        }
      }
      result_.declarations.push_back(std::move(declaration));
    }
    if (metadata_ != nullptr) {
      for (const AgentRecord &record : metadata_->records) {
        if (record.kind != AgentConstructKind::Documentation ||
            !record.public_interface) {
          continue;
        }
        InterfaceDocumentation documentation;
        if (record.anchor.is_valid()) {
          documentation.declaration = package_.symbols.symbol(record.anchor).name;
        }
        documentation.text = record.text;
        documentation.files = record.files;
        documentation.record_digest = record.record_digest;
        result_.documentation.push_back(std::move(documentation));
      }
    }
    return std::move(result_);
  }

  [[nodiscard]] InterfaceTypeGraph run_type(TypeId source) {
    InterfaceTypeGraph graph;
    graph.identity = identity_;
    if (!source.is_valid() ||
        static_cast<std::size_t>(source.value) >= package_.types.size()) {
      diagnostics_.error(
          SourceRange::invalid(), "cannot export an invalid concrete type");
      return graph;
    }
    graph.root = translate_type(source);
    graph.types = std::move(result_.types);
    return graph;
  }

private:
  [[nodiscard]] ConstantValue canonical_constant(ConstantValue value) const {
    for (ConstantValue &element : value.elements) {
      element = canonical_constant(std::move(element));
    }
    if (value.kind != ConstantKind::Procedure) return value;

    if (value.root_identity.empty() &&
        value.symbol_index != std::numeric_limits<std::uint32_t>::max() &&
        value.symbol_index < package_.symbols.symbol_count()) {
      const SymbolId referenced{value.symbol_index};
      bool imported_identity = false;
      for (const ImportedSymbol &imported : package_.imported_symbols) {
        if (imported.proxy != referenced) continue;
        value.root_identity = imported.root_identity;
        value.root_relative_path = imported.root_relative_path;
        value.text = imported.public_name;
        imported_identity = true;
        break;
      }
      if (!imported_identity) {
        value.root_identity = identity_.root_identity;
        value.root_relative_path = identity_.root_relative_path;
        value.text = package_.symbols.symbol(referenced).name;
      }
    }
    value.symbol_index = std::numeric_limits<std::uint32_t>::max();
    return value;
  }

  // Finds any type-declaration scope for a nominal TypeId. Aliases can make
  // several package symbols share a TypeId; member contents are identical, so
  // the first declaration-order owner is canonical for interface extraction.
  [[nodiscard]] std::optional<ScopeId> nominal_scope(TypeId type) const {
    for (const OwnedSemanticScope &owned : package_.owned_scopes) {
      if (package_.symbols.scope(owned.scope).kind == ScopeKind::Type &&
          package_.symbols.symbol(owned.owner).type == type) {
        return owned.scope;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::uint64_t member_offset(SymbolId member) const {
    for (const AggregateMember &aggregate : package_.aggregate_members) {
      if (aggregate.member == member) {
        return aggregate.offset;
      }
    }
    return 0;
  }

  [[nodiscard]] const EnumMemberValue *enum_member_value(
      SymbolId member) const {
    for (const EnumMemberValue &value : package_.enum_member_values) {
      if (value.member == member) return &value;
    }
    return nullptr;
  }

  // TypeStore keeps a dependent count as a package-local SymbolId. Interfaces
  // cannot expose that number, so translate it to the parameter's declaration
  // ordinal. Declaration parameter order is stable and already serialized.
  [[nodiscard]] std::optional<std::uint32_t> parameter_ordinal(
      std::uint32_t parameter_value) const {
    std::optional<SymbolId> owner;
    for (const ParametricParameterRecord &parameter :
         package_.parametric_parameters) {
      if (parameter.parameter.value == parameter_value) {
        owner = parameter.owner;
        break;
      }
    }
    if (!owner.has_value()) return std::nullopt;

    std::uint32_t ordinal = 0;
    for (const ParametricParameterRecord &parameter :
         package_.parametric_parameters) {
      if (parameter.owner != *owner) continue;
      if (parameter.parameter.value == parameter_value) return ordinal;
      ++ordinal;
    }
    return std::nullopt;
  }

  [[nodiscard]] InterfaceNominalArgument translate_argument(
      const ParametricArgument &argument) {
    InterfaceNominalArgument translated;
    translated.is_type = argument.is_type;
    if (argument.is_type) {
      translated.type = translate_type(argument.type);
    } else {
      translated.value_type = translate_type(argument.value_type);
      translated.value = argument.value;
    }
    return translated;
  }

  // Returns the original declaration identity of a nominal type. Locally
  // declared rows belong to the interface package; reconstructed rows retain
  // the dependency provenance recorded during import.
  void set_nominal_identity(TypeId source, InterfaceType &translated) const {
    const TypeKind kind = package_.types.type(source).kind;
    if (kind != TypeKind::Struct && kind != TypeKind::Enum &&
        kind != TypeKind::TaggedUnion && kind != TypeKind::RawUnion &&
        kind != TypeKind::Distinct && kind != TypeKind::TypeParameter) {
      return;
    }
    for (const ImportedType &imported : package_.imported_types) {
      if (imported.type == source) {
        translated.nominal_root_identity = imported.root_identity;
        translated.nominal_root_relative_path = imported.root_relative_path;
        translated.nominal_public_name = imported.public_name;
        return;
      }
    }
    translated.nominal_root_identity = identity_.root_identity;
    translated.nominal_root_relative_path = identity_.root_relative_path;
    translated.nominal_public_name = package_.types.type(source).name;
    if (kind == TypeKind::TypeParameter) {
      for (const ParametricParameterRecord &parameter : package_.parametric_parameters) {
        const Symbol &parameter_symbol = package_.symbols.symbol(parameter.parameter);
        if (parameter_symbol.type != source) {
          continue;
        }
        const Symbol &owner = package_.symbols.symbol(parameter.owner);
        translated.nominal_public_name =
            owner.name + "[" + parameter_symbol.name + "]";
        break;
      }
    }
  }

  [[nodiscard]] InterfaceTypeId translate_type(TypeId source) {
    assert(source.is_valid());
    assert(static_cast<std::size_t>(source.value) < translated_.size());
    if (translated_[source.value].is_valid()) {
      return translated_[source.value];
    }

    const InterfaceTypeId id{static_cast<std::uint32_t>(result_.types.size())};
    translated_[source.value] = id;
    result_.types.emplace_back();

    // Copy scalar fields before recursively appending result_.types; vector
    // growth would invalidate a reference to the placeholder row.
    const Type source_type = package_.types.type(source);
    InterfaceType translated;
    translated.kind = source_type.kind;
    translated.name = source_type.name;
    translated.layout = source_type.layout;
    translated.bit_width = source_type.bit_width;
    translated.element_count = source_type.element_count;
    if (source_type.element_count_parameter !=
        std::numeric_limits<std::uint32_t>::max()) {
      const std::optional<std::uint32_t> ordinal =
          parameter_ordinal(source_type.element_count_parameter);
      if (ordinal.has_value()) {
        translated.element_count_parameter = *ordinal;
      } else {
        diagnostics_.error(
            source_type.declaration,
            "dependent array or SIMD type has no owning value parameter");
      }
    }
    translated.member_offsets = source_type.member_offsets;
    translated.c_calling_convention = source_type.c_calling_convention;
    translated.c_representation = source_type.c_representation;
    translated.requested_alignment = source_type.requested_alignment;
    set_nominal_identity(source, translated);
    bool retained_import_arguments = false;
    for (const ImportedType &imported : package_.imported_types) {
      if (imported.type != source) {
        continue;
      }
      for (const ParametricArgument &argument : imported.arguments) {
        translated.nominal_arguments.push_back(translate_argument(argument));
      }
      retained_import_arguments = true;
      break;
    }
    if (!retained_import_arguments) {
      for (const ParametricTypeInstanceRecord &instance :
           package_.parametric_type_instances) {
        if (package_.symbols.symbol(instance.instance).type != source) {
          continue;
        }
        const Symbol &template_symbol =
            package_.symbols.symbol(instance.source);
        translated.nominal_root_identity = identity_.root_identity;
        translated.nominal_root_relative_path = identity_.root_relative_path;
        translated.nominal_public_name = template_symbol.name;
        for (const ImportedSymbol &imported : package_.imported_symbols) {
          if (imported.proxy == instance.source) {
            translated.nominal_root_identity = imported.root_identity;
            translated.nominal_root_relative_path = imported.root_relative_path;
            translated.nominal_public_name = imported.public_name;
            break;
          }
        }
        for (const ParametricArgument &argument : instance.arguments) {
          translated.nominal_arguments.push_back(translate_argument(argument));
        }
        break;
      }
    }
    if (source_type.element.is_valid()) {
      translated.element = translate_type(source_type.element);
    }
    for (TypeId member : source_type.members) {
      translated.members.push_back(translate_type(member));
    }

    if (const std::optional<ScopeId> scope = nominal_scope(source)) {
      for (SymbolId member_id : package_.symbols.scope(*scope).symbols) {
        const Symbol &member = package_.symbols.symbol(member_id);
        if (!member.type.is_valid()) {
          diagnostics_.error(
              member.name_range,
              "member '" + member.name + "' has no complete interface type");
          continue;
        }
        InterfaceMember translated_member{
            member.name,
            member.kind,
            translate_type(member.type),
            member_offset(member_id),
            false,
            {},
        };
        if (const EnumMemberValue *value = enum_member_value(member_id)) {
          translated_member.has_enum_value = true;
          translated_member.enum_value = value->value;
        }
        translated.nominal_members.push_back(std::move(translated_member));
      }
    }
    result_.types[id.value] = std::move(translated);
    return id;
  }

  PackageIdentity identity_;
  const SemanticPackage &package_;
  const ConstantTable &constants_;
  const AgentMetadataResult *metadata_ = nullptr;
  const EffectSummaryResult *effects_ = nullptr;
  DiagnosticSink &diagnostics_;
  PackageInterface result_;
  std::vector<InterfaceTypeId> translated_;
};

struct InterfaceImportCache {
  const PackageInterface *package = nullptr;
  std::vector<TypeId> translated;
};

struct ImportedNominalCache {
  TypeKind kind = TypeKind::Invalid;
  std::string root_identity;
  std::string root_relative_path;
  std::string public_name;
  std::vector<ParametricArgument> arguments;
  TypeId type;
};

// InterfaceImporter reconstructs types in one consumer store. A cache is shared
// by all aliases of the same dependency so nominal identity does not vary by
// source file or alias spelling.
class InterfaceImporter {
public:
  InterfaceImporter(SemanticPackage &consumer, DiagnosticSink &diagnostics)
      : consumer_(consumer), diagnostics_(diagnostics) {}

  [[nodiscard]] TypeId import_graph(const InterfaceTypeGraph &graph) {
    PackageInterface package;
    package.identity = graph.identity;
    package.types = graph.types;
    InterfaceImportCache &cache = cache_for(package);
    return import_type(package, cache, graph.root);
  }

  void bind(const ImportBinding &binding, const PackageInterface &package) {
    InterfaceImportCache &cache = cache_for(package);
    const Symbol import_symbol = consumer_.symbols.symbol(binding.symbol);
    const ScopeId imported_scope = consumer_.symbols.add_scope(
        ScopeKind::ImportedPackage, import_symbol.scope, binding.syntax.node.is_valid()
            ? import_symbol.name_range
            : SourceRange::invalid());
    consumer_.owned_scopes.push_back({binding.symbol, imported_scope});

    for (const InterfaceDeclaration &declaration : package.declarations) {
      Symbol proxy;
      proxy.name = declaration.name;
      proxy.kind = declaration.kind;
      proxy.visibility = Visibility::Public;
      proxy.flags = declaration.flags;
      proxy.scope = imported_scope;
      proxy.syntax = binding.syntax;
      proxy.name_range = import_symbol.name_range;
      const SymbolId proxy_id = consumer_.symbols.declare(std::move(proxy), diagnostics_);
      if (!proxy_id.is_valid()) {
        continue;
      }

      // Parameter symbols must exist before the declaration type is rebuilt:
      // a dependent `[N]T` interface row stores N's ordinal, and reconstruction
      // replaces it with this consumer's local ValueParameter SymbolId.
      const std::vector<SymbolId> parameters = bind_parametric_parameters(
          proxy_id, imported_scope, package, cache, declaration);
      const std::vector<SymbolId> previous_parameters =
          std::move(active_parameters_);
      active_parameters_ = parameters;
      consumer_.symbols.symbol_mut(proxy_id).type =
          import_type(package, cache, declaration.type);
      active_parameters_ = previous_parameters;

      consumer_.imported_symbols.push_back({
          binding.symbol,
          proxy_id,
          package.identity.root_identity,
          package.identity.root_relative_path,
          declaration.name,
          declaration.has_constant,
          declaration.constant,
          declaration.has_effect_summary,
          declaration.native_provider,
          declaration.native_linker_name_spelling,
      });
      for (const InterfaceDeclaration::Effect &effect : declaration.effects) {
        consumer_.imported_effects.push_back({
            proxy_id,
            effect.kind,
            effect.root_identity,
            effect.root_relative_path,
            effect.declaration,
            effect.detail,
        });
      }
      if (declaration.kind == SymbolKind::Type) {
        bind_nominal_members(
            proxy_id, imported_scope, package, cache, declaration.type);
      }
    }
  }

private:
  [[nodiscard]] InterfaceImportCache &cache_for(const PackageInterface &package) {
    for (InterfaceImportCache &cache : caches_) {
      if (cache.package->identity == package.identity) {
        return cache;
      }
    }
    InterfaceImportCache cache;
    cache.package = &package;
    cache.translated.resize(package.types.size());
    caches_.push_back(std::move(cache));
    return caches_.back();
  }

  [[nodiscard]] std::string qualified_name(
      const InterfaceType &source) const {
    PackageIdentity identity{
        source.nominal_root_identity,
        source.nominal_root_relative_path,
    };
    return display_package_identity(identity) + "." + source.nominal_public_name;
  }

  [[nodiscard]] std::optional<TypeId> find_nominal(
      const InterfaceType &source,
      const std::vector<ParametricArgument> &arguments) const {
    for (const ImportedNominalCache &nominal : nominals_) {
      if (nominal.kind == source.kind &&
          nominal.root_identity == source.nominal_root_identity &&
          nominal.root_relative_path == source.nominal_root_relative_path &&
          nominal.public_name == source.nominal_public_name &&
          nominal.arguments == arguments) {
        return nominal.type;
      }
    }
    // Concrete type packets are imported by short-lived InterfaceImporter
    // objects. Consult the permanent semantic provenance table as well so two
    // independent generic requests for the same foreign nominal type converge
    // on one destination TypeId.
    for (const ImportedType &imported : consumer_.imported_types) {
      if (imported.root_identity == source.nominal_root_identity &&
          imported.root_relative_path == source.nominal_root_relative_path &&
          imported.public_name == source.nominal_public_name &&
          imported.arguments == arguments && imported.type.is_valid() &&
          consumer_.types.type(imported.type).kind == source.kind) {
        return imported.type;
      }
    }
    return std::nullopt;
  }

  void remember_nominal(
      const InterfaceType &source,
      TypeId type,
      std::vector<ParametricArgument> arguments) {
    nominals_.push_back({
        source.kind,
        source.nominal_root_identity,
        source.nominal_root_relative_path,
        source.nominal_public_name,
        arguments,
        type,
    });
    consumer_.imported_types.push_back({
        type,
        source.nominal_root_identity,
        source.nominal_root_relative_path,
        source.nominal_public_name,
        std::move(arguments),
    });
  }

  [[nodiscard]] TypeId builtin_type(const InterfaceType &source) const {
    const BuiltinTypes &builtins = consumer_.types.builtins();
    switch (source.kind) {
    case TypeKind::Invalid: return builtins.invalid;
    case TypeKind::Void: return builtins.void_type;
    case TypeKind::UntypedInteger: return builtins.untyped_integer;
    case TypeKind::UntypedFloat: return builtins.untyped_float;
    default:
      if (const std::optional<TypeId> found = consumer_.types.find_builtin(source.name)) {
        return *found;
      }
      return TypeId{};
    }
  }

  // Translates one interface row. Nominal rows install their TypeId before
  // following members, which is what makes `Node { next: ^Node }` importable.
  [[nodiscard]] TypeId import_type(
      const PackageInterface &package,
      InterfaceImportCache &cache,
      InterfaceTypeId source_id) {
    if (!source_id.is_valid() ||
        static_cast<std::size_t>(source_id.value) >= package.types.size()) {
      return consumer_.types.builtins().invalid;
    }
    if (cache.translated[source_id.value].is_valid()) {
      return cache.translated[source_id.value];
    }
    const InterfaceType source = package.types[source_id.value];

    // Translate application arguments before nominal lookup. Concrete TypeIds
    // and exact values form a stable equality key inside this consumer even
    // when two dependencies re-export the same specialization.
    std::vector<ParametricArgument> nominal_arguments;
    nominal_arguments.reserve(source.nominal_arguments.size());
    for (const InterfaceNominalArgument &argument : source.nominal_arguments) {
      ParametricArgument translated;
      translated.is_type = argument.is_type;
      if (argument.is_type) {
        translated.type = import_type(package, cache, argument.type);
      } else {
        translated.value_type = import_type(package, cache, argument.value_type);
        translated.value = argument.value;
      }
      nominal_arguments.push_back(std::move(translated));
    }

    const bool nominal = source.kind == TypeKind::Struct ||
        source.kind == TypeKind::Enum || source.kind == TypeKind::TaggedUnion ||
        source.kind == TypeKind::RawUnion || source.kind == TypeKind::Distinct ||
        source.kind == TypeKind::TypeParameter;
    if (nominal) {
      if (const std::optional<TypeId> existing =
              find_nominal(source, nominal_arguments)) {
        cache.translated[source_id.value] = *existing;
        return *existing;
      }
    }

    TypeId result = builtin_type(source);
    if (result.is_valid()) {
      cache.translated[source_id.value] = result;
      return result;
    }

    switch (source.kind) {
    case TypeKind::Pointer:
      result = consumer_.types.pointer(import_type(package, cache, source.element));
      break;
    case TypeKind::MultiPointer:
      result = consumer_.types.multi_pointer(import_type(package, cache, source.element));
      break;
    case TypeKind::Slice:
      result = consumer_.types.slice(import_type(package, cache, source.element));
      break;
    case TypeKind::Array:
      if (source.element_count_parameter !=
          std::numeric_limits<std::uint32_t>::max()) {
        if (source.element_count_parameter >= active_parameters_.size()) {
          diagnostics_.error(
              SourceRange::invalid(),
              "package interface contains an invalid value-parameter ordinal");
          result = consumer_.types.builtins().invalid;
          break;
        }
        result = consumer_.types.parametric_array(
            import_type(package, cache, source.element),
            active_parameters_[source.element_count_parameter].value);
      } else {
        result = consumer_.types.array(
            import_type(package, cache, source.element), source.element_count);
      }
      break;
    case TypeKind::Simd:
      if (source.element_count_parameter !=
          std::numeric_limits<std::uint32_t>::max()) {
        if (source.element_count_parameter >= active_parameters_.size()) {
          diagnostics_.error(
              SourceRange::invalid(),
              "package interface contains an invalid value-parameter ordinal");
          result = consumer_.types.builtins().invalid;
          break;
        }
        result = consumer_.types.parametric_simd(
            import_type(package, cache, source.element),
            active_parameters_[source.element_count_parameter].value);
      } else {
        result = consumer_.types.simd(
            import_type(package, cache, source.element), source.element_count);
      }
      break;
    case TypeKind::Tuple: {
      std::vector<TypeId> members;
      for (InterfaceTypeId member : source.members) {
        members.push_back(import_type(package, cache, member));
      }
      result = consumer_.types.tuple(members);
      break;
    }
    case TypeKind::Procedure: {
      std::vector<TypeId> members;
      for (InterfaceTypeId member : source.members) {
        members.push_back(import_type(package, cache, member));
      }
      if (members.empty()) {
        result = consumer_.types.builtins().invalid;
      } else {
        const TypeId procedure_result = members.back();
        members.pop_back();
        result = consumer_.types.procedure(
            members, procedure_result, source.c_calling_convention);
      }
      break;
    }
    case TypeKind::Distinct:
      result = consumer_.types.distinct(
          qualified_name(source),
          import_type(package, cache, source.element),
          SourceRange::invalid());
      remember_nominal(source, result, nominal_arguments);
      break;
    case TypeKind::TypeParameter:
      result = consumer_.types.type_parameter(
          qualified_name(source), SourceRange::invalid());
      remember_nominal(source, result, nominal_arguments);
      break;
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::TaggedUnion:
    case TypeKind::RawUnion: {
      result = consumer_.types.begin_nominal(
          source.kind, qualified_name(source), SourceRange::invalid());
      cache.translated[source_id.value] = result;
      remember_nominal(source, result, nominal_arguments);
      consumer_.types.type_mut(result).c_representation =
          source.c_representation;
      consumer_.types.type_mut(result).requested_alignment =
          source.requested_alignment;
      TypeId element;
      if (source.element.is_valid()) {
        element = import_type(package, cache, source.element);
      }
      std::vector<TypeId> members;
      for (InterfaceTypeId member : source.members) {
        members.push_back(import_type(package, cache, member));
      }
      consumer_.types.type_mut(result).element = element;
      consumer_.types.complete_nominal(
          result, source.layout, std::move(members), source.member_offsets);

      // A nominal may arrive only as a nested procedure parameter or result,
      // with no public type declaration in this immediate package interface.
      // It still needs an owner symbol and Type scope so ordinary member lookup
      // works and a later interface export can carry those members onward.
      // '$' is not a source identifier character, keeping this compiler-owned
      // name disjoint from user declarations in the package scope.
      if (!source.nominal_members.empty()) {
        Symbol owner;
        owner.name = "$interface_nominal$" + std::to_string(result.value);
        owner.kind = SymbolKind::Type;
        owner.visibility = Visibility::Private;
        owner.scope = consumer_.package_scope;
        owner.type = result;
        owner.name_range = SourceRange::invalid();
        const SymbolId owner_id =
            consumer_.symbols.declare(std::move(owner), diagnostics_);
        if (owner_id.is_valid()) {
          bind_nominal_members(
              owner_id,
              consumer_.package_scope,
              package,
              cache,
              source_id);
        }
      }
      return result;
    }
    default:
      diagnostics_.error(
          SourceRange::invalid(),
          "package interface contains unsupported type kind '" +
              std::string(type_kind_name(source.kind)) + "'");
      result = consumer_.types.builtins().invalid;
      break;
    }
    cache.translated[source_id.value] = result;
    return result;
  }

  // Rebuilds the compile-time scope owned by an imported template. Parameter
  // symbols are ordinary consumer-local symbols whose TypeIds were translated
  // from the same interface graph as the template signature and members. This
  // shared translation is essential: substitution compares TypeIds directly.
  [[nodiscard]] std::vector<SymbolId> bind_parametric_parameters(
      SymbolId owner,
      ScopeId imported_scope,
      const PackageInterface &package,
      InterfaceImportCache &cache,
      const InterfaceDeclaration &declaration) {
    std::vector<SymbolId> result;
    result.reserve(declaration.parameters.size());
    if (declaration.parameters.empty()) {
      return result;
    }
    const ScopeId scope = consumer_.symbols.add_scope(
        ScopeKind::Parametric, imported_scope, SourceRange::invalid());
    consumer_.owned_scopes.push_back({owner, scope});
    for (const InterfaceParameter &parameter : declaration.parameters) {
      Symbol symbol;
      symbol.name = parameter.name;
      symbol.kind = parameter.kind;
      symbol.scope = scope;
      symbol.type = import_type(package, cache, parameter.type);
      symbol.syntax = consumer_.symbols.symbol(owner).syntax;
      symbol.name_range = consumer_.symbols.symbol(owner).name_range;
      const SymbolId parameter_id =
          consumer_.symbols.declare(std::move(symbol), diagnostics_);
      if (parameter_id.is_valid()) {
        result.push_back(parameter_id);
        consumer_.parametric_parameters.push_back(
            {owner, parameter_id, parameter.constraint});
      }
    }
    return result;
  }

  // Creates the member scope used by ordinary body/member lookup. The interface
  // type row is authoritative; no dependency SymbolId crosses this boundary.
  void bind_nominal_members(
      SymbolId owner,
      ScopeId imported_scope,
      const PackageInterface &package,
      InterfaceImportCache &cache,
      InterfaceTypeId type_id) {
    if (!type_id.is_valid() ||
        static_cast<std::size_t>(type_id.value) >= package.types.size()) {
      return;
    }
    const InterfaceType &type = package.types[type_id.value];
    if (type.nominal_members.empty()) {
      return;
    }
    const ScopeId scope = consumer_.symbols.add_scope(
        ScopeKind::Type, imported_scope, SourceRange::invalid());
    consumer_.owned_scopes.push_back({owner, scope});
    for (const InterfaceMember &member : type.nominal_members) {
      Symbol symbol;
      symbol.name = member.name;
      symbol.kind = member.kind;
      symbol.scope = scope;
      symbol.type = import_type(package, cache, member.type);
      symbol.syntax = consumer_.symbols.symbol(owner).syntax;
      symbol.name_range = consumer_.symbols.symbol(owner).name_range;
      const SymbolId member_id = consumer_.symbols.declare(std::move(symbol), diagnostics_);
      if (member_id.is_valid()) {
        consumer_.aggregate_members.push_back({owner, member_id, member.offset});
        if (member.has_enum_value) {
          consumer_.enum_member_values.push_back(
              {member_id, member.enum_value});
        }
      }
    }
  }

  SemanticPackage &consumer_;
  DiagnosticSink &diagnostics_;
  std::vector<InterfaceImportCache> caches_;
  std::vector<ImportedNominalCache> nominals_;
  // Temporarily names the consumer-local parameters of the declaration whose
  // type graph is being rebuilt. Only dependent array/SIMD rows consult it.
  std::vector<SymbolId> active_parameters_;
};

} // namespace

bool InterfaceTypeId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

const PackageInterface *AvailablePackageImports::find(SyntaxReference syntax) const {
  for (const AvailablePackageImport &entry : entries) {
    if (entry.syntax == syntax) {
      return entry.package;
    }
  }
  return nullptr;
}

PackageInterface build_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ConstantTable &constants,
    DiagnosticSink &diagnostics) {
  InterfaceBuilder builder(identity, package, constants, nullptr, nullptr, diagnostics);
  return builder.run();
}

PackageInterface build_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentMetadataResult &metadata,
    DiagnosticSink &diagnostics) {
  InterfaceBuilder builder(identity, package, constants, &metadata, nullptr, diagnostics);
  return builder.run();
}

PackageInterface build_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentMetadataResult &metadata,
    const EffectSummaryResult &effects,
    DiagnosticSink &diagnostics) {
  InterfaceBuilder builder(
      identity, package, constants, &metadata, &effects, diagnostics);
  return builder.run();
}

InterfaceTypeGraph export_interface_type(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    TypeId type,
    DiagnosticSink &diagnostics) {
  const ConstantTable no_constants;
  InterfaceBuilder builder(
      identity, package, no_constants, nullptr, nullptr, diagnostics);
  return builder.run_type(type);
}

TypeId import_interface_type(
    const InterfaceTypeGraph &graph,
    SemanticPackage &package,
    DiagnosticSink &diagnostics) {
  InterfaceImporter importer(package, diagnostics);
  return importer.import_graph(graph);
}

Sha256Digest hash_interface_type_graph(const InterfaceTypeGraph &graph) {
  Sha256 hash;
  hash_field(hash, "draft.interface-type-graph.v2");
  // The exporting package identity is transport context, not necessarily part
  // of the type. Builtins and purely structural types must hash identically in
  // every requester. Locally declared nominal rows already contain their exact
  // package identity in nominal_root_* and therefore remain distinct.
  hash_type_id(hash, graph.root);
  hash_u64(hash, static_cast<std::uint64_t>(graph.types.size()));
  for (const InterfaceType &type : graph.types) {
    hash_interface_type(hash, type);
  }
  return hash.finalize();
}

void bind_package_interfaces(
    SemanticPackage &package,
    const AvailablePackageImports &available,
    DiagnosticSink &diagnostics) {
  InterfaceImporter importer(package, diagnostics);
  for (ImportBinding &binding : package.imports) {
    const PackageInterface *interface = available.find(binding.syntax);
    if (interface == nullptr) {
      diagnostics.error(
          package.symbols.symbol(binding.symbol).name_range,
          "no resolved package interface is available for import '" +
              binding.package_path + "'");
      continue;
    }
    binding.root_identity = interface->identity.root_identity;
    binding.root_relative_path = interface->identity.root_relative_path;
    importer.bind(binding, *interface);
  }
}

} // namespace draft
