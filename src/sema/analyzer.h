// Package declaration collection and provider-independent semantic metadata.
//
// This pass is deliberately narrower than full type checking. It establishes
// every package declaration that can be known without evaluating expressions,
// creates file-local import scopes, assigns nominal type identities, and keeps
// the source sites needed by later target selection, denial checking, judging,
// and synthesis. Later passes enrich these stable rows instead of rebuilding a
// second, disconnected view of the package.
//
// `when` branches are parsed but not collected here because selecting both
// branches would create false duplicate declarations. Constant evaluation will
// select one branch and feed that declaration region back through the same
// collection rules. `deny` does not select a branch, so its contents are
// collected immediately while the denial site is retained as policy metadata.
//
// Relevant specification: 01-core-language.md sections 3-4, 03-agent-synthesis.md,
// 04-native-interop.md, and 05-denials-validation.md.

#pragma once

#include "base/sha256.h"
#include "sema/constant_value.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// EffectKind lives with semantic package records because imported interface
// summaries must retain these tags before the HIR effect-composition pass runs.
enum class EffectKind {
  // One reachable declaration named by stable package identity. Provider audit
  // files use this when native code may enter a Draft procedure or touch a
  // Draft global without possessing the consumer's process-local SymbolId.
  Declaration,
  PackageGlobal,
  RuntimeAssert,
  ContextField,
  Assembly,
  Unchecked,
  // The procedure calls the procedure-valued parameter at flow_parameter.
  // Call-site composition substitutes the finite target set supplied for that
  // slot instead of treating the callback as an arbitrary indirect edge.
  FlowCall,
  UnknownCall,
};

// FileSemanticScope connects one parsed file to the scope containing its import
// aliases. Ordinary declarations live in package_scope and are therefore
// visible through every file scope's parent link.
struct FileSemanticScope {
  FileId file;
  ScopeId scope;
};

// OwnedSemanticScope records the lexical scope introduced by a declaration.
// Parametric declarations can own both a Parametric scope and a nested Type or
// Procedure scope, so the same owner may appear more than once with distinct
// ScopeKind values.
struct OwnedSemanticScope {
  SymbolId owner;
  ScopeId scope;
};

// AggregateMember connects nominal type identity to the member symbol and its
// byte offset. An invalid/unknown layout uses offset zero until instantiation or
// compile-time selection completes it; callers must consult the owning Type's
// layout.known bit before treating the offset as physical.
struct AggregateMember {
  SymbolId owner;
  SymbolId member;
  std::uint64_t offset = 0;
};

// Enum values are mathematical integers until backing selection has completed.
// Keeping them beside member identity preserves explicit gaps and negative
// values for constants, switches, casts, interfaces, and native emission.
struct EnumMemberValue {
  SymbolId member;
  BigInteger value;
};

enum class TypeConstraintKind {
  AnyType,
  Integer,
  Float,
  Number,
  CompileTimeValue,
};

// ParametricParameterRecord preserves the closed constraint vocabulary. Value
// parameters use CompileTimeValue and carry their required value type on the
// parameter Symbol. Type parameters carry a unique TypeParameter TypeId.
struct ParametricParameterRecord {
  SymbolId owner;
  SymbolId parameter;
  TypeConstraintKind constraint = TypeConstraintKind::AnyType;
};

// Concrete procedure symbols are created during body checking, after the
// declaration graph is stable. Retaining their source template relationship
// lets denial and diagnostic passes apply declaration contracts to every
// monomorphized body without copying policy records.
struct ParametricInstanceRecord {
  SymbolId source;
  SymbolId instance;
};

// A nominal template argument is either a type or a compile-time scalar.
// Concrete scalar applications own value; a template may instead retain the
// ValueParameter symbol which will supply that scalar in each specialization.
// value_type records the declared scalar type in both cases.
struct ParametricArgument {
  bool is_type = true;
  TypeId type;
  TypeId value_type;
  ConstantValue value;
  SymbolId value_parameter;

  bool operator==(const ParametricArgument &) const = default;
};

// Nominal type instances must survive the short-lived TypeResolver used for
// later local annotations. Concrete argument TypeIds form the deterministic
// cache key; the instance symbol owns the concrete member scope and TypeId.
struct ParametricTypeInstanceRecord {
  SymbolId source;
  SymbolId instance;
  std::vector<ParametricArgument> arguments;
};

// ImportBinding retains the canonical source spelling of the imported package
// path separately from the local alias symbol. Workspace resolution later maps
// package_path to a root-qualified package identity and fills dependency edges.
struct ImportBinding {
  SymbolId symbol;
  std::string package_path;
  SyntaxReference syntax;
  // Filled by interface binding from WorkspaceGraph. Source collection leaves
  // these empty because physical import spelling alone is not semantic identity.
  std::string root_identity;
  std::string root_relative_path;
};

