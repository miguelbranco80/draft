// Bootstrap compiler command-line entry point and workspace target selection.
//
// The driver owns process-facing argument parsing, canonical workspace paths,
// explicit or discovered root selection, output naming, signal cancellation,
// timing report streams, and calls into reusable compiler/native/validation
// operations. It owns no source graph or semantic state beyond one synchronous
// command. Source loading, diagnostics, language meaning, generated-source
// transactions, evidence, and native mechanics stay behind their subsystem
// interfaces.
//
// Every package command receives one explicit workspace. Single-root commands
// default to package `.`, while aggregate build discovers surface package-level
// `main` declarations or accepts repeated `--root` selectors. Physical paths
// are used only for I/O; persistent manifests and derived outputs are keyed by
// canonical workspace PackageIdentity plus target. Command ordering and output
// paths must therefore remain independent of filesystem enumeration and host
// path spelling. See specification sections 3, 6 (Program entry), and 10, and
// docs/operations/command-reference.md.

#include "backend/toolchain.h"
#include "backend/foreign_summaries.h"
#include "backend/runtime_assets.h"
#include "base/timing.h"
#include "compile/compiler.h"
#include "compile/expanded_source.h"
#include "compile/resolver.h"
#include "elaborator/codex_cli.h"
#include "elaborator/resolution_store.h"
#include "interop/c_header.h"
#include "judgment/cli_policy.h"
#include "judgment/codex_cli.h"
#include "judgment/command.h"
#include "judgment/selection.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/syntax_tree.h"
#include "syntax/token.h"
#include "target/profile.h"
#include "validation/command.h"
#include "workspace/package.h"
#include "workspace/selection.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t cancellation_signal = 0;

using draft::JudgmentArtifactPath;
using draft::NamedCodexJudgmentValidator;
using draft::configure_codex_judgment_policy;
using draft::parse_judgment_artifact_path;
using draft::parse_judgment_validator;
using draft::read_judgment_artifacts;

// The driver owns report I/O while the base recorder owns only diagnostic
// data. Destruction runs on every ordinary return from main, so an enabled
// command reports the work completed before a usage or compilation error as
// well as successful work. The recorder root remains open until render() and
// therefore measures complete command handling after option pre-scan.
struct CommandTimingReport {
  explicit CommandTimingReport(draft::TimingOutput output) : recorder(output) {}

  ~CommandTimingReport() {
    if (recorder.enabled()) std::cerr << recorder.render();
  }

  draft::TimingRecorder recorder;
};

[[nodiscard]] bool is_timing_argument(std::string_view argument) {
  return argument == "--timings" || argument == "--timings=all";
}

// Timing is a common package-command option, but command-specific parsing is
// intentionally kept local below. This pre-scan selects the recorder before
// any meaningful work and rejects duplicate/conflicting requests once. Each
// command parser then only consumes the already-validated spelling.
[[nodiscard]] bool select_timing_output(
    int argc,
    char **argv,
    draft::TimingOutput &output) {
  output = draft::TimingOutput::Disabled;
  for (int index = 3; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (!is_timing_argument(argument)) continue;
    if (output != draft::TimingOutput::Disabled) {
      std::cerr << "error: --timings may be specified only once\n";
      return false;
    }
    output = argument == "--timings=all"
        ? draft::TimingOutput::All
        : draft::TimingOutput::Summary;
  }
  return true;
}

void request_cancellation(int signal) {
  (void)signal;
  cancellation_signal = 1;
}

[[nodiscard]] bool command_cancellation_requested(void *state) {
  (void)state;
  return cancellation_signal != 0;
}

// The bootstrap binary is built together with its core source tree.  Installed
// distributions will replace this build-time path with their versioned resource
// locator, but both forms feed the same explicit WorkspaceLoadOptions boundary.
void configure_core_distribution(draft::WorkspaceLoadOptions &options) {
  options.core_directory = DRAFT_CORE_DIRECTORY;
  options.core_content_identity = DRAFT_CORE_CONTENT_IDENTITY;
}

// Package commands take the workspace root explicitly. It is also the security
// boundary for the no-follow resolution and build stores, so it must not retain
// a symlink spelling such as macOS `/tmp`. Root-package selectors are resolved
// separately against this canonical directory and remain stable workspace
// identities rather than physical paths.
[[nodiscard]] bool canonical_workspace_directory(
    const std::string &spelling,
    std::filesystem::path &directory,
    std::string &reason) {
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(spelling, error);
  if (error) {
    reason = "cannot make workspace path absolute: " + error.message();
    return false;
  }
  directory = std::filesystem::canonical(absolute, error);
  if (error) {
    reason = "cannot canonicalize workspace path: " + error.message();
    return false;
  }
  if (!std::filesystem::is_directory(directory, error)) {
    reason = error
        ? "cannot inspect workspace path: " + error.message()
        : "workspace path is not a directory";
    return false;
  }
  return true;
}

// Resolves the explicit command root after the workspace boundary is known.
// The returned physical directory is used only for source I/O; all persistent
// store selection uses the paired canonical PackageIdentity.
[[nodiscard]] bool select_command_package(
    const std::string &workspace_spelling,
    std::string_view root_selector,
    std::filesystem::path &workspace_directory,
    draft::WorkspacePackageSelection &package,
    draft::DiagnosticSink &diagnostics) {
  std::string reason;
  if (!canonical_workspace_directory(
          workspace_spelling, workspace_directory, reason)) {
    diagnostics.error(draft::SourceRange::invalid(), std::move(reason));
    return false;
  }
  return draft::select_workspace_package(
      workspace_directory, root_selector, package, diagnostics);
}

[[nodiscard]] draft::ResolutionStoreKey resolution_store_key(
    const draft::TargetProfile &target,
    const draft::WorkspacePackageSelection &package) {
  return {target.facts.identity, package.identity};
}

// Resolves one user-facing target selector at the process boundary.  All
// package commands call this helper before constructing compiler options so a
// command cannot accidentally compile semantics for one profile and emit or
// validate native code for another.  Invalid selectors are usage errors, not
// source diagnostics: no Draft source has been loaded when this decision is
// made.
[[nodiscard]] bool select_command_target(
    std::string_view selector,
    draft::TargetProfile &target) {
  std::string reason;
  if (!draft::select_builtin_target_profile(selector, target, reason)) {
    std::cerr << "error: " << reason << '\n';
    return false;
  }
  if (!draft::validate_target_profile(target, reason)) {
    std::cerr << "error: invalid built-in target profile: " << reason << '\n';
    return false;
  }
  return true;
}

// Converts the closed public artifact vocabulary once for both `build` and
// `resolve --build`. Keeping the parser shared prevents the continuation path
// from acquiring subtly different output semantics from an ordinary build.
[[nodiscard]] std::optional<draft::NativeArtifactKind>
parse_native_artifact_kind(std::string_view spelling) {
  if (spelling == "executable") {
    return draft::NativeArtifactKind::Executable;
  }
  if (spelling == "object") return draft::NativeArtifactKind::Object;
  if (spelling == "static-library") {
    return draft::NativeArtifactKind::StaticLibrary;
  }
  if (spelling == "dynamic-library") {
    return draft::NativeArtifactKind::DynamicLibrary;
  }
  if (spelling == "assembly") return draft::NativeArtifactKind::Assembly;
  return std::nullopt;
}

