// Canonical provider-independent obligations for synthesis and judgment.
//
// Semantic checking first records AgentRecord rows with decoded prompts,
// attachments, anchors, and expected types. This module turns those process-
// local rows into package-independent identities and content hashes suitable
// for resolution pins, judgment evidence, and provider requests. It invokes no
// provider and performs no filesystem writes.
//
// FileId, ScopeId, SymbolId, and TypeId never enter a digest. Source identity is
// the package identity plus selected package-relative filename. Types cross the
// hash boundary through InterfaceTypeGraph, and visible bindings are sorted by
// source name after lexical shadowing is applied. Relevant specification:
// 03-agent-synthesis.md sections 8-10 and 06-compiler.md section 15.

#pragma once

#include "sema/agent_metadata.h"
#include "sema/constant.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/workspace.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace draft {

class HirProgram;
enum class ValidationKind;
struct ValidationEntry;

// One binding visible at a synthesis/judgment program point. type_digest is the
// complete canonical type graph rather than a package-local TypeId. kind stays
// explicit because a procedure, constant, and variable of the same type expose
// different source operations to a provider.
struct AgentVisibleBinding {
  std::string name;
  SymbolKind kind = SymbolKind::UnresolvedDeclaration;
  Sha256Digest type_digest;
  // A digest is useful to the compiler but opaque to a provider. This
  // canonical Draft spelling is the actual type contract presented in a
  // synthesis request. It is hashed with the rest of the obligation.
  std::string type_text;
  bool has_constant = false;
  Sha256Digest constant_digest;
  std::string constant_definition;
};

// One definition selected by the semantic dependency closure. This is
// intentionally separate from AgentVisibleBinding: a private helper reached
// through another helper may be useful explanatory context without being a
// legal unqualified name at the synthesis site. Source is one bounded,
// comment-free declaration node, never an enclosing file. Complete type graphs
// remain in AgentTypeContext and compile-time values retain their canonical
// representation here.
struct AgentDeclarationContext {
  std::string source_relative_path;
  std::string name;
  SymbolKind kind = SymbolKind::UnresolvedDeclaration;
  Sha256Digest type_digest;
  std::string type_text;
  bool has_constant = false;
  Sha256Digest constant_digest;
  std::string constant_definition;
  Sha256Digest source_digest;
  std::string source;
};

// One source-visible field of the compiler-managed runtime Context. Active
// `deny context` removes every row; `deny context.name` removes only that
// semantic field. Offset and type come from the same checked nominal layout
// used by body checking and native lowering.
struct AgentContextField {
  std::string name;
  std::uint64_t offset = 0;
  Sha256Digest type_digest;
  std::string type_text;
};

// Provider-facing target facts are copied out of the target profile so an
// adapter never has to infer architecture or assembly rules from an identity
// hash. These are semantic/compiler contract values, not facts from the host.
struct AgentTargetContext {
  std::string identity;
  std::string arch;
  std::string os;
  std::string abi;
  std::string byte_order;
  std::string object_format;
  std::string file_tag;
  std::uint64_t pointer_bits = 0;
  std::uint64_t page_size = 0;
  std::vector<std::string> features;
  std::vector<TargetSimdShape> simd_shapes;
  std::string assembly_architecture;
  std::string assembly_dialect;
  std::vector<std::string> assembly_instructions;
};

// Documentation supplied as semantic context remains associated with its
// package or enclosing declaration. Exact attachment bytes stay parallel to
// their verified identities so an adapter needs no workspace filesystem access.
struct AgentDocumentationContext {
  std::string anchor_name;
  std::string text;
  std::vector<AttachedFile> files;
  std::vector<std::string> file_contents;
  Sha256Digest record_digest;
};

// One resolved name used by a checked validation procedure. Origin fields keep
// same-spelled imports distinct without exposing a package-local SymbolId. The
// complete portable type graph is repeated here because validation compilation
// owns a separate semantic arena from the ordinary synthesis surface.
struct AgentValidationReferenceContext {
  std::string root_identity;
  std::string root_relative_path;
  std::string name;
  SymbolKind kind = SymbolKind::UnresolvedDeclaration;
  Sha256Digest type_digest;
  std::string type_text;
  Sha256Digest type_definition_digest;
  std::string type_definition;
  bool has_constant = false;
  Sha256Digest constant_digest;
  std::string constant_definition;
};

