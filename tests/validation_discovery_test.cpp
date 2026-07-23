// Validation source selection, signature proof, ordering, and harness tests.
//
// Most cases in this file exercise target-independent validation semantics by
// compiling for the Draft target matching the current host. The checked-harness
// case additionally builds and executes the result on each implemented native
// host pair. Cross-host configurations retain semantic coverage without
// pretending they can execute an artifact for another OS or architecture.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "elaborator/resolved_program.h"
#include "elaborator/resolution_overlay.h"
#include "elaborator/resolution_store.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "validation/command.h"

#include "test_directory.h"
#include "validation/evidence.h"
#include "validation/runner.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "validation_discovery_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

// Select the implemented target belonging to the host OS and architecture.
// Validation source selection includes the target identity, so a Linux test
// must compile the matching target-qualified fixture rather than silently
// reusing a Mach-O or different-architecture source.
[[nodiscard]] draft::TargetProfile validation_target() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return draft::make_aarch64_macos_profile();
#elif defined(__linux__) && defined(__aarch64__)
  return draft::make_aarch64_linux_profile();
#elif defined(__linux__) && defined(__x86_64__)
  return draft::make_x86_64_linux_profile();
#else
#error "validation discovery tests require an implemented host target"
#endif
}

// Validation fixtures select the `app` package from a larger temporary
// workspace. The ordinary manifest therefore belongs to that package, not to
// the workspace directory package `.`.
[[nodiscard]] draft::ResolutionStoreKey app_store_key(
    const draft::ResolutionManifest &manifest) {
  return {manifest.target_identity, {"workspace", "app"}};
}

// Return whether artifacts for validation_target() are native to this test
// process. Only these hosts may enter the build-and-run portion of the test;
// cross-host builds need an explicit sysroot and cannot prove execution.
[[nodiscard]] constexpr bool can_execute_validation_target() {
#if (defined(__APPLE__) && defined(__aarch64__)) || \
    (defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__)))
  return true;
#else
  return false;
#endif
}

[[nodiscard]] bool write_file(
    const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  return output.good();
}

[[nodiscard]] draft::CompileWorkspaceResult compile_validation(
    const std::filesystem::path &root,
    draft::ValidationKind kind,
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics) {
  draft::CompileWorkspaceOptions options;
  options.target = validation_target();
  options.workspace.workspace_directory = root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-validation-test-v1";
  options.validation_kind = kind;
  options.lower_mir = true;
  options.emit_llvm = true;
  options.emit_native_output = true;
  return draft::compile_workspace_with_resolution(
      sources, (root / "app").string(), std::move(options), diagnostics);
}

