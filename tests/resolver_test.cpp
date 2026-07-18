// Transactional resolver tests with a deterministic in-process provider.

#include "compile/resolver.h"

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "elaborator/provider.h"
#include "elaborator/resolution.h"
#include "elaborator/resolution_store.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
           << "    expected: i64 = ... \"compute expected value\"\n"
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

  // These two sites are deliberately in the same package-level interface
  // completeness set. The provider returns declarations that the final body
  // needs, but neither request is allowed to observe the other response.
  void write_opaque_interface_set_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "... \"declare first\"\n"
           << "... \"declare second\"\n\n"
           << "main :: proc() {\n"
           << "    assert(first + second == 42)\n"
           << "}\n";
  }

  void write_complete_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "main :: proc() {\n"
           << "}\n";
  }

  void write_test_source(std::string_view extra_statement = {}) const {
    std::ofstream source(
        package / "candidate_test.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import core/testing\n\n"
           << "test_generated_answer :: proc(test: ^testing.Test) {\n"
           << "    // Validation comments are not semantic agent context.\n"
           << "    testing.expect(test, answer() == 42)\n";
    if (!extra_statement.empty()) source << "    " << extra_statement << "\n";
    source << "}\n";
  }

  void write_benchmark_source() const {
    std::ofstream source(
        package / "candidate_bench.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import core/benchmark\n\n"
           << "bench_generated_answer :: proc(state: ^benchmark.Benchmark) {\n"
           << "}\n";
  }
};

struct FakeProviderState {
  std::size_t calls = 0;
  std::string response = "42";
  std::string last_prompt;
  std::string last_attachment;
  bool staged_responses = false;
  bool opaque_interface_responses = false;
  std::vector<draft::AgentConstructKind> kinds;
  std::vector<std::vector<std::string>> visible_binding_names;
  std::vector<draft::AgentValidationContext> last_validation_context;
};

struct FakeTestRunnerState {
  std::size_t calls = 0;
  std::size_t test_calls = 0;
  std::size_t benchmark_calls = 0;
  std::size_t selected_procedures = 0;
  bool pass = true;
  bool saw_program_identity = false;
  bool saw_manifest = false;
  bool saw_llvm = false;
};

bool run_candidate_tests(
    void *opaque,
    const draft::TargetProfile &target,
    draft::ValidationKind kind,
    const draft::CompileWorkspaceResult &compiled,
    draft::ResolutionValidationEvidence &evidence,
    draft::DiagnosticSink &diagnostics) {
  (void)diagnostics;
  auto *state = static_cast<FakeTestRunnerState *>(opaque);
  ++state->calls;
  if (kind == draft::ValidationKind::Test) {
    ++state->test_calls;
  } else if (kind == draft::ValidationKind::Benchmark) {
    ++state->benchmark_calls;
  }
  state->selected_procedures = compiled.validation_entries.size();
  state->saw_program_identity = compiled.resolved_program_digest.has_value();
  state->saw_manifest = compiled.resolution_manifest.has_value();
  for (const std::optional<draft::CompiledPackage> &package :
       compiled.packages) {
    if (package.has_value() && package->llvm.ok && !package->llvm.text.empty()) {
      state->saw_llvm = true;
    }
  }
  const bool passed = kind != draft::ValidationKind::None &&
      target.facts.identity == "draft-aarch64-macos-v5" && state->pass;
  if (passed) {
    const std::string prefix(draft::validation_kind_name(kind));
    evidence.key = draft::sha256("fixture-" + prefix + "-evidence-key");
    evidence.content_digest =
        draft::sha256("fixture-passing-" + prefix + "-attempt");
    evidence.recorded = true;
  }
  return passed;
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
  state->last_validation_context = request.obligation.validation_context;
  std::vector<std::string> visible_names;
  for (const draft::AgentVisibleBinding &binding :
       request.obligation.visible_bindings) {
    visible_names.push_back(binding.name);
  }
  state->visible_binding_names.push_back(std::move(visible_names));
  if (state->opaque_interface_responses) {
    if (request.prompt == "declare first") {
      response.source = "first :: 20;";
    } else if (request.prompt == "declare second") {
      response.source = "second :: 22;";
    } else {
      diagnostics.error(
          draft::SourceRange::invalid(),
          "opaque-set fixture received an unexpected prompt");
      return false;
    }
  } else if (state->staged_responses) {
    switch (request.obligation.kind) {
    case draft::AgentConstructKind::SynthesisDeclaration:
      response.source = "pub answer :: 42;";
      break;
    case draft::AgentConstructKind::SynthesisMember:
      response.source = "value: i64,";
      break;
    case draft::AgentConstructKind::SynthesisStatement:
      response.source =
          "assert(packet.value == answer && expected == 42)";
      break;
    case draft::AgentConstructKind::SynthesisExpression:
      response.source = "42";
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

#if defined(__APPLE__)
[[nodiscard]] bool run_executable(
    const std::filesystem::path &executable,
    const std::filesystem::path &working_directory,
    int &status) {
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    if (::chdir(working_directory.c_str()) != 0) _exit(126);
    ::execl(executable.c_str(), executable.c_str(), nullptr);
    _exit(127);
  }
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

[[nodiscard]] std::string read_binary_file(
    const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(0, std::ios::end);
  const std::streamoff end = input.tellg();
  if (!input || end < 0) return {};
  std::string contents(static_cast<std::size_t>(end), '\0');
  input.seekg(0, std::ios::beg);
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  return input ? contents : std::string();
}
#endif

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

  // An explicitly selected provider configuration is part of resolution
  // meaning. Matching typed input is not enough to reuse a pin created by a
  // different model or adapter policy.
  draft::ResolveWorkspaceOptions changed_provider =
      resolve_options(workspace, provider);
  changed_provider.provider.configuration_identity =
      "temperature-0-schema-v2";
  draft::SourceManager changed_provider_sources;
  draft::DiagnosticSink changed_provider_diagnostics;
  const draft::ResolveWorkspaceResult regenerated = draft::resolve_workspace(
      changed_provider_sources,
      workspace.package.string(),
      std::move(changed_provider),
      changed_provider_diagnostics);
  EXPECT(state, regenerated.ok);
  EXPECT(state, regenerated.synthesized_sites == 1);
  EXPECT(state, regenerated.reused_sites == 0);
  EXPECT(state, provider.calls == 2);
  EXPECT(state, regenerated.manifest.pins.size() == 1);
  if (regenerated.manifest.pins.size() == 1) {
    EXPECT(state,
        regenerated.manifest.pins[0].configuration_identity ==
            "temperature-0-schema-v2");
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
  EXPECT(state, provider.calls == 2);
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
  EXPECT(state, provider.calls == 3);

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
  EXPECT(state, resolved.synthesized_sites == 4);
  EXPECT(state, provider.calls == 4);
  EXPECT(state, provider.kinds.size() == 4);
  if (provider.kinds.size() == 4) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisDeclaration);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisMember);
    EXPECT(state, provider.kinds[2] ==
        draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, provider.kinds[3] ==
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
  EXPECT(state, resolved.manifest.format == "draft-resolution-v4");
  EXPECT(state, resolved.manifest.pins.size() == 4);
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
  EXPECT(state, composed_maps == 4);

#if defined(__APPLE__)
  // The provider is no longer in scope: link the stored resolved program twice,
  // require byte-identical executables, then launch one. This is the literal
  // native acceptance path for declaration/member/expression/body synthesis.
  const std::filesystem::path native_root = workspace.root / "native-acceptance";
  draft::NativeBuildOptions first_native;
  first_native.build_directory = (native_root / "first-build").string();
  first_native.output_path = (native_root / "first-program").string();
  first_native.allow_unpinned_toolchain = true;
  const draft::NativeBuildResult first_built = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      offline,
      first_native,
      offline_diagnostics);
  EXPECT(state, first_built.ok);
  const std::string first_bytes = first_built.ok
      ? read_binary_file(first_built.output_path)
      : std::string();
  // Output path is part of native artifact identity. Rebuild exactly the same
  // provider-free path so the comparison does not conflate program identity
  // with a changed install name or linker output name.
  draft::NativeBuildOptions second_native = first_native;
  const draft::NativeBuildResult second_built = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      offline,
      second_native,
      offline_diagnostics);
  EXPECT(state, second_built.ok);
  if (first_built.ok && second_built.ok) {
    const std::string second_bytes = read_binary_file(second_built.output_path);
    EXPECT(state, !first_bytes.empty());
    EXPECT(state, first_bytes == second_bytes);
    int process_status = 0;
    EXPECT(state,
        run_executable(first_built.output_path, native_root, process_status));
    EXPECT(state, WIFEXITED(process_status));
    if (WIFEXITED(process_status)) {
      EXPECT(state, WEXITSTATUS(process_status) == 0);
    }
  }
#endif

  draft::SourceManager reuse_sources;
  draft::DiagnosticSink reuse_diagnostics;
  const draft::ResolveWorkspaceResult reused = draft::resolve_workspace(
      reuse_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      reuse_diagnostics);
  EXPECT(state, reused.ok);
  EXPECT(state, reused.synthesized_sites == 0);
  EXPECT(state, reused.reused_sites == 4);
  EXPECT(state, provider.calls == 4);
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

[[nodiscard]] bool contains_name(
    const std::vector<std::string> &names,
    std::string_view expected) {
  for (const std::string &name : names) {
    if (name == expected) return true;
  }
  return false;
}

void test_same_interface_set_is_opaque(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_opaque_interface_set_source();
  FakeProviderState provider;
  provider.opaque_interface_responses = true;

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
  EXPECT(state, resolved.synthesized_sites == 2);
  EXPECT(state, provider.calls == 2);
  EXPECT(state, provider.kinds.size() == 2);
  EXPECT(state, provider.visible_binding_names.size() == 2);
  if (provider.kinds.size() == 2) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisDeclaration);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }
  if (provider.visible_binding_names.size() == 2) {
    // The strongest useful assertion is symmetric: request order must not make
    // the first proposal visible to the second request, and source order must
    // not invent the second declaration for the first request.
    EXPECT(state,
        !contains_name(provider.visible_binding_names[0], "first"));
    EXPECT(state,
        !contains_name(provider.visible_binding_names[0], "second"));
    EXPECT(state,
        !contains_name(provider.visible_binding_names[1], "first"));
    EXPECT(state,
        !contains_name(provider.visible_binding_names[1], "second"));
  }

  // The opaque proposals are merged only after both requests return. Their
  // combined package must then pass the normal provider-free complete compile.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_tests_gate_manifest_commit(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("candidate with tests");
  workspace.write_test_source();
  workspace.write_benchmark_source();
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
  EXPECT(state, provider.last_validation_context.size() == 2);
  if (provider.last_validation_context.size() == 2) {
    const draft::AgentValidationContext &benchmark =
        provider.last_validation_context[0];
    const draft::AgentValidationContext &test =
        provider.last_validation_context[1];
    EXPECT(state, benchmark.kind == "benchmark");
    EXPECT(state,
        benchmark.source_relative_path == "candidate_bench.draft");
    EXPECT(state,
        benchmark.source.find("bench_generated_answer") !=
            std::string::npos);
    EXPECT(state, draft::sha256(benchmark.source) == benchmark.source_digest);
    EXPECT(state, test.kind == "test");
    EXPECT(state, test.source_relative_path == "candidate_test.draft");
    EXPECT(state,
        test.source.find("test_generated_answer") != std::string::npos);
    EXPECT(state,
        test.source.find("Validation comments") == std::string::npos);
    EXPECT(state, draft::sha256(test.source) == test.source_digest);
  }
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
  passing.validation_runner.state = &runner;
  passing.validation_runner.run = run_candidate_tests;
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
  EXPECT(state, accepted.benchmarked_procedures == 1);
  EXPECT(state, accepted.manifest.evidence.size() == 2);
  if (accepted.manifest.evidence.size() == 2) {
    EXPECT(state, accepted.manifest.evidence[0].kind == "test");
    EXPECT(state, accepted.manifest.evidence[0].root_identity == "workspace");
    EXPECT(state, accepted.manifest.evidence[0].root_relative_path == "app");
    EXPECT(state, accepted.manifest.evidence[0].key ==
        draft::sha256("fixture-test-evidence-key"));
    EXPECT(state, accepted.manifest.evidence[0].content_digest ==
        draft::sha256("fixture-passing-test-attempt"));
    EXPECT(state, accepted.manifest.evidence[1].kind == "benchmark");
    EXPECT(state, accepted.manifest.evidence[1].key ==
        draft::sha256("fixture-benchmark-evidence-key"));
    EXPECT(state, accepted.manifest.evidence[1].content_digest ==
        draft::sha256("fixture-passing-benchmark-attempt"));
  }
  EXPECT(state, runner.calls == 2);
  EXPECT(state, runner.test_calls == 1);
  EXPECT(state, runner.benchmark_calls == 1);
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
  failing.validation_runner.state = &runner;
  failing.validation_runner.run = run_candidate_tests;
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
  EXPECT(state, runner.calls == 3);
  EXPECT(state, runner.test_calls == 2);
  EXPECT(state, runner.benchmark_calls == 1);
  EXPECT(state, failing_diagnostics.has_errors());

  draft::DiagnosticSink after_diagnostics;
  const draft::ResolutionManifestLoadResult after =
      draft::load_resolution_manifest(workspace.root, after_diagnostics);
  EXPECT(state, after.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state,
      draft::serialize_resolution_manifest(after.manifest) == committed);
}

