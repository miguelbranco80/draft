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
#include <optional>
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

  // The body names both an entire declaration and one aggregate field supplied
  // by early synthesis. A one-pass body checker would reject these names before
  // the provider could make the program complete.
  void write_staged_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "... \"declare answer\"\n\n"
           << "Packet :: struct {\n"
           << "    ... \"add value field\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    packet: Packet\n"
           << "    packet.value = answer\n"
           << "    ... \"verify generated values\"\n"
           << "}\n";
  }

  // The consumer cannot even resolve its body until a dependency has published
  // the generated public declaration. This forces more than one interface
  // discovery round across the package graph.
  void write_dependency_staged_source() const {
    std::error_code error;
    const std::filesystem::path dependency = root / "dep";
    std::filesystem::create_directories(dependency, error);
    if (error) std::exit(EXIT_FAILURE);
    std::ofstream dependency_source(
        dependency / "package.draft", std::ios::binary | std::ios::trunc);
    dependency_source << "package dep\n\n"
                      << "... \"declare public answer\"\n";
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import dep\n\n"
           << "main :: proc() {\n"
           << "    answer: i64 = dep.answer\n"
           << "    assert(answer == 42)\n"
           << "}\n";
  }

  void write_complete_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "main :: proc() {\n"
           << "}\n";
  }

  void write_test_source() const {
    std::ofstream source(
        package / "candidate_test.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import core/testing\n\n"
           << "test_generated_answer :: proc(test: ^testing.Test) {\n"
           << "    testing.expect(test, answer() == 42)\n"
           << "}\n";
  }
};

struct FakeProviderState {
  std::size_t calls = 0;
  std::string response = "42";
  std::string last_prompt;
  std::string last_attachment;
  bool staged_responses = false;
  std::vector<draft::AgentConstructKind> kinds;
};

struct FakeTestRunnerState {
  std::size_t calls = 0;
  std::size_t selected_procedures = 0;
  bool pass = true;
  bool saw_program_identity = false;
  bool saw_manifest = false;
  bool saw_llvm = false;
};

bool run_candidate_tests(
    void *opaque,
    const draft::TargetProfile &target,
    const draft::CompileWorkspaceResult &compiled,
    draft::DiagnosticSink &diagnostics) {
  (void)diagnostics;
  auto *state = static_cast<FakeTestRunnerState *>(opaque);
  ++state->calls;
  state->selected_procedures = compiled.validation_entries.size();
  state->saw_program_identity = compiled.resolved_program_digest.has_value();
  state->saw_manifest = compiled.resolution_manifest.has_value();
  for (const std::optional<draft::CompiledPackage> &package :
       compiled.packages) {
    if (package.has_value() && package->llvm.ok && !package->llvm.text.empty()) {
      state->saw_llvm = true;
    }
  }
  return target.facts.identity == "draft-aarch64-macos-v5" && state->pass;
}

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
  state->kinds.push_back(request.obligation.kind);
  state->last_prompt = request.prompt;
  state->last_attachment = request.attachments.empty()
      ? std::string()
      : request.attachments[0].contents;
  if (state->staged_responses) {
    switch (request.obligation.kind) {
    case draft::AgentConstructKind::SynthesisDeclaration:
      response.source = "pub answer :: 42;";
      break;
    case draft::AgentConstructKind::SynthesisMember:
      response.source = "value: i64,";
      break;
    case draft::AgentConstructKind::SynthesisStatement:
      response.source = "assert(packet.value == answer)";
      break;
    default:
      diagnostics.error(
          draft::SourceRange::invalid(),
          "fixture received an unexpected synthesis category");
      return false;
    }
  } else {
    response.source = state->response;
  }
  return true;
}

draft::CompileWorkspaceOptions compile_options(
    const TemporaryWorkspace &workspace) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = workspace.root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-bootstrap-v1";
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

draft::ExternalInputPin fake_toolchain_pin() {
  draft::ExternalInputPin pin;
  pin.kind = draft::ExternalInputKind::Toolchain;
  pin.name = "fixture-toolchain";
  pin.content_digest = draft::sha256("exact fixture toolchain tree");
  pin.entry_point = "bin/clang";
  return pin;
}