void test_checked_test_harness(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-validation-discovery-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  EXPECT(state, write_file(
      root / "app" / "package.draft",
      "package app\n"
      "main :: proc() {\n"
      "    assert(false)\n"
      "}\n"));
  // The target-qualified suffix is part of validation selection. Procedure
  // order in this file is intentionally opposite the canonical name order.
  const draft::TargetProfile target = validation_target();
  const std::filesystem::path target_test_file =
      root / "app" / ("suite_test@" + target.facts.file_tag + ".draft");
  EXPECT(state, write_file(
      target_test_file,
      "package app\n"
      "import core/testing\n"
      "test_zeta :: proc(test: ^testing.Test) {\n"
      "    testing.expect(test, true)\n"
      "}\n"
      "test_alpha :: proc(test: ^testing.Test) {\n"
      "    testing.expect(test, false)\n"
      "}\n"));

  // An ordinary resolution manifest excludes command-only validation files.
  // Validation first authenticates that base graph, then derives a distinct
  // resolved identity that includes the selected suite and its extra imports.
  draft::SourceManager ordinary_sources;
  draft::DiagnosticSink ordinary_diagnostics;
  draft::CompileWorkspaceOptions ordinary_options;
  ordinary_options.target = target;
  ordinary_options.workspace.workspace_directory = root.string();
  ordinary_options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  ordinary_options.workspace.core_content_identity =
      "draft-core-validation-test-v1";
  const std::string compiler_identity =
      ordinary_options.compiler_content_identity;
  const draft::CompileWorkspaceResult ordinary = draft::compile_workspace(
      ordinary_sources,
      (root / "app").string(),
      std::move(ordinary_options),
      ordinary_diagnostics);
  EXPECT(state, ordinary.ok);
  draft::ResolutionManifest manifest;
  manifest.target_identity = target.facts.identity;
  manifest.root_package = {"workspace", "app"};
  manifest.resolved_program_digest = draft::hash_resolved_program(
      ordinary_sources,
      ordinary.graph,
      target,
      manifest,
      compiler_identity,
      ordinary.configuration);
  EXPECT(state, draft::commit_resolution(
      root, app_store_key(manifest), manifest, {}, ordinary_diagnostics));
  if (ordinary_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        ordinary_sources, ordinary_diagnostics);
  }

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult result = compile_validation(
      root, draft::ValidationKind::Test, sources, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.resolved_program_digest.has_value());
  if (result.resolved_program_digest.has_value()) {
    EXPECT(state, *result.resolved_program_digest !=
        manifest.resolved_program_digest);
  }
  EXPECT(state, result.validation_entries.size() == 2);
  if (result.validation_entries.size() == 2) {
    EXPECT(state, result.validation_entries[0].procedure == "test_alpha");
    EXPECT(state, result.validation_entries[1].procedure == "test_zeta");
    EXPECT(state, result.validation_entries[0].state_size == 24);
    EXPECT(state, result.validation_entries[0].state_alignment == 8);
    EXPECT(state, result.validation_entries[0].failure_offset == 8);
    EXPECT(state, result.validation_entries[0].report_size == 16);
  }
  if (result.graph.root_package.is_valid()) {
    const std::optional<draft::CompiledPackage> &root_package =
        result.packages[result.graph.root_package.value];
    EXPECT(state, root_package.has_value());
    if (root_package.has_value()) {
      const std::string &llvm = root_package->llvm_module.text;
      EXPECT(state, llvm.find("define i32 @main") != std::string::npos);
      EXPECT(state, llvm.find(
          "call void @\"draft.workspace.app.test_5Falpha\"") !=
          std::string::npos);
      EXPECT(state, llvm.find(
          "call void @\"draft.workspace.app.main\"") ==
          std::string::npos);
      EXPECT(state, llvm.find("alloca [24 x i8], align 8") !=
          std::string::npos);
      EXPECT(state, llvm.find(
          "getelementptr i8, ptr %validation.state.0, i64 8") !=
          std::string::npos);
      EXPECT(state, llvm.find(
          "@\"__draft.runtime.reset_temporary_allocator\"()") !=
          std::string::npos);
      EXPECT(state, llvm.find(
          "call i64 @__draft.host_write(i32 3, "
          "ptr %validation.state.0, i64 16)") !=
          std::string::npos);
    }
  }

  // Every implemented native host must prove the emitted binary and validation
  // report. Cross-target hosts stop after lowering because execution would
  // require emulation and no longer provide a native integration result.
  if (can_execute_validation_target()) {
    draft::NativeBuildOptions native_options;
    native_options.build_directory = (root / "native-build").string();
    native_options.output_path = (root / "validation-tests").string();
    draft::DiagnosticSink native_diagnostics;
    const draft::NativeBuildResult built = draft::build_native_executable(
        target, result, native_options, native_diagnostics);
    if (native_diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(sources, native_diagnostics);
    }
    EXPECT(state, built.ok);
    if (built.ok) {
      draft::ValidationRunOptions run_options;
      run_options.executable = built.output_path;
      run_options.working_directory = (root / "app").string();
      draft::DiagnosticSink run_diagnostics;
      const draft::ValidationRunResult run =
          draft::run_validation_executable(run_options, run_diagnostics);
      EXPECT(state, run.started);
      EXPECT(state, run.exited);
      EXPECT(state, run.exit_code == 1);
      EXPECT(state, run.report.size() == 32);
      std::vector<draft::ValidationObservation> observations;
      EXPECT(state, draft::decode_validation_report(
          run.report,
          result.validation_entries,
          observations,
          run_diagnostics));
      EXPECT(state, observations.size() == 2);
      if (observations.size() == 2) {
        EXPECT(state, observations[0].checks == 1);
        EXPECT(state, observations[0].failures == 1);
        EXPECT(state, observations[1].checks == 1);
        EXPECT(state, observations[1].failures == 0);
      }
      EXPECT(state, !run_diagnostics.has_errors());
    }
  }

  // Validation-only source participates in its resolved-program identity even
  // though an ordinary build would not select this file.
  EXPECT(state, write_file(
      target_test_file,
      "package app\n"
      "import core/testing\n"
      "test_zeta :: proc(test: ^testing.Test) {\n"
      "    testing.expect(test, true)\n"
      "}\n"
      "test_alpha :: proc(test: ^testing.Test) {\n"
      "    testing.expect(test, true)\n"
      "}\n"));
  draft::SourceManager changed_sources;
  draft::DiagnosticSink changed_diagnostics;
  const draft::CompileWorkspaceResult changed = compile_validation(
      root,
      draft::ValidationKind::Test,
      changed_sources,
      changed_diagnostics);
  EXPECT(state, changed.ok);
  EXPECT(state, changed.resolved_program_digest.has_value());
  if (result.resolved_program_digest.has_value() &&
      changed.resolved_program_digest.has_value()) {
    EXPECT(state, *result.resolved_program_digest !=
        *changed.resolved_program_digest);
  }

  if (can_execute_validation_target()) {
    draft::ValidationCommandOptions command_options;
    command_options.package_directory = root / "app";
    command_options.target = target;
    command_options.workspace.workspace_directory = root.string();
    command_options.workspace.core_directory =
        std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
    command_options.workspace.core_content_identity =
        "draft-core-validation-test-v1";
    command_options.kind = draft::ValidationKind::Test;
    draft::SourceManager command_sources;
    draft::DiagnosticSink command_diagnostics;
    const draft::ValidationCommandResult command =
        draft::execute_validation_command(
            command_sources, std::move(command_options), command_diagnostics);
    if (command_diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(
          command_sources, command_diagnostics);
    }
    EXPECT(state, command.completed);
    EXPECT(state, command.passed);
    EXPECT(state, command.selected_procedures == 2);
    EXPECT(state, command.attempt == 1);
    EXPECT(state, std::filesystem::exists(
        root / ".draft" / "evidence" /
        (command.evidence_digest.hex() + ".json")));
  }

  std::filesystem::remove_all(root, error);
}

