// Offline pin application through the ordinary package and compiler pipeline.

#include "compile/compiler.h"
#include "elaborator/resolved_program.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"

#include "base/sha256.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include "test_directory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "resolution_overlay_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryWorkspace {
  draft::test::TemporaryDirectory directory{"draft-resolution-overlay-test"};
  std::filesystem::path root;
  std::filesystem::path package;

  TemporaryWorkspace() {
    root = directory.path();
    std::error_code error;
    package = root / "app";
    std::filesystem::create_directories(package, error);
    if (error) std::exit(EXIT_FAILURE);
    std::ofstream source(package / "package.draft", std::ios::binary);
    source << R"draft(package app

answer :: proc() -> i64 {
    return ... "produce forty-two"
}

main :: proc() {
}
)draft";
  }

};

draft::CompileWorkspaceOptions compile_options(
    const TemporaryWorkspace &workspace,
    bool emit_llvm) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = workspace.root.string();
  options.lower_mir = emit_llvm;
  options.emit_llvm = emit_llvm;
  return options;
}

// Tests derive the manifest spelling from the same versioned target profile as
// the compiler instead of maintaining a second target-name fixture.
std::string target_identity() {
  return draft::make_aarch64_macos_profile().facts.identity;
}

// Every fixture compiles the `app` package inside the temporary workspace.
// Bind that selected root explicitly so offline compiler loads exercise the
// same root/target namespace as resolver-produced manifests.
void bind_selected_root(draft::ResolutionManifest &manifest) {
  manifest.root_package = {"workspace", "app"};
}

draft::ResolutionStoreKey store_key_for(
    const draft::ResolutionManifest &manifest) {
  return {manifest.target_identity, manifest.root_package};
}

// Manual manifest fixtures must carry the same persistent map the resolver
// derives from an obligation. Keeping the helper here makes each negative test
// vary only the field it intends to invalidate.
void bind_source_map(
    draft::ResolutionPin &pin,
    const draft::AgentObligation &obligation,
    const draft::LoadedPackage &loaded,
    std::string_view expansion) {
  for (const draft::LoadedPackageFile &file : loaded.files) {
    if (file.source != obligation.syntax.file || !file.syntax.has_value()) {
      continue;
    }
    const draft::SourceRange range =
        file.syntax->node(obligation.syntax.node).range;
    pin.source_map = {
        obligation.root_identity,
        obligation.root_relative_path,
        file.relative_name,
        range.begin.offset,
        range.end.offset,
        static_cast<std::uint64_t>(expansion.size()),
    };
    return;
  }
}

