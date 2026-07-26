// See compiler_session.h for ownership and transaction boundaries. This file
// keeps the implementation deliberately direct: one speculative graph copy,
// one full-topology fallback, one last-good publication point, and one native
// artifact operation. There is no cache, query framework, background thread,
// or IDE-specific semantic representation.

#include "ide/compiler_session.h"

#include "syntax/lexer.h"
#include "syntax/token.h"
#include "workspace/embedded_core.h"
#include "workspace/manifest.h"
#include "workspace/package.h"
#include "workspace/selection.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace draft::ide {
namespace {

[[nodiscard]] bool is_keyword(TokenKind kind) {
  return kind >= TokenKind::KeywordPackage && kind <= TokenKind::KeywordNil;
}

[[nodiscard]] bool is_string(TokenKind kind) {
  return kind == TokenKind::StringLiteral ||
         kind == TokenKind::RawStringLiteral || kind == TokenKind::RuneLiteral;
}

[[nodiscard]] bool is_number(TokenKind kind) {
  return kind == TokenKind::IntegerLiteral || kind == TokenKind::FloatLiteral;
}

[[nodiscard]] bool begins_declaration(const std::vector<ToolingToken> &tokens,
                                      std::size_t index) {
  if (tokens[index].token_class != ToolingTokenClass::Syntax ||
      tokens[index].kind != TokenKind::Identifier) {
    return false;
  }
  for (std::size_t next = index + 1; next < tokens.size(); ++next) {
    if (tokens[next].token_class != ToolingTokenClass::Syntax)
      continue;
    return tokens[next].kind == TokenKind::Colon ||
           tokens[next].kind == TokenKind::ColonColon ||
           tokens[next].kind == TokenKind::ColonEqual;
  }
  return false;
}

void append_location(std::string &output, const SourceManager &sources,
                     SourceRange range) {
  if (!range.is_valid())
    return;
  const SourceFile &file = sources.file(range.begin.file);
  const LineColumn coordinate = sources.line_column(range.begin);
  output += " @ ";
  output += std::filesystem::path(file.display_path).filename().string();
  output += ':';
  output += std::to_string(coordinate.line);
  output += ':';
  output += std::to_string(coordinate.column);
}

[[nodiscard]] std::string_view symbol_name(const SymbolTable &symbols,
                                           SymbolId id) {
  if (!id.is_valid() || id.value >= symbols.symbol_count())
    return "<unknown>";
  return symbols.symbol(id).name;
}

[[nodiscard]] SourceRange syntax_range(const LoadedPackage &loaded,
                                       SyntaxReference syntax) {
  if (!syntax.file.is_valid() || !syntax.node.is_valid()) {
    return SourceRange::invalid();
  }
  for (const LoadedPackageFile &file : loaded.files) {
    if (file.source != syntax.file || !file.syntax.has_value())
      continue;
    return file.syntax->node(syntax.node).range;
  }
  return SourceRange::invalid();
}

// SemanticSymbol is a command-local identity. SymbolId values are package
// local, so every navigation comparison carries the owning compiled-package
// index. It is never serialized or exposed through the C facade.
struct SemanticSymbol {
  std::size_t package = 0;
  SymbolId symbol;

