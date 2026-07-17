// Public interface extraction and import reconstruction.

#include "sema/interface.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace draft {
namespace {

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
      for (const NativeBinding &binding : package_.native_bindings) {
        if (binding.symbol == id) {
          declaration.native_provider = binding.provider;
          declaration.native_linker_name_spelling = binding.linker_name_spelling;
          break;
        }
      }
      if (const ConstantValue *constant = constants_.find(id)) {
        declaration.has_constant = true;
        declaration.constant = *constant;
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

private:
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
    translated.member_offsets = source_type.member_offsets;
    translated.c_calling_convention = source_type.c_calling_convention;
    set_nominal_identity(source, translated);
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
        translated.nominal_members.push_back({
            member.name,
            member.kind,
            translate_type(member.type),
            member_offset(member_id),
        });
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
  TypeId type;
};

// InterfaceImporter reconstructs types in one consumer store. A cache is shared
// by all aliases of the same dependency so nominal identity does not vary by
// source file or alias spelling.
class InterfaceImporter {
public:
  InterfaceImporter(SemanticPackage &consumer, DiagnosticSink &diagnostics)
      : consumer_(consumer), diagnostics_(diagnostics) {}

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
      proxy.type = import_type(package, cache, declaration.type);
      proxy.syntax = binding.syntax;
      proxy.name_range = import_symbol.name_range;
      const SymbolId proxy_id = consumer_.symbols.declare(std::move(proxy), diagnostics_);
      if (!proxy_id.is_valid()) {
        continue;
      }
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

  [[nodiscard]] std::optional<TypeId> find_nominal(const InterfaceType &source) const {
    for (const ImportedNominalCache &nominal : nominals_) {
      if (nominal.kind == source.kind &&
          nominal.root_identity == source.nominal_root_identity &&
          nominal.root_relative_path == source.nominal_root_relative_path &&
          nominal.public_name == source.nominal_public_name) {
        return nominal.type;
      }
    }
    return std::nullopt;
  }

  void remember_nominal(const InterfaceType &source, TypeId type) {
    nominals_.push_back({
        source.kind,
        source.nominal_root_identity,
        source.nominal_root_relative_path,
        source.nominal_public_name,
        type,
    });
    consumer_.imported_types.push_back({
        type,
        source.nominal_root_identity,
        source.nominal_root_relative_path,
        source.nominal_public_name,
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

    const bool nominal = source.kind == TypeKind::Struct ||
        source.kind == TypeKind::Enum || source.kind == TypeKind::TaggedUnion ||
        source.kind == TypeKind::RawUnion || source.kind == TypeKind::Distinct ||
        source.kind == TypeKind::TypeParameter;
    if (nominal) {
      if (const std::optional<TypeId> existing = find_nominal(source)) {
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
      result = consumer_.types.array(
          import_type(package, cache, source.element), source.element_count);
      break;
    case TypeKind::Simd:
      result = consumer_.types.simd(
          import_type(package, cache, source.element), source.element_count);
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
      remember_nominal(source, result);
      break;
    case TypeKind::TypeParameter:
      result = consumer_.types.type_parameter(
          qualified_name(source), SourceRange::invalid());
      remember_nominal(source, result);
      break;
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::TaggedUnion:
    case TypeKind::RawUnion: {
      result = consumer_.types.begin_nominal(
          source.kind, qualified_name(source), SourceRange::invalid());
      cache.translated[source_id.value] = result;
      remember_nominal(source, result);
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
      }
    }
  }

  SemanticPackage &consumer_;
  DiagnosticSink &diagnostics_;
  std::vector<InterfaceImportCache> caches_;
  std::vector<ImportedNominalCache> nominals_;
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