void test_resolution_reuse_revalidation_and_failure(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("first prompt");
  FakeProviderState provider;

  draft::SourceManager first_sources;
  draft::DiagnosticSink first_diagnostics;
  draft::ResolveWorkspaceOptions first_options =
      resolve_options(workspace, provider);
  first_options.external_inputs_configured = true;
  first_options.external_inputs.push_back(fake_toolchain_pin());
  const draft::ResolveWorkspaceResult first = draft::resolve_workspace(
      first_sources,
      workspace.package.string(),
      std::move(first_options),
      first_diagnostics);
  if (!first.ok) {
    std::cerr << draft::render_diagnostics(first_sources, first_diagnostics);
  }
  EXPECT(state, first.ok);
  EXPECT(state, first.committed);
  EXPECT(state, first.synthesized_sites == 1);
  EXPECT(state, first.manifest.external_inputs.size() == 1);
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
  EXPECT(state, offline.resolution_manifest.has_value());
  if (offline.resolution_manifest.has_value()) {
    EXPECT(state,
        offline.resolution_manifest->external_inputs.size() == 1);
  }

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
  EXPECT(state, reuse.manifest.external_inputs.size() == 1);
  if (reuse.manifest.external_inputs.size() == 1) {
    EXPECT(state,
        reuse.manifest.external_inputs.front().content_digest ==
            fake_toolchain_pin().content_digest);
  }

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
  EXPECT(state, revalidated.manifest.external_inputs.size() == 1);

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

void test_external_inputs_commit_without_synthesis(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_complete_source();
  FakeProviderState provider;
  draft::ResolveWorkspaceOptions options = resolve_options(workspace, provider);
  options.provider = {};
  options.external_inputs_configured = true;
  options.external_inputs.push_back(fake_toolchain_pin());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      std::move(options),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.manifest.pins.empty());
  EXPECT(state, resolved.manifest.external_inputs.size() == 1);
  EXPECT(state, provider.calls == 0);

  draft::DiagnosticSink loaded_diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.root, loaded_diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.external_inputs.size() == 1);

  // The provider-free compiler still verifies the coherent program identity
  // when a manifest exists solely to lock external build inputs.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_interface_sites_precede_dependent_bodies(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_staged_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 3);
  EXPECT(state, provider.calls == 3);
  EXPECT(state, provider.kinds.size() == 3);
  if (provider.kinds.size() == 3) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisDeclaration);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisMember);
    EXPECT(state, provider.kinds[2] ==
        draft::AgentConstructKind::SynthesisStatement);
  }

  // The committed result must be consumable by the provider-free compiler,
  // which has to reproduce the same interface/body staging from stored pins.
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
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
  EXPECT(state, resolved.manifest.format == "draft-resolution-v3");
  EXPECT(state, resolved.manifest.pins.size() == 3);
  std::size_t composed_maps = 0;
  for (const draft::WorkspacePackage &package : offline.graph.packages) {
    for (const draft::LoadedPackageFile &file : package.loaded.files) {
      const std::vector<draft::SourceExpansionMap> &maps =
          offline_sources.file(file.source).expansion_maps;
      composed_maps += maps.size();
      for (std::size_t index = 1; index < maps.size(); ++index) {
        EXPECT(state, maps[index - 1].generated_end <=
            maps[index].generated_begin);
      }
    }
  }
  EXPECT(state, composed_maps == 3);

  draft::SourceManager reuse_sources;
  draft::DiagnosticSink reuse_diagnostics;
  const draft::ResolveWorkspaceResult reused = draft::resolve_workspace(
      reuse_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      reuse_diagnostics);
  EXPECT(state, reused.ok);
  EXPECT(state, reused.synthesized_sites == 0);
  EXPECT(state, reused.reused_sites == 3);
  EXPECT(state, provider.calls == 3);
}