void test_pinned_expression_reenters_compiler(TestState &state) {
  TemporaryWorkspace workspace;
  draft::SourceManager surface_sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceResult surface = draft::compile_workspace(
      surface_sources,
      workspace.package.string(),
      compile_options(workspace, false),
      diagnostics);
  if (!surface.ok) {
    std::cerr << draft::render_diagnostics(surface_sources, diagnostics);
    EXPECT(state, false);
    return;
  }
  EXPECT(state, surface.packages.size() == 1);
  if (surface.packages.size() != 1 || !surface.packages[0].has_value()) return;
  const draft::CompiledPackage &surface_package = *surface.packages[0];
  EXPECT(state, surface_package.obligations.obligations.size() == 1);
  if (surface_package.obligations.obligations.size() != 1) return;
  const draft::AgentObligation &obligation =
      surface_package.obligations.obligations[0];

  draft::GeneratedExpansion expansion;
  expansion.source = "42";
  expansion.digest = draft::sha256(expansion.source);
  draft::ResolutionPin pin;
  pin.site_identity = obligation.site_identity;
  pin.kind = obligation.kind;
  pin.input_digest = obligation.input_digest;
  pin.expansion_digest = expansion.digest;
  pin.provider_identity = "deterministic-test-provider";
  pin.model_identity = "fixture-v1";
  pin.configuration_identity = "resolver-config-v1";
  bind_source_map(
      pin, obligation, surface.graph.packages[0].loaded, expansion.source);
  draft::ResolutionManifest manifest;
  manifest.target_identity = target_identity();
  bind_selected_root(manifest);
  manifest.resolved_program_digest = draft::sha256("resolved fixture");
  manifest.pins.push_back(pin);
  EXPECT(state,
      draft::commit_resolution(
          workspace.root,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics));

  const draft::ResolutionSurfacePackage input{
      &surface_package.identity,
      &surface.graph.packages[0].loaded,
      &surface_package.obligations,
  };
  const draft::ResolutionOverlayResult overlay =
      draft::build_resolution_overlays(
          surface_sources,
          std::span<const draft::ResolutionSurfacePackage>(&input, 1),
          manifest,
          target_identity(),
          workspace.root,
          draft::ResolutionInputVerification::RequireCurrentInput,
          {},
          diagnostics);
  if (!overlay.ok) {
    std::cerr << draft::render_diagnostics(surface_sources, diagnostics);
  }
  EXPECT(state, overlay.ok);
  EXPECT(state, overlay.applied_sites == 1);
  EXPECT(state, overlay.sources.size() == 1);
  if (overlay.sources.size() == 1) {
    const draft::PackageSourceOverride &source = overlay.sources[0].source;
    EXPECT(state, source.expansion_maps.size() == 1);
    if (source.expansion_maps.size() == 1) {
      const draft::SourceExpansionMap &map = source.expansion_maps[0];
      EXPECT(state, map.site_identity == obligation.site_identity);
      EXPECT(state, map.surface_begin.line == 4);
      EXPECT(state,
          std::string_view(source.contents).substr(
              map.generated_begin,
              map.generated_end - map.generated_begin) == expansion.source);

      // A map remains useful after the complete override moves into a fresh
      // SourceManager. Diagnostic presentation should retain both the exact
      // generated byte and its handwritten synthesis site.
      draft::SourceManager mapped_sources;
      const draft::FileId mapped_file = mapped_sources.add_source(
          "package.draft [resolved]",
          source.contents,
          source.expansion_maps);
      draft::DiagnosticSink mapped_diagnostics;
      mapped_diagnostics.error(
          draft::SourceRange::at(mapped_file, map.generated_begin),
          "generated fixture failure");
      const std::string rendered =
          draft::render_diagnostics(mapped_sources, mapped_diagnostics);
      EXPECT(state, rendered.find("generated from synthesis site " +
          obligation.site_identity) != std::string::npos);
      EXPECT(state, rendered.find("package.draft:4:") != std::string::npos);
    }
  }

  // Resolution computes the coherent program identity only after the proposed
  // expansion has passed a complete ordinary compilation. The test performs
  // that provider-independent transaction phase explicitly, then republishes
  // the same content-addressed expansion under the final manifest digest.
  draft::SourceManager identity_sources;
  draft::CompileWorkspaceOptions identity_options =
      compile_options(workspace, false);
  identity_options.workspace.source_overrides = overlay.sources;
  draft::DiagnosticSink identity_diagnostics;
  const draft::CompileWorkspaceResult identity_program =
      draft::compile_workspace(
          identity_sources,
          workspace.package.string(),
          identity_options,
          identity_diagnostics);
  EXPECT(state, identity_program.ok);
  if (!identity_program.ok) return;
  manifest.resolved_program_digest = draft::hash_resolved_program(
      identity_sources,
      identity_program.graph,
      identity_options.target,
      manifest,
      identity_options.compiler_content_identity,
      identity_options.configuration);
  draft::CompileConfiguration disabled_assertions =
      identity_options.configuration;
  disabled_assertions.runtime_assertions =
      draft::RuntimeAssertionMode::Off;
  EXPECT(
      state,
      manifest.resolved_program_digest != draft::hash_resolved_program(
          identity_sources,
          identity_program.graph,
          identity_options.target,
          manifest,
          identity_options.compiler_content_identity,
          disabled_assertions));
  EXPECT(state,
      draft::commit_resolution(
          workspace.root,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(),
          diagnostics));

  // The public offline compiler repeats the same surface-to-overlay operation
  // internally. No provider object or callback is available in this process.
  draft::SourceManager resolved_sources;
  draft::DiagnosticSink resolved_diagnostics;
  const draft::CompileWorkspaceResult resolved =
      draft::compile_workspace_with_resolution(
          resolved_sources,
          workspace.package.string(),
          compile_options(workspace, true),
          resolved_diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(
        resolved_sources, resolved_diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, !resolved_diagnostics.has_errors());
  if (resolved.packages.size() == 1 && resolved.packages[0].has_value()) {
    EXPECT(state,
        resolved.packages[0]->obligations.obligations.empty());
    EXPECT(state,
        resolved.packages[0]->llvm.text.find("ret i64 42") !=
            std::string::npos);
  }

  manifest.resolved_program_digest = draft::sha256("wrong program identity");
  EXPECT(state,
      draft::commit_resolution(
          workspace.root,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(),
          diagnostics));
  draft::SourceManager stale_program_sources;
  draft::DiagnosticSink stale_program_diagnostics;
  const draft::CompileWorkspaceResult stale_program =
      draft::compile_workspace_with_resolution(
          stale_program_sources,
          workspace.package.string(),
          compile_options(workspace, false),
          stale_program_diagnostics);
  EXPECT(state, !stale_program.ok);
  bool saw_stale_program = false;
  for (const draft::Diagnostic &diagnostic :
       stale_program_diagnostics.diagnostics()) {
    if (diagnostic.message.find("resolved-program identity is stale") !=
        std::string::npos) {
      saw_stale_program = true;
    }
  }
  EXPECT(state, saw_stale_program);
}

void test_generated_judgment_is_rejected(TestState &state) {
  TemporaryWorkspace workspace;
  {
    std::ofstream source(
        workspace.package / "package.draft",
        std::ios::binary | std::ios::trunc);
    source << R"draft(package app

main :: proc() {
    ... "produce one statement"
}
)draft";
  }
  draft::SourceManager surface_sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult surface = draft::compile_workspace(
      surface_sources,
      workspace.package.string(),
      compile_options(workspace, false),
      diagnostics);
  if (!surface.ok || !surface.packages[0].has_value()) {
    EXPECT(state, false);
    return;
  }
  const draft::AgentObligation &obligation =
      surface.packages[0]->obligations.obligations[0];
  draft::GeneratedExpansion expansion{
      draft::sha256("judge \"generated claim\";"),
      "judge \"generated claim\";",
  };
  draft::ResolutionPin pin;
  pin.site_identity = obligation.site_identity;
  pin.kind = obligation.kind;
  pin.input_digest = obligation.input_digest;
  pin.expansion_digest = expansion.digest;
  pin.provider_identity = "test-provider";
  pin.model_identity = "fixture";
  pin.configuration_identity = "resolver-config-v1";
  bind_source_map(
      pin, obligation, surface.graph.packages[0].loaded, expansion.source);
  draft::ResolutionManifest manifest;
  manifest.target_identity = target_identity();
  bind_selected_root(manifest);
  manifest.resolved_program_digest = draft::sha256("invalid generated judgment");
  manifest.pins.push_back(pin);
  EXPECT(state,
      draft::commit_resolution(
          workspace.root,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics));

  draft::SourceManager resolved_sources;
  draft::DiagnosticSink resolved_diagnostics;
  const draft::CompileWorkspaceResult resolved =
      draft::compile_workspace_with_resolution(
          resolved_sources,
          workspace.package.string(),
          compile_options(workspace, false),
          resolved_diagnostics);
  EXPECT(state, !resolved.ok);
  EXPECT(state, resolved_diagnostics.has_errors());
  bool saw_generated_judgment = false;
  for (const draft::Diagnostic &diagnostic :
       resolved_diagnostics.diagnostics()) {
    if (diagnostic.message.find("may not introduce a judgment") !=
        std::string::npos) {
      saw_generated_judgment = true;
    }
  }
  EXPECT(state, saw_generated_judgment);
}

