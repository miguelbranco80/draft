// Offline pin application through the ordinary package and compiler pipeline.

#include "compile/compiler.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"

#include "base/sha256.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

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
  std::filesystem::path root;
  std::filesystem::path package;

  TemporaryWorkspace() {
    std::error_code error;
    root = std::filesystem::temp_directory_path(error) /
        "draft-resolution-overlay-test";
    if (error) std::exit(EXIT_FAILURE);
    package = root / "app";
    std::filesystem::remove_all(root, error);
    error.clear();
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

  ~TemporaryWorkspace() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
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
  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("resolved fixture");
  manifest.pins.push_back(pin);
  EXPECT(state,
      draft::commit_resolution(
          workspace.root,
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
          "aarch64-apple-macos",
          workspace.root,
          diagnostics);
  if (!overlay.ok) {
    std::cerr << draft::render_diagnostics(surface_sources, diagnostics);
  }
  EXPECT(state, overlay.ok);
  EXPECT(state, overlay.applied_sites == 1);
  EXPECT(state, overlay.sources.size() == 1);

  draft::SourceManager resolved_sources;
  draft::CompileWorkspaceOptions resolved_options =
      compile_options(workspace, true);
  resolved_options.workspace.source_overrides = overlay.sources;
  draft::DiagnosticSink resolved_diagnostics;
  const draft::CompileWorkspaceResult resolved = draft::compile_workspace(
      resolved_sources,
      workspace.package.string(),
      std::move(resolved_options),
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
  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("stale program");
  manifest.pins.push_back(pin);
  EXPECT(state,
      draft::commit_resolution(
          workspace.root,
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
          "aarch64-apple-macos",
          workspace.root,
          stale_diagnostics);
  EXPECT(state, !overlay.ok);
  EXPECT(state, overlay.sources.empty());
  EXPECT(state, stale_diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_pinned_expression_reenters_compiler(state);
  test_stale_pin_produces_no_overlay(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolution-overlay expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolution overlay tests passed\n";
  return EXIT_SUCCESS;
}