// ImportedSymbol connects a consumer-local proxy SymbolId to the file-local
// import alias and the dependency's public declaration identity. proxy is used
// in HIR exactly like a local symbol; later package/MIR lowering consults this
// row to emit an inter-package reference instead of allocating local storage.
// Ready public constants carry their value so `when` and runtime constant
// folding do not reevaluate dependency source.
struct ImportedSymbol {
  SymbolId import_symbol;
  SymbolId proxy;
  std::string root_identity;
  std::string root_relative_path;
  std::string public_name;
  bool has_constant = false;
  ConstantValue constant;
  bool has_effect_summary = false;
  std::string native_provider;
  std::string native_linker_name_spelling;
};

// Public documentation crosses an import only as explicit, content-addressed
// provider context. The consumer keeps the local import alias separately from
// the dependency declaration anchor so denial filtering can redact docs for a
// denied member without hiding unrelated package design material.
struct ImportedDocumentationFile {
  std::string relative_path;
  std::uint64_t size = 0;
  Sha256Digest digest;
};

struct ImportedDocumentation {
  SymbolId import_symbol;
  std::string declaration;
  std::string text;
  std::vector<ImportedDocumentationFile> files;
  std::vector<std::string> file_contents;
  Sha256Digest record_digest;
};

// A use of a public parametric procedure creates a concrete consumer-local
// proxy immediately so ordinary call checking can continue. The executable
// body still belongs to the defining package. Compiler orchestration transfers
// these ordered concrete arguments into that package before its body pass and
// then fills the generated proxy's ImportedSymbol public_name with the stable
// linker-level instance name.
//
// Argument TypeIds are local to the requesting package. They are deliberately
// short-lived and must be exported through InterfaceTypeGraph before another
// SemanticPackage consumes them.
struct ImportedProcedureInstance {
  SymbolId source_proxy;
  SymbolId instance_proxy;
  std::string root_identity;
  std::string root_relative_path;
  std::string public_template_name;
  std::vector<ParametricArgument> arguments;
};

// Canonical call contract for one procedure leaf returned by an imported
// declaration. Slots refer to the factory declaration's parameters; effects
// describe the returned procedure when it is later called.
struct ImportedReturnFlowSlot {
  std::uint32_t parameter = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::string> path;
  bool context = false;
};

struct ImportedEffect;

struct ImportedFlowValue {
  std::vector<ImportedReturnFlowSlot> flow_slots;
  std::vector<ImportedEffect> contract_effects;
  bool unknown = false;
};

struct ImportedFlowField {
  std::vector<std::string> path;
  ImportedFlowValue value;
};

struct ImportedFlowArgument {
  std::vector<ImportedFlowField> fields;
};

// ImportedEffect is one canonical dependency effect attached to a public
// procedure proxy. Origin fields identify referenced dependency declarations
// without importing their private SymbolIds into the consumer. Flow arguments
// retain nested callback invocations in the same provider-independent form.
struct ImportedEffect {
  SymbolId procedure_proxy;
  EffectKind kind = EffectKind::UnknownCall;
  std::string root_identity;
  std::string root_relative_path;
  std::string declaration;
  std::string detail;
  std::uint32_t flow_parameter = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::string> flow_path;
  bool flow_context = false;
  std::vector<ImportedFlowArgument> flow_arguments;
};

struct ImportedProcedureReturn {
  SymbolId procedure_proxy;
  std::vector<std::string> path;
  std::vector<ImportedReturnFlowSlot> flow_slots;
  std::vector<ImportedEffect> contract_effects;
  bool unknown = false;
};

// Consumer-local form of a public procedure's typed write-back contract.
// Symbols inside value effects have already been rebound to the imported
// procedure proxy; origin slots still name that procedure's formal parameters.
struct ImportedProcedureWrite {
  SymbolId procedure_proxy;
  std::uint32_t parameter = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t indirection = 0;
  std::vector<std::string> path;
  std::vector<ImportedReturnFlowSlot> value_flow_slots;
  std::vector<ImportedEffect> value_contract_effects;
  bool value_unknown = false;
};

// ImportedType preserves nominal identity after an interface type has been
// reconstructed in a consumer-local TypeStore. This prevents a downstream
// interface from rebaptizing `dep:T` as `consumer:T` when it merely exposes the
// dependency type in a public signature. Structural types need no provenance;
// their identity follows their recursively translated components.
struct ImportedType {
  TypeId type;
  std::string root_identity;
  std::string root_relative_path;
  std::string public_name;
  // Concrete nominal template applications keep their consumer-local type
  // arguments separately from the template's public identity. An empty vector
  // denotes an ordinary nominal or the unspecialized template declaration.
  std::vector<ParametricArgument> arguments;
};

