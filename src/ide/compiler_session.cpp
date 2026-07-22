// See compiler_session.h for ownership and transaction boundaries. This file
// keeps the implementation deliberately direct: one speculative graph copy,
// one full-topology fallback, one last-good publication point, and one native
// artifact operation. There is no cache, query framework, background thread,
// or IDE-specific semantic representation.

#include "ide/compiler_session.h"

#include "syntax/lexer.h"
#include "syntax/token.h"
#include "workspace/package.h"
#include "workspace/selection.h"

#include <algorithm>
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

} // namespace

CompilerSession::CompilerSession(CompilerConfiguration configuration)
    : configuration_(std::move(configuration)) {
  root_identity_.root_identity = "workspace";
  root_identity_.root_relative_path = configuration_.root_relative_path;
  source_path_ = configuration_.root_package_directory /
                 configuration_.source_relative_name;
}

CompilerSession::~CompilerSession() {
  std::error_code error;
  std::filesystem::remove_all(build_directory_, error);
}

bool CompilerSession::refresh_root_options(DiagnosticSink &diagnostics) {
  SourceManager discovery_sources;
  WorkspaceLoadOptions workspace_options = compile_options().workspace;
  workspace_options.package_options.file_tag =
      configuration_.target.facts.file_tag;
  const ExecutableRootDiscoveryResult discovered = discover_executable_roots(
      discovery_sources, workspace_options, diagnostics);
  if (!discovered.ok || diagnostics.has_errors())
    return false;

  std::vector<WorkspacePackageSelection> selections = discovered.roots;
  const bool contains_current =
      std::any_of(selections.begin(), selections.end(),
                  [this](const WorkspacePackageSelection &selection) {
                    return selection.identity.root_relative_path ==
                           configuration_.root_relative_path;
                  });
  if (!contains_current) {
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
    PackageLoadOptions package_options;
    package_options.file_tag = configuration_.target.facts.file_tag;
    const PackageLoadResult loaded =
        load_package(discovery_sources, selection.physical_directory.string(),
                     package_options, diagnostics);
    if (!loaded.ok || diagnostics.has_errors())
      return false;

    std::string source_name;
    for (const LoadedPackageFile &file : loaded.package.files) {
      if (file.kind != PackageFileKind::DraftSource)
        continue;
      if (selection.identity.root_relative_path ==
              configuration_.root_relative_path &&
          file.relative_name == configuration_.source_relative_name) {
        source_name = file.relative_name;
        break;
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
    });
  }

  root_options_ = std::move(options);
  selected_root_ = selected;
  return true;
}

bool CompilerSession::initialize(DiagnosticSink &diagnostics) {
  if (!refresh_root_options(diagnostics))
    return false;
  return create_build_directory(diagnostics);
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
#if defined(_WIN32)
      output_path_ = build_directory_ / "program.exe";
#else
      output_path_ = build_directory_ / "program";
#endif
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
  syntax_spans_.clear();
  for (std::string &text : tooling_text_)
    text.clear();
  diagnostics_text_.clear();
  diagnostic_count_ = 0;
  built_output_path_.clear();
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
  root_identity_.root_identity = "workspace";
  root_identity_.root_relative_path = root.root_relative_path;
  source_path_ = root.physical_directory / root.source_relative_name;
  selected_root_ = index;
  reset_checked_program();
  return true;
}

bool CompilerSession::select_target(TargetProfile target,
                                    DiagnosticSink &diagnostics) {
  const CompilerConfiguration previous_configuration = configuration_;
  const std::vector<RootOption> previous_options = root_options_;
  const std::size_t previous_selected = selected_root_;
  configuration_.target = std::move(target);
  if (!refresh_root_options(diagnostics)) {
    configuration_ = previous_configuration;
    root_options_ = previous_options;
    selected_root_ = previous_selected;
    return false;
  }
  const RootOption selected = root_options_[selected_root_];
  configuration_.root_relative_path = selected.root_relative_path;
  configuration_.root_package_directory = selected.physical_directory;
  configuration_.source_relative_name = selected.source_relative_name;
  root_identity_.root_identity = "workspace";
  root_identity_.root_relative_path = selected.root_relative_path;
  source_path_ = selected.physical_directory / selected.source_relative_name;
  reset_checked_program();
  return true;
}

const TargetProfile &CompilerSession::target() const {
  return configuration_.target;
}

CompileWorkspaceOptions CompilerSession::compile_options() const {
  CompileWorkspaceOptions options;
  options.target = configuration_.target;
  options.workspace.workspace_directory =
      configuration_.workspace_directory.string();
  options.workspace.core_directory = DRAFT_CORE_DIRECTORY;
  options.workspace.core_content_identity = DRAFT_CORE_CONTENT_IDENTITY;
  options.emit_program_entry = true;
  return options;
}

WorkspaceSourceOverride
CompilerSession::source_override(std::string_view source) const {
  WorkspaceSourceOverride result;
  result.identity = root_identity_;
  result.source.relative_name = configuration_.source_relative_name;
  result.source.contents = std::string(source);
  return result;
}