[[nodiscard]] bool parse_foreign_provider(
    std::string_view spelling,
    draft::ForeignProviderInput &input,
    std::string &reason) {
  const std::size_t equal = spelling.find('=');
  const std::size_t colon = equal == std::string_view::npos
      ? std::string_view::npos
      : spelling.find(':', equal + 1);
  if (equal == 0 || equal == std::string_view::npos ||
      colon == equal + 1 || colon == std::string_view::npos ||
      colon + 1 >= spelling.size()) {
    reason = "provider mapping must be name=object|archive|shared-library:path";
    return false;
  }
  input.provider = std::string(spelling.substr(0, equal));
  const std::string_view kind = spelling.substr(equal + 1, colon - equal - 1);
  if (kind == "object") {
    input.kind = draft::ForeignArtifactKind::Object;
  } else if (kind == "archive") {
    input.kind = draft::ForeignArtifactKind::Archive;
  } else if (kind == "shared-library") {
    input.kind = draft::ForeignArtifactKind::SharedLibrary;
  } else {
    reason = "provider artifact kind must be object, archive, or shared-library";
    return false;
  }
  std::error_code error;
  input.path = std::filesystem::absolute(
      std::filesystem::path(spelling.substr(colon + 1)), error).lexically_normal();
  if (error) {
    reason = "cannot make provider artifact path absolute: " + error.message();
    return false;
  }
  return true;
}

[[nodiscard]] bool parse_foreign_provider_summary(
    std::string_view spelling,
    draft::ForeignProviderSummaryInput &input,
    std::string &reason) {
  const std::size_t colon = spelling.find(':');
  if (colon == 0 || colon == std::string_view::npos ||
      colon + 1 >= spelling.size()) {
    reason = "provider summary mapping must be name:path";
    return false;
  }
  input.provider = std::string(spelling.substr(0, colon));
  std::error_code error;
  input.path = std::filesystem::absolute(
      std::filesystem::path(spelling.substr(colon + 1)), error).lexically_normal();
  if (error) {
    reason = "cannot make provider summary path absolute: " + error.message();
    return false;
  }
  return true;
}

// Runtime assets use a logical name independent of their relocatable physical
// root. The backend performs shape, duplicate, and content checks; the driver
// owns only CLI spelling and conversion to an absolute path.
[[nodiscard]] bool parse_runtime_asset(
    std::string_view spelling,
    draft::RuntimeAssetInput &input,
    std::string &reason) {
  const std::size_t colon = spelling.find(':');
  if (colon == 0 || colon == std::string_view::npos ||
      colon + 1 >= spelling.size()) {
    reason = "runtime asset mapping must be name:path";
    return false;
  }
  input.name = std::string(spelling.substr(0, colon));
  std::error_code error;
  input.path = std::filesystem::absolute(
      std::filesystem::path(spelling.substr(colon + 1)), error).lexically_normal();
  if (error) {
    reason = "cannot make runtime asset path absolute: " + error.message();
    return false;
  }
  return true;
}

// Semantic compilation must consume the exact summary set selected by an
// existing manifest. A development build without a manifest still validates
// summary-to-artifact binding, but has no persistent pin set to compare.
[[nodiscard]] bool load_foreign_provider_audits(
    const std::string &workspace_directory,
    const draft::ResolutionStoreKey &store_key,
    const std::vector<draft::ForeignProviderSummaryInput> &summary_inputs,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    std::vector<draft::ForeignProviderAudit> &audits,
    draft::DiagnosticSink &diagnostics) {
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(
          workspace_directory, store_key, diagnostics);
  if (loaded.state == draft::ResolutionManifestLoadState::Invalid) return false;
  if (loaded.state == draft::ResolutionManifestLoadState::Loaded) {
    return draft::verify_foreign_provider_summary_inputs(
        summary_inputs,
        foreign_providers,
        loaded.manifest.external_inputs,
        audits,
        diagnostics);
  }
  std::vector<draft::ExternalInputPin> ignored;
  return draft::pin_foreign_provider_summary_inputs(
      summary_inputs, foreign_providers, ignored, audits, diagnostics);
}

[[nodiscard]] std::string escaped(std::string_view text) {
  std::string result;
  for (const char byte : text) {
    switch (byte) {
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    case '\\': result += "\\\\"; break;
    default:
      // Token dumps must not change with the process locale. ASCII graphic and
      // space bytes are printed directly; all other bytes use a fixed marker.
      if (byte >= 0x20 && byte <= 0x7e) {
        result += byte;
      } else {
        result += '?';
      }
      break;
    }
  }
  return result;
}

