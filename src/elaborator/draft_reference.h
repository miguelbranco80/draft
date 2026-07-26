// Embedded factual Draft reference bundle and command-owned materialization.
//
// The build converts five focused documents shared with the repository's
// write-draft-code skill into immutable byte rows. The bundle deliberately
// excludes SKILL.md, skill metadata, repository workflow instructions, and
// core package source: operation policy belongs in each Codex adapter's
// developer instructions, while the typed compiler request supplies exact
// program context. This module owns only the immutable reference bytes and one
// temporary realization used by a provider-enabled compiler command.
//
// Relative paths are fixed build inputs, never provider-controlled paths. The
// materialized directory is private, read-only after construction, and removed
// by its owner. Its physical path must never enter Draft program identity.
// Relevant language context: specification 03 sections 9-10.

#pragma once

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <string_view>

namespace draft {

// One file in the compiled factual-reference bundle. relative_path is one
// trusted leaf filename. contents borrows static binary storage for the process
// lifetime and may contain any byte. Row order is the canonical order used for
// the bundle digest and materialization.
struct EmbeddedDraftReferenceFile {
  std::string_view relative_path;
  std::string_view contents;
};

// Returns build-generated immutable rows in lexicographic path order. The
// returned span and every referenced byte remain stable forever.
[[nodiscard]] std::span<const EmbeddedDraftReferenceFile>
embedded_draft_reference_files();

// Hashes a length-framed path-and-content stream for every embedded row. This
// digest is provider configuration provenance, not synthesis-input freshness.
[[nodiscard]] Sha256Digest embedded_draft_reference_digest();

// MaterializedDraftReference owns one private directory for one compiler
// command. materialize is idempotent: the first successful call writes every
// embedded file, makes it read-only, and records its root; later calls reuse
// that exact root. The value is neither copyable nor movable because provider
// state has one owner and concurrent requests only borrow it after preparation.
class MaterializedDraftReference {
public:
  MaterializedDraftReference() = default;
  MaterializedDraftReference(const MaterializedDraftReference &) = delete;
  MaterializedDraftReference &
  operator=(const MaterializedDraftReference &) = delete;
  MaterializedDraftReference(MaterializedDraftReference &&) = delete;
  MaterializedDraftReference &operator=(MaterializedDraftReference &&) = delete;
  ~MaterializedDraftReference();

  // Creates the complete tree or reports one compiler-owned diagnostic. No
  // partially created path becomes visible through root(). On failure, any
  // temporary tree is removed before returning.
  [[nodiscard]] bool materialize(DiagnosticSink &diagnostics);

  // Empty means materialize has not succeeded. Once nonempty, the path and
  // bytes remain stable until this value is destroyed.
  [[nodiscard]] const std::filesystem::path &root() const { return root_; }

private:
  std::filesystem::path root_;
};

} // namespace draft
