// Bootstrap compiler command-line entry point.
//
// The driver is intentionally thin: it owns process-facing argument and stream
// behavior, while source loading, diagnostics, syntax, semantics, and codegen
// remain reusable modules. The initial `lex` command is an inspectable front-end
// probes and will remain useful after `check`, `build`, `resolve`, and `judge`
// are added. They print exact token spellings, grammar structure, or the complete
// versioned target profile without embedding phase logic in this file.

#include "backend/toolchain.h"
#include "backend/foreign_summaries.h"
#include "base/timing.h"
#include "compile/compiler.h"
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
#include "workspace/workspace.h"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

// Package commands derive the workspace root from the package's parent. That
// parent is also the security boundary for the no-follow resolution store, so
// it must not retain a symlink spelling such as macOS `/tmp`. Canonicalizing the
// already-existing package first resolves every parent component consistently
// while leaving output paths and relocatable external inputs under their own
// separate policies.
[[nodiscard]] bool canonical_package_directory(
    const std::string &spelling,
    std::filesystem::path &directory,
    std::string &reason) {
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(spelling, error);
  if (error) {
    reason = "cannot make package path absolute: " + error.message();
    return false;
  }
  directory = std::filesystem::canonical(absolute, error);
  if (error) {
    reason = "cannot canonicalize package path: " + error.message();
    return false;
  }
  if (!std::filesystem::is_directory(directory, error)) {
    reason = error
        ? "cannot inspect package path: " + error.message()
        : "package path is not a directory";
    return false;
  }
  return true;
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
    const std::vector<draft::ForeignProviderSummaryInput> &summary_inputs,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    std::vector<draft::ForeignProviderAudit> &audits,
    draft::DiagnosticSink &diagnostics) {
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace_directory, diagnostics);
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