void test_dependency_interface_rounds(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_dependency_staged_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.synthesized_sites == 1);
  EXPECT(state, provider.calls == 1);
  EXPECT(state, provider.kinds.size() == 1);
  if (provider.kinds.size() == 1) {
    EXPECT(state, provider.kinds.front() ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }

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
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, offline.packages.size() == 2);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_tests_gate_manifest_commit(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("candidate with tests");
  workspace.write_test_source();
  FakeProviderState provider;

  // Merely type-checking the generated expression is insufficient when the
  // selected package contains typed tests. With no execution boundary the
  // resolver must reject the transaction and leave no visible manifest.
  draft::SourceManager missing_sources;
  draft::DiagnosticSink missing_diagnostics;
  const draft::ResolveWorkspaceResult missing_runner =
      draft::resolve_workspace(
          missing_sources,
          workspace.package.string(),
          resolve_options(workspace, provider),
          missing_diagnostics);
  EXPECT(state, !missing_runner.ok);
  EXPECT(state, !missing_runner.committed);
  EXPECT(state, missing_runner.tested_procedures == 1);
  EXPECT(state, missing_diagnostics.has_errors());
  draft::DiagnosticSink missing_manifest_diagnostics;
  const draft::ResolutionManifestLoadResult missing_manifest =
      draft::load_resolution_manifest(
          workspace.root, missing_manifest_diagnostics);
  EXPECT(state,
      missing_manifest.state == draft::ResolutionManifestLoadState::Missing);

  FakeTestRunnerState runner;
  draft::ResolveWorkspaceOptions passing = resolve_options(workspace, provider);
  passing.test_runner.state = &runner;
  passing.test_runner.run = run_candidate_tests;
  draft::SourceManager passing_sources;
  draft::DiagnosticSink passing_diagnostics;
  const draft::ResolveWorkspaceResult accepted = draft::resolve_workspace(
      passing_sources,
      workspace.package.string(),
      std::move(passing),
      passing_diagnostics);
  if (!accepted.ok) {
    std::cerr << draft::render_diagnostics(
        passing_sources, passing_diagnostics);
  }
  EXPECT(state, accepted.ok);
  EXPECT(state, accepted.committed);
  EXPECT(state, accepted.tested_procedures == 1);
  EXPECT(state, runner.calls == 1);
  EXPECT(state, runner.selected_procedures == 1);
  EXPECT(state, runner.saw_program_identity);
  EXPECT(state, runner.saw_manifest);
  EXPECT(state, runner.saw_llvm);

  draft::DiagnosticSink before_diagnostics;
  const draft::ResolutionManifestLoadResult before =
      draft::load_resolution_manifest(workspace.root, before_diagnostics);
  EXPECT(state, before.state == draft::ResolutionManifestLoadState::Loaded);
  const std::string committed =
      draft::serialize_resolution_manifest(before.manifest);

  // A later failing test run rejects a newly typed candidate after provider
  // work but before the one authoritative manifest rename.
  workspace.write_source("changed candidate rejected by tests");
  runner.pass = false;
  draft::ResolveWorkspaceOptions failing = resolve_options(workspace, provider);
  failing.test_runner.state = &runner;
  failing.test_runner.run = run_candidate_tests;
  draft::SourceManager failing_sources;
  draft::DiagnosticSink failing_diagnostics;
  const draft::ResolveWorkspaceResult rejected = draft::resolve_workspace(
      failing_sources,
      workspace.package.string(),
      std::move(failing),
      failing_diagnostics);
  EXPECT(state, !rejected.ok);
  EXPECT(state, !rejected.committed);
  EXPECT(state, rejected.tested_procedures == 1);
  EXPECT(state, runner.calls == 2);
  EXPECT(state, failing_diagnostics.has_errors());

  draft::DiagnosticSink after_diagnostics;
  const draft::ResolutionManifestLoadResult after =
      draft::load_resolution_manifest(workspace.root, after_diagnostics);
  EXPECT(state, after.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state,
      draft::serialize_resolution_manifest(after.manifest) == committed);
}

} // namespace

int main() {
  TestState state;
  test_resolution_reuse_revalidation_and_failure(state);
  test_external_inputs_commit_without_synthesis(state);
  test_interface_sites_precede_dependent_bodies(state);
  test_dependency_interface_rounds(state);
  test_tests_gate_manifest_commit(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolver tests passed\n";
  return EXIT_SUCCESS;
}