void test_stale_pin_produces_no_overlay(TestState &state) {
  TemporaryWorkspace workspace;
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult surface = draft::compile_workspace(
      sources,
      workspace.package.string(),
      compile_options(workspace, false),
      diagnostics);
  if (!surface.ok || !surface.packages[0].has_value()) {
    EXPECT(state, false);
    return;
  }
  const draft::CompiledPackage &package = *surface.packages[0];
  const draft::AgentObligation &obligation = package.obligations.obligations[0];
  draft::GeneratedExpansion expansion{draft::sha256("42"), "42"};
  draft::ResolutionPin pin;
  pin.site_identity = obligation.site_identity;
  pin.kind = obligation.kind;
  pin.input_digest = draft::sha256("stale input");
  pin.expansion_digest = expansion.digest;
  pin.provider_identity = "test-provider";
  pin.model_identity = "fixture";
  pin.configuration_identity = "resolver-config-v1";
  bind_source_map(
      pin, obligation, surface.graph.packages[0].loaded, expansion.source);
  draft::ResolutionManifest manifest;
  manifest.target_identity = target_identity();
  bind_selected_root(manifest);
  manifest.resolved_program_digest = draft::sha256("stale program");
  manifest.pins.push_back(pin);
  EXPECT(state,
      draft::commit_resolution(
          workspace.root,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics));

  const draft::ResolutionSurfacePackage input{
      &package.identity,
      &surface.graph.packages[0].loaded,
      &package.obligations,
  };
  draft::DiagnosticSink stale_diagnostics;
  const draft::ResolutionOverlayResult overlay =
      draft::build_resolution_overlays(
          sources,
          std::span<const draft::ResolutionSurfacePackage>(&input, 1),
          manifest,
          target_identity(),
          workspace.root,
          draft::ResolutionInputVerification::RequireCurrentInput,
          {},
          stale_diagnostics);
  EXPECT(state, !overlay.ok);
  EXPECT(state, overlay.sources.empty());
  EXPECT(state, stale_diagnostics.has_errors());

  // A validation graph may reuse this pin only after its caller has separately
  // authenticated the ordinary graph. The overlay still requires the same
  // structural site, grammar category, source coordinates, and generated
  // object; only the already-proved ordinary input comparison is omitted.
  draft::DiagnosticSink authenticated_diagnostics;
  const draft::ResolutionOverlayResult authenticated =
      draft::build_resolution_overlays(
          sources,
          std::span<const draft::ResolutionSurfacePackage>(&input, 1),
          manifest,
          target_identity(),
          workspace.root,
          draft::ResolutionInputVerification::AuthenticatedManifest,
          {},
          authenticated_diagnostics);
  EXPECT(state, authenticated.ok);
  EXPECT(state, authenticated.applied_sites == 1);
  EXPECT(state, !authenticated_diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_pinned_expression_reenters_compiler(state);
  test_stale_pin_produces_no_overlay(state);
  test_generated_judgment_is_rejected(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolution-overlay expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolution overlay tests passed\n";
  return EXIT_SUCCESS;
}