void test_authenticated_synthesis_enters_validation_graph(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-validation-synthesis-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  EXPECT(state, write_file(
      root / "app" / "package.draft",
      "package app\n"
      "... \"declare answer\"\n"
      "main :: proc() -> int {\n"
      "    return 0\n"
      "}\n"));
  EXPECT(state, write_file(
      root / "app" / "generated_test.draft",
      "package app\n"
      "import core/testing\n"
      "test_generated :: proc(test: ^testing.Test) {\n"
      "    testing.expect(test, answer == 42)\n"
      "}\n"));

  // Construct the same declaration-stage transaction as the resolver. The
  // manifest input includes the validation file as syntax context, while the
  // expansion becomes ordinary package source before that file is type-checked.
  draft::CompileWorkspaceOptions surface_options;
  surface_options.target = validation_target();
  surface_options.workspace.workspace_directory = root.string();
  surface_options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  surface_options.workspace.core_content_identity =
      "draft-core-validation-test-v1";
  surface_options.stage =
      draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  draft::SourceManager surface_sources;
  draft::DiagnosticSink surface_diagnostics;
  const draft::CompileWorkspaceResult surface = draft::compile_workspace(
      surface_sources,
      (root / "app").string(),
      surface_options,
      surface_diagnostics);
  if (!surface.ok || surface.packages.size() != 1 ||
      !surface.packages[0].has_value() ||
      surface.packages[0]->obligations.obligations.size() != 1) {
    std::cerr << draft::render_diagnostics(
        surface_sources, surface_diagnostics);
    EXPECT(state, false);
    std::filesystem::remove_all(root, error);
    return;
  }

  const draft::AgentObligation &obligation =
      surface.packages[0]->obligations.obligations[0];
  draft::GeneratedExpansion expansion;
  expansion.source = "answer :: cast[i64](42);";
  expansion.digest = draft::sha256(expansion.source);
  draft::ResolutionPin pin;
  pin.site_identity = obligation.site_identity;
  pin.kind = obligation.kind;
  pin.input_digest = obligation.input_digest;
  pin.expansion_digest = expansion.digest;
  pin.provider_identity = "validation-test-provider";
  pin.model_identity = "fixture-v1";
  pin.configuration_identity = "fixture-config-v1";
  for (const draft::LoadedPackageFile &file :
       surface.graph.packages[0].loaded.files) {
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
        static_cast<std::uint64_t>(expansion.source.size()),
    };
  }
  draft::ResolutionManifest manifest;
  manifest.target_identity = surface_options.target.facts.identity;
  manifest.root_package = {"workspace", "app"};
  manifest.pins.push_back(pin);
  const draft::ResolutionSurfacePackage surface_package{
      &surface.packages[0]->identity,
      &surface.graph.packages[0].loaded,
      &surface.packages[0]->obligations,
  };
  const draft::ResolutionOverlayResult overlay =
      draft::build_resolution_overlays(
          surface_sources,
          std::span<const draft::ResolutionSurfacePackage>(
              &surface_package, 1),
          manifest,
          surface_options.target.facts.identity,
          root,
          draft::ResolutionInputVerification::RequireCurrentInput,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          surface_diagnostics);
  EXPECT(state, overlay.ok);
  if (!overlay.ok) {
    std::cerr << draft::render_diagnostics(
        surface_sources, surface_diagnostics);
    std::filesystem::remove_all(root, error);
    return;
  }

  draft::CompileWorkspaceOptions resolved_options = surface_options;
  resolved_options.stage = draft::CompileWorkspaceStage::Complete;
  resolved_options.workspace.source_overrides = overlay.sources;
  draft::SourceManager resolved_sources;
  draft::DiagnosticSink resolved_diagnostics;
  const draft::CompileWorkspaceResult resolved = draft::compile_workspace(
      resolved_sources,
      (root / "app").string(),
      resolved_options,
      resolved_diagnostics);
  EXPECT(state, resolved.ok);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(
        resolved_sources, resolved_diagnostics);
    std::filesystem::remove_all(root, error);
    return;
  }
  manifest.resolved_program_digest = draft::hash_resolved_program(
      resolved_sources,
      resolved.graph,
      resolved_options.target,
      manifest,
      resolved_options.compiler_content_identity,
      resolved_options.configuration);
  EXPECT(state, draft::commit_resolution(
      root,
      app_store_key(manifest),
      manifest,
      std::span<const draft::GeneratedExpansion>(&expansion, 1),
      resolved_diagnostics));

  // Validation first authenticates the ordinary input digest above. Its
  // command-only graph may then install that exact pin even though the derived
  // obligation has validation-mode context, and the generated declaration is
  // checked normally when the test body resolves `answer`.
  draft::SourceManager validation_sources;
  draft::DiagnosticSink validation_diagnostics;
  const draft::CompileWorkspaceResult validation = compile_validation(
      root,
      draft::ValidationKind::Test,
      validation_sources,
      validation_diagnostics);
  if (!validation.ok) {
    std::cerr << draft::render_diagnostics(
        validation_sources, validation_diagnostics);
  }
  EXPECT(state, validation.ok);
  EXPECT(state, !validation_diagnostics.has_errors());
  EXPECT(state, validation.validation_entries.size() == 1);

  std::filesystem::remove_all(root, error);
}

