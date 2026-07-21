// Canonical in-memory package interfaces and file-local import binding.
//
// Every SemanticPackage owns a local SymbolTable and TypeStore, so their integer
// IDs cannot cross a package boundary. This module converts public declarations
// into a package-independent interface table, then reconstructs referenced types
// in a consumer's TypeStore. Import aliases receive explicit ImportedPackage
// scopes containing consumer-local proxy symbols. No dependency syntax tree or
// private declaration scope is consulted after the interface is built.
//
// Interface rows are deliberately plain data in deterministic declaration and
// type-discovery order. They are the semantic precursor to the canonical hashed
// interface serialization used by provider-free builds and synthesis
// contexts. Source
// ranges are excluded because physical checkout paths and local FileIds are not
// semantic package identity.
//
// Relevant specification: docs/specification/01-core-language.md section 3, package visibility and
// canonical public interfaces.

#pragma once

#include "sema/agent_metadata.h"
#include "sema/analyzer.h"
#include "sema/constant.h"
#include "sema/effect.h"
#include "source/diagnostic.h"
#include "workspace/workspace.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace draft {

// InterfaceTypeId is local to one PackageInterface::types vector. It is never a
// TypeId and must be translated before use in a consuming SemanticPackage.
struct InterfaceTypeId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const InterfaceTypeId &) const = default;
};

// InterfaceMember preserves the source-visible member namespace of a nominal
// type. offset is the natural byte offset recorded by layout; enum alternatives
// also retain their backing type. A public type exposes all of its members even
// though the owning package declaration itself is the visibility gate.
struct InterfaceMember {
  std::string name;
  SymbolKind kind = SymbolKind::Field;
  InterfaceTypeId type;
  std::uint64_t offset = 0;
  bool has_enum_value = false;
  BigInteger enum_value;
};

struct InterfaceNominalArgument {
  bool is_type = true;
  InterfaceTypeId type;
  InterfaceTypeId value_type;
  ConstantValue value;
  // Parameter leaves use zero-based ordinals in the declaration which owns the
  // surrounding type graph. Concrete arguments leave this expression invalid.
  IntegerExpression value_expression;
  // The defining package owns a full procedure-dependent source recipe. No
  // package-local recipe index or syntax coordinate crosses this boundary.
  bool owner_evaluated_value = false;
};

// InterfaceType is a package-independent type graph row. element and members
// refer only to the same interface table. Nominal rows include member symbols;
// structural rows leave nominal_members empty. name is a builtin spelling or a
// declaration-local name and is qualified with PackageIdentity when imported.
struct InterfaceType {
  TypeKind kind = TypeKind::Invalid;
  std::string name;
  // These fields normally identify nominal, distinct, and type-parameter rows.
  // A published concrete structural alias application also uses them on its
  // root as an application cache key. name remains useful for builtins and
  // diagnostics.
  std::string nominal_root_identity;
  std::string nominal_root_relative_path;
  std::string nominal_public_name;
  TypeLayout layout;
  std::uint32_t bit_width = 0;
  InterfaceTypeId element;
  std::uint64_t element_count = 0;
  // Parameter leaves use zero-based declaration ordinals. A consumer remaps
  // them to local ValueParameter SymbolIds while rebuilding the template graph.
  IntegerExpression element_count_expression;
  // True means the compact expression above is intentionally absent: the
  // defining package owns a procedure-dependent recipe and must return a
  // concrete instantiation when arguments become known. No source coordinate,
  // local symbol, or implementation detail crosses this interface boundary.
  bool owner_evaluated_element_count = false;
  // Structural generic alias application whose defining package retains the
  // full value recipe. Like the array/SIMD marker, this is interface identity;
  // its package-local side-table index is never serialized.
  bool owner_evaluated_type_application = false;
  std::vector<InterfaceTypeId> members;
  std::vector<std::uint64_t> member_offsets;
  bool c_calling_convention = false;
  bool c_representation = false;
  std::uint32_t requested_alignment = 0;
  std::vector<InterfaceMember> nominal_members;
  // A generic type application retains the template identity in the
  // nominal_* fields and its type/value arguments here. Nominal applications
  // preserve type identity; concrete structural aliases use the same packet
  // only to cache their canonical result. Arguments may still name the
  // surrounding template's value parameter across package boundaries.
  std::vector<InterfaceNominalArgument> nominal_arguments;
};

// Public parametric declarations carry an ordered, source-independent copy of
// their compile-time parameter list. The parameter's interface type is either
// its unique TypeParameter row or, for a future value parameter, the required
// value type. Keeping this metadata on the declaration is what lets a consumer
// check constraints and instantiate a template without dependency syntax.
struct InterfaceParameter {
  std::string name;
  SymbolKind kind = SymbolKind::TypeParameter;
  TypeConstraintKind constraint = TypeConstraintKind::AnyType;
  InterfaceTypeId type;
};