// Runs one dependency-ordered provider-free pipeline. Until a build manifest
// supplies explicit roots, the requested package's parent is the workspace
// root. `check` stops after typed HIR/interfaces; `emit-llvm` additionally
// lowers MIR and prints each package module without invoking LLVM or a linker.
int compile_package(
    const std::string &directory,
    bool emit_llvm,
    const draft::TargetProfile &target,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path absolute_directory;
  std::string path_error;
  if (!canonical_package_directory(
          directory, absolute_directory, path_error)) {
    std::cerr << "error: " << path_error << '\n';
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(options.workspace);
  options.lower_mir = emit_llvm;
  options.emit_llvm = emit_llvm;
  options.timings = timings;
  draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          absolute_directory.string(),
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

int build_package(
    const std::string &directory,
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
  std::filesystem::path absolute_directory;
  std::string package_path_error;
  if (!canonical_package_directory(
          directory, absolute_directory, package_path_error)) {
    std::cerr << "error: " << package_path_error << '\n';
    return 1;
  }
  std::error_code path_error;
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
  compile_options.configuration.runtime_assertions = runtime_assertions;
  compile_options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(compile_options.workspace);
  if (!load_foreign_provider_audits(
          compile_options.workspace.workspace_directory,
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
          absolute_directory.string(),
          std::move(compile_options),
          diagnostics);
  if (compiled.ok) {
    const std::filesystem::path build_directory =
        absolute_directory / ".draft" / "build";
    const std::string package_name = absolute_directory.filename().string();
    std::filesystem::path output;
    switch (artifact_kind) {
    case draft::NativeArtifactKind::Executable:
      output = build_directory / package_name;
      break;
    case draft::NativeArtifactKind::Object:
      output = build_directory / (package_name + ".o");
      break;
    case draft::NativeArtifactKind::StaticLibrary:
      output = build_directory / ("lib" + package_name + ".a");
      break;
    case draft::NativeArtifactKind::DynamicLibrary:
      output = build_directory /
          ("lib" + package_name +
           (target.facts.object_format == "elf" ? ".so" : ".dylib"));
      break;
    case draft::NativeArtifactKind::Assembly:
      output = build_directory / (package_name + "-assembly");
      break;
    }
    if (requested_output.has_value()) {
      output = std::filesystem::absolute(*requested_output, path_error);
      if (path_error) {
        diagnostics.error(
            draft::SourceRange::invalid(),
            "cannot make native output path absolute: " +
                path_error.message());
      }
    }
    draft::NativeBuildOptions native_options;
    native_options.build_directory = build_directory.string();
    native_options.output_path = output.string();
    native_options.artifact_kind = artifact_kind;
    native_options.foreign_providers = foreign_providers;
    native_options.runtime_assets = runtime_assets;
    native_options.timings = timings;
    if (!diagnostics.has_errors()) {
      const draft::NativeBuildResult built = draft::build_native_artifact(
          target, compiled, native_options, diagnostics);
      if (built.ok) {
        std::cout << "built " << built.output_path << '\n';
        if (!built.debug_symbols_path.empty()) {
          std::cout << "debug symbols " << built.debug_symbols_path << '\n';
        }
      }
    }
  }
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

int validate_package(
    const std::string &directory,
    const draft::TargetProfile &target,
    draft::ValidationKind kind,
    const std::vector<draft::ValidationInstrumentationKind> &instrumentation,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path absolute_directory;
  std::string path_error;
  if (!canonical_package_directory(
          directory, absolute_directory, path_error)) {
    std::cerr << "error: " << path_error << '\n';
    return 1;
  }

  draft::ValidationCommandOptions options;
  options.package_directory = absolute_directory;
  options.target = target;
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(options.workspace);
  if (!load_foreign_provider_audits(
          options.workspace.workspace_directory,
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
    const std::string &directory,
    const draft::TargetProfile &target,
    const std::optional<std::string> &requested_output,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path absolute_directory;
  std::string package_path_error;
  if (!canonical_package_directory(
          directory, absolute_directory, package_path_error)) {
    std::cerr << "error: " << package_path_error << '\n';
    return 1;
  }
  std::error_code path_error;

  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(options.workspace);
  options.emit_program_entry = false;
  options.timings = timings;
  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          absolute_directory.string(),
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
        std::filesystem::path output = absolute_directory / ".draft" / "build" /
            (absolute_directory.filename().string() + ".h");
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

struct ResolutionValidationRunnerState {
  draft::ValidationCommandOptions options;
  draft::ValidationCommandResult result;
};

struct ResolutionJudgmentRunnerState {
  draft::JudgmentCommandOptions options;
  draft::JudgmentCommandResult result;
  draft::TimingRecorder *timings = nullptr;
};

// Native execution remains outside the semantic resolver. This adapter accepts
// its immutable typed candidate, runs the normal compiler-owned validation harness,
// and records audit/revocation history before resolution commits. Only passing
// evidence is returned for selection by the later manifest publication.
bool run_resolution_candidate_validation(
    void *opaque,
    const draft::TargetProfile &target,
    draft::ValidationKind kind,
    const draft::CompileWorkspaceResult &compiled,
    draft::ResolutionValidationEvidence &evidence,
    draft::DiagnosticSink &diagnostics) {
  auto *state = static_cast<ResolutionValidationRunnerState *>(opaque);
  state->options.target = target;
  state->options.kind = kind;
  const std::size_t before = diagnostics.error_count();
  state->result = draft::execute_precommit_validation(
      compiled, state->options, diagnostics);
  if (!state->result.completed) {
    if (diagnostics.error_count() == before) {
      diagnostics.error(
          draft::SourceRange::invalid(),
          "resolution candidate validation could not complete");
    }
    return false;
  }
  if (!state->result.passed) {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "resolution candidate " +
            std::string(draft::validation_kind_name(kind)) +
            " failed for " +
            std::to_string(state->result.selected_procedures) +
            " selected procedures");
    return false;
  }
  evidence.key = state->result.evidence_key;
  evidence.content_digest = state->result.evidence_digest;
  evidence.recorded = true;
  return true;
}

// A resolution profile judges the same in-memory candidate that the resolver
// is about to publish. Evidence attempts become durable as they complete, but
// this adapter returns rows only after the whole selected profile passes. The
// resolver then folds those rows into its one final manifest transaction.
bool run_resolution_candidate_judgment(
    void *opaque,
    const draft::TargetProfile &target,
    const draft::CompileWorkspaceResult &compiled,
    std::vector<draft::ResolutionEvidencePin> &evidence,
    std::size_t &selected_judgments,
    draft::DiagnosticSink &diagnostics) {
  auto *state = static_cast<ResolutionJudgmentRunnerState *>(opaque);
  draft::TimingScope timing = state->timings != nullptr
      ? state->timings->scope("resolution judgment execution")
      : draft::TimingScope{};
  state->options.target = target;
  const std::size_t before = diagnostics.error_count();
  state->result = draft::execute_judgment_command(
      compiled, state->options, diagnostics);
  selected_judgments = state->result.selected_judgments;
  if (!state->result.completed) {
    if (diagnostics.error_count() == before) {
      diagnostics.error(
          draft::SourceRange::invalid(),
          "resolution candidate judgment could not complete");
    }
    return false;
  }
  if (!state->result.passed) {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "resolution candidate judgment failed for " +
            std::to_string(state->result.selected_judgments) +
            " selected judgments");
    return false;
  }

  draft::JudgmentSelection selection;
  if (!draft::select_judgment_sites(
          compiled, state->options.selectors, selection, diagnostics)) {
    return false;
  }
  std::vector<draft::ResolutionEvidencePin> current;
  // An empty selector list is the complete profile, so it intentionally starts
  // from an empty set. A partial profile may preserve unselected judgment rows,
  // but only rows the resolver carried forward after proving the resolved
  // program digest unchanged are visible here.
  if (!state->options.selectors.empty() &&
      compiled.resolution_manifest.has_value()) {
    for (const draft::ResolutionEvidencePin &pin :
         compiled.resolution_manifest->evidence) {
      if (pin.kind == "judgment") current.push_back(pin);
    }
  }
  return draft::replace_selected_judgment_evidence(
      state->options.workspace_directory,
      current,
      selection,
      state->result.evidence,
      evidence,
      diagnostics);
}

// Resolve and judge first run the complete provider-independent front end, so
// malformed source, attachment-policy violations, and typed obligation errors
// are reported before any model call. Resolve may receive one explicit Codex
// adapter options; a program with only fresh pins still performs no synthesis
// provider call. Resolve may additionally run a selected judgment profile over
// its complete candidate before the one manifest commit. Judge runs only after
// the same provider-free compilation and selects its all-pass evidence with an
// atomic manifest compare-and-replace.
int run_agent_command(
    const std::string &directory,
    AgentCommandKind command,
    const draft::TargetProfile &target,
    bool revalidate = false,
    const std::optional<draft::CodexCliProviderOptions> &codex = std::nullopt,
    const std::vector<draft::ForeignProviderInput> &foreign_providers = {},
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries = {},
    const std::vector<draft::RuntimeAssetInput> &runtime_assets = {},
    const std::vector<draft::ValidationInstrumentationKind> &instrumentation = {},
    const std::vector<NamedCodexJudgmentValidator> &judgment_validators = {},
    const std::vector<draft::JudgmentRequestArtifact> &judgment_artifacts = {},
    const std::vector<std::string> &judgment_selectors = {},
    bool list_judgments = false,
    bool judge_during_resolution = false,
    draft::RuntimeAssertionMode runtime_assertions =
        draft::RuntimeAssertionMode::On,
    draft::TimingRecorder *timings = nullptr) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path absolute_directory;
  std::string path_error;
  if (!canonical_package_directory(
          directory, absolute_directory, path_error)) {
    std::cerr << "error: " << path_error << '\n';
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.configuration.runtime_assertions = runtime_assertions;
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(options.workspace);
  options.timings = timings;
  if (command == AgentCommandKind::Resolve) {
    // Validate the selected profile even when this particular package has no
    // test or benchmark declarations. Otherwise an unsupported request could
    // disappear merely because the resolver never needs to call its runner.
    if (!draft::validate_validation_instrumentation(
            options.target, instrumentation, diagnostics)) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
      return 1;
    }
    draft::ResolveWorkspaceOptions resolve_options;
    resolve_options.compile = std::move(options);
    resolve_options.revalidate = revalidate;
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
    std::vector<draft::CodexCliProviderState> judgment_codex_states;
    ResolutionJudgmentRunnerState judgment_state;
    judgment_state.timings = timings;
    if (judge_during_resolution) {
      judgment_state.options.workspace_directory =
          absolute_directory.parent_path();
      judgment_state.options.target = resolve_options.compile.target;
      judgment_state.options.selectors = judgment_selectors;
      if (!configure_codex_judgment_policy(
              codex,
              judgment_validators,
              judgment_artifacts,
              judgment_codex_states,
              judgment_state.options,
              diagnostics)) {
        std::cerr << draft::render_diagnostics(sources, diagnostics);
        return 1;
      }
      resolve_options.judgment_runner.state = &judgment_state;
      resolve_options.judgment_runner.run =
          run_resolution_candidate_judgment;
    }
    ResolutionValidationRunnerState validation_state;
    validation_state.options.package_directory = absolute_directory;
    validation_state.options.target = resolve_options.compile.target;
    validation_state.options.instrumentation = instrumentation;
    validation_state.options.foreign_providers = foreign_providers;
    validation_state.options.runtime_assets = runtime_assets;
    validation_state.options.timings = timings;
    resolve_options.validation_runner.state = &validation_state;
    resolve_options.validation_runner.run =
        run_resolution_candidate_validation;
    draft::TimingScope resolve_timing = timings != nullptr
        ? timings->scope("resolution provider workflow")
        : draft::TimingScope{};
    const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
        sources,
        absolute_directory.string(),
        std::move(resolve_options),
        diagnostics);
    if (resolved.ok) {
      if (!resolved.committed) {
        if (judge_during_resolution) {
          std::cout << "no synthesis or judgment sites require resolution\n";
        } else {
          std::cout << "no synthesis sites require resolution\n";
        }
      } else {
        std::cout << "resolved " << resolved.manifest.pins.size()
                  << " synthesis sites (" << resolved.synthesized_sites
                  << " synthesized, " << resolved.reused_sites
                  << " reused); passed " << resolved.tested_procedures
                  << " tests and " << resolved.benchmarked_procedures
                  << " benchmarks and " << resolved.judged_sites
                  << " judgments before commit\n";
      }
    }
    if (!diagnostics.diagnostics().empty()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    return resolved.ok && !diagnostics.has_errors() ? 0 : 1;
  }

  if (!load_foreign_provider_audits(
          options.workspace.workspace_directory,
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
          absolute_directory.string(),
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
  judgment_options.workspace_directory =
      absolute_directory.parent_path();
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
              << " selected judgments; resolution manifest unchanged\n";
    return 1;
  }

  draft::ResolutionManifest replacement;
  if (compiled.resolution_manifest.has_value()) {
    replacement = *compiled.resolution_manifest;
  } else {
    replacement.target_identity = target.facts.identity;
    replacement.resolved_program_digest = *compiled.resolved_program_digest;
  }
  std::vector<draft::ResolutionEvidencePin> updated_evidence;
  if (!draft::replace_selected_judgment_evidence(
          absolute_directory.parent_path(),
          replacement.evidence,
          selection,
          judged.evidence,
          updated_evidence,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  replacement.evidence = std::move(updated_evidence);
  if (!draft::commit_resolution_manifest_if_unchanged(
          absolute_directory.parent_path(),
          compiled.resolution_manifest,
          replacement,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  std::cout << "judge passed: " << judged.selected_judgments
            << " selected judgments; selected " << judged.evidence.size()
            << " evidence rows in resolution manifest\n";
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

void print_usage() {
  std::cerr << "usage:\n"
            << "  draftc lex <file.draft>\n"
            << "  draftc syntax <file.draft>\n"
            << "  draftc check <package-directory> [--target aarch64-macos|aarch64-linux]\n"
            << "  draftc emit-llvm <package-directory> [--target aarch64-macos|aarch64-linux]\n"
            << "  draftc emit-c-header <package-directory> [-o <output.h>]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "  draftc build <package-directory> [-o <output>]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--kind executable|object|static-library|dynamic-library|assembly]\n"
            << "      [--assertions=off]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "  draftc test <package-directory>\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--instrument address|lifetime|undefined-operation|allocator-poisoning|race]...\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "  draftc bench <package-directory> [--verify]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--instrument address|lifetime|undefined-operation|allocator-poisoning|race]...\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "  draftc resolve <package-directory> [--revalidate] [--judge]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--instrument address|lifetime|undefined-operation|allocator-poisoning|race]...\n"
            << "      [--assertions=off]\n"
            << "      [--judge-select <selector>]...\n"
            << "      [--judge-validator <identity>:<model>]...\n"
            << "      [--judge-artifact <kind>:<path>]...\n"
            << "      [--codex-distribution-root <directory>\n"
            << "       --codex-executable <path> --codex-model <model>]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "      [--runtime-asset name:<file-or-directory>]...\n"
            << "  draftc judge <package-directory> [<selector>...] [--list]\n"
            << "      [--target aarch64-macos|aarch64-linux]\n"
            << "      [--assertions=off]\n"
            << "      [--judge-validator <identity>:<model>]...\n"
            << "      [--judge-artifact <kind>:<path>]...\n"
            << "      [--codex-distribution-root <directory>\n"
            << "       --codex-executable <path> --codex-model <model>]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "  draftc target [--target aarch64-macos|aarch64-linux]\n"
            << "\n"
            << "  package commands accept --timings or --timings=all\n";
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
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set && index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return compile_package(
        argv[2], std::string_view(argv[1]) == "emit-llvm", target, timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "emit-c-header") {
    std::optional<std::string> output;
    bool target_set = false;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "-o" && !output.has_value() && index + 1 < argc) {
        output = argv[++index];
      } else if (argument == "--target" && !target_set && index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return emit_c_header_package(argv[2], target, output, timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "resolve") {
    bool revalidate = false;
    bool judge_during_resolution = false;
    bool assertions_off = false;
    bool target_set = false;
    std::optional<std::string> codex_distribution_root;
    std::optional<std::string> codex_executable;
    std::optional<std::string> codex_model;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    std::vector<draft::RuntimeAssetInput> runtime_assets;
    std::vector<draft::ValidationInstrumentationKind> instrumentation;
    std::vector<NamedCodexJudgmentValidator> judgment_validators;
    std::vector<JudgmentArtifactPath> judgment_artifact_paths;
    std::vector<std::string> judgment_selectors;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--revalidate" && !revalidate) {
        revalidate = true;
      } else if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "--judge" &&
                 !judge_during_resolution) {
        judge_during_resolution = true;
      } else if (argument == "--judge-select" && index + 1 < argc) {
        judge_during_resolution = true;
        judgment_selectors.emplace_back(argv[++index]);
      } else if (argument == "--codex-executable" &&
                 !codex_executable.has_value() && index + 1 < argc) {
        codex_executable = argv[++index];
      } else if (argument == "--codex-distribution-root" &&
                 !codex_distribution_root.has_value() && index + 1 < argc) {
        codex_distribution_root = argv[++index];
      } else if (argument == "--codex-model" &&
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
        judge_during_resolution = true;
        judgment_validators.push_back(std::move(validator));
      } else if (argument == "--judge-artifact" && index + 1 < argc) {
        JudgmentArtifactPath artifact;
        std::string reason;
        if (!parse_judgment_artifact_path(
                argv[++index], artifact, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        judge_during_resolution = true;
        judgment_artifact_paths.push_back(std::move(artifact));
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
    const bool has_codex_models =
        codex_model.has_value() || !judgment_validators.empty();
    if (codex_executable.has_value() != codex_distribution_root.has_value() ||
        codex_executable.has_value() != has_codex_models ||
        (revalidate && has_codex_models) ||
        (revalidate && judge_during_resolution)) {
      print_usage();
      return 2;
    }
    cancellation_signal = 0;
    (void)std::signal(SIGINT, request_cancellation);
    std::optional<draft::CodexCliProviderOptions> codex;
    if (codex_executable.has_value()) {
      if (codex_model.has_value()) {
        codex.emplace();
        codex->distribution_root = *codex_distribution_root;
        codex->executable = *codex_executable;
        codex->model = *codex_model;
        codex->cancellation_requested = command_cancellation_requested;
      }
      for (NamedCodexJudgmentValidator &validator : judgment_validators) {
        validator.codex.distribution_root = *codex_distribution_root;
        validator.codex.executable = *codex_executable;
        validator.codex.cancellation_requested =
            command_cancellation_requested;
      }
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
        AgentCommandKind::Resolve,
        target,
        revalidate,
        codex,
        foreign_providers,
        provider_summaries,
        runtime_assets,
        instrumentation,
        judgment_validators,
        judgment_artifacts,
        judgment_selectors,
        false,
        judge_during_resolution,
        assertions_off
            ? draft::RuntimeAssertionMode::Off
            : draft::RuntimeAssertionMode::On,
        timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "judge") {
    std::optional<std::string> codex_distribution_root;
    std::optional<std::string> codex_executable;
    std::optional<std::string> codex_model;
    bool list_judgments = false;
    bool assertions_off = false;
    bool target_set = false;
    std::vector<std::string> judgment_selectors;
    std::vector<NamedCodexJudgmentValidator> judgment_validators;
    std::vector<JudgmentArtifactPath> judgment_artifact_paths;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--codex-executable" &&
          !codex_executable.has_value() && index + 1 < argc) {
        codex_executable = argv[++index];
      } else if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "--codex-distribution-root" &&
                 !codex_distribution_root.has_value() && index + 1 < argc) {
        codex_distribution_root = argv[++index];
      } else if (argument == "--codex-model" &&
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
    const bool has_codex_models =
        codex_model.has_value() || !judgment_validators.empty();
    if (codex_executable.has_value() !=
            codex_distribution_root.has_value() ||
        codex_executable.has_value() != has_codex_models ||
        (codex_model.has_value() && !judgment_validators.empty()) ||
        (list_judgments &&
         (has_codex_models || !judgment_artifact_paths.empty()))) {
      print_usage();
      return 2;
    }
    cancellation_signal = 0;
    (void)std::signal(SIGINT, request_cancellation);
    std::optional<draft::CodexCliProviderOptions> codex;
    if (codex_executable.has_value()) {
      if (codex_model.has_value()) {
        codex.emplace();
        codex->distribution_root = *codex_distribution_root;
        codex->executable = *codex_executable;
        codex->model = *codex_model;
        codex->cancellation_requested = command_cancellation_requested;
      }
      for (NamedCodexJudgmentValidator &validator : judgment_validators) {
        validator.codex.distribution_root = *codex_distribution_root;
        validator.codex.executable = *codex_executable;
        validator.codex.cancellation_requested =
            command_cancellation_requested;
      }
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
        AgentCommandKind::Judge,
        target,
        false,
        codex,
        foreign_providers,
        provider_summaries,
        {},
        {},
        judgment_validators,
        judgment_artifacts,
        judgment_selectors,
        list_judgments,
        false,
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
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    std::vector<draft::RuntimeAssetInput> runtime_assets;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target)) return 2;
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "--kind" && !artifact_kind_set &&
                 index + 1 < argc) {
        const std::string_view spelling(argv[++index]);
        artifact_kind_set = true;
        if (spelling == "executable") {
          artifact_kind = draft::NativeArtifactKind::Executable;
        } else if (spelling == "object") {
          artifact_kind = draft::NativeArtifactKind::Object;
        } else if (spelling == "static-library") {
          artifact_kind = draft::NativeArtifactKind::StaticLibrary;
        } else if (spelling == "dynamic-library") {
          artifact_kind = draft::NativeArtifactKind::DynamicLibrary;
        } else if (spelling == "assembly") {
          artifact_kind = draft::NativeArtifactKind::Assembly;
        } else {
          print_usage();
          return 2;
        }
      } else if (argument == "-o" && index + 1 < argc) {
        ++index;
        output = argv[index];
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
    return build_package(
        argv[2],
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
