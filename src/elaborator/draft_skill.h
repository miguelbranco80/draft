// Embedded Draft coding skill and command-owned materialization.
//
// The build converts the repository's write-draft-code skill into immutable
// byte rows. This module exposes those exact rows and owns the one temporary
// directory used by a provider-enabled compiler command. It does not know about
// Codex prompts, synthesis obligations, or compiler semantics; the Codex adapter
// decides when to materialize the bytes and how to expose them to a child.
//
// Relative paths are fixed build inputs, never provider-controlled paths. The
// materialized directory is private, read-only after construction, and removed
// by its owner. Its physical path is command-local and must never enter Draft
// program identity. Relevant language context: specification 03 sections 9-10.

#pragma once

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <string_view>

namespace draft {

// One file in the compiled skill bundle. relative_path uses forward slashes and
// is confined to the skill root. contents borrows static binary storage for the
// lifetime of the process and may contain any byte, including zero. Row order is
// the canonical order used for the bundle digest and materialization.
struct EmbeddedDraftSkillFile {
  std::string_view relative_path;
  std::string_view contents;
};

// Returns the build-generated immutable rows in lexicographic relative-path
// order. The returned span and every referenced byte remain stable forever.
[[nodiscard]] std::span<const EmbeddedDraftSkillFile>
embedded_draft_skill_files();

// Hashes a length-framed path-and-content stream for every embedded row. This
// digest is provider configuration provenance, not synthesis input freshness.
[[nodiscard]] Sha256Digest embedded_draft_skill_digest();

// MaterializedDraftSkill owns one private directory for one compiler command.
// materialize is idempotent: the first successful call writes every embedded
// file, makes the tree read-only, and records its root; later calls reuse that
// exact root. The value is deliberately neither copyable nor movable because a
// provider state has one unambiguous owner and concurrent calls only borrow it
// after preparation has completed.
class MaterializedDraftSkill {
public:
  MaterializedDraftSkill() = default;
  MaterializedDraftSkill(const MaterializedDraftSkill &) = delete;
  MaterializedDraftSkill &operator=(const MaterializedDraftSkill &) = delete;
  MaterializedDraftSkill(MaterializedDraftSkill &&) = delete;
  MaterializedDraftSkill &operator=(MaterializedDraftSkill &&) = delete;
  ~MaterializedDraftSkill();

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
