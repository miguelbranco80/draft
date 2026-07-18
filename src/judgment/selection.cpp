// Direct selector and manifest-row logic for judgment commands.

#include "judgment/selection.h"

#include "judgment/evidence_store.h"
#include "workspace/workspace.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace draft {
namespace {

[[nodiscard]] const WorkspacePackage *find_package(
    const CompileWorkspaceResult &compiled,
    const AgentObligation &obligation) {
  for (const WorkspacePackage &package : compiled.graph.packages) {
    if (package.identity.root_identity == obligation.root_identity &&
        package.identity.root_relative_path == obligation.root_relative_path) {
      return &package;
    }
  }
  return nullptr;
}

[[nodiscard]] bool package_matches(
    const JudgmentSiteDescription &site,
    std::string_view selector) {
  return selector == site.package_selector ||
      selector == site.package_identity;
}

[[nodiscard]] bool selector_matches(
    const JudgmentSiteDescription &site,
    std::string_view selector) {
  if (selector == site.site_identity || package_matches(site, selector)) {
    return true;
  }
  const std::size_t colon = selector.rfind(':');
  if (colon == std::string_view::npos || colon == 0 ||
      colon + 1 == selector.size()) {
    return false;
  }
  return package_matches(site, selector.substr(0, colon)) &&
      site.anchor_name == selector.substr(colon + 1);
}

void selection_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(SourceRange::invalid(), std::move(message));
}

[[nodiscard]] bool selected(
    const JudgmentSelection &selection,
    std::string_view site_identity) {
  for (const JudgmentSiteDescription &site : selection.sites) {
    if (site.site_identity == site_identity) return true;
  }
  return false;
}

} // namespace

std::vector<JudgmentSiteDescription> discover_judgment_sites(
    const CompileWorkspaceResult &compiled) {
  std::vector<JudgmentSiteDescription> result;
  for (const std::optional<CompiledPackage> &compiled_package :
       compiled.packages) {
    if (!compiled_package.has_value()) continue;
    for (const AgentObligation &obligation :
         compiled_package->obligations.obligations) {
      if (obligation.kind != AgentConstructKind::Judgment) continue;
      JudgmentSiteDescription site;
      site.site_identity = obligation.site_identity;
      site.package_identity = obligation.root_identity + ":" +
          obligation.root_relative_path;
      site.package_selector = obligation.root_relative_path;
      const WorkspacePackage *package = find_package(compiled, obligation);
      if (site.package_selector == "." && package != nullptr) {
        site.package_selector = package->loaded.short_name;
      }
      site.anchor_name = obligation.anchor_name;
      site.source_relative_path = obligation.source_relative_path;
      site.occurrence = obligation.occurrence;
      result.push_back(std::move(site));
    }
  }
  return result;
}

bool select_judgment_sites(
    const CompileWorkspaceResult &compiled,
    const std::vector<std::string> &selectors,
    JudgmentSelection &selection,
    DiagnosticSink &diagnostics) {
  selection = {};
  const std::vector<JudgmentSiteDescription> discovered =
      discover_judgment_sites(compiled);
  if (selectors.empty()) {
    selection.sites = discovered;
    return true;
  }

  for (const std::string &selector : selectors) {
    if (selector.empty()) {
      selection_error(diagnostics, "judgment selector must not be empty");
      return false;
    }
    bool matched = false;
    for (const JudgmentSiteDescription &site : discovered) {
      if (selector_matches(site, selector)) matched = true;
    }
    if (!matched) {
      selection_error(
          diagnostics, "judgment selector matched no site: '" + selector + "'");
      return false;
    }
  }

  for (const JudgmentSiteDescription &site : discovered) {
    bool matched = false;
    for (const std::string &selector : selectors) {
      if (selector_matches(site, selector)) {
        matched = true;
        break;
      }
    }
    if (matched) selection.sites.push_back(site);
  }
  return true;
}

bool judgment_selection_contains(
    const JudgmentSelection &selection,
    std::string_view site_identity) {
  return selected(selection, site_identity);
}

bool replace_selected_judgment_evidence(
    const std::filesystem::path &workspace_directory,
    const std::vector<ResolutionEvidencePin> &current,
    const JudgmentSelection &selection,
    const std::vector<ResolutionEvidencePin> &replacement,
    std::vector<ResolutionEvidencePin> &result,
    DiagnosticSink &diagnostics) {
  result.clear();
  for (const ResolutionEvidencePin &pin : current) {
    if (pin.kind != "judgment") {
      result.push_back(pin);
      continue;
    }
    JudgmentEvidenceState state;
    if (!load_judgment_evidence_state(
            workspace_directory, pin.key, state, diagnostics)) {
      return false;
    }
    if (!state.latest_evidence.has_value()) {
      selection_error(
          diagnostics,
          "cannot map manifest judgment row to its stable site identity");
      return false;
    }
    if (!selected(selection, state.latest_evidence->claim.site_identity)) {
      result.push_back(pin);
    }
  }

  std::vector<std::string> replacement_sites;
  for (const ResolutionEvidencePin &pin : replacement) {
    if (pin.kind != "judgment") {
      selection_error(
          diagnostics, "judgment replacement contains non-judgment evidence");
      return false;
    }
    JudgmentEvidenceState state;
    if (!load_judgment_evidence_state(
            workspace_directory, pin.key, state, diagnostics)) {
      return false;
    }
    if (state.status != JudgmentEvidenceStateStatus::Active ||
        !state.active_digest.has_value() ||
        !state.active_evidence.has_value() ||
        *state.active_digest != pin.content_digest ||
        !selected(selection, state.active_evidence->claim.site_identity)) {
      selection_error(
          diagnostics,
          "replacement judgment row is not an active selected site attempt");
      return false;
    }
    for (const std::string &site : replacement_sites) {
      if (site == state.active_evidence->claim.site_identity) {
        selection_error(
            diagnostics, "judgment replacement contains a duplicate site");
        return false;
      }
    }
    replacement_sites.push_back(state.active_evidence->claim.site_identity);
    result.push_back(pin);
  }
  if (replacement_sites.size() != selection.sites.size()) {
    selection_error(
        diagnostics,
        "judgment replacement does not cover every selected site");
    return false;
  }
  return true;
}

} // namespace draft
