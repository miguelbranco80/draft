// Provider-independent docs, judgment, synthesis, and attachment metadata.
//
// Parsing preserves each agent-facing construct as a SemanticSite. This phase
// turns those sites into durable semantic records: decoded inline text, typed
// synthesis expectations, anchored visibility, canonical package-relative
// attachment paths, exact byte hashes, and one framed record digest. It performs
// no provider call and has no runtime lowering.
//
// Attached files are read only through the owning LoadedPackage directory.
// Absolute paths, `..`, symlinks, and paths escaping the canonical package root
// are rejected. Folder traversal order is normalized before hashing; filesystem
// enumeration order is never semantic. SourceManager continues to own syntax
// source, while each metadata record owns decoded strings and digests.
//
// Relevant specification: 03-agent-synthesis.md sections 8-9.

#pragma once

#include "base/sha256.h"
#include "sema/analyzer.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace draft {

enum class AgentConstructKind {
  Documentation,
  Judgment,
  SynthesisDeclaration,
  SynthesisMember,
  SynthesisStatement,
  SynthesisExpression,
  SynthesisAssembly,
};

// AttachmentPolicy is a versioned compiler input. Empty allowed_extensions
// permits every regular-file extension; nonempty entries include their leading
// dot. ignored_directory_names are exact path components skipped during folder
// traversal after symlink rejection.
struct AttachmentPolicy {
  std::string identity = "draft-attachments-v1";
  std::size_t maximum_files_per_site = 1024;
  std::uint64_t maximum_bytes_per_site = 64U * 1024U * 1024U;
  std::vector<std::string> allowed_extensions;
  std::vector<std::string> ignored_directory_names{
      ".git", ".draft", "build", "dist"};
  bool ignore_hidden_names = true;
};

// AttachedFile identifies one exact byte input. relative_path is normalized
// from the package root with `/` separators. size is bytes. digest hashes file
// bytes only; the enclosing AgentRecord digest frames path, size, and digest.
struct AttachedFile {
  std::string relative_path;
  std::uint64_t size = 0;
  Sha256Digest digest;
};

// AgentRecord is one selected surface construct. text is docs inline design
// context, a judgment claim, or an optional synthesis prompt. anchor is package
// level when invalid. public_interface is true only for package documentation or
// documentation attached to a public declaration. record_digest is independent
// of physical paths and FileIds; stable manifest identity later combines it with
// canonical package and structural site identity.
struct AgentRecord {
  AgentConstructKind kind = AgentConstructKind::Documentation;
  SyntaxReference syntax;
  ScopeId scope;
  SymbolId anchor;
  TypeId expected_type;
  std::string text;
  std::vector<AttachedFile> files;
  bool public_interface = false;
  Sha256Digest record_digest;
};

struct AgentMetadataResult {
  bool ok = false;
  std::vector<AgentRecord> records;
};

// Collects every docs/judge/synthesis site currently present in package.sites.
// Call after body checking to include statement, expression, and assembly sites.
[[nodiscard]] AgentMetadataResult collect_agent_metadata(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AttachmentPolicy &policy,
    DiagnosticSink &diagnostics);

} // namespace draft
