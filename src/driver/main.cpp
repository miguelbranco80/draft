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
// Every package command receives one package path. The nearest ancestor
// `draft.workspace` marker establishes the import and derived-state boundary;
// without one, that package is a standalone workspace. Aggregate build treats
// its path as a recursive search scope and discovers surface package-level
// `main` declarations. Physical paths are used only for I/O; persistent state
// is keyed by canonical workspace PackageIdentity plus target. Command ordering
// and output paths therefore remain independent of filesystem enumeration and
// host path spelling. See specification sections 3, 6 (Program entry), and 10,
// and docs/operations/command-reference.md.

#include "backend/foreign_summaries.h"
#include "backend/runtime_assets.h"
#include "backend/toolchain.h"
#include "base/child_process.h"
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
#include "workspace/embedded_core.h"
#include "workspace/manifest.h"
#include "workspace/package.h"
#include "workspace/selection.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

// CommandManifestContext is the one process-lifetime view of an optional
// workspace manifest. opened_directory is the user's exact path.
// package_directory differs only for `run <workspace>`, where an explicit
// default program is selected. effective_build is the exact-path view used by
// single-root commands. Aggregate build retains the raw manifest and resolves
// the same precedence separately for every discovered root.
struct CommandManifestContext {
  std::filesystem::path workspace_directory;
  std::filesystem::path opened_directory;
  std::filesystem::path package_directory;
  draft::WorkspaceManifest manifest;
  const draft::ProgramConfiguration *program = nullptr;
  draft::BuildDefaults effective_build;
};

// BuildCommandOverrides retains only values explicitly supplied on this build
// command. Aggregate builds must apply those values after each discovered
// root's manifest configuration; collapsing them into one pre-discovery value
// would make it impossible to distinguish a CLI override from a workspace
// default which another named program is allowed to replace.
struct BuildCommandOverrides {
  std::optional<draft::TargetProfile> target;
  std::optional<std::string> output;
  std::optional<draft::NativeArtifactKind> artifact_kind;
  std::optional<draft::NativeOptimizationLevel> optimization;
  std::optional<bool> emit_debug_symbols;
  std::optional<draft::RuntimeAssertionMode> runtime_assertions;
  std::optional<std::vector<draft::ForeignProviderInput>> foreign_providers;
  std::optional<std::vector<draft::ForeignProviderSummaryInput>>
      provider_summaries;
  std::optional<std::vector<draft::RuntimeAssetInput>> runtime_assets;
};

// EffectiveBuildConfiguration is the complete operational input for one root.
// It contains no borrowed manifest strings. Every relative manifest path has
// already been resolved against the workspace, while CLI paths retain their
// ordinary process-facing interpretation until native publication.
struct EffectiveBuildConfiguration {
  draft::TargetProfile target = draft::make_aarch64_macos_profile();
  std::optional<std::string> output;
  draft::NativeArtifactKind artifact_kind =
      draft::NativeArtifactKind::Executable;
  draft::NativeOptimizationLevel optimization =
      draft::NativeOptimizationLevel::O0;
  bool emit_debug_symbols = false;
  draft::RuntimeAssertionMode runtime_assertions =
      draft::RuntimeAssertionMode::On;
  std::vector<draft::ForeignProviderInput> foreign_providers;
  std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
  std::vector<draft::RuntimeAssetInput> runtime_assets;
};

[[nodiscard]] bool
prepare_command_manifest(const std::filesystem::path &command_path,
                         bool select_default_program,
                         CommandManifestContext &context, std::string &reason) {
  draft::DiagnosticSink diagnostics;
  std::filesystem::path manifest_path;
  if (!draft::locate_command_scope(command_path, context.workspace_directory,
                                   context.opened_directory, manifest_path,
                                   diagnostics)) {
    reason = diagnostics.diagnostics().empty()
                 ? "cannot locate command path"
                 : diagnostics.diagnostics().front().message;
    return false;
  }
  if (!draft::load_workspace_manifest(manifest_path, context.manifest,
                                      reason)) {
    return false;
  }

  context.package_directory = context.opened_directory;
  std::string relative_root = ".";
  if (context.opened_directory != context.workspace_directory) {
    relative_root =
        context.opened_directory.lexically_relative(context.workspace_directory)
            .generic_string();
  }
  context.program =
      draft::find_program_by_root(context.manifest, relative_root);
  if (select_default_program &&
      context.opened_directory == context.workspace_directory &&
      context.manifest.default_program.has_value()) {
    context.program = draft::find_program_by_name(
        context.manifest, *context.manifest.default_program);
    if (context.program != nullptr) {
      context.package_directory =
          context.workspace_directory / context.program->root;
      std::error_code error;
      context.package_directory =
          std::filesystem::canonical(context.package_directory, error);
      if (error || !std::filesystem::is_directory(context.package_directory)) {
        reason = "default program root is unavailable: '" +
                 context.program->root + "'";
        return false;
      }
    }
  }

  context.effective_build = context.manifest.build;
  if (context.program != nullptr) {
    draft::merge_build_defaults(context.effective_build,
                                context.program->build);
  }
  return true;
}

[[nodiscard]] bool is_package_command(std::string_view command) {
  return command == "check" || command == "emit-llvm" ||
         command == "emit-c-header" || command == "expand" ||
         command == "build" || command == "run" || command == "test" ||
         command == "bench" || command == "resolve" || command == "judge";
}

[[nodiscard]] std::string
manifest_output_path(const CommandManifestContext &context,
                     std::string_view spelling) {
  const std::filesystem::path path(spelling);
  return path.is_absolute() ? path.string()
                            : (context.workspace_directory / path).string();
}

// Provider-like rows put their path after the first colon. Resolve only that
// path against the workspace; the provider identity and kind remain exact
// driver syntax. Using the first delimiter preserves a later Windows drive
// colon in `provider=kind:C:/path`.
[[nodiscard]] std::string
manifest_named_path(const CommandManifestContext &context,
                    std::string_view spelling) {
  return draft::resolve_manifest_mapping_path(context.workspace_directory,
                                              spelling);
}

