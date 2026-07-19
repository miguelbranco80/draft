// Direct selector logic for judgment commands.

#include "judgment/selection.h"

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

} // namespace draft