  bool operator==(const SemanticSymbol &) const = default;
};

// reference_name_range narrows a resolved HIR expression to its final authored
// identifier token. A member expression's HIR range includes its base
// (`value.field`), while navigation must select only `field`. Ordinary body HIR
// does not retain SyntaxReference on every expression, but its source range and
// the parsed file's ordered token vector are exact. Binary search plus the few
// tokens inside that expression is therefore O(log tokens), avoiding both text
// reparsing in the IDE and an expression-by-syntax-node quadratic scan.
[[nodiscard]] SourceRange
reference_name_range(const LoadedPackage &loaded,
                     const HirExpression &expression) {
  if (!expression.range.is_valid())
    return SourceRange::invalid();
  for (const LoadedPackageFile &file : loaded.files) {
    if (file.source != expression.range.begin.file || !file.syntax.has_value())
      continue;
    const std::vector<Token> &tokens = file.syntax->tokens();
    auto token = std::lower_bound(
        tokens.begin(), tokens.end(), expression.range.begin.offset,
        [](const Token &candidate, std::uint32_t offset) {
          return candidate.range.end.offset <= offset;
        });
    SourceRange final_identifier = SourceRange::invalid();
    for (; token != tokens.end() &&
           token->range.begin.offset < expression.range.end.offset;
         ++token) {
      if (token->kind == TokenKind::Identifier &&
          token->range.begin.offset >= expression.range.begin.offset &&
          token->range.end.offset <= expression.range.end.offset) {
        final_identifier = token->range;
      }
    }
    if (final_identifier.is_valid())
      return final_identifier;
  }
  return expression.range;
}

[[nodiscard]] std::optional<std::size_t>
compiled_package_index(const CompileWorkspaceResult &compiled,
                       const PackageIdentity &identity) {
  for (std::size_t index = 0; index < compiled.graph.packages.size(); ++index) {
    if (compiled.graph.packages[index].identity == identity)
      return index;
  }
  return std::nullopt;
}

// canonical_symbol follows a consumer-local ImportedSymbol proxy to the
// defining package's public declaration. Local variables, fields, parameters,
// and package declarations remain in their current package. Public concrete
// instances may use linkage_name rather than their source name, so both stable
// declaration spellings participate in the provider lookup.
[[nodiscard]] std::optional<SemanticSymbol>
canonical_symbol(const CompileWorkspaceResult &compiled,
                 std::size_t package_index, SymbolId symbol) {
  if (package_index >= compiled.packages.size() ||
      !compiled.packages[package_index].has_value()) {
    return std::nullopt;
  }
  const SemanticPackage &semantic =
      compiled.packages[package_index]->bodies.package;
  if (!symbol.is_valid() || symbol.value >= semantic.symbols.symbol_count())
    return std::nullopt;

  for (const ImportedSymbol &imported : semantic.imported_symbols_for_read()) {
    if (imported.proxy != symbol)
      continue;
    const PackageIdentity owner{
        imported.root_identity,
        imported.root_relative_path,
    };
    const std::optional<std::size_t> owner_index =
        compiled_package_index(compiled, owner);
    if (!owner_index.has_value() || *owner_index >= compiled.packages.size() ||
        !compiled.packages[*owner_index].has_value()) {
      return std::nullopt;
    }
    const SymbolTable &owner_symbols =
        compiled.packages[*owner_index]->bodies.package.symbols;
    for (std::size_t candidate = 0; candidate < owner_symbols.symbol_count();
         ++candidate) {
      const Symbol &definition =
          owner_symbols.symbol(SymbolId{static_cast<std::uint32_t>(candidate)});
      if (definition.name == imported.public_name ||
          (!definition.linkage_name.empty() &&
           definition.linkage_name == imported.public_name)) {
        return SemanticSymbol{
            *owner_index,
            SymbolId{static_cast<std::uint32_t>(candidate)},
        };
      }
    }
    return std::nullopt;
  }
  return SemanticSymbol{package_index, symbol};
}

[[nodiscard]] bool same_foreign_effect(const ForeignAuditEffect &left,
                                       const ForeignAuditEffect &right) {
  return left.kind == right.kind && left.root_identity == right.root_identity &&
         left.root_relative_path == right.root_relative_path &&
         left.declaration == right.declaration && left.detail == right.detail &&
         left.flow_parameter == right.flow_parameter &&
         left.flow_path == right.flow_path &&
         left.flow_context == right.flow_context;
}

[[nodiscard]] bool same_foreign_audit(const ForeignProviderAudit &left,
                                      const ForeignProviderAudit &right) {
  if (left.provider != right.provider ||
      left.symbols.size() != right.symbols.size()) {
    return false;
  }
  for (std::size_t symbol = 0; symbol < left.symbols.size(); ++symbol) {
    const ForeignAuditSymbol &left_symbol = left.symbols[symbol];
    const ForeignAuditSymbol &right_symbol = right.symbols[symbol];
    if (left_symbol.linker_name != right_symbol.linker_name ||
        left_symbol.effects.size() != right_symbol.effects.size()) {
      return false;
    }
    for (std::size_t effect = 0; effect < left_symbol.effects.size();
         ++effect) {
      if (!same_foreign_effect(left_symbol.effects[effect],
                               right_symbol.effects[effect])) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool
same_program_policy(const EffectiveProgramConfiguration &left,
                    const EffectiveProgramConfiguration &right) {
  if (left.build.target.facts.identity != right.build.target.facts.identity ||
      left.build.artifact_kind != right.build.artifact_kind ||
      left.build.optimization != right.build.optimization ||
      left.build.runtime_assertions != right.build.runtime_assertions ||
      left.build.emit_debug_symbols != right.build.emit_debug_symbols ||
      left.build.output_path != right.build.output_path ||
      left.build.foreign_providers.size() !=
          right.build.foreign_providers.size() ||
      left.foreign_provider_audits.size() !=
          right.foreign_provider_audits.size() ||
      left.build.runtime_assets.size() != right.build.runtime_assets.size() ||
      left.arguments != right.arguments ||
      left.environment != right.environment ||
      left.working_directory != right.working_directory) {
    return false;
  }
  for (std::size_t index = 0; index < left.build.foreign_providers.size();
       ++index) {
    const ForeignProviderInput &left_input =
        left.build.foreign_providers[index];
    const ForeignProviderInput &right_input =
        right.build.foreign_providers[index];
    if (left_input.provider != right_input.provider ||
        left_input.kind != right_input.kind ||
        left_input.path != right_input.path) {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.foreign_provider_audits.size();
       ++index) {
    if (!same_foreign_audit(left.foreign_provider_audits[index],
                            right.foreign_provider_audits[index])) {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.build.runtime_assets.size();
       ++index) {
    if (left.build.runtime_assets[index].name !=
            right.build.runtime_assets[index].name ||
        left.build.runtime_assets[index].path !=
            right.build.runtime_assets[index].path) {
      return false;
    }
  }
  return true;
}

} // namespace

CompilerSession::CompilerSession(CompilerConfiguration configuration)
    : configuration_(std::move(configuration)),
      fallback_target_(configuration_.target) {
  source_path_ = configuration_.root_package_directory /
                 configuration_.source_relative_name;
}

CompilerSession::~CompilerSession() {
  std::error_code error;
  std::filesystem::remove_all(build_directory_, error);
}

bool CompilerSession::resolve_program_configuration(
    const WorkspaceManifest &manifest, std::string_view root,
    EffectiveProgramConfiguration &result, DiagnosticSink &diagnostics) const {
  BuildDefaults defaults = effective_build_defaults(manifest, root);
  EffectiveProgramConfiguration resolved;
  std::string reason;

  // A user-selected target is an IDE command override. Otherwise the manifest
  // may specialize each named root, with the create-time host target remaining
  // the fallback for roots that say nothing. Remove the lower-priority target
  // spelling before shared interpretation so an explicitly selected target is
  // a genuine replacement, not a post-parse patch.
  if (configuration_.target_is_explicit)
    defaults.target.reset();
  if (!resolve_build_policy(configuration_.workspace_directory, defaults,
                            configuration_.target, resolved.build, reason)) {
    diagnostics.error(SourceRange::invalid(),
                      "program '" + std::string(root) + "': " + reason);
    return false;
  }
  if (!load_foreign_provider_summaries(
          resolved.build.provider_summaries, resolved.build.foreign_providers,
          resolved.foreign_provider_audits, diagnostics)) {
    return false;
  }

  const ProgramConfiguration *program = find_program_by_root(manifest, root);
  if (program != nullptr) {
    // Run rows do not affect checking or native publication. Preserve their
    // source order and make the optional directory process-facing, but defer
    // existence/changeability to the actual F5 launch so an unused program's
    // stale run directory cannot prevent the workspace from opening.
    resolved.arguments = program->arguments;
    resolved.environment = program->environment;
    if (program->working_directory.has_value()) {
      const std::filesystem::path spelling(*program->working_directory);
      const std::filesystem::path candidate =
          spelling.is_absolute()
              ? spelling
              : configuration_.workspace_directory / spelling;
      std::error_code error;
      const std::filesystem::path absolute =
          std::filesystem::absolute(candidate, error).lexically_normal();
      if (error) {
        diagnostics.error(SourceRange::invalid(),
                          "program '" + std::string(root) +
                              "' working directory cannot be made absolute: " +
                              error.message());
        return false;
      }
      resolved.working_directory = absolute;
    }
  }
  result = std::move(resolved);
  return true;
}

bool CompilerSession::read_workspace_manifest(
    WorkspaceManifest &manifest, DiagnosticSink &diagnostics) const {
  std::string manifest_error;
  const std::filesystem::path manifest_path =
      configuration_.workspace_directory / WorkspaceManifestName;
  std::error_code marker_error;
  const std::filesystem::file_status marker_status =
      std::filesystem::symlink_status(manifest_path, marker_error);
  const bool marker_missing =
      marker_error == std::errc::no_such_file_or_directory ||
      (!marker_error && !std::filesystem::exists(marker_status));
  if (marker_error && !marker_missing) {
    diagnostics.error(SourceRange::invalid(),
                      "cannot inspect workspace manifest: " +
                          marker_error.message());
    return false;
  }
  const bool marker_present = !marker_missing;
  if (marker_present && (std::filesystem::is_symlink(marker_status) ||
                         !std::filesystem::is_regular_file(marker_status))) {
    diagnostics.error(SourceRange::invalid(),
                      "workspace manifest must be a regular non-symlink file");
    return false;
  }
  if (!load_workspace_manifest(marker_present ? manifest_path
                                              : std::filesystem::path{},
                               manifest, manifest_error)) {
    diagnostics.error(SourceRange::invalid(), std::move(manifest_error));
    return false;
  }
  return true;
}

bool CompilerSession::refresh_root_options(const WorkspaceManifest &manifest,
                                           DiagnosticSink &diagnostics) {
  SourceManager discovery_sources;
  WorkspaceLoadOptions workspace_options;
  workspace_options.workspace_directory =
      configuration_.workspace_directory.string();
  workspace_options.core_files = embedded_core_files();
  workspace_options.core_content_identity = embedded_core_content_identity();
  workspace_options.package_options.file_tag =
      configuration_.target.facts.file_tag;
  // Root discovery and source inventory are part of this long-lived workspace
  // session. Reuse the same workers later used by checks and native builds;
  // otherwise opening a workspace would create short-lived pools before the
  // compiler session proper had begun.
  workspace_options.package_options.work_executor = work_executor_.get();
  std::vector<std::filesystem::path> excluded;
  if (!resolve_workspace_exclusions(configuration_.workspace_directory,
                                    manifest.excludes, excluded, diagnostics))
    return false;

  // Resolve and select every named program before fallback discovery. Their
  // exact directories are skipped as fallback-target candidates but not as
  // traversal roots, then inspected once below under their effective target.
  // This prevents an irrelevant malformed target-qualified file from making a
  // correctly configured program impossible to open.
  struct ConfiguredProgramSelection {
    const ProgramConfiguration *program = nullptr;
    EffectiveProgramConfiguration configuration;
    WorkspacePackageSelection selection;
  };
  std::vector<const ProgramConfiguration *> configured_programs;
  configured_programs.reserve(manifest.programs.size());
  for (const ProgramConfiguration &program : manifest.programs)
    configured_programs.push_back(&program);
  std::sort(
      configured_programs.begin(), configured_programs.end(),
      [](const ProgramConfiguration *left, const ProgramConfiguration *right) {
        return left->root < right->root;
      });
  std::vector<ConfiguredProgramSelection> configured;
  std::vector<std::filesystem::path> independently_inspected;
  configured.reserve(configured_programs.size());
  independently_inspected.reserve(configured_programs.size());
  for (const ProgramConfiguration *program : configured_programs) {
    ConfiguredProgramSelection row;
    row.program = program;
    if (!resolve_program_configuration(manifest, program->root,
                                       row.configuration, diagnostics) ||
        !select_workspace_package(configuration_.workspace_directory,
                                  program->root, row.selection, diagnostics)) {
      return false;
    }
    independently_inspected.push_back(row.selection.physical_directory);
    configured.push_back(std::move(row));
  }
  const ExecutableRootDiscoveryResult discovered = discover_executable_roots(
      discovery_sources, configuration_.workspace_directory, workspace_options,
      diagnostics, excluded, independently_inspected);
  if (!discovered.ok || diagnostics.has_errors())
    return false;

  std::vector<WorkspacePackageSelection> selections = discovered.roots;
  // Recursive discovery uses one fallback target. Named programs are exact
  // roots and may select target-qualified source of their own, so reinspect
  // those packages under the same effective configuration later used by
  // Check/F5. Manifest section order cannot affect the visible root order.
  for (ConfiguredProgramSelection &row : configured) {
    WorkspaceLoadOptions program_options = workspace_options;
    program_options.package_options.file_tag =
        row.configuration.build.target.facts.file_tag;
    const ExecutablePackageInspectionResult inspected =
        inspect_executable_package(discovery_sources, row.selection,
                                   program_options, diagnostics);
    if (!inspected.ok || diagnostics.has_errors())
      return false;
    selections.erase(
        std::remove_if(selections.begin(), selections.end(),
                       [&row](const WorkspacePackageSelection &candidate) {
                         return candidate.identity.root_relative_path ==
                                row.program->root;
                       }),
        selections.end());
    if (inspected.contains_main)
      selections.push_back(std::move(row.selection));
  }
  std::sort(selections.begin(), selections.end(),
            [](const WorkspacePackageSelection &left,
               const WorkspacePackageSelection &right) {
              return left.identity.root_relative_path <
                     right.identity.root_relative_path;
            });
  selections.erase(std::unique(selections.begin(), selections.end(),
                               [](const WorkspacePackageSelection &left,
                                  const WorkspacePackageSelection &right) {
                                 return left.identity == right.identity;
                               }),
                   selections.end());

  const bool has_configured_root = !configuration_.root_relative_path.empty();
  const bool contains_current =
      has_configured_root &&
      std::any_of(selections.begin(), selections.end(),
                  [this](const WorkspacePackageSelection &selection) {
                    return selection.identity.root_relative_path ==
                           configuration_.root_relative_path;
                  });
  if (has_configured_root && !contains_current) {
    WorkspacePackageSelection current;
    current.identity.root_identity = "workspace";
    current.identity.root_relative_path = configuration_.root_relative_path;
    current.physical_directory = configuration_.root_package_directory;
    selections.push_back(std::move(current));
    std::sort(selections.begin(), selections.end(),
              [](const WorkspacePackageSelection &left,
                 const WorkspacePackageSelection &right) {
                return left.identity.root_relative_path <
                       right.identity.root_relative_path;
              });
  }

  std::vector<RootOption> options;
  options.reserve(selections.size());
  std::size_t selected = 0;
  for (const WorkspacePackageSelection &selection : selections) {
    EffectiveProgramConfiguration program_configuration;
    const auto configured_row = std::lower_bound(
        configured.begin(), configured.end(),
        selection.identity.root_relative_path,
        [](const ConfiguredProgramSelection &row, std::string_view root) {
          return row.program->root < root;
        });
    if (configured_row != configured.end() &&
        configured_row->program->root ==
            selection.identity.root_relative_path) {
      // Named rows were resolved before target-sensitive root inspection. Move
      // that exact snapshot into the published option instead of rereading its
      // provider summaries and reparsing native mappings a second time. A root
      // refresh is therefore one coherent observation of external policy.
      program_configuration = std::move(configured_row->configuration);
    } else if (!resolve_program_configuration(
                   manifest, selection.identity.root_relative_path,
                   program_configuration, diagnostics)) {
      return false;
    }
    PackageLoadOptions package_options = workspace_options.package_options;
    package_options.file_tag =
        program_configuration.build.target.facts.file_tag;
    const PackageLoadResult loaded =
        load_package(discovery_sources, selection.physical_directory.string(),
                     package_options, diagnostics);
    if (!loaded.ok || diagnostics.has_errors())
      return false;

    std::string source_name;
    std::vector<SourceOption> source_options;
    for (const LoadedPackageFile &file : loaded.package.files) {
      if (file.kind != PackageFileKind::DraftSource)
        continue;
      std::string display_name;
      if (selection.identity.root_relative_path == ".") {
        display_name = file.relative_name;
      } else {
        display_name =
            selection.identity.root_relative_path + "/" + file.relative_name;
      }
      source_options.push_back({
          std::move(display_name),
          selection.physical_directory / file.relative_name,
          selection.identity,
          file.relative_name,
      });
      const bool source_preference_applies =
          !has_configured_root || selection.identity.root_relative_path ==
                                      configuration_.root_relative_path;
      if (source_preference_applies &&
          file.relative_name == configuration_.source_relative_name) {
        source_name = file.relative_name;
        continue;
      }
      if (source_name.empty())
        source_name = file.relative_name;
    }
    if (source_name.empty()) {
      diagnostics.error(SourceRange::invalid(),
                        "workspace root '" +
                            selection.identity.root_relative_path +
                            "' has no selected Draft source file");
      return false;
    }
    if (selection.identity.root_relative_path ==
        configuration_.root_relative_path) {
      selected = options.size();
    }
    options.push_back({
        selection.identity.root_relative_path,
        selection.physical_directory,
        std::move(source_name),
        std::move(source_options),
        std::move(program_configuration),
    });
  }

  if (options.empty()) {
    diagnostics.error(SourceRange::invalid(),
                      "workspace contains no executable Draft root; select a "
                      "root explicitly when opening a library workspace");
    return false;
  }

  root_options_ = std::move(options);
  selected_root_ = selected;
  return true;
}

bool CompilerSession::initialize(DiagnosticSink &diagnostics) {
  WorkspaceManifest manifest;
  if (!read_workspace_manifest(manifest, diagnostics) ||
      !refresh_root_options(manifest, diagnostics)) {
    return false;
  }
  manifest_ = std::move(manifest);
  const RootOption &selected = root_options_[selected_root_];
  configuration_.root_relative_path = selected.root_relative_path;
  configuration_.root_package_directory = selected.physical_directory;
  configuration_.source_relative_name = selected.source_relative_name;
  source_path_ = selected.physical_directory / selected.source_relative_name;
  select_root_sources(selected);
  return create_build_directory(diagnostics);
}

bool CompilerSession::refresh_configuration(DiagnosticSink &diagnostics) {
  WorkspaceManifest manifest;
  if (!read_workspace_manifest(manifest, diagnostics))
    return false;

  if (!same_workspace_manifest(manifest_, manifest)) {
    // A saved manifest is one atomic operator-policy replacement. Rebuild the
    // root table before publishing any portion of it, preserve the current root
    // by identity when it still exists, and discard semantic products because
    // target, assertions, or denial inputs may have changed.
    if (!refresh_root_options(manifest, diagnostics))
      return false;
    manifest_ = std::move(manifest);
    const RootOption selected = root_options_[selected_root_];
    configuration_.root_relative_path = selected.root_relative_path;
    configuration_.root_package_directory = selected.physical_directory;
    configuration_.source_relative_name = selected.source_relative_name;
    source_path_ = selected.physical_directory / selected.source_relative_name;
    reset_checked_program();
    select_root_sources(selected);
    return true;
  }

  // Summary files are external semantic inputs and can change without changing
  // draft.workspace. Re-resolve the active row on each foreground operation;
  // structural equality retains the checked graph when those bytes and all
  // ordinary build/run policy remain unchanged.
  EffectiveProgramConfiguration resolved;
  if (!resolve_program_configuration(
          manifest, root_options_[selected_root_].root_relative_path, resolved,
          diagnostics)) {
    return false;
  }
  if (!same_program_policy(root_options_[selected_root_].program, resolved)) {
    root_options_[selected_root_].program = std::move(resolved);
    reset_checked_program();
  }
  return true;
}

bool CompilerSession::create_build_directory(DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::path temporary_root =
      std::filesystem::temp_directory_path(error);
  if (error) {
    diagnostics.error(SourceRange::invalid(),
                      "cannot locate a temporary directory for Turbo Draft: " +
                          error.message());
    return false;
  }

  // create_directory is the cross-process arbitration operation. Time makes a
  // collision unusual; the bounded suffix search makes collision correctness
  // independent of clock precision, process scheduling, and thread timing.
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  constexpr std::size_t kMaximumAttempts = 1024;
  for (std::size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    const std::filesystem::path candidate =
        temporary_root / ("draftide-build-" + std::to_string(nonce) + "-" +
                          std::to_string(attempt));
    error.clear();
    if (std::filesystem::create_directory(candidate, error)) {
      build_directory_ = candidate;
      return true;
    }
    if (error && error != std::errc::file_exists) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create a temporary Turbo Draft build directory: " +
              error.message());
      return false;
    }
  }
  diagnostics.error(
      SourceRange::invalid(),
      "cannot reserve a unique temporary Turbo Draft build directory");
  return false;
}

void CompilerSession::reset_checked_program() {
  last_good_sources_ = SourceManager{};
  last_good_.reset();
  latest_check_succeeded_ = false;
  navigation_definition_.reset();
  navigation_usages_.clear();
  syntax_spans_.clear();
  for (std::string &text : tooling_text_)
    text.clear();
  package_tree_rows_.clear();
  diagnostics_text_.clear();
  diagnostic_count_ = 0;
  built_output_path_.clear();
}

const EffectiveProgramConfiguration &CompilerSession::active_program() const {
  // Every public operation occurs after initialize has published one selected
  // root. Keeping the assertion here makes accidental pre-initialization use a
  // compiler invariant instead of silently choosing unrelated defaults.
  assert(!root_options_.empty() && selected_root_ < root_options_.size());
  return root_options_[selected_root_].program;
}

std::filesystem::path CompilerSession::default_output_path() const {
  const EffectiveProgramConfiguration &program = active_program();
  switch (program.build.artifact_kind) {
  case NativeArtifactKind::Executable:
    return build_directory_ /
           (program.build.target.facts.object_format == "coff" ? "program.exe"
                                                               : "program");
  case NativeArtifactKind::Object:
    return build_directory_ /
           (program.build.target.facts.object_format == "coff" ? "program.obj"
                                                               : "program.o");
  case NativeArtifactKind::StaticLibrary:
    return program.build.target.facts.object_format == "coff"
               ? build_directory_ / "program.lib"
               : build_directory_ / "libprogram.a";
  case NativeArtifactKind::DynamicLibrary:
    if (program.build.target.facts.object_format == "coff")
      return build_directory_ / "program.dll";
    return build_directory_ / (program.build.target.facts.object_format == "elf"
                                   ? "libprogram.so"
                                   : "libprogram.dylib");
  case NativeArtifactKind::Assembly:
    return build_directory_ / "program-assembly";
  }
  return build_directory_ / "program";
}

void CompilerSession::select_root_sources(const RootOption &root) {
  source_options_ = root.sources;
}

void CompilerSession::rebuild_source_options() {
  if (!last_good_.has_value())
    return;
  std::vector<SourceOption> sources;
  for (const WorkspacePackage &package : last_good_->graph.packages) {
    // Core and pinned dependency files are inspectable compiler inputs but are
    // not editable members of this workspace. The editor source projection
    // exposes only paths whose semantic root is the user's open workspace.
    if (package.identity.root_identity != "workspace")
      continue;
    for (const LoadedPackageFile &file : package.loaded.files) {
      if (file.kind != PackageFileKind::DraftSource)
        continue;
      std::string display_name;
      if (package.identity.root_relative_path == ".") {
        display_name = file.relative_name;
      } else {
        display_name =
            package.identity.root_relative_path + "/" + file.relative_name;
      }
      sources.push_back({
          std::move(display_name),
          std::filesystem::path(package.loaded.physical_directory) /
              file.relative_name,
          package.identity,
          file.relative_name,
      });
    }
  }
  std::sort(sources.begin(), sources.end(),
            [](const SourceOption &left, const SourceOption &right) {
              return left.display_name < right.display_name;
            });
  source_options_ = std::move(sources);
  if (selected_root_ < root_options_.size())
    root_options_[selected_root_].sources = source_options_;
}

std::size_t CompilerSession::root_count() const { return root_options_.size(); }

std::size_t CompilerSession::selected_root() const { return selected_root_; }

std::string_view CompilerSession::root_name(std::size_t index) const {
  if (index >= root_options_.size())
    return {};
  return root_options_[index].root_relative_path;
}

bool CompilerSession::select_root(std::size_t index,
                                  DiagnosticSink &diagnostics) {
  if (index >= root_options_.size()) {
    diagnostics.error(SourceRange::invalid(),
                      "selected workspace root is out of range");
    return false;
  }
  const RootOption &root = root_options_[index];
  configuration_.root_relative_path = root.root_relative_path;
  configuration_.root_package_directory = root.physical_directory;
  configuration_.source_relative_name = root.source_relative_name;
  source_path_ = root.physical_directory / root.source_relative_name;
  selected_root_ = index;
  reset_checked_program();
  select_root_sources(root);
  return true;
}

bool CompilerSession::select_target(TargetProfile target,
                                    DiagnosticSink &diagnostics) {
  WorkspaceManifest manifest;
  if (!read_workspace_manifest(manifest, diagnostics))
    return false;
  const CompilerConfiguration previous_configuration = configuration_;
  const TargetProfile previous_fallback = fallback_target_;
  const WorkspaceManifest previous_manifest = manifest_;
  const std::vector<RootOption> previous_options = root_options_;
  const std::size_t previous_selected = selected_root_;
  configuration_.target = std::move(target);
  configuration_.target_is_explicit = true;
  fallback_target_ = configuration_.target;
  if (!refresh_root_options(manifest, diagnostics)) {
    configuration_ = previous_configuration;
    fallback_target_ = previous_fallback;
    manifest_ = previous_manifest;
    root_options_ = previous_options;
    selected_root_ = previous_selected;
    return false;
  }
  manifest_ = std::move(manifest);
  const RootOption selected = root_options_[selected_root_];
  configuration_.root_relative_path = selected.root_relative_path;
  configuration_.root_package_directory = selected.physical_directory;
  configuration_.source_relative_name = selected.source_relative_name;
  source_path_ = selected.physical_directory / selected.source_relative_name;
  reset_checked_program();
  select_root_sources(selected);
  return true;
}

const TargetProfile &CompilerSession::target() const {
  return active_program().build.target;
}

bool CompilerSession::target_is_explicit() const {
  return configuration_.target_is_explicit;
}

const TargetProfile &CompilerSession::fallback_target() const {
  return fallback_target_;
}

std::size_t CompilerSession::source_count() const {
  return source_options_.size();
}

std::string_view CompilerSession::source_name(std::size_t index) const {
  if (index >= source_options_.size())
    return {};
  return source_options_[index].display_name;
}

const std::filesystem::path &
CompilerSession::source_path(std::size_t index) const {
  static const std::filesystem::path empty;
  if (index >= source_options_.size())
    return empty;
  return source_options_[index].physical_path;
}

CompileWorkspaceOptions CompilerSession::compile_options() const {
  CompileWorkspaceOptions options;
  const EffectiveProgramConfiguration &program = active_program();
  options.target = program.build.target;
  options.workspace.workspace_directory =
      configuration_.workspace_directory.string();
  options.workspace.core_files = embedded_core_files();
  options.workspace.core_content_identity = embedded_core_content_identity();
  options.configuration.runtime_assertions = program.build.runtime_assertions
                                                 ? RuntimeAssertionMode::On
                                                 : RuntimeAssertionMode::Off;
  options.emit_program_entry =
      program.build.artifact_kind == NativeArtifactKind::Executable;
  options.foreign_provider_audits = program.foreign_provider_audits;
  options.work_executor = work_executor_;
  return options;
}

std::optional<std::vector<WorkspaceSourceOverride>>
CompilerSession::source_overrides(std::span<const SourceOverlay> overlays,
                                  DiagnosticSink &diagnostics) const {
  std::vector<WorkspaceSourceOverride> result;
  result.reserve(overlays.size());
  for (const SourceOverlay &overlay : overlays) {
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(overlay.physical_path, error);
    if (error) {
      diagnostics.error(SourceRange::invalid(),
                        "cannot identify an editor source path: " +
                            error.message());
      return std::nullopt;
    }
    const auto option =
        std::find_if(source_options_.begin(), source_options_.end(),
                     [&canonical](const SourceOption &source) {
                       return source.physical_path == canonical;
                     });
    if (option == source_options_.end()) {
      diagnostics.error(
          SourceRange::invalid(),
          "editor source is not in the active checked package graph: " +
              canonical.string());
      return std::nullopt;
    }
    const bool duplicate = std::any_of(
        result.begin(), result.end(),
        [&option](const WorkspaceSourceOverride &existing) {
          return existing.identity == option->identity &&
                 existing.source.relative_name == option->relative_name;
        });
    if (duplicate) {
      diagnostics.error(SourceRange::invalid(),
                        "editor supplied the same source overlay twice");
      return std::nullopt;
    }
    WorkspaceSourceOverride converted;
    converted.identity = option->identity;
    converted.source.relative_name = option->relative_name;
    converted.source.contents = std::string(overlay.contents);
    result.push_back(std::move(converted));
  }
  return result;
}

void CompilerSession::collect_syntax_spans(const SourceOverlay &active) {
  syntax_spans_.clear();
  SourceManager sources;
  const FileId file = sources.add_source(active.physical_path.string(),
                                         std::string(active.contents));
  DiagnosticSink ignored_diagnostics;
  const std::vector<ToolingToken> tokens =
      lex_source_for_tooling(sources, file, ignored_diagnostics);
  syntax_spans_.reserve(tokens.size());
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const ToolingToken &token = tokens[index];
    SyntaxStyle style = SyntaxStyle::Plain;
    if (token.token_class == ToolingTokenClass::LineComment ||
        token.token_class == ToolingTokenClass::BlockComment) {
      style = SyntaxStyle::Comment;
    } else if (token.kind == TokenKind::Invalid) {
      style = SyntaxStyle::Invalid;
    } else if (is_keyword(token.kind)) {
      style = SyntaxStyle::Keyword;
    } else if (is_string(token.kind)) {
      style = SyntaxStyle::String;
    } else if (is_number(token.kind)) {
      style = SyntaxStyle::Number;
    } else if (begins_declaration(tokens, index)) {
      style = SyntaxStyle::Declaration;
    }
    if (style == SyntaxStyle::Plain || !token.range.is_valid())
      continue;
    syntax_spans_.push_back({
        token.range.begin.offset,
        token.range.end.offset,
        style,
    });
  }
}

void CompilerSession::colorize(const SourceOverlay &source) {
  // Keep this public operation as a named phase boundary rather than exposing
  // the span vector to mutation. collect_syntax_spans owns the exact mapping
  // from production tokens to the IDE's intentionally small style vocabulary.
  collect_syntax_spans(source);
}

void CompilerSession::rebuild_tooling_index() {
  for (std::string &text : tooling_text_)
    text.clear();
  package_tree_rows_.clear();
  if (!last_good_.has_value())
    return;

  std::string &packages =
      tooling_text_[static_cast<std::size_t>(ToolingSection::Packages)];
  std::string &declarations =
      tooling_text_[static_cast<std::size_t>(ToolingSection::Declarations)];
  std::string &references =
      tooling_text_[static_cast<std::size_t>(ToolingSection::References)];
  std::string &effects =
      tooling_text_[static_cast<std::size_t>(ToolingSection::Effects)];
  std::string &denials =
      tooling_text_[static_cast<std::size_t>(ToolingSection::Denials)];

  const CompileWorkspaceResult &compiled = *last_good_;

  // Package rows use graph order and import rows use source order, both of
  // which are deterministic compiler products. The view never re-enumerates
  // the filesystem or constructs a second dependency graph.
  packages += "Target: ";
  packages += active_program().build.target.facts.file_tag;
  packages += "\nPackages and dependencies\n";
  for (std::size_t package_index = 0;
       package_index < compiled.graph.packages.size(); ++package_index) {
    const WorkspacePackage &workspace_package =
        compiled.graph.packages[package_index];
    const bool root = package_index == compiled.graph.root_package.value;
    std::string package_label =
        display_package_identity(workspace_package.identity);
    package_label += " (";
    package_label += workspace_package.loaded.short_name;
    package_label += ')';
    package_tree_rows_.push_back({
        std::move(package_label),
        package_index,
        PackageTreeRowKind::Package,
        root,
        false,
    });
    const std::size_t package_row_index = package_tree_rows_.size() - 1;
    packages += root ? "* " : "  ";
    packages += display_package_identity(workspace_package.identity);
    packages += " (";
    packages += workspace_package.loaded.short_name;
    packages += ")\n";
    for (const PackageImport &import : compiled.graph.imports) {
      if (import.importing_package.value != package_index)
        continue;
      package_tree_rows_[package_row_index].has_children = true;
      packages += "    -> ";
      packages += display_package_identity(
          compiled.graph.package(import.imported_package).identity);
      packages += " via ";
      packages += import.path;
      packages += '\n';

      std::string import_label = import.path;
      import_label += " -> ";
      import_label += display_package_identity(
          compiled.graph.package(import.imported_package).identity);
      package_tree_rows_.push_back({
          std::move(import_label),
          package_index,
          PackageTreeRowKind::Import,
          false,
          false,
      });
    }
  }

  declarations += "Declarations and visibility\n";
  references += "References and calls\n";
  effects += "Procedure effects\n";
  denials += "Denial regions\n";
  for (std::size_t package_index = 0; package_index < compiled.packages.size();
       ++package_index) {
    if (!compiled.packages[package_index].has_value())
      continue;
    const CompiledPackage &package = *compiled.packages[package_index];
    const SemanticPackage &semantic = package.bodies.package;
    const SymbolTable &symbols = semantic.symbols;
    const std::string identity = display_package_identity(package.identity);

    declarations += "\n[" + identity + "]\n";
    const std::vector<SymbolId> package_symbols =
        symbols.symbols_in_scope(semantic.package_scope);
    for (SymbolId id : package_symbols) {
      const Symbol &symbol = symbols.symbol(id);
      declarations +=
          symbol.visibility == Visibility::Public ? "public " : "private ";
      declarations += symbol_kind_name(symbol.kind);
      declarations += ' ';
      declarations += symbol.name;
      if (symbol.flags.foreign)
        declarations += " [foreign]";
      if (symbol.flags.exported)
        declarations += " [export]";
      if (symbol.flags.parametric)
        declarations += " [parametric]";
      append_location(declarations, last_good_sources_, symbol.name_range);
      declarations += '\n';
    }

    references += "\n[" + identity + "]\n";
    for (std::size_t work_index : package.selected_procedure_work) {
      if (work_index >= package.bodies.procedures.size())
        continue;
      const ProcedureBodyHirResult &procedure =
          package.bodies.procedures[work_index];
      const std::string_view owner = symbol_name(symbols, procedure.symbol);
      for (std::size_t expression_index = 0;
           expression_index < procedure.program.expression_count();
           ++expression_index) {
        const HirExpression &expression = procedure.program.expression(
            HirExpressionId{static_cast<std::uint32_t>(expression_index)});
        if (!expression.symbol.is_valid())
          continue;
        references += "ref ";
        references += owner;
        references += " -> ";
        references += symbol_name(symbols, expression.symbol);
        append_location(references, last_good_sources_, expression.range);
        references += '\n';
      }
    }
    for (const DirectProcedureEffectSummary &procedure :
         package.direct_effects.procedures) {
      for (const ProcedureInvocationSummary &call :
           procedure.direct_invocations) {
        references += "call ";
        references += symbol_name(symbols, procedure.procedure);
        references += " -> ";
        references += symbol_name(symbols, call.callee);
        references += '\n';
      }
      for (const ProcedureFlowInvocationSummary &call :
           procedure.direct_flow_calls) {
        static_cast<void>(call);
        references += "call ";
        references += symbol_name(symbols, procedure.procedure);
        references += " -> <procedure value>\n";
      }
    }

    effects += "\n[" + identity + "]\n";
    for (const ProcedureEffectSummary &procedure : package.effects.procedures) {
      effects += symbol_name(symbols, procedure.procedure);
      if (procedure.effects.empty()) {
        effects += ": none\n";
        continue;
      }
      effects += ":\n";
      for (const SemanticEffect &effect : procedure.effects) {
        effects += "  - ";
        effects += effect_kind_name(effect.kind);
        if (!effect.declaration.empty()) {
          effects += " ";
          effects += effect.declaration;
        } else if (!effect.text.empty()) {
          effects += " ";
          effects += effect.text;
        }
        effects += '\n';
      }
    }

    denials += "\n[" + identity + "]\n";
    const AppendOnlyTableView<DeclarationDenial> package_denials =
        semantic.declaration_denials_for_read();
    if (package_denials.empty())
      denials += "none\n";
    for (const DeclarationDenial &denial : package_denials) {
      denials += "deny on ";
      denials += symbol_name(symbols, denial.declaration);
      append_location(
          denials, last_good_sources_,
          syntax_range(compiled.graph.packages[package_index].loaded,
                       denial.denial));
      denials += '\n';
    }
  }
}

void CompilerSession::publish_diagnostics(const SourceManager &sources,
                                          const DiagnosticSink &diagnostics) {
  diagnostics_text_ = render_diagnostics(sources, diagnostics);
  diagnostic_count_ = static_cast<std::uint32_t>(std::min<std::size_t>(
      diagnostics.error_count(), std::numeric_limits<std::uint32_t>::max()));
}

bool CompilerSession::fresh_check(
    const std::vector<WorkspaceSourceOverride> &overrides,
    SourceManager &candidate_sources, CompileWorkspaceResult &candidate,
    DiagnosticSink &diagnostics) {
  CompileWorkspaceOptions options = compile_options();
  options.workspace.source_overrides = overrides;
  candidate = compile_workspace_with_resolution(
      candidate_sources, configuration_.root_package_directory.string(),
      std::move(options), diagnostics);
  return candidate.ok && !diagnostics.has_errors();
}

CheckResult CompilerSession::check(std::span<const SourceOverlay> overlays,
                                   std::size_t active_overlay) {
  // Navigation describes the exact latest accepted bytes. Clear it before any
  // validation so every early return and failed candidate makes retained
  // last-good semantics inaccessible to F12 by construction.
  latest_check_succeeded_ = false;
  navigation_definition_.reset();
  navigation_usages_.clear();
  if (overlays.empty() || active_overlay >= overlays.size()) {
    syntax_spans_.clear();
    SourceManager sources;
    DiagnosticSink diagnostics;
    diagnostics.error(SourceRange::invalid(),
                      "compiler check requires one active source overlay");
    publish_diagnostics(sources, diagnostics);
    return {false, diagnostic_count_};
  }

  DiagnosticSink configuration_diagnostics;
  if (!refresh_configuration(configuration_diagnostics)) {
    syntax_spans_.clear();
    SourceManager sources;
    publish_diagnostics(sources, configuration_diagnostics);
    return {false, diagnostic_count_};
  }

  DiagnosticSink conversion_diagnostics;
  const std::optional<std::vector<WorkspaceSourceOverride>> converted =
      source_overrides(overlays, conversion_diagnostics);
  if (!converted.has_value()) {
    syntax_spans_.clear();
    SourceManager sources;
    publish_diagnostics(sources, conversion_diagnostics);
    return {false, diagnostic_count_};
  }
  std::error_code active_error;
  source_path_ = std::filesystem::weakly_canonical(
      overlays[active_overlay].physical_path, active_error);
  if (active_error)
    source_path_ = overlays[active_overlay].physical_path;
  collect_syntax_spans(overlays[active_overlay]);

  // The common path copies the last immutable command-local graph, installs
  // every open complete-file interface change, and resumes semantics.
  // Diagnostics are private until this candidate is known to be the one shown
  // to the user.
  if (last_good_.has_value()) {
    SourceManager candidate_sources = last_good_sources_;
    CompileWorkspaceResult candidate = *last_good_;
    DiagnosticSink diagnostics;
    CompileWorkspaceOptions options = compile_options();
    const bool applied = apply_compiled_workspace_source_overrides(
        candidate_sources, *converted, WorkspaceSemanticChange::Interface,
        options, candidate, diagnostics);
    const bool completed =
        applied &&
        continue_compiled_workspace_semantics(
            candidate_sources, configuration_.root_package_directory.string(),
            options, candidate, diagnostics);
    if (completed && candidate.ok && !diagnostics.has_errors()) {
      last_good_sources_ = std::move(candidate_sources);
      last_good_ = std::move(candidate);
      rebuild_source_options();
      rebuild_tooling_index();
      publish_diagnostics(last_good_sources_, diagnostics);
      latest_check_succeeded_ = true;
      return {true, diagnostic_count_};
    }
  }

  // Import/package topology changes cannot mutate stable PackageIds in place.
  // A fresh candidate with the same in-memory file overrides is the explicit
  // slow path and is also used before the first successful program exists.
  SourceManager fresh_sources;
  CompileWorkspaceResult fresh;
  DiagnosticSink fresh_diagnostics;
  if (fresh_check(*converted, fresh_sources, fresh, fresh_diagnostics)) {
    last_good_sources_ = std::move(fresh_sources);
    last_good_ = std::move(fresh);
    rebuild_source_options();
    rebuild_tooling_index();
    publish_diagnostics(last_good_sources_, fresh_diagnostics);
    latest_check_succeeded_ = true;
    return {true, diagnostic_count_};
  }
  publish_diagnostics(fresh_sources, fresh_diagnostics);
  return {false, diagnostic_count_};
}

CheckResult CompilerSession::build(std::span<const SourceOverlay> overlays,
                                   std::size_t active_overlay) {
  built_output_path_.clear();
  const CheckResult checked = check(overlays, active_overlay);
  if (!checked.ok)
    return checked;
  return build_checked();
}

CheckResult CompilerSession::build_checked() {
  if (!last_good_.has_value())
    return {false, diagnostic_count_};
  SourceManager sources = last_good_sources_;
  CompileWorkspaceResult compiled = *last_good_;
  DiagnosticSink diagnostics;
  CompileWorkspaceOptions options = compile_options();
  const EffectiveProgramConfiguration &program = active_program();
  options.lower_mir = true;
  options.emit_native_output = true;
  options.native_output.output_kind =
      program.build.artifact_kind == NativeArtifactKind::Assembly
          ? LlvmNativeOutputKind::Assembly
          : LlvmNativeOutputKind::Object;
  options.native_output.optimization = program.build.optimization;
  options.emit_debug_information = program.build.emit_debug_symbols;
  if (!continue_compiled_workspace(sources, options, compiled, diagnostics)) {
    publish_diagnostics(sources, diagnostics);
    return {false, diagnostic_count_};
  }

  NativeBuildOptions native;
  native.build_directory = build_directory_.string();
  native.output_path =
      program.build.output_path.value_or(default_output_path()).string();
  native.artifact_kind = program.build.artifact_kind;
  native.optimization = program.build.optimization;
  native.emit_debug_symbols = program.build.emit_debug_symbols;
  native.foreign_providers = program.build.foreign_providers;
  native.runtime_assets = program.build.runtime_assets;
  native.work_executor = options.work_executor;
  const NativeBuildResult built = build_native_artifact(
      program.build.target, compiled, native, diagnostics);
  if (!built.ok) {
    publish_diagnostics(sources, diagnostics);
    return {false, diagnostic_count_};
  }
  built_output_path_ = built.output_path;
  built_artifact_kind_ = program.build.artifact_kind;
  publish_diagnostics(sources, diagnostics);
  return {true, diagnostic_count_};
}

const std::vector<SyntaxSpan> &CompilerSession::syntax_spans() const {
  return syntax_spans_;
}

std::string_view CompilerSession::diagnostics_text() const {
  return diagnostics_text_;
}

std::string_view CompilerSession::tooling_text(ToolingSection section) const {
  const std::size_t index = static_cast<std::size_t>(section);
  if (index >= tooling_text_.size())
    return {};
  return tooling_text_[index];
}

const std::vector<PackageTreeRow> &CompilerSession::package_tree_rows() const {
  return package_tree_rows_;
}

const std::filesystem::path &CompilerSession::workspace_directory() const {
  return configuration_.workspace_directory;
}

const std::filesystem::path &CompilerSession::source_path() const {
  return source_path_;
}

const std::filesystem::path &CompilerSession::built_output_path() const {
  return built_output_path_;
}

NativeArtifactKind CompilerSession::built_artifact_kind() const {
  return built_artifact_kind_;
}

std::span<const std::string> CompilerSession::run_arguments() const {
  return active_program().arguments;
}

std::span<const std::string> CompilerSession::run_environment() const {
  return active_program().environment;
}

const std::optional<std::filesystem::path> &
CompilerSession::run_working_directory() const {
  return active_program().working_directory;
}

std::string CompilerSession::build_configuration_text() const {
  const EffectiveProgramConfiguration &program = active_program();
  const ResolvedBuildPolicy &build = program.build;
  std::string text;
  const auto line = [&text](std::string_view name, std::string_view value) {
    text += name;
    text += ": ";
    text += value;
    text += '\n';
  };
  line("root package", root_options_[selected_root_].root_relative_path);
  line("target", build.target.facts.file_tag);
  line("optimization", native_optimization_level_name(build.optimization));
  line("artifact", native_artifact_kind_name(build.artifact_kind));
  const std::filesystem::path output =
      build.output_path.value_or(default_output_path());
  line("output", output.string());
  line("debug symbols", build.emit_debug_symbols ? "on" : "off");
  line("runtime assertions", build.runtime_assertions ? "on" : "off");

  if (build.foreign_providers.empty()) {
    line("providers", "none");
  } else {
    for (const ForeignProviderInput &provider : build.foreign_providers) {
      std::string value = provider.provider;
      value += '=';
      value += foreign_artifact_kind_name(provider.kind);
      value += ':';
      value += provider.path.string();
      line("provider", value);
    }
  }
  for (const ForeignProviderSummaryInput &summary : build.provider_summaries) {
    std::string value = summary.provider;
    value += ':';
    value += summary.path.string();
    line("provider summary", value);
  }
  for (const RuntimeAssetInput &asset : build.runtime_assets) {
    std::string value = asset.name;
    value += ':';
    value += asset.path.string();
    line("runtime asset", value);
  }
  return text;
}

std::string CompilerSession::session_summary_text() const {
  const ResolvedBuildPolicy &build = active_program().build;
  std::string workspace_name =
      configuration_.workspace_directory.filename().string();
  if (workspace_name.empty())
    workspace_name = configuration_.workspace_directory.string();
  std::string text = "Workspace: ";
  text += workspace_name;
  text += "  Root: ";
  text += root_options_[selected_root_].root_relative_path;
  text += "  ";
  text += build.target.facts.file_tag;
  text += "  ";
  text += native_optimization_level_name(build.optimization);
  return text;
}

std::string CompilerSession::navigation_source_path(std::size_t source) const {
  if (!last_good_.has_value() ||
      source > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  const FileId file{static_cast<std::uint32_t>(source)};
  for (const WorkspacePackage &package : last_good_->graph.packages) {
    for (const LoadedPackageFile &loaded : package.loaded.files) {
      if (loaded.source != file)
        continue;
      if (package.identity.root_identity == "workspace") {
        const std::filesystem::path physical =
            std::filesystem::path(package.loaded.physical_directory) /
            loaded.relative_name;
        std::error_code error;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(physical, error);
        return (error ? physical.lexically_normal() : canonical).string();
      }
      // Compiler-distributed and pinned dependency source may have no usable
      // host path. A semantic identity prefixed outside the filesystem
      // namespace lets DraftIDE retain one read-only buffer without pretending
      // it can save that source back to disk.
      std::string virtual_path = "draft-source:";
      virtual_path += display_package_identity(package.identity);
      virtual_path += '/';
      virtual_path += loaded.relative_name;
      return virtual_path;
    }
  }
  return {};
}

std::string_view
CompilerSession::navigation_source_text(std::size_t source) const {
  if (!last_good_.has_value() || source >= last_good_sources_.file_count() ||
      source > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  return last_good_sources_.text(FileId{static_cast<std::uint32_t>(source)});
}

bool CompilerSession::navigation_source_editable(std::size_t source) const {
  if (!last_good_.has_value() ||
      source > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const FileId file{static_cast<std::uint32_t>(source)};
  for (const WorkspacePackage &package : last_good_->graph.packages) {
    for (const LoadedPackageFile &loaded : package.loaded.files) {
      if (loaded.source == file)
        return package.identity.root_identity == "workspace";
    }
  }
  return false;
}

const std::optional<NavigationLocation> &
CompilerSession::navigation_definition() const {
  return navigation_definition_;
}

std::span<const NavigationLocation> CompilerSession::navigation_usages() const {
  return navigation_usages_;
}

NavigationStatus CompilerSession::prepare_navigation(std::string_view path,
                                                     std::size_t byte_offset) {
  navigation_definition_.reset();
  navigation_usages_.clear();
  if (!last_good_.has_value())
    return NavigationStatus::Unavailable;
  if (!latest_check_succeeded_)
    return NavigationStatus::CurrentCheckFailed;
  if (path.empty() || byte_offset > std::numeric_limits<std::uint32_t>::max())
    return NavigationStatus::SourceNotFound;

  const CompileWorkspaceResult &compiled = *last_good_;
  std::optional<std::size_t> source_package;
  FileId source_file;
  std::error_code requested_error;
  const std::filesystem::path requested_canonical =
      std::filesystem::weakly_canonical(std::filesystem::path(path),
                                        requested_error);
  for (std::size_t package_index = 0;
       package_index < compiled.graph.packages.size() &&
       !source_package.has_value();
       ++package_index) {
    const WorkspacePackage &package = compiled.graph.packages[package_index];
    for (const LoadedPackageFile &file : package.loaded.files) {
      if (file.kind != PackageFileKind::DraftSource)
        continue;
      const std::string candidate = navigation_source_path(file.source.value);
      bool matches = candidate == path;
      if (!matches && !requested_error &&
          package.identity.root_identity == "workspace") {
        std::error_code candidate_error;
        const std::filesystem::path candidate_canonical =
            std::filesystem::weakly_canonical(candidate, candidate_error);
        matches =
            !candidate_error && candidate_canonical == requested_canonical;
      }
      if (matches) {
        source_package = package_index;
        source_file = file.source;
        break;
      }
    }
  }
  if (!source_package.has_value() || !source_file.is_valid() ||
      source_file.value >= last_good_sources_.file_count() ||
      byte_offset > last_good_sources_.text(source_file).size()) {
    return NavigationStatus::SourceNotFound;
  }
  if (*source_package >= compiled.packages.size() ||
      !compiled.packages[*source_package].has_value()) {
    return NavigationStatus::SourceNotFound;
  }

  const CompiledPackage &source_compiled = *compiled.packages[*source_package];
  const SemanticPackage &source_semantic = source_compiled.bodies.package;
  struct CursorCandidate {
    bool found = false;
    bool interior = false;
    std::size_t width = 0;
    SymbolId symbol;
  } candidate;
  const auto consider = [&](SourceRange range, SymbolId symbol) {
    if (!range.is_valid() || range.begin.file != source_file ||
        !symbol.is_valid() || range.end.offset < range.begin.offset) {
      return;
    }
    const bool interior =
        byte_offset >= range.begin.offset && byte_offset < range.end.offset;
    const bool trailing_boundary = byte_offset == range.end.offset &&
                                   range.end.offset != range.begin.offset;
    if (!interior && !trailing_boundary)
      return;
    const std::size_t width = range.end.offset - range.begin.offset;
    if (!candidate.found || (interior && !candidate.interior) ||
        (interior == candidate.interior && width < candidate.width)) {
      candidate = {true, interior, width, symbol};
    }
  };

  // Symbol name ranges cover declarations, parameters, locals, fields, and
  // imported proxy names already published into the canonical package table.
  for (std::size_t index = 0; index < source_semantic.symbols.symbol_count();
       ++index) {
    const SymbolId symbol{static_cast<std::uint32_t>(index)};
    consider(source_semantic.symbols.symbol(symbol).name_range, symbol);
  }
  // Only selected body products belong to the current program graph. The body
  // table may retain an unselected specialization from an earlier command-local
  // demand, and scanning it would violate the same no-stale-navigation rule as
  // consulting last-good after a failed check.
  for (std::size_t work_index : source_compiled.selected_procedure_work) {
    if (work_index >= source_compiled.bodies.procedures.size())
      continue;
    const ProcedureBodyHirResult &body =
        source_compiled.bodies.procedures[work_index];
    for (std::size_t expression_index = 0;
         expression_index < body.program.expression_count();
         ++expression_index) {
      const HirExpression &expression = body.program.expression(
          HirExpressionId{static_cast<std::uint32_t>(expression_index)});
      if (!expression.symbol.is_valid())
        continue;
      consider(reference_name_range(
                   compiled.graph.packages[*source_package].loaded, expression),
               expression.symbol);
    }
  }
  if (!candidate.found)
    return NavigationStatus::NoSymbol;

  const std::optional<SemanticSymbol> selected =
      canonical_symbol(compiled, *source_package, candidate.symbol);
  if (!selected.has_value() || selected->package >= compiled.packages.size() ||
      !compiled.packages[selected->package].has_value()) {
    return NavigationStatus::NoDefinition;
  }
  const SemanticPackage &definition_semantic =
      compiled.packages[selected->package]->bodies.package;
  if (!selected->symbol.is_valid() ||
      selected->symbol.value >= definition_semantic.symbols.symbol_count()) {
    return NavigationStatus::NoDefinition;
  }
  const SourceRange definition_range =
      definition_semantic.symbols.symbol(selected->symbol).name_range;
  if (!definition_range.is_valid() ||
      definition_range.begin.file.value >= last_good_sources_.file_count()) {
    return NavigationStatus::NoDefinition;
  }
  const auto location = [this](SourceRange range) {
    const LineColumn coordinate = last_good_sources_.line_column(range.begin);
    return NavigationLocation{
        range.begin.file.value, range.begin.offset, range.end.offset,
        coordinate.line,        coordinate.column,
    };
  };
  navigation_definition_ = location(definition_range);

  // Usages are emitted in deterministic SourceManager order, then byte order.
  // Duplicate HIR expressions can represent one surface token (for example a
  // direct call plus its callee); exact range deduplication keeps one visible
  // row without introducing a second semantic identity system.
  for (std::size_t package_index = 0; package_index < compiled.packages.size();
       ++package_index) {
    if (!compiled.packages[package_index].has_value())
      continue;
    const CompiledPackage &package = *compiled.packages[package_index];
    for (std::size_t work_index : package.selected_procedure_work) {
      if (work_index >= package.bodies.procedures.size())
        continue;
      const ProcedureBodyHirResult &body =
          package.bodies.procedures[work_index];
      for (std::size_t expression_index = 0;
           expression_index < body.program.expression_count();
           ++expression_index) {
        const HirExpression &expression = body.program.expression(
            HirExpressionId{static_cast<std::uint32_t>(expression_index)});
        if (!expression.symbol.is_valid())
          continue;
        const std::optional<SemanticSymbol> reference =
            canonical_symbol(compiled, package_index, expression.symbol);
        if (!reference.has_value() || *reference != *selected)
          continue;
        const SourceRange range = reference_name_range(
            compiled.graph.packages[package_index].loaded, expression);
        if (range.is_valid())
          navigation_usages_.push_back(location(range));
      }
    }
  }
  std::sort(
      navigation_usages_.begin(), navigation_usages_.end(),
      [](const NavigationLocation &left, const NavigationLocation &right) {
        if (left.source != right.source)
          return left.source < right.source;
        if (left.start != right.start)
          return left.start < right.start;
        return left.end < right.end;
      });
  navigation_usages_.erase(
      std::unique(
          navigation_usages_.begin(), navigation_usages_.end(),
          [](const NavigationLocation &left, const NavigationLocation &right) {
            return left.source == right.source && left.start == right.start &&
                   left.end == right.end;
          }),
      navigation_usages_.end());
  return NavigationStatus::Ready;
}

} // namespace draft::ide
