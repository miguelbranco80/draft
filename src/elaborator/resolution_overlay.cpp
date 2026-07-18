// Exact source-range replacement for offline resolved-program construction.
//
// Inputs are immutable surface source, typed obligation rows, a parsed pin
// manifest, and the content-addressed expansion store. The output is a set of
// complete in-memory Draft files keyed by semantic package identity. SourceEdit
// rows are temporary and own expansion bytes only until each file is composed.
//
// This module intentionally depends on workspace source representations but not
// semantic checking or provider execution. Its important invariant is that it
// changes bytes only: the next compiler pass owns every meaning assigned to the
// result. Relevant specification: 03-agent-synthesis.md sections 9-10.

#include "elaborator/resolution_overlay.h"

#include "elaborator/resolution_store.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

// One verified replacement in surface coordinates. file and range refer to the
// same caller-owned SourceManager. site_identity is retained only to make a
// coincident-range ordering deterministic before that invalid overlap is
// diagnosed.
struct SourceEdit {
  FileId file;
  SourceRange range;
  std::string replacement;
  std::string site_identity;
};

// Documentation and judgment obligations do not replace source. Keeping this
// classification local prevents evidence rows from entering the pin path.
[[nodiscard]] bool is_synthesis(AgentConstructKind kind) {
  return kind == AgentConstructKind::SynthesisDeclaration ||
      kind == AgentConstructKind::SynthesisMember ||
      kind == AgentConstructKind::SynthesisStatement ||
      kind == AgentConstructKind::SynthesisExpression ||
      kind == AgentConstructKind::SynthesisAssembly;
}

// Returns the unique pin selected by persistent site identity. Manifest parsing
// has already rejected duplicate identities, so the first match is definitive.
[[nodiscard]] const ResolutionPin *find_pin(
    const ResolutionManifest &manifest,
    std::string_view site_identity,
    std::size_t &index) {
  for (std::size_t candidate = 0; candidate < manifest.pins.size(); ++candidate) {
    if (manifest.pins[candidate].site_identity == site_identity) {
      index = candidate;
      return &manifest.pins[candidate];
    }
  }
  return nullptr;
}

// Resolves a process-local FileId only inside its owning loaded package. FileId
// values never become package-independent identities.
[[nodiscard]] const LoadedPackageFile *find_file(
    const LoadedPackage &loaded,
    FileId file) {
  for (const LoadedPackageFile &candidate : loaded.files) {
    if (candidate.source == file) return &candidate;
  }
  return nullptr;
}

// Converts the obligation's process-local syntax route to its exact half-open
// source replacement range. Invalid compiler rows return an invalid range and
// are diagnosed by the public operation rather than asserted.
[[nodiscard]] SourceRange obligation_range(
    const LoadedPackage &loaded,
    const AgentObligation &obligation) {
  const LoadedPackageFile *file = find_file(loaded, obligation.syntax.file);
  if (file == nullptr || !file->syntax.has_value() ||
      !obligation.syntax.node.is_valid()) {
    return SourceRange::invalid();
  }
  return file->syntax->node(obligation.syntax.node).range;
}

// Resolver transactions have checked proposal bytes in memory before those
// objects exist in the persistent store. Offline builds pass an empty span and
// therefore always take the store path.
[[nodiscard]] const GeneratedExpansion *find_staged_expansion(
    std::span<const GeneratedExpansion> expansions,
    const Sha256Digest &digest) {
  for (const GeneratedExpansion &expansion : expansions) {
    if (expansion.digest == digest) return &expansion;
  }
  return nullptr;
}

} // namespace