// A filename alone never makes a test executable. These rows come only from
// validation/discovery after body checking has proved the exact core nominal
// signature and target layout. References are the typed semantic names used by
// that procedure body, not additions to the synthesis site's usable scope.
struct AgentValidationProcedureContext {
  std::string name;
  Sha256Digest type_digest;
  std::string type_text;
  Sha256Digest type_definition_digest;
  std::string type_definition;
  std::uint64_t state_size = 0;
  std::uint64_t state_alignment = 1;
  std::uint64_t failure_offset = 0;
  std::uint64_t report_size = 0;
  std::vector<AgentValidationReferenceContext> references;
};

// Tests and benchmarks are not ordinary build inputs, but their selected
// source is authoritative synthesis context. Each row is one target-selected
// package file rendered from nontrivia syntax. Complete body-stage obligations
// additionally carry checked procedures and resolved references. Keeping the
// kind and relative filename explicit makes this useful to a provider without
// exposing a host path or quietly treating a benchmark as a test.
struct AgentValidationContext {
  std::string kind;
  std::string source_relative_path;
  std::string source;
  Sha256Digest source_digest;
  bool typing_complete = false;
  std::vector<AgentValidationProcedureContext> procedures;
};

// A body or member site is meaningful only inside its declaration. The
// canonical source is reconstructed from syntax tokens, so it retains exact
// identifiers and literals while deliberately omitting lexical comments. An
// invalid anchor (for example package-declaration synthesis) has present=false
// instead of inventing a synthetic declaration.
struct AgentEnclosingDeclarationContext {
  bool present = false;
  std::string name;
  SymbolKind kind = SymbolKind::UnresolvedDeclaration;
  std::string source;
  Sha256Digest source_digest;
  std::string semantic_skeleton;
  Sha256Digest semantic_skeleton_digest;
};

// A canonical structured entry decision on the path to a body-level judgment
// or synthesis site. subject and values are normalized, comment-free Draft
// expressions. A true or false condition has no values; a switch case lists
// its matching labels; a switch default lists every explicit label that failed
// at dispatch; loop rows describe the decision/iterable that admitted the
// current iteration. These are historical control-flow facts, not assertions
// that mutable source expressions would re-evaluate identically at the site.
// The subject type is both readable and backed by the complete
// AgentTypeContext graph referenced by type_digest.
enum class AgentBranchRefinementKind {
  ConditionTrue,
  ConditionFalse,
  SwitchCase,
  SwitchDefault,
  LoopConditionTrue,
  LoopIteration,
};

struct AgentBranchRefinement {
  AgentBranchRefinementKind kind =
      AgentBranchRefinementKind::ConditionTrue;
  // These source/digest pairs use the same normalization and immediate
  // provider-boundary recheck as enclosing declarations.
  std::string subject;
  Sha256Digest subject_digest;
  Sha256Digest type_digest;
  std::string type_text;
  // Parallel authored-order arrays. Their sizes must match, including the
  // empty arrays used by true/false condition facts.
  std::vector<std::string> values;
  std::vector<Sha256Digest> value_digests;
};

// One source-authored selector from a lexically enclosing deny region. The
// spelling is canonical nontrivia Draft source such as `assert`, `asm`, or
// `context.allocator`. Semantic denial checking remains authoritative; this
// row makes the already-active policy visible to synthesis.
struct AgentActiveDenial {
  std::string selector;
  Sha256Digest selector_digest;
};

// Ordered compile-time parameters of the enclosing generic declaration.
// constraint uses Draft source vocabulary (`type`, `integer`, `float`,
// `number`, or `value`) rather than exposing the compiler enum to providers.
struct AgentParametricParameter {
  std::string name;
  SymbolKind kind = SymbolKind::TypeParameter;
  std::string constraint;
  std::string type_text;
  Sha256Digest type_digest;
};

// Complete canonical graph for a type referenced by the expected result or a
// visible binding. type_digest is the compact identity used on those rows;
// definition_digest covers the readable graph including nominal member names,
// layouts, offsets, and generic arguments.
struct AgentTypeContext {
  Sha256Digest type_digest;
  Sha256Digest definition_digest;
  std::string definition;
};

