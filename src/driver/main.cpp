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
#include "compile/compiler.h"
#include "compile/resolver.h"
#include "elaborator/codex_cli.h"
#include "elaborator/resolution_store.h"
#include "interop/c_header.h"
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

[[nodiscard]] bool absolute_locked_roots(
    const std::string &toolchain,
    const std::string &sdk,
    draft::LockedNativeInputRoots &roots,
    std::string &reason) {
  std::error_code error;
  roots.toolchain_root =
      std::filesystem::absolute(toolchain, error).lexically_normal();
  if (error) {
    reason = "cannot make toolchain root absolute: " + error.message();
    return false;
  }
  roots.sdk_root = std::filesystem::absolute(sdk, error).lexically_normal();
  if (error) {
    reason = "cannot make SDK root absolute: " + error.message();
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
int print_target() {
  const draft::TargetProfile profile = draft::make_aarch64_macos_profile();
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
int compile_package(const std::string &directory, bool emit_llvm) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error);
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(options.workspace);
  options.lower_mir = emit_llvm;
  options.emit_llvm = emit_llvm;
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
    const std::optional<std::string> &requested_output,
    draft::NativeArtifactKind artifact_kind,
    bool allow_host_toolchain,
    const std::optional<draft::LockedNativeInputRoots> &locked_inputs,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries,
    bool require_test_evidence,
    bool require_benchmark_evidence) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error);
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
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
  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          absolute_directory.string(),
          std::move(compile_options),
          diagnostics);
  if (compiled.ok) {
    auto verify_evidence = [&](draft::ValidationKind kind) {
      draft::ValidationEvidenceRequirement requirement;
      requirement.package_directory = absolute_directory;
      requirement.target = target;
      requirement.workspace.workspace_directory =
          absolute_directory.parent_path().string();
      configure_core_distribution(requirement.workspace);
      requirement.kind = kind;
      requirement.foreign_provider_audits =
          compiled.foreign_provider_audits;
      draft::Sha256Digest active;
      if (draft::verify_active_validation_evidence(
              sources,
              std::move(requirement),
              active,
              diagnostics)) {
        std::cout << "verified " << draft::validation_kind_name(kind)
                  << " evidence " << active.hex() << '\n';
      }
    };
    if (require_test_evidence) {
      verify_evidence(draft::ValidationKind::Test);
    }
    if (require_benchmark_evidence) {
      verify_evidence(draft::ValidationKind::Benchmark);
    }
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
      output = build_directory / ("lib" + package_name + ".dylib");
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
    native_options.allow_unpinned_toolchain = allow_host_toolchain;
    native_options.foreign_providers = foreign_providers;
    if (locked_inputs.has_value()) {
      native_options.locked = true;
      native_options.locked_inputs = *locked_inputs;
    }
    if (!diagnostics.has_errors()) {
      const draft::NativeBuildResult built = draft::build_native_artifact(
          target, compiled, native_options, diagnostics);
      if (built.ok) {
        std::cout << "built " << built.output_path << '\n';
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
    draft::ValidationKind kind,
    bool allow_host_toolchain,
    const std::optional<draft::LockedNativeInputRoots> &locked_inputs,
    const std::vector<draft::ForeignProviderInput> &foreign_providers,
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error).lexically_normal();
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }

  draft::ValidationCommandOptions options;
  options.package_directory = absolute_directory;
  options.target = draft::make_aarch64_macos_profile();
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
  options.allow_unpinned_toolchain = allow_host_toolchain;
  options.foreign_providers = foreign_providers;
  if (locked_inputs.has_value()) {
    options.locked = true;
    options.locked_inputs = *locked_inputs;
  }
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
    const std::optional<std::string> &requested_output) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error);
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(options.workspace);
  options.emit_program_entry = false;
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
      const draft::CHeaderResult header = draft::emit_c_header(
          compiled.packages[root]->semantics.package, {}, diagnostics);
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

