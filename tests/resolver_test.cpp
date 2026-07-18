// Transactional resolver tests with a deterministic in-process provider.

#include "compile/resolver.h"

#include "compile/compiler.h"
#include "elaborator/provider.h"
#include "elaborator/resolution.h"
#include "elaborator/resolution_store.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "resolver_test.cpp:" << line
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
    root = std::filesystem::temp_directory_path(error) / "draft-resolver-test";
    if (error) std::exit(EXIT_FAILURE);
    package = root / "app";
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(package, error);
    if (error) std::exit(EXIT_FAILURE);
    std::ofstream attachment(package / "PROMPT.txt", std::ios::binary);
    attachment << "exact attachment bytes\n";
  }

  ~TemporaryWorkspace() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  // Rewrites only surface source; .draft remains untouched so tests can observe
  // stale pin behavior and atomic preservation of the previous manifest.
  void write_source(std::string_view prompt) const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "answer :: proc() -> i64 {\n"
           << "    return ... \"" << prompt << "\" file \"PROMPT.txt\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "}\n";
  }
};

struct FakeProviderState {
  std::size_t calls = 0;
  std::string response = "42";
  std::string last_prompt;
  std::string last_attachment;
};

// The fake intentionally performs no language validation. This proves the
// resolver, rather than the provider, is responsible for accepting a proposal.
bool synthesize(
    void *opaque,
    const draft::SynthesisRequest &request,
    draft::SynthesisResponse &response,
    draft::DiagnosticSink &diagnostics) {
  (void)diagnostics;
  auto *state = static_cast<FakeProviderState *>(opaque);
  ++state->calls;
  state->last_prompt = request.prompt;
  state->last_attachment = request.attachments.empty()
      ? std::string()
      : request.attachments[0].contents;
  response.source = state->response;
  return true;
}

draft::CompileWorkspaceOptions compile_options(
    const TemporaryWorkspace &workspace) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = workspace.root.string();
  return options;
}

draft::ResolveWorkspaceOptions resolve_options(
    const TemporaryWorkspace &workspace,
    FakeProviderState &provider_state) {
  draft::ResolveWorkspaceOptions options;
  options.compile = compile_options(workspace);
  options.provider.provider_identity = "deterministic-fake-provider-v1";
  options.provider.model_identity = "fixture-model-v1";
  options.provider.configuration_identity = "temperature-0-schema-v1";
  options.provider.state = &provider_state;
  options.provider.synthesize = synthesize;
  return options;
}

void test_resolution_reuse_revalidation_and_failure(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("first prompt");
  FakeProviderState provider;

  draft::SourceManager first_sources;
  draft::DiagnosticSink first_diagnostics;
  const draft::ResolveWorkspaceResult first = draft::resolve_workspace(
      first_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      first_diagnostics);
  if (!first.ok) {
    std::cerr << draft::render_diagnostics(first_sources, first_diagnostics);
  }
  EXPECT(state, first.ok);
  EXPECT(state, first.committed);
  EXPECT(state, first.synthesized_sites == 1);
  EXPECT(state, provider.calls == 1);
  EXPECT(state, provider.last_prompt == "first prompt");
  EXPECT(state, provider.last_attachment == "exact attachment bytes\n");

  // A fresh pin builds with no provider boundary in scope.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  draft::CompileWorkspaceOptions offline_options = compile_options(workspace);
  offline_options.lower_mir = true;
  offline_options.emit_llvm = true;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          offline_options,
          offline_diagnostics);
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());

  draft::SourceManager reuse_sources;
  draft::DiagnosticSink reuse_diagnostics;
  const draft::ResolveWorkspaceResult reuse = draft::resolve_workspace(
      reuse_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      reuse_diagnostics);
  EXPECT(state, reuse.ok);
  EXPECT(state, reuse.reused_sites == 1);
  EXPECT(state, reuse.synthesized_sites == 0);
  EXPECT(state, provider.calls == 1);

  // Revalidation accepts the same generated bytes under a changed obligation
  // only after a complete new compile; it never invokes the provider.
  workspace.write_source("changed for revalidation");
  draft::ResolveWorkspaceOptions revalidate =
      resolve_options(workspace, provider);
  revalidate.revalidate = true;
  draft::SourceManager revalidate_sources;
  draft::DiagnosticSink revalidate_diagnostics;
  const draft::ResolveWorkspaceResult revalidated = draft::resolve_workspace(
      revalidate_sources,
      workspace.package.string(),
      std::move(revalidate),
      revalidate_diagnostics);
  EXPECT(state, revalidated.ok);
  EXPECT(state, revalidated.reused_sites == 1);
  EXPECT(state, provider.calls == 1);

  draft::DiagnosticSink before_failure_diagnostics;
  const draft::ResolutionManifestLoadResult before_failure =
      draft::load_resolution_manifest(
          workspace.root, before_failure_diagnostics);
  EXPECT(state,
      before_failure.state == draft::ResolutionManifestLoadState::Loaded);
  const std::string committed_manifest =
      draft::serialize_resolution_manifest(before_failure.manifest);

  // A syntactically invalid provider proposal fails the ordinary compiler. The
  // previously committed manifest remains byte-for-byte authoritative.
  workspace.write_source("changed for invalid proposal");
  provider.response = "judge \"not an expression\";";
  draft::SourceManager failure_sources;
  draft::DiagnosticSink failure_diagnostics;
  const draft::ResolveWorkspaceResult failure = draft::resolve_workspace(
      failure_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      failure_diagnostics);
  EXPECT(state, !failure.ok);
  EXPECT(state, !failure.committed);
  EXPECT(state, failure_diagnostics.has_errors());
  EXPECT(state, provider.calls == 2);

  draft::DiagnosticSink after_failure_diagnostics;
  const draft::ResolutionManifestLoadResult after_failure =
      draft::load_resolution_manifest(workspace.root, after_failure_diagnostics);
  EXPECT(state,
      after_failure.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state,
      draft::serialize_resolution_manifest(after_failure.manifest) ==
          committed_manifest);
}

} // namespace

int main() {
  TestState state;
  test_resolution_reuse_revalidation_and_failure(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolver tests passed\n";
  return EXIT_SUCCESS;
}