// InterfaceDeclaration is one `pub` package binding in declaration order.
// constant is meaningful only when has_constant is true. flags retain language
// properties such as parametric and foreign; imported proxies never reinterpret
// those flags as consumer-local definitions.
struct InterfaceDeclaration {
  std::string name;
  SymbolKind kind = SymbolKind::UnresolvedDeclaration;
  SymbolFlags flags;
  InterfaceTypeId type;
  bool has_constant = false;
  ConstantValue constant;
  bool has_effect_summary = false;
  std::string native_provider;
  std::string native_linker_name_spelling;
  std::vector<InterfaceParameter> parameters;
  // A static pack is signature-generation metadata, not an InterfaceType row:
  // the public source procedure type contains only its fixed prefix and result.
  // Consumers reproduce the marker from these canonical source facts and send
  // the ordered concrete tail types with each specialization request.
  bool has_static_argument_pack = false;
  std::string static_argument_pack_name;
  std::uint32_t static_argument_pack_fixed_parameter_count = 0;
  struct ReturnFlowSlot {
    std::uint32_t parameter =
        std::numeric_limits<std::uint32_t>::max();
    std::vector<std::string> path;
    bool context = false;
  };
  struct Effect;
  struct FlowValue {
    std::vector<ReturnFlowSlot> flow_slots;
    std::vector<Effect> contract_effects;
    bool unknown = false;
  };
  struct FlowField {
    std::vector<std::string> path;
    FlowValue value;
  };
  struct FlowArgument {
    std::vector<FlowField> fields;
  };
  struct Effect {
    EffectKind kind = EffectKind::UnknownCall;
    std::string root_identity;
    std::string root_relative_path;
    std::string declaration;
    std::string detail;
    std::uint32_t flow_parameter =
        std::numeric_limits<std::uint32_t>::max();
    std::vector<std::string> flow_path;
    bool flow_context = false;
    // Arguments supplied when this FlowCall invokes its selected callback.
    // Their values are canonical contracts rooted in this declaration's
    // formal parameters, so arbitrary finite higher-order calls remain exact.
    std::vector<FlowArgument> flow_arguments;
  };
  std::vector<Effect> effects;
  struct ReturnValue {
    std::vector<std::string> path;
    std::vector<ReturnFlowSlot> flow_slots;
    // These effects are the contract of the returned procedure when called.
    // They are deliberately separate from the factory's own effects above.
    std::vector<Effect> contract_effects;
    bool unknown = false;
  };
  std::vector<ReturnValue> return_values;
  // A canonical write-back contract for one public procedure. The destination
  // is a formal parameter plus a dereference count and typed field path; the
  // value uses the same provider-neutral representation as a returned value.
  struct FieldWrite {
    std::uint32_t parameter =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t indirection = 0;
    std::vector<std::string> path;
    std::vector<ReturnFlowSlot> value_flow_slots;
    std::vector<Effect> value_contract_effects;
    bool value_unknown = false;
  };
  std::vector<FieldWrite> field_writes;
};

// One consumer-requested specialization of a public procedure template. The
// source template remains the public declaration; this row publishes only the
// concrete body contract needed by callers that requested the exact ordered
// arguments. instance_name is the canonical linker identity shared with every
// consumer proxy. Argument types use this interface's type graph, never the
// defining package's process-local TypeIds. `pack_types` is the ordered static
// tail and participates in identity after the named `arguments` packet.
struct InterfaceProcedureInstance {
  std::string template_name;
  std::string instance_name;
  std::vector<InterfaceNominalArgument> arguments;
  std::vector<InterfaceTypeId> pack_types;
  bool has_effect_summary = false;
  std::vector<InterfaceDeclaration::Effect> effects;
  std::vector<InterfaceDeclaration::ReturnValue> return_values;
  std::vector<InterfaceDeclaration::FieldWrite> field_writes;
};

// InterfaceDocumentation contains public design context and its exact explicit
// attachment bytes for process-local dependency context construction. It
// intentionally omits FileId, SyntaxReference, and physical paths and is never
// serialized into a resolution manifest. declaration is empty for package
// documentation and names the public anchor otherwise. files are canonical
// package-relative attachment records parallel to file_contents.
struct InterfaceDocumentation {
  std::string declaration;
  std::string text;
  std::vector<AttachedFile> files;
  std::vector<std::string> file_contents;
  Sha256Digest record_digest;
};

