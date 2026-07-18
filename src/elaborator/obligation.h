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
#include <string>
#include <vector>

namespace draft {

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
// is a zero-based discriminator among same-kind sites under one file/anchor; a
// future association UI may retain the identity across ambiguous structural
// moves without changing the manifest representation.
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
  AgentTargetContext target;
  AgentEnclosingDeclarationContext enclosing_declaration;
  std::vector<AgentActiveDenial> active_denials;
  std::vector<AgentContextField> context_fields;
  std::vector<AgentParametricParameter> parametric_parameters;
  std::vector<AgentTypeContext> type_contexts;
  std::vector<AgentImportedPackageContext> imported_packages;
  std::vector<AgentJudgmentContext> guiding_judgments;
  std::vector<AgentDocumentationContext> documentation;
};

struct AgentObligationResult {
  bool ok = false;
  std::vector<AgentObligation> obligations;
};

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
    DiagnosticSink &diagnostics);

[[nodiscard]] std::string_view agent_construct_kind_name(
    AgentConstructKind kind);

} // namespace draft