ResolutionOverlayResult build_resolution_overlays(
    const SourceManager &surface_sources,
    std::span<const ResolutionSurfacePackage> packages,
    const ResolutionManifest &manifest,
    std::string_view target_identity,
    const std::filesystem::path &workspace_directory,
    std::span<const GeneratedExpansion> staged_expansions,
    DiagnosticSink &diagnostics) {
  ResolutionOverlayResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  if (manifest.target_identity != target_identity) {
    diagnostics.error(
        SourceRange::invalid(),
        "resolution manifest target '" + manifest.target_identity +
            "' does not match selected target '" +
            std::string(target_identity) + "'");
    return result;
  }

  std::vector<bool> matched_pins(manifest.pins.size(), false);
  std::vector<SourceEdit> edits;
  for (const ResolutionSurfacePackage &package : packages) {
    if (package.identity == nullptr || package.loaded == nullptr ||
        package.obligations == nullptr) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolution overlay received an incomplete surface package");
      continue;
    }
    for (const AgentObligation &obligation :
         package.obligations->obligations) {
      if (!is_synthesis(obligation.kind)) continue;
      const SourceRange range = obligation_range(*package.loaded, obligation);
      std::size_t pin_index = 0;
      const ResolutionPin *pin =
          find_pin(manifest, obligation.site_identity, pin_index);
      if (pin == nullptr) {
        diagnostics.error(
            range,
            "synthesis site has no resolution pin; run 'draftc resolve'");
        continue;
      }
      matched_pins[pin_index] = true;
      if (pin->kind != obligation.kind) {
        diagnostics.error(range, "resolution pin grammar category is stale");
        continue;
      }
      if (pin->input_digest != obligation.input_digest) {
        diagnostics.error(
            range,
            "resolution pin is stale because its synthesis input changed");
        continue;
      }
      const LoadedPackageFile *file =
          find_file(*package.loaded, obligation.syntax.file);
      if (file == nullptr || file->kind != PackageFileKind::DraftSource ||
          !range.is_valid()) {
        diagnostics.error(
            SourceRange::invalid(),
            "synthesis obligation has no replaceable Draft source range");
        continue;
      }
      std::string expansion;
      const GeneratedExpansion *staged =
          find_staged_expansion(staged_expansions, pin->expansion_digest);
      if (staged != nullptr) {
        if (sha256(staged->source) != staged->digest) {
          diagnostics.error(
              range,
              "staged generated expansion does not match its content identity");
          continue;
        }
        expansion = staged->source;
      } else {
        if (!load_generated_expansion(
                workspace_directory,
                pin->expansion_digest,
                expansion,
                diagnostics)) {
          continue;
        }
      }
      edits.push_back({
          file->source,
          range,
          std::move(expansion),
          obligation.site_identity,
      });
    }
  }

  // A manifest for a different selected graph must not be partially consumed.
  // Otherwise an obsolete or ambiguous site association could remain hidden in
  // a nominally locked build.
  for (std::size_t index = 0; index < manifest.pins.size(); ++index) {
    if (!matched_pins[index]) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolution manifest pin does not match a selected synthesis site '" +
              manifest.pins[index].site_identity + "'");
    }
  }
  if (diagnostics.error_count() != initial_errors) return result;

  // Compose one complete override per edited file. Ascending source order makes
  // overlap detection direct and preserves untouched bytes exactly. The
  // generated fragment is inserted verbatim; grammar-specific validity is
  // established only by the subsequent ordinary parse and semantic pass.
  for (const ResolutionSurfacePackage &package : packages) {
    for (const LoadedPackageFile &file : package.loaded->files) {
      std::vector<const SourceEdit *> file_edits;
      for (const SourceEdit &edit : edits) {
        if (edit.file == file.source) file_edits.push_back(&edit);
      }
      if (file_edits.empty()) continue;
      std::sort(
          file_edits.begin(),
          file_edits.end(),
          [](const SourceEdit *left, const SourceEdit *right) {
            if (left->range.begin.offset != right->range.begin.offset) {
              return left->range.begin.offset < right->range.begin.offset;
            }
            return left->site_identity < right->site_identity;
          });

      const std::string_view surface = surface_sources.text(file.source);
      std::size_t cursor = 0;
      std::string resolved;
      for (const SourceEdit *edit : file_edits) {
        const std::size_t begin = edit->range.begin.offset;
        const std::size_t end = edit->range.end.offset;
        if (edit->range.begin.file != file.source ||
            edit->range.end.file != file.source || begin < cursor ||
            end < begin || end > surface.size()) {
          diagnostics.error(
              edit->range,
              "resolution sites overlap or escape their surface source file");
          continue;
        }
        resolved.append(surface.substr(cursor, begin - cursor));
        resolved.append(edit->replacement);
        cursor = end;
        ++result.applied_sites;
      }
      if (diagnostics.error_count() != initial_errors) continue;
      resolved.append(surface.substr(cursor));
      result.sources.push_back({
          *package.identity,
          {file.relative_name, std::move(resolved)},
      });
    }
  }
  if (diagnostics.error_count() != initial_errors) {
    result.sources.clear();
    result.applied_sites = 0;
    return result;
  }
  result.ok = true;
  return result;
}

} // namespace draft