// Compact public interface for one file-local imported package alias visible at
// the site. The definition contains declarations, constants, native bindings,
// and typed effect/return/write contracts; referenced declaration types reuse
// AgentTypeContext graphs.
struct AgentImportedPackageContext {
  std::string alias;
  std::string root_identity;
  std::string root_relative_path;
  Sha256Digest definition_digest;
  std::string definition;
  std::vector<AgentDocumentationContext> documentation;
};

// A surface judgment whose position permits it to guide this synthesis site.
// These are claims, not passing verdicts: including one never evaluates it or
// weakens ordinary syntax, type, denial, test, or benchmark validation.
struct AgentJudgmentContext {
  std::string anchor_name;
  std::string claim;
  std::vector<AttachedFile> files;
  std::vector<std::string> file_contents;
  Sha256Digest record_digest;
};

// AgentObligation is the immutable input side of one provider transaction.
// site_identity deliberately excludes prompt and type content so ordinary edits
// stale the input digest without automatically baptizing a new site. occurrence
// is the lowest available discriminator among same-kind sites under one
// file/anchor. Generated-source maps reserve discriminators consumed in earlier
// interface rounds, so a newly selected conditional site cannot collide with a
// site already replaced in the same transaction. A future association UI may
// retain the identity across ambiguous structural moves without changing the
// manifest representation.
struct AgentObligation {
  AgentConstructKind kind = AgentConstructKind::Documentation;
  // syntax is a process-local route back to the surface site for diagnostics
  // and exact source replacement. It is intentionally excluded from every
  // persistent identity because FileId and NodeId depend on load order.
  SyntaxReference syntax;
  std::string site_identity;
  Sha256Digest input_digest;
  std::string root_identity;
  std::string root_relative_path;
  std::string source_relative_path;
  std::string anchor_name;
  std::uint64_t occurrence = 0;
  Sha256Digest record_digest;
  Sha256Digest expected_type_digest;
  std::string expected_type_text;
  std::vector<AgentVisibleBinding> visible_bindings;
  std::vector<AgentDeclarationContext> relevant_declarations;
  AgentTargetContext target;
  AgentEnclosingDeclarationContext enclosing_declaration;
  std::vector<AgentBranchRefinement> branch_refinements;
  std::vector<AgentActiveDenial> active_denials;
  std::vector<AgentContextField> context_fields;
  std::vector<AgentParametricParameter> parametric_parameters;
  std::vector<AgentTypeContext> type_contexts;
  std::vector<AgentImportedPackageContext> imported_packages;
  std::vector<AgentJudgmentContext> guiding_judgments;
  std::vector<AgentDocumentationContext> documentation;
  std::vector<AgentValidationContext> validation_context;
};

struct AgentObligationResult {
  bool ok = false;
  std::vector<AgentObligation> obligations;
};

// Collects target-selected `_test.draft` and `_bench.draft` files from a
// package load performed with both validation switches enabled. The returned
// rows are canonical, comment-free syntax and preserve package filename order.
[[nodiscard]] std::vector<AgentValidationContext>
collect_agent_validation_context(
    const SourceManager &sources,
    const LoadedPackage &loaded);

// Adds checked test/benchmark facts from a separately compiled validation
// package to matching syntax-only rows. entries may contain the whole workspace;
// package identity and source-relative filename provide the stable join.
[[nodiscard]] bool enrich_agent_validation_context(
    const PackageIdentity &identity,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const HirProgram &hir,
    ValidationKind kind,
    std::span<const ValidationEntry> entries,
    std::vector<AgentValidationContext> &context,
    DiagnosticSink &diagnostics);

// Builds obligations for judgments and every synthesis grammar category. Docs
// remain inputs through AgentRecord and package interfaces but do not form
// independently executable obligations. Diagnostics report malformed semantic
// rows rather than manufacturing identities from invalid local IDs.
[[nodiscard]] AgentObligationResult build_agent_obligations(
    const PackageIdentity &identity,
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const ConstantTable &constants,
    const AgentMetadataResult &metadata,
    const TargetProfile &target,
    DiagnosticSink &diagnostics,
    std::span<const AgentValidationContext> validation_context = {},
    const HirProgram *hir = nullptr);

[[nodiscard]] std::string_view agent_construct_kind_name(
    AgentConstructKind kind);

[[nodiscard]] std::string_view agent_branch_refinement_kind_name(
    AgentBranchRefinementKind kind);

} // namespace draft