// A concrete type argument sometimes has to cross a package boundary even
// though it is not part of either package's public API.  The important example
// is `dependency.make[Private_App_Type]()`: the dependency must compile the
// concrete body, and therefore needs the exact layout and nominal identity of
// the app type.  This small packet reuses the canonical interface type graph
// without pretending that the private type is a published declaration.
//
// The graph is self-contained. root selects one row in types; all row-to-row
// references remain local InterfaceTypeId values. identity is the package from
// which the graph was exported and is used to qualify locally declared nominal
// rows. No SourceRange, SymbolId, or TypeId leaves its owning package.
struct InterfaceTypeGraph {
  PackageIdentity identity;
  std::vector<InterfaceType> types;
  InterfaceTypeId root;
};

struct PackageInterface {
  PackageIdentity identity;
  std::string short_name;
  std::vector<InterfaceType> types;
  std::vector<InterfaceDeclaration> declarations;
  // Concrete effect rows are demand-driven compilation results, not new
  // source-visible declarations. They are keyed by public template identity,
  // canonical ordered arguments, and the shared stable instance name.
  std::vector<InterfaceProcedureInstance> procedure_instances;
  std::vector<InterfaceDocumentation> documentation;
};

// AvailablePackageImport binds one exact source import clause to the already
// analyzed dependency interface selected by WorkspaceGraph. The pointer must
// remain valid only for the analyze_package_semantics call that consumes it.
struct AvailablePackageImport {
  SyntaxReference syntax;
  const PackageInterface *package = nullptr;
};

struct AvailablePackageImports {
  PackageIdentity consumer_identity;
  std::vector<AvailablePackageImport> entries;

  [[nodiscard]] const PackageInterface *find(SyntaxReference syntax) const;
};

// Builds a deterministic public interface. Invalid or unresolved public types
// are diagnosed and make the returned interface unsuitable for publication;
// callers already use the shared DiagnosticSink error count as the phase result.
[[nodiscard]] PackageInterface build_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ConstantTable &constants,
    DiagnosticSink &diagnostics);

// Adds selected public documentation from the provider-independent metadata
// pass. The first overload remains useful to semantic unit tests with no docs.
[[nodiscard]] PackageInterface build_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentMetadataResult &metadata,
    DiagnosticSink &diagnostics);

// Complete publication form used by the driver after HIR effect composition.
[[nodiscard]] PackageInterface build_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentMetadataResult &metadata,
    const EffectSummaryResult &effects,
    DiagnosticSink &diagnostics);

// Complete workspace publication form. active_external_instances contains the
// package-local symbols of exactly those concrete public specializations still
// selected by current consumer bodies. Completed but unselected body products
// remain in the append-only SemanticPackage and must not leak into its public
// interface.
[[nodiscard]] PackageInterface build_package_interface(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentMetadataResult &metadata,
    const EffectSummaryResult &effects,
    std::span<const SymbolId> active_external_instances,
    DiagnosticSink &diagnostics);

// Moves one concrete TypeId between package-local TypeStores through the same
// representation used by public interfaces.  Export preserves private nominal
// identity for monomorphization only; it does not add a declaration to a public
// PackageInterface. Import interns structural types and reuses previously
// imported nominal identities in the destination package.
[[nodiscard]] InterfaceTypeGraph export_interface_type(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    TypeId type,
    DiagnosticSink &diagnostics);

// Publication form for a concrete structural generic application. Structural
// TypeIds intentionally have no nominal identity, so the explicit template and
// arguments annotate the root row with the exact application key consumed by a
// retried declaration or enclosing generic-layout product.
[[nodiscard]] InterfaceTypeGraph export_interface_type_application(
    const PackageIdentity &identity,
    const SemanticPackage &package,
    TypeId type,
    SymbolId source,
    const std::vector<ParametricArgument> &arguments,
    DiagnosticSink &diagnostics);

[[nodiscard]] TypeId import_interface_type(
    const InterfaceTypeGraph &graph,
    SemanticPackage &package,
    DiagnosticSink &diagnostics);

// Canonical type-graph hashing is used only to make stable, collision-resistant
// linker names for concrete generic procedure bodies. The full 256-bit digest
// remains available to future manifests even though linker names currently use
// a readable prefix of its hexadecimal form.
[[nodiscard]] Sha256Digest hash_interface_type_graph(
    const InterfaceTypeGraph &graph);

// Reconstructs every available interface type in package.types, creates one
// ImportedPackage scope per file-local alias, and fills imported_symbols with
// consumer-local proxies. Every ImportBinding must have one matching available
// interface; absence is a closed-graph/compiler-orchestration diagnostic.
void bind_package_interfaces(
    SemanticPackage &package,
    const AvailablePackageImports &available,
    DiagnosticSink &diagnostics);

} // namespace draft
