// Stable judgment discovery, selector matching, and partial manifest updates.

#pragma once

#include "compile/compiler.h"
#include "elaborator/resolution.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// One user-visible judgment identity in canonical compiled obligation order.
// package_selector is the shortest stable spelling accepted by the selector
// matcher for this workspace graph; site_identity is always the unambiguous
// exact selector persisted in evidence.
struct JudgmentSiteDescription {
  std::string site_identity;
  std::string package_selector;
  std::string package_identity;
  std::string anchor_name;
  std::string source_relative_path;
  std::uint64_t occurrence = 0;
};

struct JudgmentSelection {
  std::vector<JudgmentSiteDescription> sites;
};

[[nodiscard]] std::vector<JudgmentSiteDescription> discover_judgment_sites(
    const CompileWorkspaceResult &compiled);

// Empty selectors mean every site. Otherwise each spelling must match at least
// one site. Accepted forms are an exact site identity, a package selector, or
// `<package>:<anchor>` for all judgments in one declaration/region. Multiple
// selectors form a de-duplicated union in canonical site order.
[[nodiscard]] bool select_judgment_sites(
    const CompileWorkspaceResult &compiled,
    const std::vector<std::string> &selectors,
    JudgmentSelection &selection,
    DiagnosticSink &diagnostics);

[[nodiscard]] bool judgment_selection_contains(
    const JudgmentSelection &selection,
    std::string_view site_identity);

// Replaces only rows belonging to selected site identities. Old rows are
// mapped through their typed evidence histories because v4 manifest rows store
// package/key/content rather than duplicating site identity. Corrupt or missing
// history fails closed instead of risking removal of an unrelated row.
[[nodiscard]] bool replace_selected_judgment_evidence(
    const std::filesystem::path &workspace_directory,
    const std::vector<ResolutionEvidencePin> &current,
    const JudgmentSelection &selection,
    const std::vector<ResolutionEvidencePin> &replacement,
    std::vector<ResolutionEvidencePin> &result,
    DiagnosticSink &diagnostics);

} // namespace draft