void CompilerSession::collect_syntax_spans(std::string_view source) {
  syntax_spans_.clear();
  SourceManager sources;
  const FileId file =
      sources.add_source(source_path_.string(), std::string(source));
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

void CompilerSession::rebuild_tooling_index() {
  for (std::string &text : tooling_text_)
    text.clear();
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
  packages += configuration_.target.facts.file_tag;
  packages += "\nPackages and dependencies\n";
  for (std::size_t package_index = 0;
       package_index < compiled.graph.packages.size(); ++package_index) {
    const WorkspacePackage &workspace_package =
        compiled.graph.packages[package_index];
    packages +=
        package_index == compiled.graph.root_package.value ? "* " : "  ";
    packages += display_package_identity(workspace_package.identity);
    packages += " (";
    packages += workspace_package.loaded.short_name;
    packages += ")\n";
    for (const PackageImport &import : compiled.graph.imports) {
      if (import.importing_package.value != package_index)
        continue;
      packages += "    -> ";
      packages += display_package_identity(
          compiled.graph.package(import.imported_package).identity);
      packages += " via ";
      packages += import.path;
      packages += '\n';
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

bool CompilerSession::fresh_check(std::string_view source,
                                  SourceManager &candidate_sources,
                                  CompileWorkspaceResult &candidate,
                                  DiagnosticSink &diagnostics) {
  CompileWorkspaceOptions options = compile_options();
  options.workspace.source_overrides.push_back(source_override(source));
  candidate = compile_workspace_with_resolution(
      candidate_sources, configuration_.root_package_directory.string(),
      std::move(options), diagnostics);
  return candidate.ok && !diagnostics.has_errors();
}

CheckResult CompilerSession::check(std::string_view source) {
  collect_syntax_spans(source);

  // The common path copies the last immutable command-local graph, installs
  // one complete-file interface change, and resumes semantics. Diagnostics are
  // private until this candidate is known to be the one shown to the user.
  if (last_good_.has_value()) {
    SourceManager candidate_sources = last_good_sources_;
    CompileWorkspaceResult candidate = *last_good_;
    DiagnosticSink diagnostics;
    CompileWorkspaceOptions options = compile_options();
    const bool applied = apply_compiled_workspace_source_overrides(
        candidate_sources, {source_override(source)},
        WorkspaceSemanticChange::Interface, options, candidate, diagnostics);
    const bool completed =
        applied &&
        continue_compiled_workspace_semantics(
            candidate_sources, configuration_.root_package_directory.string(),
            options, candidate, diagnostics);
    if (completed && candidate.ok && !diagnostics.has_errors()) {
      last_good_sources_ = std::move(candidate_sources);
      last_good_ = std::move(candidate);
      rebuild_tooling_index();
      publish_diagnostics(last_good_sources_, diagnostics);
      return {true, diagnostic_count_};
    }
  }

  // Import/package topology changes cannot mutate stable PackageIds in place.
  // A fresh candidate with the same in-memory file override is the explicit
  // slow path and is also used before the first successful program exists.
  SourceManager fresh_sources;
  CompileWorkspaceResult fresh;
  DiagnosticSink fresh_diagnostics;
  if (fresh_check(source, fresh_sources, fresh, fresh_diagnostics)) {
    const std::size_t root = fresh.graph.root_package.value;
    if (root < fresh.graph.packages.size()) {
      root_identity_ = fresh.graph.packages[root].identity;
    }
    last_good_sources_ = std::move(fresh_sources);
    last_good_ = std::move(fresh);
    rebuild_tooling_index();
    publish_diagnostics(last_good_sources_, fresh_diagnostics);
    return {true, diagnostic_count_};
  }
  publish_diagnostics(fresh_sources, fresh_diagnostics);
  return {false, diagnostic_count_};
}

CheckResult CompilerSession::build(std::string_view source) {
  built_output_path_.clear();
  const CheckResult checked = check(source);
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
  options.lower_mir = true;
  options.emit_llvm = true;
  if (!continue_compiled_workspace(sources, options, compiled, diagnostics)) {
    publish_diagnostics(sources, diagnostics);
    return {false, diagnostic_count_};
  }

  NativeBuildOptions native;
  native.build_directory = build_directory_.string();
  native.output_path = output_path_.string();
  native.artifact_kind = NativeArtifactKind::Executable;
  native.optimization = NativeOptimizationLevel::O0;
  const NativeBuildResult built = build_native_executable(
      configuration_.target, compiled, native, diagnostics);
  if (!built.ok) {
    publish_diagnostics(sources, diagnostics);
    return {false, diagnostic_count_};
  }
  built_output_path_ = built.output_path;
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

const std::filesystem::path &CompilerSession::source_path() const {
  return source_path_;
}

const std::filesystem::path &CompilerSession::built_output_path() const {
  return built_output_path_;
}

} // namespace draft::ide