[[nodiscard]] bool load_manifest_native_inputs(
    const CommandManifestContext &context,
    std::vector<draft::ForeignProviderInput> &providers,
    std::vector<draft::ForeignProviderSummaryInput> &summaries,
    std::vector<draft::RuntimeAssetInput> &assets, std::string &reason) {
  for (const std::string &spelling : context.effective_build.providers) {
    draft::ForeignProviderInput input;
    if (!draft::parse_foreign_provider_input(manifest_named_path(context, spelling), input,
                                reason)) {
      return false;
    }
    providers.push_back(std::move(input));
  }
  for (const std::string &spelling :
       context.effective_build.provider_summaries) {
    draft::ForeignProviderSummaryInput input;
    if (!draft::parse_foreign_provider_summary_input(manifest_named_path(context, spelling),
                                        input, reason)) {
      return false;
    }
    summaries.push_back(std::move(input));
  }
  for (const std::string &spelling : context.effective_build.runtime_assets) {
    draft::RuntimeAssetInput input;
    if (!draft::parse_runtime_asset_input(manifest_named_path(context, spelling), input,
                             reason)) {
      return false;
    }
    assets.push_back(std::move(input));
  }
  return true;
}

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
    if (std::string_view(argv[1]) == "run" && argument == "--")
      break;
    if (!is_timing_argument(argument))
      continue;
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

// Core is an immutable part of the compiler distribution. Workspace loading
// consumes the generated byte rows directly, so moving draftc cannot alter
// `import core/...` and ordinary builds perform no ambient core lookup.
void configure_core_distribution(draft::WorkspaceLoadOptions &options) {
  options.core_files = draft::embedded_core_files();
  options.core_content_identity = draft::embedded_core_content_identity();
}

