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
};

// AgentObligation is the immutable input side of one provider transaction.
// site_identity deliberately excludes prompt and type content so ordinary edits
// stale the input digest without automatically baptizing a new site. occurrence
// is a zero-based discriminator among same-kind sites under one file/anchor; a
// future association UI may retain the identity across ambiguous structural
// moves without changing the manifest representation.
struct AgentObligation {
  AgentConstructKind kind = AgentConstructKind::Documentation;
  std::string site_identity;
  Sha256Digest input_digest;
  std::string root_identity;
  std::string root_relative_path;
  std::string source_relative_path;
  std::string anchor_name;
  std::uint64_t occurrence = 0;
  Sha256Digest record_digest;
  Sha256Digest expected_type_digest;
  std::vector<AgentVisibleBinding> visible_bindings;
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
    const AgentMetadataResult &metadata,
    const TargetProfile &target,
    DiagnosticSink &diagnostics);

[[nodiscard]] std::string_view agent_construct_kind_name(
    AgentConstructKind kind);

} // namespace draft