// DeclarationDenial attaches a lexical `deny` contract to every declaration
// contributed by that declaration region. The selector syntax is resolved only
// after imports and declarations are complete; parametric instantiations retain
// the same contract through their owner SymbolId.
struct DeclarationDenial {
  SymbolId declaration;
  SyntaxReference denial;
};

enum class SemanticSiteKind {
  Documentation,
  Judgment,
  SynthesisDeclaration,
  SynthesisMember,
  SynthesisStatement,
  SynthesisExpression,
  SynthesisAssembly,
  ConditionalDeclaration,
  ConditionalMember,
  ConditionalStatement,
  DenialDeclaration,
  DenialMember,
  DenialStatement,
  DenialExpression,
};

// A semantic site is a zero-runtime source construct anchored in a lexical
// scope. Documentation may additionally anchor to the first symbol of the
// immediately following declaration. An invalid anchor means package-level or
// not-yet-selected metadata; it never means the site was discarded.
struct SemanticSite {
  SemanticSiteKind kind = SemanticSiteKind::Documentation;
  SyntaxReference syntax;
  ScopeId scope;
  SymbolId anchor;
  // Synthesis expressions receive their contextual result type during body
  // checking. Other categories leave this invalid until their grammar-specific
  // obligation builder computes a more detailed expected form.
  TypeId expected_type;
};

enum class NativeBindingKind {
  ForeignImport,
  CExport,
};

// NativeBinding records linkage facts visible during declaration collection.
// linker_name_spelling is the exact quoted token, including quotes and escapes,
// or the local Draft name when no explicit spelling is present. Literal decoding
// and ABI validation belong to the interop semantic pass.
struct NativeBinding {
  NativeBindingKind kind = NativeBindingKind::ForeignImport;
  SymbolId symbol;
  std::string provider;
  std::string linker_name_spelling;
  SyntaxReference syntax;
};

// SemanticPackage is the append-only semantic foundation for one folder
// package. Public fields are intentional: compiler passes operate on explicit
// table rows and stable IDs rather than a deep accessor/object hierarchy.
struct SemanticPackage {
  std::string short_name;
  TypeStore types;
  SymbolTable symbols;
  ScopeId package_scope;
  // Body checking selects the source-visible core/runtime.Context type when
  // that package is imported, or installs an ABI-identical private type when
  // it is not.  MIR uses this row for compiler-managed lexical context copies.
  TypeId runtime_context_type;
  std::vector<FileSemanticScope> files;
  std::vector<OwnedSemanticScope> owned_scopes;
  std::vector<AggregateMember> aggregate_members;
  std::vector<EnumMemberValue> enum_member_values;
  std::vector<ParametricParameterRecord> parametric_parameters;
  std::vector<ParametricInstanceRecord> parametric_instances;
  std::vector<ParametricTypeInstanceRecord> parametric_type_instances;
  std::vector<ImportBinding> imports;
  std::vector<ImportedSymbol> imported_symbols;
  std::vector<ImportedDocumentation> imported_documentation;
  std::vector<ImportedProcedureInstance> imported_procedure_instances;
  std::vector<ImportedType> imported_types;
  std::vector<ImportedEffect> imported_effects;
  std::vector<ImportedProcedureReturn> imported_returns;
  std::vector<ImportedProcedureWrite> imported_writes;
  std::vector<DeclarationDenial> declaration_denials;
  std::vector<SemanticSite> sites;
  std::vector<NativeBinding> native_bindings;
};

// One selection chooses the true or false branch of a particular parsed `when`.
// References are stable while the owning SyntaxTree is unchanged. Keeping the
// decision outside SyntaxTree permits deterministic semantic rounds without
// mutating or cloning parsed source.
struct ConditionalSelection {
  SyntaxReference site;
  bool select_true = false;
};

struct ConditionalSelections {
  std::vector<ConditionalSelection> entries;

  [[nodiscard]] const ConditionalSelection *find(SyntaxReference site) const;
};

// Collects declarations from every parsed Draft file in canonical package-file
// order. Assembly files have no Draft declarations and remain in LoadedPackage
// for the target assembly/link pass. Syntax errors may already exist; this pass
// continues over recovered trees and reports only semantic collection errors.
[[nodiscard]] SemanticPackage collect_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &package,
    DiagnosticSink &diagnostics);

// Selection-aware form used by staged semantic analysis. A selected branch is
// collected into the surrounding scope; an absent selection remains a pending
// ConditionalDeclaration site and contributes no declarations.
[[nodiscard]] SemanticPackage collect_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &package,
    const ConditionalSelections &selections,
    DiagnosticSink &diagnostics);

[[nodiscard]] std::string_view semantic_site_kind_name(SemanticSiteKind kind);

} // namespace draft