int lex_file(const std::string &path) {
  draft::SourceManager sources;
  const draft::LoadFileResult load = sources.load_file(path);
  if (!load.ok) {
    std::cerr << "error: " << load.error << '\n';
    return 1;
  }

  draft::DiagnosticSink diagnostics;
  const std::vector<draft::Token> tokens = draft::lex_source(sources, load.file, diagnostics);
  for (const draft::Token &token : tokens) {
    const draft::LineColumn position = sources.line_column(token.range.begin);
    std::cout << position.line << ':' << position.column << ' '
              << draft::token_kind_name(token.kind);
    if (token.inserted) {
      std::cout << " [inserted]";
    } else if (token.kind != draft::TokenKind::EndOfFile) {
      std::cout << " \"" << escaped(sources.text(token.range)) << '"';
    }
    std::cout << '\n';
  }

  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

int parse_file(const std::string &path) {
  draft::SourceManager sources;
  const draft::LoadFileResult load = sources.load_file(path);
  if (!load.ok) {
    std::cerr << "error: " << load.error << '\n';
    return 1;
  }

  draft::DiagnosticSink diagnostics;
  const draft::SyntaxTree tree = draft::parse_source_file(sources, load.file, diagnostics);
  std::cout << draft::dump_syntax_tree(tree);
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

// Prints the profile in a stable line-oriented form suitable for inspection and
// simple snapshot tests. This is not the eventual canonical serialized build
// input, whose format will be versioned independently.
int print_target(const draft::TargetProfile &profile) {
  std::string reason;
  if (!draft::validate_target_profile(profile, reason)) {
    std::cerr << "error: invalid built-in target profile: " << reason << '\n';
    return 1;
  }
  std::cout << "identity " << profile.facts.identity << '\n'
            << "triple " << profile.llvm_triple << '\n'
            << "data-layout " << profile.llvm_data_layout << '\n'
            << "file-tag " << profile.facts.file_tag << '\n'
            << "pointer-bits " << profile.facts.pointer_bits << '\n'
            << "page-size " << profile.facts.page_size << '\n'
            << "object-format " << profile.facts.object_format << '\n'
            << "simd-shapes";
  for (const draft::TargetSimdShape &shape : profile.facts.simd_shapes) {
    std::cout << ' ' << shape.element << 'x' << shape.lanes;
  }
  std::cout << '\n'
            << "assembly-dialect " << profile.parsed_assembly_dialect << '\n'
            << "assembly-instructions";
  for (const std::string &instruction : profile.parsed_assembly_instructions) {
    std::cout << ' ' << instruction;
  }
  std::cout << '\n' << "system-link-library " << profile.system_link_library
            << '\n' << "system-link-providers";
  for (const std::string &provider : profile.system_link_providers) {
    std::cout << ' ' << provider;
  }
  std::cout << '\n' << "system-foreign-summaries";
  for (const draft::SystemForeignSummary &summary :
       profile.system_foreign_summaries) {
    std::cout << ' ' << summary.provider << ':' << summary.linker_name;
    for (std::uint32_t parameter : summary.callback_parameters) {
      std::cout << "@callback-" << parameter;
    }
  }
  std::cout << '\n'
            << "relocation-model "
            << draft::relocation_model_name(profile.relocation_model) << '\n'
            << "code-model " << draft::code_model_name(profile.code_model) << '\n'
            << "tls-model " << draft::tls_model_name(profile.tls_model) << '\n';
  return 0;
}

// Runs one dependency-ordered provider-free pipeline for an explicit package
// inside the requested workspace. `check` stops after typed HIR/interfaces;
// `emit-llvm` additionally lowers MIR and prints each package module without
// invoking LLVM or a linker.
int compile_package(
    const std::string &workspace_spelling,
    std::string_view root_selector,
    bool emit_llvm,
    const draft::TargetProfile &target,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(
          workspace_spelling,
          root_selector,
          workspace_directory,
          selected_package,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(options.workspace);
  options.lower_mir = emit_llvm;
  options.emit_llvm = emit_llvm;
  options.timings = timings;
  draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          selected_package.physical_directory.string(),
          std::move(options),
          diagnostics);
  std::size_t symbol_count = 0;
  std::size_t type_count = 0;
  std::size_t procedure_count = 0;
  std::size_t agent_record_count = 0;
  if (compiled.ok) {
    for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
      const draft::CompiledPackage &package = *compiled.packages[index];
      symbol_count += package.semantics.package.symbols.symbol_count();
      type_count += package.semantics.package.types.size();
      procedure_count += package.bodies.checked_procedures;
      agent_record_count += package.metadata.records.size();
      if (emit_llvm) {
        std::cout << "; ----- package "
                  << draft::display_package_identity(package.identity)
                  << " -----\n"
                  << package.llvm.text;
      }
    }
    if (!emit_llvm) {
      const draft::WorkspacePackage &root =
          compiled.graph.package(compiled.graph.root_package);
      std::cout << "checked package graph rooted at " << root.loaded.short_name << ": "
                << compiled.graph.packages.size() << " packages, "
                << symbol_count << " symbols, "
                << type_count << " types, "
                << procedure_count << " procedure bodies, "
                << agent_record_count << " agent records\n";
    }
  }

  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

// Materializes the exact final SourceManager buffers selected by one complete
// provider-free compilation. The command authenticates the same external
// program inputs as a native build before publishing the projection, but it
// never invokes a linker, provider, validation command, or resolution-store
// mutation. The output directory must be absent so old files cannot survive a
// later graph and masquerade as part of the current expanded program.
int expand_package(
    const std::string &workspace_spelling,
    std::string_view root_selector,
    const std::filesystem::path &output_directory,
    const draft::TargetProfile &target,
    draft::RuntimeAssertionMode runtime_assertions,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(
          workspace_spelling,
          root_selector,
          workspace_directory,
          selected_package,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.configuration.runtime_assertions = runtime_assertions;
  options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(options.workspace);
  options.timings = timings;
  if (!load_foreign_provider_audits(
          options.workspace.workspace_directory,
          resolution_store_key(target, selected_package),
          provider_summaries,
          foreign_providers,
          options.foreign_provider_audits,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          selected_package.physical_directory.string(),
          std::move(options),
          diagnostics);
  bool authenticated_inputs = compiled.ok;
  if (compiled.ok && compiled.resolution_manifest.has_value()) {
    // Source projection does not consume native artifacts or runtime assets,
    // but their content hashes are part of this manifest's resolved-program
    // identity. Re-authenticate the complete sets before labeling the output
    // with that identity instead of trusting unrelated current filesystem
    // bytes that happen to share the same logical names.
    std::vector<draft::VerifiedForeignProviderInput> verified_providers;
    std::vector<draft::VerifiedRuntimeAssetInput> verified_assets;
    authenticated_inputs = draft::verify_foreign_provider_inputs(
        foreign_providers,
        compiled.resolution_manifest->external_inputs,
        verified_providers,
        diagnostics);
    if (authenticated_inputs) {
      authenticated_inputs = draft::verify_runtime_asset_inputs(
          runtime_assets,
          compiled.resolution_manifest->external_inputs,
          verified_assets,
          diagnostics);
    }
  } else if (compiled.ok &&
             (!foreign_providers.empty() || !provider_summaries.empty() ||
              !runtime_assets.empty())) {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "expanded source cannot attach external inputs to a handwritten "
        "program without a resolution manifest");
    authenticated_inputs = false;
  }

  bool materialized = false;
  if (authenticated_inputs && !diagnostics.has_errors()) {
    const draft::ExpandedSourceProjectionResult projected =
        draft::materialize_expanded_source(
            sources, compiled, output_directory, diagnostics);
    if (projected.ok) {
      materialized = true;
      std::cout << "expanded " << projected.source_files
                << " source files with " << projected.mapped_expansions
                << " generated mappings to "
                << projected.output_directory.string() << '\n';
    }
  }
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return materialized && !diagnostics.has_errors() ? 0 : 1;
}

// Emits one native artifact from an already lowered graph. Both ordinary build
// and resolve-build use this exact function; only the owner of `compiled`
// differs. The helper selects process-facing output paths and invokes the native
// adapter, but never reloads Draft source or a resolution manifest.
[[nodiscard]] bool emit_native_package(
    const std::filesystem::path &workspace_directory,
    const draft::WorkspacePackageSelection &selected_package,
    const draft::TargetProfile &target,
    const std::optional<std::string> &requested_output,
    draft::NativeArtifactKind artifact_kind,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings,
    const draft::CompileWorkspaceResult &compiled,
    draft::DiagnosticSink &diagnostics) {
  if (!compiled.ok ||
      compiled.progress != draft::CompileWorkspaceProgress::TargetLowering) {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "native emission requires the final lowered compiler graph");
    return false;
  }

  // Artifact namespaces mirror the package's workspace-relative folder path.
  // Fixed `workspace` and `packages` rows keep root `.` distinct from a child
  // literally named `workspace`; the target file tag prevents cross-target
  // scratch and outputs from replacing one another.
  std::filesystem::path artifact_directory = workspace_directory / ".draft" /
      "build" / target.facts.file_tag;
  if (selected_package.identity.root_relative_path == ".") {
    artifact_directory /= "workspace";
  } else {
    artifact_directory /= "packages";
    artifact_directory /= selected_package.identity.root_relative_path;
  }
  const std::filesystem::path build_directory =
      artifact_directory / ".native";
  const std::string package_name = compiled.graph
      .package(compiled.graph.root_package).loaded.short_name;
  std::filesystem::path output;
  switch (artifact_kind) {
  case draft::NativeArtifactKind::Executable:
    output = artifact_directory / package_name;
    break;
  case draft::NativeArtifactKind::Object:
    output = artifact_directory / (package_name + ".o");
    break;
  case draft::NativeArtifactKind::StaticLibrary:
    output = artifact_directory / ("lib" + package_name + ".a");
    break;
  case draft::NativeArtifactKind::DynamicLibrary:
    output = artifact_directory /
        ("lib" + package_name +
         (target.facts.object_format == "elf" ? ".so" : ".dylib"));
    break;
  case draft::NativeArtifactKind::Assembly:
    output = artifact_directory / (package_name + "-assembly");
    break;
  }
  if (requested_output.has_value()) {
    std::error_code path_error;
    output = std::filesystem::absolute(*requested_output, path_error);
    if (path_error) {
      diagnostics.error(
          draft::SourceRange::invalid(),
          "cannot make native output path absolute: " +
              path_error.message());
      return false;
    }
  }

  draft::NativeBuildOptions native_options;
  native_options.build_directory = build_directory.string();
  native_options.output_path = output.string();
  native_options.artifact_kind = artifact_kind;
  native_options.foreign_providers = foreign_providers;
  native_options.runtime_assets = runtime_assets;
  native_options.timings = timings;
  const draft::NativeBuildResult built = draft::build_native_artifact(
      target, compiled, native_options, diagnostics);
  if (!built.ok) return false;
  std::cout << "built " << built.output_path << '\n';
  if (!built.debug_symbols_path.empty()) {
    std::cout << "debug symbols " << built.debug_symbols_path << '\n';
  }
  return true;
}

// Builds one already-selected root. Aggregate workspace builds call this in
// canonical root-path order and stop at the first failure; an artifact already
// completed for an earlier root remains a valid independently namespaced
// result, while the aggregate command still returns failure.
int build_selected_package(
    const std::filesystem::path &workspace_directory,
    const draft::WorkspacePackageSelection &selected_package,
    const draft::TargetProfile &target,
    const std::optional<std::string> &requested_output,
    draft::NativeArtifactKind artifact_kind,
    draft::RuntimeAssertionMode runtime_assertions,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
  compile_options.configuration.runtime_assertions = runtime_assertions;
  compile_options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(compile_options.workspace);
  if (!load_foreign_provider_audits(
          compile_options.workspace.workspace_directory,
          resolution_store_key(target, selected_package),
          provider_summaries,
          foreign_providers,
          compile_options.foreign_provider_audits,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  compile_options.lower_mir = true;
  compile_options.emit_llvm = true;
  compile_options.emit_program_entry =
      artifact_kind == draft::NativeArtifactKind::Executable;
  compile_options.timings = timings;
  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          selected_package.physical_directory.string(),
          std::move(compile_options),
          diagnostics);
  const bool built = compiled.ok && emit_native_package(
      workspace_directory,
      selected_package,
      target,
      requested_output,
      artifact_kind,
      foreign_providers,
      runtime_assets,
      timings,
      compiled,
      diagnostics);
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return built && !diagnostics.has_errors() ? 0 : 1;
}

// Selects the complete executable target set for `build <workspace>`. Without
// `--root`, discovery returns every ordinary package-level `main`; repeated
// selectors replace discovery with an explicit subset and may therefore name a
// library root for non-executable artifact kinds. Selection is complete before
// output validation, so `-o` can be rejected deterministically for a multi-root
// build without compiling or writing an artifact.
int build_workspace(
    const std::string &workspace_spelling,
    const std::vector<std::string> &root_selectors,
    const draft::TargetProfile &target,
    const std::optional<std::string> &requested_output,
    draft::NativeArtifactKind artifact_kind,
    draft::RuntimeAssertionMode runtime_assertions,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings) {
  std::filesystem::path workspace_directory;
  std::string path_error;
  if (!canonical_workspace_directory(
          workspace_spelling, workspace_directory, path_error)) {
    std::cerr << "error: " << path_error << '\n';
    return 1;
  }

  draft::SourceManager discovery_sources;
  draft::DiagnosticSink discovery_diagnostics;
  std::vector<draft::WorkspacePackageSelection> roots;
  if (root_selectors.empty()) {
    draft::WorkspaceLoadOptions discovery_options;
    discovery_options.workspace_directory = workspace_directory.string();
    configure_core_distribution(discovery_options);
    discovery_options.package_options.file_tag = target.facts.file_tag;
    const draft::ExecutableRootDiscoveryResult discovered =
        draft::discover_executable_roots(
            discovery_sources, discovery_options, discovery_diagnostics);
    if (discovered.ok) roots = discovered.roots;
  } else {
    roots.reserve(root_selectors.size());
    for (const std::string &selector : root_selectors) {
      draft::WorkspacePackageSelection root;
      if (!draft::select_workspace_package(
              workspace_directory,
              selector,
              root,
              discovery_diagnostics)) {
        break;
      }
      roots.push_back(std::move(root));
    }
    std::sort(
        roots.begin(),
        roots.end(),
        [](const draft::WorkspacePackageSelection &left,
           const draft::WorkspacePackageSelection &right) {
          return left.identity.root_relative_path <
              right.identity.root_relative_path;
        });
    for (std::size_t index = 1; index < roots.size(); ++index) {
      if (roots[index - 1].identity == roots[index].identity) {
        discovery_diagnostics.error(
            draft::SourceRange::invalid(),
            "duplicate build root selector '" +
                roots[index].identity.root_relative_path + "'");
        break;
      }
    }
  }

  if (!discovery_diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(
        discovery_sources, discovery_diagnostics);
  }
  if (discovery_diagnostics.has_errors()) return 1;
  if (roots.empty()) {
    std::cerr << "error: workspace contains no executable packages\n";
    return 1;
  }
  if (requested_output.has_value() && roots.size() != 1) {
    std::cerr << "error: -o requires exactly one selected build root\n";
    return 2;
  }

  for (const draft::WorkspacePackageSelection &root : roots) {
    const int result = build_selected_package(
        workspace_directory,
        root,
        target,
        requested_output,
        artifact_kind,
        runtime_assertions,
        foreign_providers,
        provider_summaries,
        runtime_assets,
        timings);
    if (result != 0) return result;
  }
  return 0;
}

int validate_package(
    const std::string &workspace_spelling,
    std::string_view root_selector,
    const draft::TargetProfile &target,
    draft::ValidationKind kind,
    const std::vector<draft::ValidationInstrumentationKind> &instrumentation,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(
          workspace_spelling,
          root_selector,
          workspace_directory,
          selected_package,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  draft::ValidationCommandOptions options;
  options.package_directory = selected_package.physical_directory;
  options.target = target;
  options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(options.workspace);
  if (!load_foreign_provider_audits(
          options.workspace.workspace_directory,
          resolution_store_key(target, selected_package),
          provider_summaries,
          foreign_providers,
          options.foreign_provider_audits,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  options.kind = kind;
  options.instrumentation = instrumentation;
  options.foreign_providers = foreign_providers;
  options.runtime_assets = runtime_assets;
  options.timings = timings;
  const draft::ValidationCommandResult result =
      draft::execute_validation_command(
          sources, std::move(options), diagnostics);

  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  if (!result.completed) return 1;
  const std::string_view command = draft::validation_kind_name(kind);
  if (!result.passed) {
    std::cerr << command << " failed: "
              << result.selected_procedures
              << " selected procedures; evidence attempt "
              << result.attempt << " revoked key\n";
    return 1;
  }
  std::cout << command << " passed: "
            << result.selected_procedures
            << " selected procedures; evidence "
            << result.evidence_digest.hex() << " (attempt "
            << result.attempt << ")\n";
  return 0;
}

int emit_c_header_package(
    const std::string &workspace_spelling,
    std::string_view root_selector,
    const draft::TargetProfile &target,
    const std::optional<std::string> &requested_output,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(
          workspace_spelling,
          root_selector,
          workspace_directory,
          selected_package,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  std::error_code path_error;

  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(options.workspace);
  options.emit_program_entry = false;
  options.timings = timings;
  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          selected_package.physical_directory.string(),
          std::move(options),
          diagnostics);
  if (compiled.ok) {
    const std::size_t root =
        static_cast<std::size_t>(compiled.graph.root_package.value);
    if (root >= compiled.packages.size() ||
        !compiled.packages[root].has_value()) {
      diagnostics.error(
          draft::SourceRange::invalid(),
          "C header emission cannot find the compiled root package");
    } else {
      draft::TimingScope header_timing = timings != nullptr
          ? timings->scope("C header emission")
          : draft::TimingScope{};
      const draft::CHeaderResult header = draft::emit_c_header(
          compiled.packages[root]->semantics.package,
          target,
          {},
          diagnostics);
      if (header.ok) {
        std::filesystem::path output = workspace_directory / ".draft" /
            "build" / target.facts.file_tag;
        if (selected_package.identity.root_relative_path == ".") {
          output /= "workspace";
        } else {
          output /= "packages";
          output /= selected_package.identity.root_relative_path;
        }
        output /= compiled.graph.package(compiled.graph.root_package)
            .loaded.short_name + ".h";
        if (requested_output.has_value()) {
          output = std::filesystem::absolute(*requested_output, path_error);
          if (path_error) {
            diagnostics.error(
                draft::SourceRange::invalid(),
                "cannot make C header output path absolute: " +
                    path_error.message());
          }
        }
        if (!diagnostics.has_errors()) {
          std::filesystem::create_directories(output.parent_path(), path_error);
          if (path_error) {
            diagnostics.error(
                draft::SourceRange::invalid(),
                "cannot create C header output directory: " +
                    path_error.message());
          } else {
            const std::filesystem::path temporary = output.string() + ".tmp";
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
              diagnostics.error(
                  draft::SourceRange::invalid(),
                  "cannot open temporary C header output");
            } else {
              stream.write(
                  header.text.data(),
                  static_cast<std::streamsize>(header.text.size()));
              stream.close();
              if (!stream) {
                diagnostics.error(
                    draft::SourceRange::invalid(),
                    "cannot write temporary C header output");
              } else {
                std::filesystem::rename(temporary, output, path_error);
                if (path_error) {
                  diagnostics.error(
                      draft::SourceRange::invalid(),
                      "cannot commit C header output: " +
                          path_error.message());
                } else {
                  std::cout << "emitted " << output.string() << " ("
                            << header.export_count << " exports)\n";
                }
              }
            }
          }
        }
      }
    }
  }
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

enum class AgentCommandKind {
  Resolve,
  Judge,
};

// Optional native continuation requested by `resolve --build`. Resolution
// first commits checked source and returns its semantic graph. This record owns
// the process-facing artifact choice used to continue that exact graph through
// MIR/LLVM and the native adapter only after the transaction succeeds.
struct ResolveBuildRequest {
  std::optional<std::string> output;
  draft::NativeArtifactKind artifact_kind =
      draft::NativeArtifactKind::Executable;
};

// Resolve and judge first run the complete provider-independent front end, so
// malformed source, attachment-policy violations, and typed obligation errors
// are reported before any model call. Resolve may receive one explicit Codex
// adapter options; a program with only fresh pins still performs no synthesis
// provider call. Judge runs only after the same provider-free compilation and
// records evidence in its independent store without mutating source selection.
int run_agent_command(
    const std::string &workspace_spelling,
    std::string_view root_selector,
    AgentCommandKind command,
    const draft::TargetProfile &target,
    bool revalidate = false,
    bool regenerate = false,
    const std::vector<std::string> &regeneration_site_identities = {},
    const std::optional<draft::CodexCliProviderOptions> &codex = std::nullopt,
    const std::vector<draft::ForeignProviderInput> &foreign_providers = {},
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries = {},
    const std::vector<draft::RuntimeAssetInput> &runtime_assets = {},
    const std::vector<NamedCodexJudgmentValidator> &judgment_validators = {},
    const std::vector<draft::JudgmentRequestArtifact> &judgment_artifacts = {},
    const std::vector<std::string> &judgment_selectors = {},
    bool list_judgments = false,
    draft::RuntimeAssertionMode runtime_assertions =
        draft::RuntimeAssertionMode::On,
    draft::TimingRecorder *timings = nullptr,
    const std::optional<ResolveBuildRequest> &resolve_build = std::nullopt) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(
          workspace_spelling,
          root_selector,
          workspace_directory,
          selected_package,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.configuration.runtime_assertions = runtime_assertions;
  options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(options.workspace);
  options.timings = timings;
  if (command == AgentCommandKind::Resolve) {
    std::optional<draft::CompileWorkspaceOptions> build_options;
    draft::ResolveWorkspaceOptions resolve_options;
    resolve_options.compile = std::move(options);
    resolve_options.revalidate = revalidate;
    resolve_options.regenerate = regenerate;
    resolve_options.regeneration_site_identities =
        regeneration_site_identities;
    resolve_options.cancellation_requested = command_cancellation_requested;
    const bool external_inputs_configured = !foreign_providers.empty() ||
        !provider_summaries.empty() || !runtime_assets.empty();
    if (!external_inputs_configured) {
      // A preserved manifest summary remains a semantic compiler input. The
      // relocatable summary and matching artifact must be supplied again;
      // compiling without them while retaining the old manifest claim would
      // silently turn audited edges back into unknown calls.
      const draft::ResolutionManifestLoadResult loaded =
          draft::load_resolution_manifest(
              resolve_options.compile.workspace.workspace_directory,
              resolution_store_key(target, selected_package),
              diagnostics);
      if (loaded.state == draft::ResolutionManifestLoadState::Invalid ||
          (loaded.state == draft::ResolutionManifestLoadState::Loaded &&
           !draft::verify_foreign_provider_summary_inputs(
               {},
               {},
               loaded.manifest.external_inputs,
               resolve_options.compile.foreign_provider_audits,
               diagnostics))) {
        std::cerr << draft::render_diagnostics(sources, diagnostics);
        return 1;
      }
    }
    if (external_inputs_configured) {
      resolve_options.external_inputs_configured = true;
      if (!draft::pin_runtime_asset_inputs(
              runtime_assets,
              resolve_options.external_inputs,
              diagnostics)) {
        std::cerr << draft::render_diagnostics(sources, diagnostics);
        return 1;
      }
      if (!draft::pin_foreign_provider_inputs(
              foreign_providers,
              resolve_options.external_inputs,
              diagnostics)) {
        std::cerr << draft::render_diagnostics(sources, diagnostics);
        return 1;
      }
      if (!draft::pin_foreign_provider_summary_inputs(
              provider_summaries,
              foreign_providers,
              resolve_options.external_inputs,
              resolve_options.compile.foreign_provider_audits,
              diagnostics)) {
        std::cerr << draft::render_diagnostics(sources, diagnostics);
        return 1;
      }
    }
    // Copy after external provider summaries have populated the semantic audit
    // set. The post-commit continuation must receive the same target,
    // configuration, and foreign facts as the graph produced by resolution;
    // only its backend and entry-point requests differ.
    if (resolve_build.has_value()) {
      build_options = resolve_options.compile;
      build_options->lower_mir = true;
      build_options->emit_llvm = true;
      build_options->emit_program_entry =
          resolve_build->artifact_kind ==
          draft::NativeArtifactKind::Executable;
    }
    // State is a separate stack object because the function table deliberately
    // borrows it. Its lifetime encloses the entire synchronous resolver call.
    draft::CodexCliProviderState codex_state;
    if (codex.has_value()) {
      resolve_options.provider = draft::configure_codex_cli_provider(
          *codex, codex_state, diagnostics);
      if (resolve_options.provider.synthesize == nullptr) {
        std::cerr << draft::render_diagnostics(sources, diagnostics);
        return 1;
      }
    }
    draft::TimingScope resolve_timing = timings != nullptr
        ? timings->scope("resolution provider workflow")
        : draft::TimingScope{};
    draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
        sources,
        selected_package.physical_directory.string(),
        std::move(resolve_options),
        diagnostics);
    resolve_timing.finish();
    bool native_ok = true;
    if (resolved.ok) {
      if (!resolved.committed) {
        std::cout << "no synthesis sites require resolution\n";
      } else {
        std::cout << "resolved " << resolved.manifest.pins.size()
                  << " synthesis sites (" << resolved.synthesized_sites
                  << " synthesized, " << resolved.reused_sites
                  << " reused";
        if (resolved.regenerated_sites != 0) {
          std::cout << ", " << resolved.regenerated_sites
                    << " explicitly regenerated";
        }
        std::cout << ")\n";
      }
      if (resolve_build.has_value()) {
        if (!resolved.compiled_program.has_value()) {
          diagnostics.error(
              draft::SourceRange::invalid(),
              "resolution completed without returning its checked graph");
          native_ok = false;
        } else {
          // Resolution has already published the generated-source transaction.
          // Backend failure below therefore affects only the requested
          // artifact. The mutable result retains all declaration, type, and HIR
          // state needed by target lowering; no manifest or source is reloaded.
          native_ok = draft::continue_compiled_workspace(
              sources,
              *build_options,
              *resolved.compiled_program,
              diagnostics);
          if (native_ok) {
            native_ok = emit_native_package(
                workspace_directory,
                selected_package,
                target,
                resolve_build->output,
                resolve_build->artifact_kind,
                foreign_providers,
                runtime_assets,
                timings,
                *resolved.compiled_program,
                diagnostics);
          }
        }
      }
    }
    if (!diagnostics.diagnostics().empty()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    return resolved.ok && native_ok && !diagnostics.has_errors() ? 0 : 1;
  }

  if (!load_foreign_provider_audits(
          options.workspace.workspace_directory,
          resolution_store_key(target, selected_package),
          provider_summaries,
          foreign_providers,
          options.foreign_provider_audits,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          selected_package.physical_directory.string(),
          std::move(options),
          diagnostics);
  if (!compiled.ok) {
    if (!diagnostics.diagnostics().empty()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    return 1;
  }
  if (!compiled.resolution_manifest.has_value() &&
      (!foreign_providers.empty() || !provider_summaries.empty())) {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "judgment cannot introduce external-input pins for a handwritten "
        "program; run 'draftc resolve' with those inputs first");
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  draft::JudgmentSelection selection;
  if (!draft::select_judgment_sites(
          compiled, judgment_selectors, selection, diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  if (list_judgments) {
    for (const draft::JudgmentSiteDescription &site : selection.sites) {
      std::cout << site.site_identity << ' ' << site.package_identity << ' '
                << site.anchor_name << ' ' << site.source_relative_path << ':'
                << site.occurrence << '\n';
    }
    return 0;
  }
  if (selection.sites.empty()) {
    std::cout << "no judgment sites require execution\n";
    return 0;
  }

  std::vector<draft::CodexCliProviderState> judgment_codex_states;
  draft::JudgmentCommandOptions judgment_options;
  judgment_options.workspace_directory = workspace_directory;
  judgment_options.target = target;
  judgment_options.selectors = judgment_selectors;
  if (!configure_codex_judgment_policy(
          codex,
          judgment_validators,
          judgment_artifacts,
          judgment_codex_states,
          judgment_options,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  draft::TimingScope judgment_timing = timings != nullptr
      ? timings->scope("judgment execution")
      : draft::TimingScope{};
  const draft::JudgmentCommandResult judged = draft::execute_judgment_command(
      compiled, std::move(judgment_options), diagnostics);
  judgment_timing.finish();
  if (!judged.completed) {
    if (!diagnostics.diagnostics().empty()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    return 1;
  }
  if (!judged.passed) {
    std::cerr << "judge failed: " << judged.selected_judgments
              << " selected judgments\n";
    return 1;
  }
  std::cout << "judge passed: " << judged.selected_judgments
            << " selected judgments; recorded " << judged.evidence.size()
            << " evidence objects\n";
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

void print_usage() {
  std::cerr << "usage:\n"
            << "  draftc lex <file.draft>\n"
            << "  draftc syntax <file.draft>\n"
            << "  draftc check <workspace> [--root <package>]\n"
            << "      [--target aarch64-macos|aarch64-linux] [--timings|--timings=all]\n"
            << "  draftc emit-llvm <workspace> [--root <package>]\n"
            << "      [--target aarch64-macos|aarch64-linux] [--timings|--timings=all]\n"
            << "  draftc emit-c-header <workspace> [--root <package>] [-o <output.h>]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--timings|--timings=all]\n"
            << "  draftc expand <workspace> [--root <package>] --out <directory>\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--assertions=off]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "      [--timings|--timings=all]\n"
            << "  draftc build <workspace> [--root <package>]... [-o <output>]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--kind executable|object|static-library|dynamic-library|assembly]\n"
            << "      [--assertions=off]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "      [--timings|--timings=all]\n"
            << "  draftc test <workspace> [--root <package>]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--instrument address|lifetime|undefined-operation|allocator-poisoning|race]...\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "      [--timings|--timings=all]\n"
            << "  draftc bench <workspace> [--root <package>] [--verify]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--instrument address|lifetime|undefined-operation|allocator-poisoning|race]...\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "      [--timings|--timings=all]\n"
            << "  draftc resolve <workspace> [--root <package>] [--revalidate] [--build]\n"
            << "      [--regenerate [site-id]]\n"
            << "      [-o <output>]\n"
            << "      [--kind executable|object|static-library|dynamic-library|assembly]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--assertions=off]\n"
            << "      [--model <model>]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "      [--timings|--timings=all]\n"
            << "  draftc judge <workspace> [--root <package>] [<selector>...] [--list]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--assertions=off]\n"
            << "      [--judge-validator <identity>:<model>]...\n"
            << "      [--judge-artifact <kind>:<path>]...\n"
            << "      [--model <model>]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--timings|--timings=all]\n"
            << "  draftc target [--target aarch64-macos|aarch64-linux]\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "lex") {
    return lex_file(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "syntax") {
    return parse_file(argv[2]);
  }

  draft::TimingOutput timing_output = draft::TimingOutput::Disabled;
  if (!select_timing_output(argc, argv, timing_output)) return 2;
  CommandTimingReport timing_report(timing_output);
  draft::TimingRecorder *timings = timing_report.recorder.enabled()
      ? &timing_report.recorder
      : nullptr;

  // macOS remains the compatibility default, but every command that consumes
  // a package accepts the same explicit selector.  Keeping one value in main
  // makes it mechanically difficult for a branch to parse the option and then
  // forget to pass the chosen profile into its operation.
  draft::TargetProfile target = draft::make_aarch64_macos_profile();
  if (argc >= 3 &&
      (std::string_view(argv[1]) == "check" ||
       std::string_view(argv[1]) == "emit-llvm")) {
    bool target_set = false;
    bool root_set = false;
    std::string root_selector = ".";
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set && index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--root" && !root_set && index + 1 < argc) {
        root_set = true;
        root_selector = argv[++index];
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return compile_package(
        argv[2],
        root_selector,
        std::string_view(argv[1]) == "emit-llvm",
        target,
        timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "emit-c-header") {
    std::optional<std::string> output;
    bool target_set = false;
    bool root_set = false;
    std::string root_selector = ".";
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "-o" && !output.has_value() && index + 1 < argc) {
        output = argv[++index];
      } else if (argument == "--target" && !target_set && index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--root" && !root_set && index + 1 < argc) {
        root_set = true;
        root_selector = argv[++index];
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return emit_c_header_package(
        argv[2], root_selector, target, output, timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "expand") {
    std::optional<std::filesystem::path> output_directory;
    bool assertions_off = false;
    bool target_set = false;
    bool root_set = false;
    std::string root_selector = ".";
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    std::vector<draft::RuntimeAssetInput> runtime_assets;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--out" && !output_directory.has_value() &&
          index + 1 < argc) {
        output_directory = argv[++index];
      } else if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--root" && !root_set && index + 1 < argc) {
        root_set = true;
        root_selector = argv[++index];
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!parse_foreign_provider(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!parse_foreign_provider_summary(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!parse_runtime_asset(argv[++index], asset, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        runtime_assets.push_back(std::move(asset));
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    if (!output_directory.has_value()) {
      print_usage();
      return 2;
    }
    return expand_package(
        argv[2],
        root_selector,
        *output_directory,
        target,
        assertions_off
            ? draft::RuntimeAssertionMode::Off
            : draft::RuntimeAssertionMode::On,
        foreign_providers,
        provider_summaries,
        runtime_assets,
        timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "resolve") {
    bool revalidate = false;
    bool regenerate = false;
    bool build_after_resolution = false;
    bool assertions_off = false;
    bool target_set = false;
    bool root_set = false;
    bool artifact_kind_set = false;
    std::string root_selector = ".";
    std::optional<std::string> codex_model;
    std::optional<std::string> output;
    draft::NativeArtifactKind artifact_kind =
        draft::NativeArtifactKind::Executable;
    std::vector<std::string> regeneration_site_identities;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    std::vector<draft::RuntimeAssetInput> runtime_assets;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--revalidate" && !revalidate) {
        revalidate = true;
      } else if (argument == "--build" && !build_after_resolution) {
        build_after_resolution = true;
      } else if (argument == "--regenerate" && !regenerate) {
        regenerate = true;
        if (index + 1 < argc &&
            !std::string_view(argv[index + 1]).starts_with('-')) {
          regeneration_site_identities.emplace_back(argv[++index]);
        }
      } else if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--root" && !root_set && index + 1 < argc) {
        root_set = true;
        root_selector = argv[++index];
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "-o" && !output.has_value() &&
                 index + 1 < argc) {
        output = argv[++index];
      } else if (argument == "--kind" && !artifact_kind_set &&
                 index + 1 < argc) {
        const std::optional<draft::NativeArtifactKind> parsed =
            parse_native_artifact_kind(argv[++index]);
        if (!parsed.has_value()) {
          print_usage();
          return 2;
        }
        artifact_kind = *parsed;
        artifact_kind_set = true;
      } else if (argument == "--model" &&
                 !codex_model.has_value() && index + 1 < argc) {
        codex_model = argv[++index];
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!parse_foreign_provider(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!parse_foreign_provider_summary(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!parse_runtime_asset(argv[++index], asset, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        runtime_assets.push_back(std::move(asset));
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    if ((revalidate && codex_model.has_value()) ||
        (revalidate && regenerate) ||
        (!build_after_resolution &&
         (output.has_value() || artifact_kind_set))) {
      print_usage();
      return 2;
    }
    cancellation_signal = 0;
    (void)std::signal(SIGINT, request_cancellation);
    std::optional<draft::CodexCliProviderOptions> codex;
    if (!revalidate) {
      codex.emplace();
      if (codex_model.has_value()) codex->model = *codex_model;
      codex->cancellation_requested = command_cancellation_requested;
    }
    std::optional<ResolveBuildRequest> resolve_build;
    if (build_after_resolution) {
      resolve_build.emplace();
      resolve_build->output = output;
      resolve_build->artifact_kind = artifact_kind;
    }
    return run_agent_command(
        argv[2],
        root_selector,
        AgentCommandKind::Resolve,
        target,
        revalidate,
        regenerate,
        regeneration_site_identities,
        codex,
        foreign_providers,
        provider_summaries,
        runtime_assets,
        {},
        {},
        {},
        false,
        assertions_off
            ? draft::RuntimeAssertionMode::Off
            : draft::RuntimeAssertionMode::On,
        timings,
        resolve_build);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "judge") {
    std::optional<std::string> codex_model;
    bool list_judgments = false;
    bool assertions_off = false;
    bool target_set = false;
    bool root_set = false;
    std::string root_selector = ".";
    std::vector<std::string> judgment_selectors;
    std::vector<NamedCodexJudgmentValidator> judgment_validators;
    std::vector<JudgmentArtifactPath> judgment_artifact_paths;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--root" && !root_set && index + 1 < argc) {
        root_set = true;
        root_selector = argv[++index];
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "--model" &&
                 !codex_model.has_value() && index + 1 < argc) {
        codex_model = argv[++index];
      } else if (argument == "--judge-validator" && index + 1 < argc) {
        NamedCodexJudgmentValidator validator;
        std::string reason;
        if (!parse_judgment_validator(
                argv[++index],
                validator.identity,
                validator.codex.model,
                reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        judgment_validators.push_back(std::move(validator));
      } else if (argument == "--judge-artifact" && index + 1 < argc) {
        JudgmentArtifactPath artifact;
        std::string reason;
        if (!parse_judgment_artifact_path(
                argv[++index], artifact, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        judgment_artifact_paths.push_back(std::move(artifact));
      } else if (argument == "--list" && !list_judgments) {
        list_judgments = true;
      } else if (argument == "--select" && index + 1 < argc) {
        judgment_selectors.emplace_back(argv[++index]);
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!parse_foreign_provider(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!parse_foreign_provider_summary(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (is_timing_argument(argument)) {
        continue;
      } else if (!argument.starts_with('-')) {
        judgment_selectors.emplace_back(argument);
      } else {
        print_usage();
        return 2;
      }
    }
    if ((codex_model.has_value() && !judgment_validators.empty()) ||
        (list_judgments &&
         (codex_model.has_value() || !judgment_validators.empty() ||
          !judgment_artifact_paths.empty()))) {
      print_usage();
      return 2;
    }
    cancellation_signal = 0;
    (void)std::signal(SIGINT, request_cancellation);
    std::optional<draft::CodexCliProviderOptions> codex;
    if (!list_judgments && judgment_validators.empty()) {
      codex.emplace();
      if (codex_model.has_value()) codex->model = *codex_model;
      codex->cancellation_requested = command_cancellation_requested;
    }
    for (NamedCodexJudgmentValidator &validator : judgment_validators) {
      validator.codex.cancellation_requested =
          command_cancellation_requested;
    }
    std::vector<draft::JudgmentRequestArtifact> judgment_artifacts;
    std::string artifact_reason;
    if (!read_judgment_artifacts(
            judgment_artifact_paths,
            judgment_artifacts,
            artifact_reason)) {
      std::cerr << "error: " << artifact_reason << '\n';
      return 1;
    }
    return run_agent_command(
        argv[2],
        root_selector,
        AgentCommandKind::Judge,
        target,
        false,
        false,
        {},
        codex,
        foreign_providers,
        provider_summaries,
        {},
        judgment_validators,
        judgment_artifacts,
        judgment_selectors,
        list_judgments,
        assertions_off
            ? draft::RuntimeAssertionMode::Off
            : draft::RuntimeAssertionMode::On,
        timings);
  }
  if (argc >= 3 &&
      (std::string_view(argv[1]) == "test" ||
       std::string_view(argv[1]) == "bench")) {
    const draft::ValidationKind validation_kind =
        std::string_view(argv[1]) == "test"
        ? draft::ValidationKind::Test
        : draft::ValidationKind::Benchmark;
    bool verify = false;
    bool target_set = false;
    bool root_set = false;
    std::string root_selector = ".";
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    std::vector<draft::RuntimeAssetInput> runtime_assets;
    std::vector<draft::ValidationInstrumentationKind> instrumentation;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--root" && !root_set && index + 1 < argc) {
        root_set = true;
        root_selector = argv[++index];
      } else if (argument == "--verify" && !verify &&
                 validation_kind == draft::ValidationKind::Benchmark) {
        // Bench always executes and records fresh evidence. `--verify` is an
        // explicit release/CI spelling retained for readable automation.
        verify = true;
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!parse_foreign_provider(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!parse_foreign_provider_summary(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!parse_runtime_asset(argv[++index], asset, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        runtime_assets.push_back(std::move(asset));
      } else if (argument == "--instrument" && index + 1 < argc) {
        const std::string_view spelling(argv[++index]);
        const std::optional<draft::ValidationInstrumentationKind> parsed =
            draft::parse_validation_instrumentation(spelling);
        if (!parsed.has_value()) {
          std::cerr << "error: unknown validation instrumentation '"
                    << spelling << "'\n";
          return 2;
        }
        instrumentation.push_back(*parsed);
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return validate_package(
        argv[2],
        root_selector,
        target,
        validation_kind,
        instrumentation,
        foreign_providers,
        provider_summaries,
        runtime_assets,
        timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "build") {
    std::optional<std::string> output;
    draft::NativeArtifactKind artifact_kind =
        draft::NativeArtifactKind::Executable;
    bool artifact_kind_set = false;
    bool assertions_off = false;
    bool target_set = false;
    std::vector<std::string> root_selectors;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    std::vector<draft::RuntimeAssetInput> runtime_assets;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--root" && index + 1 < argc) {
        root_selectors.emplace_back(argv[++index]);
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "--kind" && !artifact_kind_set &&
                 index + 1 < argc) {
        const std::optional<draft::NativeArtifactKind> parsed =
            parse_native_artifact_kind(argv[++index]);
        if (!parsed.has_value()) {
          print_usage();
          return 2;
        }
        artifact_kind = *parsed;
        artifact_kind_set = true;
      } else if (argument == "-o" && !output.has_value() &&
                 index + 1 < argc) {
        output = argv[++index];
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!parse_foreign_provider(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!parse_foreign_provider_summary(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!parse_runtime_asset(argv[++index], asset, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        runtime_assets.push_back(std::move(asset));
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return build_workspace(
        argv[2],
        root_selectors,
        target,
        output,
        artifact_kind,
        assertions_off
            ? draft::RuntimeAssertionMode::Off
            : draft::RuntimeAssertionMode::On,
        foreign_providers,
        provider_summaries,
        runtime_assets,
        timings);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "target") {
    bool target_set = false;
    for (int index = 2; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set && index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else {
        print_usage();
        return 2;
      }
    }
    return print_target(target);
  }
  print_usage();
  return 2;
}