// Resolve and judge first run the complete provider-independent front end, so
// malformed source, attachment-policy violations, and typed obligation errors
// are reported before any model call. Resolve may receive one explicit Codex
// adapter configuration; a program with only fresh pins still performs no
// provider call. Judge remains a typed no-op boundary until its separate
// evidence protocol is implemented.
int run_agent_command(
    const std::string &directory,
    AgentCommandKind command,
    bool revalidate = false,
    bool allow_host_toolchain = false,
    const std::optional<draft::CodexCliProviderOptions> &codex = std::nullopt,
    const std::optional<draft::LockedNativeInputRoots> &locked_inputs =
        std::nullopt,
    const std::vector<draft::ForeignProviderInput> &foreign_providers = {},
    const std::vector<draft::ForeignProviderSummaryInput> &provider_summaries = {}) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error);
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  configure_core_distribution(options.workspace);
  if (command == AgentCommandKind::Resolve) {
    draft::ResolveWorkspaceOptions resolve_options;
    resolve_options.compile = std::move(options);
    resolve_options.revalidate = revalidate;
    resolve_options.cancellation_requested = command_cancellation_requested;
    const bool external_inputs_configured = locked_inputs.has_value() ||
        !foreign_providers.empty() || !provider_summaries.empty();
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
      if (locked_inputs.has_value() &&
          !draft::pin_locked_native_inputs(
              *locked_inputs,
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
    ResolutionValidationRunnerState validation_state;
    validation_state.options.package_directory = absolute_directory;
    validation_state.options.target = resolve_options.compile.target;
    validation_state.options.allow_unpinned_toolchain = allow_host_toolchain;
    validation_state.options.foreign_providers = foreign_providers;
    if (locked_inputs.has_value()) {
      validation_state.options.locked = true;
      validation_state.options.locked_inputs = *locked_inputs;
    }
    resolve_options.validation_runner.state = &validation_state;
    resolve_options.validation_runner.run =
        run_resolution_candidate_validation;
    const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
        sources,
        absolute_directory.string(),
        std::move(resolve_options),
        diagnostics);
    if (resolved.ok) {
      if (!resolved.committed) {
        std::cout << "no synthesis sites require resolution\n";
      } else {
        std::cout << "resolved " << resolved.manifest.pins.size()
                  << " synthesis sites (" << resolved.synthesized_sites
                  << " synthesized, " << resolved.reused_sites
                  << " reused); passed " << resolved.tested_procedures
                  << " tests and " << resolved.benchmarked_procedures
                  << " benchmarks before commit\n";
      }
    }
    if (!diagnostics.diagnostics().empty()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    return resolved.ok && !diagnostics.has_errors() ? 0 : 1;
  }

  const draft::CompileWorkspaceResult compiled =
      draft::compile_workspace_with_resolution(
          sources,
          absolute_directory.string(),
          std::move(options),
          diagnostics);
  std::size_t matching_sites = 0;
  if (compiled.ok) {
    for (const std::optional<draft::CompiledPackage> &package :
         compiled.packages) {
      if (!package.has_value()) continue;
      for (const draft::AgentObligation &obligation :
           package->obligations.obligations) {
        if (command == AgentCommandKind::Judge &&
            obligation.kind == draft::AgentConstructKind::Judgment) {
          ++matching_sites;
        }
      }
    }
    if (matching_sites == 0) {
      std::cout << "no judgment sites require execution\n";
    } else {
      diagnostics.error(
          draft::SourceRange::invalid(),
          "judgment provider is not configured");
    }
  }
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

void print_usage() {
  std::cerr << "usage:\n"
            << "  draftc lex <file.draft>\n"
            << "  draftc syntax <file.draft>\n"
            << "  draftc check <package-directory>\n"
            << "  draftc emit-llvm <package-directory>\n"
            << "  draftc emit-c-header <package-directory> [-o <output.h>]\n"
            << "  draftc build <package-directory> [-o <output>]\n"
            << "      [--kind executable|object|static-library|dynamic-library|assembly]\n"
            << "      [--allow-host-toolchain]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "  draftc build <package-directory> --locked\n"
            << "      --toolchain-root <directory> --sdk-root <directory> [-o <output>]\n"
            << "      [--require-test-evidence] [--require-benchmark-evidence]\n"
            << "  draftc test <package-directory> [--allow-host-toolchain]\n"
            << "      [--locked --toolchain-root <directory> --sdk-root <directory>]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "  draftc bench <package-directory> [--verify] [--allow-host-toolchain]\n"
            << "      [--locked --toolchain-root <directory> --sdk-root <directory>]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "  draftc resolve <package-directory> [--revalidate]\n"
            << "      [--codex-executable <path> --codex-model <model>]\n"
            << "      [--allow-host-toolchain]\n"
            << "      [--toolchain-root <directory> --sdk-root <directory>]\n"
            << "      [--provider name=object|archive|shared-library:<path>]...\n"
            << "      [--provider-summary name:<path>]...\n"
            << "  draftc judge <package-directory>\n"
            << "  draftc target\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "lex") {
    return lex_file(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "syntax") {
    return parse_file(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "check") {
    return compile_package(argv[2], false);
  }
  if (argc == 3 && std::string_view(argv[1]) == "emit-llvm") {
    return compile_package(argv[2], true);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "emit-c-header") {
    std::optional<std::string> output;
    if (argc == 5 && std::string_view(argv[3]) == "-o") {
      output = argv[4];
    } else if (argc != 3) {
      print_usage();
      return 2;
    }
    return emit_c_header_package(argv[2], output);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "resolve") {
    bool revalidate = false;
    bool allow_host_toolchain = false;
    std::optional<std::string> codex_executable;
    std::optional<std::string> codex_model;
    std::optional<std::string> toolchain_root;
    std::optional<std::string> sdk_root;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--revalidate" && !revalidate) {
        revalidate = true;
      } else if (argument == "--allow-host-toolchain" &&
                 !allow_host_toolchain) {
        allow_host_toolchain = true;
      } else if (argument == "--codex-executable" &&
                 !codex_executable.has_value() && index + 1 < argc) {
        codex_executable = argv[++index];
      } else if (argument == "--codex-model" &&
                 !codex_model.has_value() && index + 1 < argc) {
        codex_model = argv[++index];
      } else if (argument == "--toolchain-root" &&
                 !toolchain_root.has_value() && index + 1 < argc) {
        toolchain_root = argv[++index];
      } else if (argument == "--sdk-root" &&
                 !sdk_root.has_value() && index + 1 < argc) {
        sdk_root = argv[++index];
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
      } else {
        print_usage();
        return 2;
      }
    }
    if (codex_executable.has_value() != codex_model.has_value() ||
        toolchain_root.has_value() != sdk_root.has_value() ||
        (revalidate && codex_executable.has_value()) ||
        (allow_host_toolchain && toolchain_root.has_value())) {
      print_usage();
      return 2;
    }
    cancellation_signal = 0;
    (void)std::signal(SIGINT, request_cancellation);
    std::optional<draft::CodexCliProviderOptions> codex;
    if (codex_executable.has_value()) {
      codex.emplace();
      codex->executable = *codex_executable;
      codex->model = *codex_model;
      codex->cancellation_requested = command_cancellation_requested;
    }
    std::optional<draft::LockedNativeInputRoots> locked_inputs;
    if (toolchain_root.has_value()) {
      locked_inputs.emplace();
      std::string reason;
      if (!absolute_locked_roots(
              *toolchain_root, *sdk_root, *locked_inputs, reason)) {
        std::cerr << "error: " << reason << '\n';
        return 1;
      }
    }
    return run_agent_command(
        argv[2],
        AgentCommandKind::Resolve,
        revalidate,
        allow_host_toolchain,
        codex,
        locked_inputs,
        foreign_providers,
        provider_summaries);
  }
  if (argc == 3 && std::string_view(argv[1]) == "judge") {
    return run_agent_command(argv[2], AgentCommandKind::Judge);
  }
  if (argc >= 3 &&
      (std::string_view(argv[1]) == "test" ||
       std::string_view(argv[1]) == "bench")) {
    const draft::ValidationKind validation_kind =
        std::string_view(argv[1]) == "test"
        ? draft::ValidationKind::Test
        : draft::ValidationKind::Benchmark;
    bool allow_host_toolchain = false;
    bool locked = false;
    bool verify = false;
    std::optional<std::string> toolchain_root;
    std::optional<std::string> sdk_root;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--allow-host-toolchain" && !allow_host_toolchain) {
        allow_host_toolchain = true;
      } else if (argument == "--locked" && !locked) {
        locked = true;
      } else if (argument == "--verify" && !verify &&
                 validation_kind == draft::ValidationKind::Benchmark) {
        // Bench executes and records fresh evidence; --verify is the explicit
        // release/CI spelling distinguished from locked build-time evidence
        // reuse, which never enters this command path.
        verify = true;
      } else if (argument == "--toolchain-root" &&
                 !toolchain_root.has_value() && index + 1 < argc) {
        toolchain_root = argv[++index];
      } else if (argument == "--sdk-root" &&
                 !sdk_root.has_value() && index + 1 < argc) {
        sdk_root = argv[++index];
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
      } else {
        print_usage();
        return 2;
      }
    }
    if (toolchain_root.has_value() != sdk_root.has_value() ||
        locked != toolchain_root.has_value() ||
        (locked && allow_host_toolchain)) {
      print_usage();
      return 2;
    }
    std::optional<draft::LockedNativeInputRoots> locked_inputs;
    if (locked) {
      locked_inputs.emplace();
      std::string reason;
      if (!absolute_locked_roots(
              *toolchain_root, *sdk_root, *locked_inputs, reason)) {
        std::cerr << "error: " << reason << '\n';
        return 1;
      }
    }
    return validate_package(
        argv[2],
        validation_kind,
        allow_host_toolchain,
        locked_inputs,
        foreign_providers,
        provider_summaries);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "build") {
    std::optional<std::string> output;
    draft::NativeArtifactKind artifact_kind =
        draft::NativeArtifactKind::Executable;
    bool artifact_kind_set = false;
    bool allow_host_toolchain = false;
    bool locked = false;
    bool require_test_evidence = false;
    bool require_benchmark_evidence = false;
    std::optional<std::string> toolchain_root;
    std::optional<std::string> sdk_root;
    std::vector<draft::ForeignProviderInput> foreign_providers;
    std::vector<draft::ForeignProviderSummaryInput> provider_summaries;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--allow-host-toolchain") {
        allow_host_toolchain = true;
      } else if (argument == "--require-test-evidence" &&
                 !require_test_evidence) {
        require_test_evidence = true;
      } else if (argument == "--require-benchmark-evidence" &&
                 !require_benchmark_evidence) {
        require_benchmark_evidence = true;
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
      } else if (argument == "--locked" && !locked) {
        locked = true;
      } else if (argument == "--toolchain-root" &&
                 !toolchain_root.has_value() && index + 1 < argc) {
        toolchain_root = argv[++index];
      } else if (argument == "--sdk-root" &&
                 !sdk_root.has_value() && index + 1 < argc) {
        sdk_root = argv[++index];
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
      } else {
        print_usage();
        return 2;
      }
    }
    if (toolchain_root.has_value() != sdk_root.has_value() ||
        locked != toolchain_root.has_value() ||
        (locked && allow_host_toolchain) ||
        ((require_test_evidence || require_benchmark_evidence) && !locked)) {
      print_usage();
      return 2;
    }
    std::optional<draft::LockedNativeInputRoots> locked_inputs;
    if (locked) {
      locked_inputs.emplace();
      std::string reason;
      if (!absolute_locked_roots(
              *toolchain_root, *sdk_root, *locked_inputs, reason)) {
        std::cerr << "error: " << reason << '\n';
        return 1;
      }
    }
    return build_package(
        argv[2],
        output,
        artifact_kind,
        allow_host_toolchain,
        locked_inputs,
        foreign_providers,
        provider_summaries,
        require_test_evidence,
        require_benchmark_evidence);
  }
  if (argc == 2 && std::string_view(argv[1]) == "target") {
    return print_target();
  }
  print_usage();
  return 2;
}