void test_invalid_signature_is_rejected(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-validation-signature-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  EXPECT(state, write_file(root / "app" / "package.draft", "package app\n"));
  EXPECT(state, write_file(
      root / "app" / "bad_test.draft",
      "package app\n"
      "import core/testing\n"
      "test_bad :: proc(test: ^testing.Test) -> int {\n"
      "    return 0\n"
      "}\n"));

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult result = compile_validation(
      root, draft::ValidationKind::Test, sources, diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("must return void") != std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_spoofed_core_nominal_is_rejected(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-validation-spoof-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "testing", error);
  EXPECT(state, !error);
  if (error) return;

  EXPECT(state, write_file(
      root / "testing" / "package.draft",
      "package testing\n"
      "pub Test :: struct {\n"
      "    checks: usize,\n"
      "    failures: usize,\n"
      "    user: rawptr,\n"
      "}\n"));
  EXPECT(state, write_file(root / "app" / "package.draft", "package app\n"));
  EXPECT(state, write_file(
      root / "app" / "spoof_test.draft",
      "package app\n"
      "import testing\n"
      "test_spoof :: proc(test: ^testing.Test) {}\n"));

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult result = compile_validation(
      root, draft::ValidationKind::Test, sources, diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  if (rendered.find("must accept ^testing.Test") == std::string::npos) {
    std::cerr << rendered;
  }
  EXPECT(state, rendered.find("must accept ^testing.Test") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  TestState state;
  test_checked_test_harness(state);
  test_authenticated_synthesis_enters_validation_graph(state);
  test_invalid_signature_is_rejected(state);
  test_spoofed_core_nominal_is_rejected(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