void test_validation_context_stales_synthesis(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("validation context freshness");
  workspace.write_test_source();
  FakeProviderState provider;
  FakeTestRunnerState runner;

  auto options = [&]() {
    draft::ResolveWorkspaceOptions value = resolve_options(workspace, provider);
    value.validation_runner.state = &runner;
    value.validation_runner.run = run_candidate_tests;
    return value;
  };

  draft::SourceManager first_sources;
  draft::DiagnosticSink first_diagnostics;
  const draft::ResolveWorkspaceResult first = draft::resolve_workspace(
      first_sources,
      workspace.package.string(),
      options(),
      first_diagnostics);
  if (!first.ok) {
    std::cerr << draft::render_diagnostics(first_sources, first_diagnostics);
  }
  EXPECT(state, first.ok);
  EXPECT(state, first.synthesized_sites == 1);
  EXPECT(state, provider.calls == 1);

  // The surface program and author prompt are unchanged. Altering a selected
  // test statement alone must change the obligation and force a new proposal;
  // otherwise a pin could survive after its authoritative acceptance context
  // changed.
  workspace.write_test_source("testing.expect(test, answer() >= 0)");
  draft::SourceManager changed_sources;
  draft::DiagnosticSink changed_diagnostics;
  const draft::ResolveWorkspaceResult changed = draft::resolve_workspace(
      changed_sources,
      workspace.package.string(),
      options(),
      changed_diagnostics);
  if (!changed.ok) {
    std::cerr << draft::render_diagnostics(
        changed_sources, changed_diagnostics);
  }
  EXPECT(state, changed.ok);
  EXPECT(state, changed.synthesized_sites == 1);
  EXPECT(state, changed.reused_sites == 0);
  EXPECT(state, provider.calls == 2);
}

} // namespace

int main() {
  TestState state;
  test_resolution_reuse_revalidation_and_failure(state);
  test_external_inputs_commit_without_synthesis(state);
  test_interface_sites_precede_dependent_bodies(state);
  test_dependency_interface_rounds(state);
  test_same_interface_set_is_opaque(state);
  test_tests_gate_manifest_commit(state);
  test_validation_context_stales_synthesis(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolver tests passed\n";
  return EXIT_SUCCESS;
}