// Resolves the user-supplied package path and its nearest workspace boundary.
// The returned physical directory is used only for source I/O; all persistent
// state uses the paired canonical PackageIdentity.
[[nodiscard]] bool
select_command_package(const std::string &package_spelling,
                       std::filesystem::path &workspace_directory,
                       draft::WorkspacePackageSelection &package,
                       draft::DiagnosticSink &diagnostics) {
  draft::CommandPackageSelection located;
  if (!draft::locate_command_package(package_spelling, located, diagnostics)) {
    return false;
  }
  workspace_directory = std::move(located.workspace_directory);
  package = std::move(located.package);
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

// Parses one argument already known to begin with `-O`. Draft exposes only two
// levels, so every other spelling receives a specific usage error instead of
// falling through to the full command synopsis. `seen` enforces one explicit
// choice even when duplicate spellings agree, keeping command provenance
// unambiguous.
[[nodiscard]] bool parse_native_optimization_argument(
    std::string_view argument,
    bool &seen,
    draft::NativeOptimizationLevel &optimization) {
  if (seen) {
    std::cerr << "error: optimization level may be specified only once\n";
    return false;
  }
  seen = true;
  if (argument == "-O0") {
    optimization = draft::NativeOptimizationLevel::O0;
    return true;
  }
  if (argument == "-O2") {
    optimization = draft::NativeOptimizationLevel::O2;
    return true;
  }
  std::cerr << "error: unsupported optimization level '" << argument
            << "'; expected -O0 or -O2\n";
  return false;
}

// Resolves the three-layer build policy for one exact package root. Workspace
// defaults are copied first, the matching named program is merged second, and
// explicit CLI values replace the result last. Keeping this operation pure per
// root is what lets `draftc build .` honor different targets, providers, and
// output policies without changing recursive discovery order.
[[nodiscard]] bool resolve_effective_build_configuration(
    const CommandManifestContext &context,
    const draft::ProgramConfiguration *program,
    const BuildCommandOverrides &overrides,
    EffectiveBuildConfiguration &result,
    std::string &reason) {
  CommandManifestContext selected_context = context;
  selected_context.program = program;
  selected_context.effective_build = context.manifest.build;
  if (program != nullptr) {
    draft::merge_build_defaults(selected_context.effective_build,
                                program->build);
  }

  EffectiveBuildConfiguration selected;
  if (selected_context.effective_build.target.has_value()) {
    if (!draft::select_builtin_target_profile(
            *selected_context.effective_build.target, selected.target,
            reason)) {
      return false;
    }
    if (!draft::validate_target_profile(selected.target, reason)) {
      reason = "invalid built-in target profile: " + reason;
      return false;
    }
  }
  if (selected_context.effective_build.output.has_value()) {
    selected.output = manifest_output_path(
        selected_context, *selected_context.effective_build.output);
  }
  if (selected_context.effective_build.artifact_kind.has_value()) {
    const std::optional<draft::NativeArtifactKind> parsed =
        draft::parse_native_artifact_kind(
            *selected_context.effective_build.artifact_kind);
    if (!parsed.has_value()) {
      reason = "invalid manifest artifact kind '" +
          *selected_context.effective_build.artifact_kind + "'";
      return false;
    }
    selected.artifact_kind = *parsed;
  }
  if (selected_context.effective_build.optimization.has_value()) {
    const std::string_view spelling =
        *selected_context.effective_build.optimization;
    if (spelling == "O0") {
      selected.optimization = draft::NativeOptimizationLevel::O0;
    } else if (spelling == "O2") {
      selected.optimization = draft::NativeOptimizationLevel::O2;
    } else {
      reason = "invalid manifest optimization '" + std::string(spelling) +
          "'; expected O0 or O2";
      return false;
    }
  }
  if (selected_context.effective_build.debug_symbols.has_value()) {
    selected.emit_debug_symbols =
        *selected_context.effective_build.debug_symbols;
  }
  if (selected_context.effective_build.assertions.has_value()) {
    selected.runtime_assertions =
        *selected_context.effective_build.assertions
        ? draft::RuntimeAssertionMode::On
        : draft::RuntimeAssertionMode::Off;
  }
  if (!load_manifest_native_inputs(
          selected_context, selected.foreign_providers,
          selected.provider_summaries, selected.runtime_assets, reason)) {
    return false;
  }

  if (overrides.target.has_value()) selected.target = *overrides.target;
  if (overrides.output.has_value()) selected.output = overrides.output;
  if (overrides.artifact_kind.has_value()) {
    selected.artifact_kind = *overrides.artifact_kind;
  }
  if (overrides.optimization.has_value()) {
    selected.optimization = *overrides.optimization;
  }
  if (overrides.emit_debug_symbols.has_value()) {
    selected.emit_debug_symbols = *overrides.emit_debug_symbols;
  }
  if (overrides.runtime_assertions.has_value()) {
    selected.runtime_assertions = *overrides.runtime_assertions;
  }
  if (overrides.foreign_providers.has_value()) {
    selected.foreign_providers = *overrides.foreign_providers;
  }
  if (overrides.provider_summaries.has_value()) {
    selected.provider_summaries = *overrides.provider_summaries;
  }
  if (overrides.runtime_assets.has_value()) {
    selected.runtime_assets = *overrides.runtime_assets;
  }
  result = std::move(selected);
  return true;
}

[[nodiscard]] bool same_target(const draft::TargetProfile &left,
                               const draft::TargetProfile &right) {
  return left.facts.identity == right.facts.identity;
}

// All paths entering this helper are canonical. A component comparison avoids
// the textual-prefix bug where `/work/tools-two` appears to be below
// `/work/tools` while accepting the search directory itself.
[[nodiscard]] bool path_is_within_or_same(
    const std::filesystem::path &base,
    const std::filesystem::path &candidate) {
  if (base == candidate) return true;
  const std::filesystem::path relative = candidate.lexically_relative(base);
  if (relative.empty() || relative.is_absolute()) return false;
  return std::none_of(
      relative.begin(), relative.end(),
      [](const std::filesystem::path &component) { return component == ".."; });
}

[[nodiscard]] bool path_is_excluded(
    const std::filesystem::path &candidate,
    const std::vector<std::filesystem::path> &excluded) {
  return std::any_of(
      excluded.begin(), excluded.end(),
      [&candidate](const std::filesystem::path &root) {
        return path_is_within_or_same(root, candidate);
      });
}

// Foreign summaries are explicit semantic inputs for this invocation. They are
// parsed every time rather than being authenticated through a resolution
// manifest: a native artifact digest cannot describe its transitive runtime
// environment, and accepted Draft source does not own that environment.
[[nodiscard]] bool load_foreign_provider_audits(
    const std::vector<draft::ForeignProviderSummaryInput> &summary_inputs,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    std::vector<draft::ForeignProviderAudit> &audits,
    draft::DiagnosticSink &diagnostics) {
  return draft::load_foreign_provider_summaries(
      summary_inputs, foreign_providers, audits, diagnostics);
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
// `emit-llvm` additionally lowers MIR and prints one independently compilable
// complete LLVM module per semantic package without invoking object emission or
// a linker.
int compile_package(const std::string &package_spelling, bool emit_llvm,
                    const draft::TargetProfile &target,
                    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(package_spelling, workspace_directory,
                              selected_package, diagnostics)) {
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
      symbol_count += package.bodies.package.symbols.symbol_count();
      type_count += package.bodies.package.types.size();
      procedure_count += package.bodies.checked_procedures;
      agent_record_count += package.metadata.records.size();
      if (emit_llvm) {
        std::cout << "; ----- package "
                  << draft::display_package_identity(package.identity)
                  << " LLVM module -----\n"
                  << package.llvm_module.text;
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
// provider-free compilation. Native linker inputs and deployment assets are
// operational and do not enter this source projection. The command never
// invokes a linker, provider, validation command, or resolution-store mutation.
// The output directory must be absent so old files cannot survive a later graph
// and masquerade as part of the current expanded program.
int expand_package(
    const std::string &package_spelling,
    const std::filesystem::path &output_directory,
    const draft::TargetProfile &target,
    draft::RuntimeAssertionMode runtime_assertions,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(package_spelling, workspace_directory,
                              selected_package, diagnostics)) {
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
  bool materialized = false;
  if (compiled.ok && !diagnostics.has_errors()) {
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
    draft::NativeOptimizationLevel optimization,
    bool emit_debug_symbols,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings,
    const std::shared_ptr<draft::WorkExecutor> &work_executor,
    const draft::CompileWorkspaceResult &compiled,
    draft::DiagnosticSink &diagnostics,
    std::filesystem::path *published_output = nullptr) {
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
    output = artifact_directory /
        (package_name +
         (target.facts.object_format == "coff" ? ".exe" : ""));
    break;
  case draft::NativeArtifactKind::Object:
    output = artifact_directory /
        (package_name +
         (target.facts.object_format == "coff" ? ".obj" : ".o"));
    break;
  case draft::NativeArtifactKind::StaticLibrary:
    output = target.facts.object_format == "coff"
        ? artifact_directory / (package_name + ".lib")
        : artifact_directory / ("lib" + package_name + ".a");
    break;
  case draft::NativeArtifactKind::DynamicLibrary:
    if (target.facts.object_format == "coff") {
      output = artifact_directory / (package_name + ".dll");
    } else {
      output = artifact_directory /
          ("lib" + package_name +
           (target.facts.object_format == "elf" ? ".so" : ".dylib"));
    }
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
  native_options.optimization = optimization;
  native_options.emit_debug_symbols = emit_debug_symbols;
  native_options.foreign_providers = foreign_providers;
  native_options.runtime_assets = runtime_assets;
  native_options.timings = timings;
  native_options.work_executor = work_executor;
  const draft::NativeBuildResult built = draft::build_native_artifact(
      target, compiled, native_options, diagnostics);
  if (!built.ok)
    return false;
  if (published_output != nullptr) {
    *published_output = built.output_path;
  }
  std::cout << "built " << built.output_path << '\n';
  if (!built.debug_symbols_path.empty()) {
    std::cout << "debug symbols " << built.debug_symbols_path << '\n';
  }
  if (!built.import_library_path.empty()) {
    std::cout << "import library " << built.import_library_path << '\n';
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
    draft::NativeOptimizationLevel optimization,
    bool emit_debug_symbols,
    draft::RuntimeAssertionMode runtime_assertions,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings,
    const std::shared_ptr<draft::WorkExecutor> &work_executor,
    std::filesystem::path *published_output = nullptr) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
  compile_options.configuration.runtime_assertions = runtime_assertions;
  compile_options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(compile_options.workspace);
  if (!load_foreign_provider_audits(
          provider_summaries,
          foreign_providers,
          compile_options.foreign_provider_audits,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  compile_options.lower_mir = true;
  compile_options.emit_native_output = true;
  compile_options.native_output.output_kind =
      artifact_kind == draft::NativeArtifactKind::Assembly
      ? draft::LlvmNativeOutputKind::Assembly
      : draft::LlvmNativeOutputKind::Object;
  compile_options.native_output.optimization = optimization;
  compile_options.emit_debug_information = emit_debug_symbols;
  compile_options.emit_program_entry =
      artifact_kind == draft::NativeArtifactKind::Executable;
  compile_options.timings = timings;
  compile_options.work_executor = work_executor;
  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources, selected_package.physical_directory.string(),
          std::move(compile_options), diagnostics);
  const bool built =
      compiled.ok &&
      emit_native_package(workspace_directory, selected_package, target,
                          requested_output, artifact_kind, optimization,
                          emit_debug_symbols, foreign_providers, runtime_assets,
                          timings, work_executor, compiled, diagnostics,
                          published_output);
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return built && !diagnostics.has_errors() ? 0 : 1;
}

// Selects the complete target set for `build <path>`. Discovery recursively
// finds every ordinary package-level `main` below path for every artifact kind,
// which keeps assembly/object inspection useful for aggregate builds. When no
// program exists and a non-executable kind was requested, path is treated as
// one exact library package. Selection and every per-root configuration are
// complete before output validation, so a CLI `-o` or colliding manifest output
// is rejected for a multi-program build without writing an artifact.
int build_workspace(const CommandManifestContext &context,
                    const BuildCommandOverrides &overrides,
                    draft::TimingRecorder *timings) {
  // Root discovery, every selected compilation, and final native emission are
  // all phases of this one user command. Create its executor before discovery
  // so candidate-package parsing cannot silently start a temporary pool which
  // is then discarded before semantic compilation begins.
  std::shared_ptr<draft::WorkExecutor> work_executor =
      std::make_shared<draft::WorkExecutor>();
  draft::SourceManager discovery_sources;
  draft::DiagnosticSink discovery_diagnostics;
  const std::filesystem::path &workspace_directory =
      context.workspace_directory;
  const std::filesystem::path &search_directory = context.opened_directory;
  const draft::WorkspaceManifest &manifest = context.manifest;
  std::vector<std::filesystem::path> excluded_directories;
  for (const std::string &relative : manifest.excludes) {
    const std::filesystem::path candidate = workspace_directory / relative;
    std::error_code status_error;
    if (std::filesystem::is_directory(candidate, status_error)) {
      excluded_directories.push_back(candidate);
    } else if (status_error) {
      std::cerr << "error: cannot inspect excluded directory '"
                << candidate.string() << "': " << status_error.message()
                << '\n';
      return 1;
    }
  }

  EffectiveBuildConfiguration base_configuration;
  std::string configuration_error;
  if (!resolve_effective_build_configuration(
          context, nullptr, overrides, base_configuration,
          configuration_error)) {
    std::cerr << "error: " << configuration_error << '\n';
    return 2;
  }

  std::vector<draft::WorkspacePackageSelection> roots;
  draft::WorkspaceLoadOptions discovery_options;
  discovery_options.workspace_directory = workspace_directory.string();
  configure_core_distribution(discovery_options);
  discovery_options.package_options.file_tag =
      base_configuration.target.facts.file_tag;
  discovery_options.package_options.work_executor = work_executor.get();
  const draft::ExecutableRootDiscoveryResult discovered =
      draft::discover_executable_roots(discovery_sources, search_directory,
                                       discovery_options, discovery_diagnostics,
                                       excluded_directories);
  if (discovered.ok)
    roots = discovered.roots;

  // A named program may select a target whose file-qualified source differs
  // from the workspace default. Reinspect only those exact program packages;
  // unnamed packages retain the common discovery target. Program pointers are
  // sorted by root so manifest section order cannot change diagnostics or the
  // eventual canonical build order.
  std::vector<const draft::ProgramConfiguration *> configured_programs;
  configured_programs.reserve(manifest.programs.size());
  for (const draft::ProgramConfiguration &program : manifest.programs) {
    configured_programs.push_back(&program);
  }
  std::sort(
      configured_programs.begin(), configured_programs.end(),
      [](const draft::ProgramConfiguration *left,
         const draft::ProgramConfiguration *right) {
        return left->root < right->root;
      });
  for (const draft::ProgramConfiguration *program : configured_programs) {
    const std::filesystem::path lexical_program =
        (workspace_directory / program->root).lexically_normal();
    if (!path_is_within_or_same(search_directory, lexical_program) ||
        path_is_excluded(lexical_program, excluded_directories)) {
      continue;
    }

    EffectiveBuildConfiguration configured;
    if (!resolve_effective_build_configuration(
            context, program, overrides, configured, configuration_error)) {
      std::cerr << "error: program '" << program->name
                << "': " << configuration_error << '\n';
      return 2;
    }
    if (same_target(configured.target, base_configuration.target))
      continue;

    draft::WorkspacePackageSelection selected;
    if (!draft::select_workspace_package(
            workspace_directory, program->root, selected,
            discovery_diagnostics)) {
      continue;
    }
    roots.erase(
        std::remove_if(
            roots.begin(), roots.end(),
            [program](const draft::WorkspacePackageSelection &root) {
              return root.identity.root_relative_path == program->root;
            }),
        roots.end());

    draft::WorkspaceLoadOptions configured_options = discovery_options;
    configured_options.package_options.file_tag = configured.target.facts.file_tag;
    const draft::ExecutablePackageInspectionResult inspected =
        draft::inspect_executable_package(
            discovery_sources, selected, configured_options,
            discovery_diagnostics);
    if (inspected.ok && inspected.contains_main) {
      roots.push_back(std::move(selected));
    }
  }

  std::sort(
      roots.begin(), roots.end(),
      [](const draft::WorkspacePackageSelection &left,
         const draft::WorkspacePackageSelection &right) {
        return left.identity.root_relative_path <
            right.identity.root_relative_path;
      });
  roots.erase(
      std::unique(
          roots.begin(), roots.end(),
          [](const draft::WorkspacePackageSelection &left,
             const draft::WorkspacePackageSelection &right) {
            return left.identity == right.identity;
          }),
      roots.end());

  if (roots.empty() && !discovery_diagnostics.has_errors()) {
    draft::WorkspacePackageSelection root;
    if (draft::identify_workspace_package(workspace_directory, search_directory,
                                          root, discovery_diagnostics)) {
      const draft::ProgramConfiguration *program =
          draft::find_program_by_root(
              manifest, root.identity.root_relative_path);
      EffectiveBuildConfiguration exact_configuration;
      if (!resolve_effective_build_configuration(
              context, program, overrides, exact_configuration,
              configuration_error)) {
        std::cerr << "error: " << configuration_error << '\n';
        return 2;
      }
      if (exact_configuration.artifact_kind !=
          draft::NativeArtifactKind::Executable) {
        roots.push_back(std::move(root));
      }
    }
  }

  if (!discovery_diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(
        discovery_sources, discovery_diagnostics);
  }
  if (discovery_diagnostics.has_errors()) return 1;
  if (roots.empty()) {
    std::cerr << "error: build path contains no executable packages\n";
    return 1;
  }
  if (overrides.output.has_value() && roots.size() != 1) {
    std::cerr << "error: -o requires exactly one selected build root\n";
    return 2;
  }

  struct ConfiguredRoot {
    draft::WorkspacePackageSelection selection;
    EffectiveBuildConfiguration build;
  };
  std::vector<ConfiguredRoot> configured_roots;
  configured_roots.reserve(roots.size());
  for (const draft::WorkspacePackageSelection &root : roots) {
    const draft::ProgramConfiguration *program =
        draft::find_program_by_root(
            manifest, root.identity.root_relative_path);
    EffectiveBuildConfiguration build;
    if (!resolve_effective_build_configuration(
            context, program, overrides, build, configuration_error)) {
      std::cerr << "error: root '" << root.identity.root_relative_path
                << "': " << configuration_error << '\n';
      return 2;
    }
    configured_roots.push_back({root, std::move(build)});
  }

  // Explicit output paths have no automatic root namespace. Validate all of
  // them before the first compiler graph starts so one completed artifact can
  // never be overwritten by a later program in the same aggregate command.
  for (std::size_t index = 0; index < configured_roots.size(); ++index) {
    if (!configured_roots[index].build.output.has_value()) continue;
    std::error_code output_error;
    const std::filesystem::path output = std::filesystem::absolute(
        *configured_roots[index].build.output, output_error).lexically_normal();
    if (output_error) {
      std::cerr << "error: cannot make native output path absolute: "
                << output_error.message() << '\n';
      return 2;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (!configured_roots[previous].build.output.has_value()) continue;
      std::error_code previous_error;
      const std::filesystem::path previous_output =
          std::filesystem::absolute(
              *configured_roots[previous].build.output, previous_error)
              .lexically_normal();
      if (!previous_error && output == previous_output) {
        std::cerr << "error: roots '"
                  << configured_roots[previous]
                         .selection.identity.root_relative_path
                  << "' and '"
                  << configured_roots[index]
                         .selection.identity.root_relative_path
                  << "' select the same explicit output path\n";
        return 2;
      }
    }
  }

  // All selected roots deliberately own independent SourceManagers and
  // compiler product graphs. Only the sleeping worker set crosses those roots;
  // the executor retains no source, syntax, semantic, or artifact products.
  for (const ConfiguredRoot &root : configured_roots) {
    const int result = build_selected_package(
        workspace_directory,
        root.selection,
        root.build.target,
        root.build.output,
        root.build.artifact_kind,
        root.build.optimization,
        root.build.emit_debug_symbols,
        root.build.runtime_assertions,
        root.build.foreign_providers,
        root.build.provider_summaries,
        root.build.runtime_assets,
        timings,
        work_executor);
    if (result != 0) return result;
  }
  return 0;
}

// Builds one exact executable package, then replaces the compiler operation
// with an ordinary foreground child attached to the same terminal. Build
// workers and LLVM state are no longer active when the child starts. A launch
// failure is a compiler-command failure; an ordinary program exit is returned
// unchanged, while POSIX signal termination follows the conventional 128+N
// shell status solely as the draftc process exit value.
int run_package(
    const std::string &package_spelling, const draft::TargetProfile &target,
    const std::optional<std::string> &requested_output,
    draft::NativeOptimizationLevel optimization, bool emit_debug_symbols,
    draft::RuntimeAssertionMode runtime_assertions,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    const std::vector<std::string> &arguments,
    const std::optional<std::filesystem::path> &working_directory,
    const std::vector<std::string> &environment,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(package_spelling, workspace_directory,
                              selected_package, diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  std::shared_ptr<draft::WorkExecutor> work_executor =
      std::make_shared<draft::WorkExecutor>();
  std::filesystem::path executable;
  const int build_result = build_selected_package(
      workspace_directory, selected_package, target, requested_output,
      draft::NativeArtifactKind::Executable, optimization, emit_debug_symbols,
      runtime_assertions, foreign_providers, provider_summaries, runtime_assets,
      timings, work_executor, &executable);
  if (build_result != 0)
    return build_result;
  work_executor.reset();

  draft::ChildProcessOptions child;
  child.executable = executable;
  child.arguments = arguments;
  child.working_directory = working_directory;
  child.environment_overrides = environment;
  const draft::ChildProcessResult ran = draft::run_child_process(child);
  if (!ran.error.empty()) {
    std::cerr << "error: cannot run '" << executable.string()
              << "': " << ran.error << '\n';
    return 1;
  }
  if (!ran.started) {
    std::cerr << "error: child process did not start\n";
    return 1;
  }
  if (ran.exited)
    return ran.exit_code;
  return ran.signal > 0 && ran.signal < 128 ? 128 + ran.signal : 1;
}

int validate_package(
    const std::string &package_spelling, const draft::TargetProfile &target,
    draft::ValidationKind kind, draft::NativeOptimizationLevel optimization,
    const std::vector<draft::ValidationInstrumentationKind> &instrumentation,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    const std::vector<draft::RuntimeAssetInput> &runtime_assets,
    draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(package_spelling, workspace_directory,
                              selected_package, diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }

  draft::ValidationCommandOptions options;
  options.package_directory = selected_package.physical_directory;
  options.target = target;
  options.workspace.workspace_directory = workspace_directory.string();
  configure_core_distribution(options.workspace);
  if (!load_foreign_provider_audits(
          provider_summaries,
          foreign_providers,
          options.foreign_provider_audits,
          diagnostics)) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return 1;
  }
  options.kind = kind;
  options.optimization = optimization;
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

int emit_c_header_package(const std::string &package_spelling,
                          const draft::TargetProfile &target,
                          const std::optional<std::string> &requested_output,
                          draft::TimingRecorder *timings) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::filesystem::path workspace_directory;
  draft::WorkspacePackageSelection selected_package;
  if (!select_command_package(package_spelling, workspace_directory,
                              selected_package, diagnostics)) {
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
          compiled.packages[root]->bodies.package,
          compiled.packages[root]->c_abi,
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
  draft::NativeOptimizationLevel optimization =
      draft::NativeOptimizationLevel::O0;
  bool emit_debug_symbols = false;
};

// Resolve and judge first run the complete provider-independent front end, so
// malformed source, attachment-policy violations, and typed obligation errors
// are reported before any model call. Resolve may receive one explicit Codex
// adapter options; a program with only fresh pins still performs no synthesis
// provider call. Judge runs only after the same provider-free compilation and
// records evidence in its independent store without mutating source selection.
int run_agent_command(
    const std::string &package_spelling, AgentCommandKind command,
    const draft::TargetProfile &target, bool revalidate = false,
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
  if (!select_command_package(package_spelling, workspace_directory,
                              selected_package, diagnostics)) {
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
    if (!load_foreign_provider_audits(
            provider_summaries,
            foreign_providers,
            resolve_options.compile.foreign_provider_audits,
            diagnostics)) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
      return 1;
    }
    // Copy after external provider summaries have populated the semantic audit
    // set. The post-commit continuation must receive the same target,
    // configuration, and foreign facts as the graph produced by resolution;
    // only its backend and entry-point requests differ.
    if (resolve_build.has_value()) {
      build_options = resolve_options.compile;
      build_options->lower_mir = true;
      build_options->emit_native_output = true;
      build_options->native_output.output_kind =
          resolve_build->artifact_kind ==
                  draft::NativeArtifactKind::Assembly
              ? draft::LlvmNativeOutputKind::Assembly
              : draft::LlvmNativeOutputKind::Object;
      build_options->native_output.optimization =
          resolve_build->optimization;
      build_options->emit_debug_information =
          resolve_build->emit_debug_symbols;
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
                resolve_build->optimization,
                resolve_build->emit_debug_symbols,
                foreign_providers,
                runtime_assets,
                timings,
                build_options->work_executor,
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
  std::cerr
      << "usage:\n"
      << "  draftc lex <file.draft>\n"
      << "  draftc syntax <file.draft>\n"
      << "  draftc check <package>\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] "
         "[--timings|--timings=all]\n"
      << "  draftc emit-llvm <package>\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] "
         "[--timings|--timings=all]\n"
      << "  draftc emit-c-header <package> [-o <output.h>]\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc expand <package> --out <directory>\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [--assertions=off]\n"
      << "      [--provider name=object|archive|shared-library:<path>]...\n"
      << "      [--provider-summary name:<path>]...\n"
      << "      [--runtime-asset name:<file-or-directory>]...\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc build <package-or-directory> [-o <output>]\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [--kind "
         "executable|object|static-library|dynamic-library|assembly]\n"
      << "      [-O0|-O2]\n"
      << "      [--debug-symbols]\n"
      << "      [--assertions=off]\n"
      << "      [--provider name=object|archive|shared-library:<path>]...\n"
      << "      [--provider-summary name:<path>]...\n"
      << "      [--runtime-asset name:<file-or-directory>]...\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc run <package-or-workspace> [build options] [-- "
         "<argument>...]\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [-O0|-O2] [-o <output>] [--debug-symbols] [--assertions=off]\n"
      << "      [--cwd <directory>] [--env NAME=value]...\n"
      << "      [--provider name=object|archive|shared-library:<path>]...\n"
      << "      [--provider-summary name:<path>]...\n"
      << "      [--runtime-asset name:<file-or-directory>]...\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc test <package>\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [-O0|-O2]\n"
      << "      [--instrument "
         "address|lifetime|undefined-operation|allocator-poisoning|race]...\n"
      << "      [--provider name=object|archive|shared-library:<path>]...\n"
      << "      [--provider-summary name:<path>]...\n"
      << "      [--runtime-asset name:<file-or-directory>]...\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc bench <package> [--verify]\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [-O0|-O2]\n"
      << "      [--instrument "
         "address|lifetime|undefined-operation|allocator-poisoning|race]...\n"
      << "      [--provider name=object|archive|shared-library:<path>]...\n"
      << "      [--provider-summary name:<path>]...\n"
      << "      [--runtime-asset name:<file-or-directory>]...\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc resolve <package> [--revalidate] [--build]\n"
      << "      [--regenerate [site-id]]\n"
      << "      [-o <output>]\n"
      << "      [--kind "
         "executable|object|static-library|dynamic-library|assembly]\n"
      << "      [-O0|-O2] (with --build)\n"
      << "      [--debug-symbols] (with --build)\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [--assertions=off]\n"
      << "      [--model <model>]\n"
      << "      [--provider name=object|archive|shared-library:<path>]...\n"
      << "      [--provider-summary name:<path>]...\n"
      << "      [--runtime-asset name:<file-or-directory>]...\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc judge <package> [<selector>...] [--list]\n"
      << "      [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n"
      << "      [--assertions=off]\n"
      << "      [--judge-validator <identity>:<model>]...\n"
      << "      [--judge-artifact <kind>:<path>]...\n"
      << "      [--model <model>]\n"
      << "      [--provider name=object|archive|shared-library:<path>]...\n"
      << "      [--provider-summary name:<path>]...\n"
      << "      [--timings|--timings=all]\n"
      << "  draftc target [--target "
         "aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "lex") {
    return lex_file(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "syntax") {
    return parse_file(argv[2]);
  }

  CommandManifestContext command_context;
  std::string command_path;
  if (argc >= 3 && is_package_command(argv[1])) {
    std::string manifest_error;
    if (!prepare_command_manifest(argv[2], std::string_view(argv[1]) == "run",
                                  command_context, manifest_error)) {
      std::cerr << "error: " << manifest_error << '\n';
      return 1;
    }
    command_path = command_context.package_directory.string();
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
  if (command_context.effective_build.target.has_value() &&
      !select_command_target(*command_context.effective_build.target, target)) {
    return 2;
  }
  if (argc >= 3 && (std::string_view(argv[1]) == "check" ||
                    std::string_view(argv[1]) == "emit-llvm")) {
    bool target_set = false;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set && index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target))
          return 2;
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return compile_package(command_path,
                           std::string_view(argv[1]) == "emit-llvm", target,
                           timings);
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
        if (!select_command_target(argv[++index], target))
          return 2;
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return emit_c_header_package(command_path, target, output, timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "expand") {
    std::optional<std::filesystem::path> output_directory;
    bool assertions_off = false;
    bool target_set = false;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--out" && !output_directory.has_value() &&
          index + 1 < argc) {
        output_directory = argv[++index];
      } else if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target))
          return 2;
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!draft::parse_foreign_provider_input(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!draft::parse_foreign_provider_summary_input(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
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
    return expand_package(command_path, *output_directory, target,
                          assertions_off ? draft::RuntimeAssertionMode::Off
                                         : draft::RuntimeAssertionMode::On,
                          foreign_providers, provider_summaries, timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "resolve") {
    bool revalidate = false;
    bool regenerate = false;
    bool build_after_resolution = false;
    bool assertions_off = false;
    bool target_set = false;
    bool artifact_kind_set = false;
    bool optimization_set = false;
    bool emit_debug_symbols = false;
    std::optional<std::string> codex_model;
    std::optional<std::string> output;
    draft::NativeArtifactKind artifact_kind =
        draft::NativeArtifactKind::Executable;
    draft::NativeOptimizationLevel optimization =
        draft::NativeOptimizationLevel::O0;
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
        if (!select_command_target(argv[++index], target))
          return 2;
      } else if (argument == "--assertions=off" && !assertions_off) {
        assertions_off = true;
      } else if (argument == "-o" && !output.has_value() &&
                 index + 1 < argc) {
        output = argv[++index];
      } else if (argument == "--kind" && !artifact_kind_set &&
                 index + 1 < argc) {
        const std::optional<draft::NativeArtifactKind> parsed =
            draft::parse_native_artifact_kind(argv[++index]);
        if (!parsed.has_value()) {
          print_usage();
          return 2;
        }
        artifact_kind = *parsed;
        artifact_kind_set = true;
      } else if (argument.starts_with("-O")) {
        if (!parse_native_optimization_argument(
                argument, optimization_set, optimization)) {
          return 2;
        }
      } else if (argument == "--debug-symbols" &&
                 !emit_debug_symbols) {
        emit_debug_symbols = true;
      } else if (argument == "--model" &&
                 !codex_model.has_value() && index + 1 < argc) {
        codex_model = argv[++index];
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!draft::parse_foreign_provider_input(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!draft::parse_foreign_provider_summary_input(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!draft::parse_runtime_asset_input(argv[++index], asset, reason)) {
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
    if (!build_after_resolution &&
        (optimization_set || emit_debug_symbols)) {
      std::cerr << "error: -O0, -O2, and --debug-symbols require "
                   "resolve --build\n";
      return 2;
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
      resolve_build->optimization = optimization;
      resolve_build->emit_debug_symbols = emit_debug_symbols;
    }
    return run_agent_command(
        command_path, AgentCommandKind::Resolve, target, revalidate, regenerate,
        regeneration_site_identities, codex, foreign_providers,
        provider_summaries, runtime_assets, {}, {}, {}, false,
        assertions_off ? draft::RuntimeAssertionMode::Off
                       : draft::RuntimeAssertionMode::On,
        timings, resolve_build);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "judge") {
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
      if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target))
          return 2;
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
        if (!draft::parse_foreign_provider_input(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!draft::parse_foreign_provider_summary_input(
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
        command_path, AgentCommandKind::Judge, target, false, false, {}, codex,
        foreign_providers, provider_summaries, {}, judgment_validators,
        judgment_artifacts, judgment_selectors, list_judgments,
        assertions_off ? draft::RuntimeAssertionMode::Off
                       : draft::RuntimeAssertionMode::On,
        timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "run") {
    std::optional<std::string> output;
    bool output_set = false;
    draft::NativeOptimizationLevel optimization =
        draft::NativeOptimizationLevel::O0;
    bool optimization_set = false;
    bool emit_debug_symbols = false;
    bool debug_symbols_set = false;
    bool assertions_off = false;
    bool assertions_set = false;
    bool target_set = false;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    std::vector<draft::RuntimeAssetInput> runtime_assets;
    std::vector<std::string> program_arguments;
    std::optional<std::filesystem::path> working_directory;
    std::vector<std::string> environment;
    bool cli_providers = false;
    bool cli_provider_summaries = false;
    bool cli_runtime_assets = false;
    bool cli_environment = false;
    bool cli_working_directory = false;

    if (command_context.effective_build.output.has_value()) {
      output = manifest_output_path(command_context,
                                    *command_context.effective_build.output);
    }
    if (command_context.effective_build.optimization.has_value()) {
      const std::string spelling =
          "-" + *command_context.effective_build.optimization;
      if (!parse_native_optimization_argument(spelling, optimization_set,
                                              optimization)) {
        return 2;
      }
      optimization_set = false;
    }
    if (command_context.effective_build.debug_symbols.has_value()) {
      emit_debug_symbols = *command_context.effective_build.debug_symbols;
    }
    if (command_context.effective_build.assertions.has_value()) {
      assertions_off = !*command_context.effective_build.assertions;
    }
    std::string manifest_input_error;
    if (!load_manifest_native_inputs(command_context, foreign_providers,
                                     provider_summaries, runtime_assets,
                                     manifest_input_error)) {
      std::cerr << "error: " << manifest_input_error << '\n';
      return 2;
    }
    if (command_context.program != nullptr) {
      program_arguments = command_context.program->arguments;
      environment = command_context.program->environment;
      if (command_context.program->working_directory.has_value()) {
        const std::filesystem::path configured(
            *command_context.program->working_directory);
        working_directory =
            configured.is_absolute()
                ? configured
                : command_context.workspace_directory / configured;
      }
    }

    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--") {
        program_arguments.clear();
        for (++index; index < argc; ++index) {
          program_arguments.emplace_back(argv[index]);
        }
        break;
      }
      if (argument == "--target" && !target_set && index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target))
          return 2;
      } else if (argument == "--assertions=off" && !assertions_set) {
        assertions_off = true;
        assertions_set = true;
      } else if (argument.starts_with("-O")) {
        if (!parse_native_optimization_argument(argument, optimization_set,
                                                optimization)) {
          return 2;
        }
      } else if (argument == "--debug-symbols" && !debug_symbols_set) {
        emit_debug_symbols = true;
        debug_symbols_set = true;
      } else if (argument == "-o" && !output_set && index + 1 < argc) {
        output = argv[++index];
        output_set = true;
      } else if (argument == "--cwd" && !cli_working_directory &&
                 index + 1 < argc) {
        std::error_code error;
        working_directory = std::filesystem::absolute(argv[++index], error);
        if (error) {
          std::cerr << "error: cannot make run working directory absolute: "
                    << error.message() << '\n';
          return 2;
        }
        cli_working_directory = true;
      } else if (argument == "--env" && index + 1 < argc) {
        if (!cli_environment) {
          environment.clear();
          cli_environment = true;
        }
        environment.emplace_back(argv[++index]);
      } else if (argument == "--provider" && index + 1 < argc) {
        if (!cli_providers) {
          foreign_providers.clear();
          cli_providers = true;
        }
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!draft::parse_foreign_provider_input(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        if (!cli_provider_summaries) {
          provider_summaries.clear();
          cli_provider_summaries = true;
        }
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!draft::parse_foreign_provider_summary_input(argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        if (!cli_runtime_assets) {
          runtime_assets.clear();
          cli_runtime_assets = true;
        }
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!draft::parse_runtime_asset_input(argv[++index], asset, reason)) {
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
    if (working_directory.has_value()) {
      std::error_code error;
      const std::filesystem::path canonical =
          std::filesystem::canonical(*working_directory, error);
      if (error || !std::filesystem::is_directory(canonical)) {
        std::cerr << "error: run working directory is unavailable\n";
        return 2;
      }
      working_directory = canonical;
    }
    return run_package(
        command_path, target, output, optimization, emit_debug_symbols,
        assertions_off ? draft::RuntimeAssertionMode::Off
                       : draft::RuntimeAssertionMode::On,
        foreign_providers, provider_summaries, runtime_assets,
        program_arguments, working_directory, environment, timings);
  }
  if (argc >= 3 && (std::string_view(argv[1]) == "test" ||
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
    draft::NativeOptimizationLevel optimization =
        draft::NativeOptimizationLevel::O0;
    bool optimization_set = false;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target))
          return 2;
      } else if (argument == "--verify" && !verify &&
                 validation_kind == draft::ValidationKind::Benchmark) {
        // Bench always executes and records fresh evidence. `--verify` is an
        // explicit release/CI spelling retained for readable automation.
        verify = true;
      } else if (argument == "--provider" && index + 1 < argc) {
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!draft::parse_foreign_provider_input(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        foreign_providers.push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!draft::parse_foreign_provider_summary_input(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        provider_summaries.push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!draft::parse_runtime_asset_input(argv[++index], asset, reason)) {
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
      } else if (argument.starts_with("-O")) {
        if (!parse_native_optimization_argument(
                argument, optimization_set, optimization)) {
          return 2;
        }
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return validate_package(command_path, target, validation_kind, optimization,
                            instrumentation, foreign_providers,
                            provider_summaries, runtime_assets, timings);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "build") {
    BuildCommandOverrides overrides;
    bool output_set = false;
    draft::NativeArtifactKind parsed_artifact_kind =
        draft::NativeArtifactKind::Executable;
    bool artifact_kind_set = false;
    draft::NativeOptimizationLevel parsed_optimization =
        draft::NativeOptimizationLevel::O0;
    bool optimization_set = false;
    bool debug_symbols_set = false;
    bool assertions_set = false;
    bool target_set = false;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--target" && !target_set &&
                 index + 1 < argc) {
        target_set = true;
        if (!select_command_target(argv[++index], target))
          return 2;
        overrides.target = target;
      } else if (argument == "--assertions=off" && !assertions_set) {
        assertions_set = true;
        overrides.runtime_assertions = draft::RuntimeAssertionMode::Off;
      } else if (argument == "--kind" && !artifact_kind_set &&
                 index + 1 < argc) {
        const std::optional<draft::NativeArtifactKind> parsed =
            draft::parse_native_artifact_kind(argv[++index]);
        if (!parsed.has_value()) {
          print_usage();
          return 2;
        }
        parsed_artifact_kind = *parsed;
        artifact_kind_set = true;
        overrides.artifact_kind = parsed_artifact_kind;
      } else if (argument.starts_with("-O")) {
        if (!parse_native_optimization_argument(
                argument, optimization_set, parsed_optimization)) {
          return 2;
        }
        overrides.optimization = parsed_optimization;
      } else if (argument == "--debug-symbols" && !debug_symbols_set) {
        debug_symbols_set = true;
        overrides.emit_debug_symbols = true;
      } else if (argument == "-o" && !output_set && index + 1 < argc) {
        overrides.output = std::string(argv[++index]);
        output_set = true;
      } else if (argument == "--provider" && index + 1 < argc) {
        if (!overrides.foreign_providers.has_value())
          overrides.foreign_providers.emplace();
        draft::ForeignProviderInput provider;
        std::string reason;
        if (!draft::parse_foreign_provider_input(argv[++index], provider, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        overrides.foreign_providers->push_back(std::move(provider));
      } else if (argument == "--provider-summary" && index + 1 < argc) {
        if (!overrides.provider_summaries.has_value())
          overrides.provider_summaries.emplace();
        draft::ForeignProviderSummaryInput summary;
        std::string reason;
        if (!draft::parse_foreign_provider_summary_input(
                argv[++index], summary, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        overrides.provider_summaries->push_back(std::move(summary));
      } else if (argument == "--runtime-asset" && index + 1 < argc) {
        if (!overrides.runtime_assets.has_value())
          overrides.runtime_assets.emplace();
        draft::RuntimeAssetInput asset;
        std::string reason;
        if (!draft::parse_runtime_asset_input(argv[++index], asset, reason)) {
          std::cerr << "error: " << reason << '\n';
          return 2;
        }
        overrides.runtime_assets->push_back(std::move(asset));
      } else if (is_timing_argument(argument)) {
        continue;
      } else {
        print_usage();
        return 2;
      }
    }
    return build_workspace(command_context, overrides, timings);
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
