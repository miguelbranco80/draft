// Bootstrap compiler command-line entry point.
//
// The driver is intentionally thin: it owns process-facing argument and stream
// behavior, while source loading, diagnostics, syntax, semantics, and codegen
// remain reusable modules. The initial `lex` command is an inspectable front-end
// probes and will remain useful after `check`, `build`, `resolve`, and `judge`
// are added. They print exact token spellings, grammar structure, or the complete
// versioned target profile without embedding phase logic in this file.

#include "source/diagnostic.h"
#include "source/source.h"
#include "sema/agent_metadata.h"
#include "sema/body_checker.h"
#include "sema/denial.h"
#include "sema/effect.h"
#include "sema/semantic.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/syntax_tree.h"
#include "syntax/token.h"
#include "target/profile.h"
#include "workspace/package.h"
#include "workspace/workspace.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

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
            << "assembly-dialect " << profile.parsed_assembly_dialect << '\n'
            << "relocation-model "
            << draft::relocation_model_name(profile.relocation_model) << '\n'
            << "code-model " << draft::code_model_name(profile.code_model) << '\n'
            << "tls-model " << draft::tls_model_name(profile.tls_model) << '\n';
  return 0;
}

// Runs the complete provider-free front end currently available for one root
// package and all workspace-relative imports. Until the build manifest command
// supplies an explicit workspace root, the parent of the requested package is
// the command's canonical workspace root. No dependency or core roots are
// inferred from the environment. `check` never invokes an agent, assembler,
// linker, or network service.
int check_package(const std::string &directory) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  std::string profile_error;
  if (!draft::validate_target_profile(target, profile_error)) {
    std::cerr << "error: invalid built-in target profile: " << profile_error << '\n';
    return 1;
  }

  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error);
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }

  draft::WorkspaceLoadOptions options;
  options.workspace_directory = absolute_directory.parent_path().string();
  options.package_options.file_tag = target.facts.file_tag;
  draft::WorkspaceLoadResult loaded = draft::load_workspace(
      sources, absolute_directory.string(), options, diagnostics);
  std::size_t symbol_count = 0;
  std::size_t type_count = 0;
  std::size_t procedure_count = 0;
  std::size_t agent_record_count = 0;
  bool checked_all = loaded.ok;
  if (loaded.ok) {
    std::vector<std::optional<draft::PackageInterface>> interfaces(
        loaded.graph.packages.size());
    // DFS discovery places every dependency after its importer. Reversing that
    // order checks dependencies before consumers and is the order the canonical
    // interface pass will use to publish imported names.
    for (std::size_t remaining = loaded.graph.packages.size(); remaining > 0; --remaining) {
      const std::size_t package_index = remaining - 1;
      draft::WorkspacePackage &workspace_package = loaded.graph.packages[package_index];
      draft::AvailablePackageImports available;
      for (const draft::PackageImport &import : loaded.graph.imports) {
        if (static_cast<std::size_t>(import.importing_package.value) != package_index) {
          continue;
        }
        const std::size_t dependency_index =
            static_cast<std::size_t>(import.imported_package.value);
        if (dependency_index >= interfaces.size() ||
            !interfaces[dependency_index].has_value()) {
          diagnostics.error(
              draft::SourceRange::invalid(),
              "internal package order did not produce a dependency interface");
          checked_all = false;
          continue;
        }
        available.entries.push_back({
            {import.file, import.syntax},
            &*interfaces[dependency_index],
        });
      }
      draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
          sources, workspace_package.loaded, target.facts, available, diagnostics);
      draft::BodyCheckResult bodies;
      if (semantics.ok) {
        bodies = draft::check_package_bodies(
            sources,
            workspace_package.loaded,
            semantics.selections,
            semantics.package,
            semantics.constants,
            diagnostics);
        if (bodies.ok) {
          const draft::AttachmentPolicy attachment_policy;
          const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
              sources,
              workspace_package.loaded,
              semantics.package,
              attachment_policy,
              diagnostics);
          const draft::EffectSummaryResult effects = draft::summarize_package_effects(
              semantics.package, bodies.program);
          const bool denials_ok = draft::check_package_denials(
              sources,
              workspace_package.loaded,
              semantics.package,
              bodies.program,
              effects,
              diagnostics);
          const draft::PackageId package_id{static_cast<std::uint32_t>(package_index)};
          interfaces[package_index] = draft::build_package_interface(
              loaded.graph.package(package_id).identity,
              semantics.package,
              semantics.constants,
              metadata,
              effects,
              diagnostics);
          agent_record_count += metadata.records.size();
          if (!metadata.ok) {
            checked_all = false;
          }
          if (!denials_ok) {
            checked_all = false;
          }
        }
      }
      if (!semantics.ok || !bodies.ok) {
        checked_all = false;
        continue;
      }
      symbol_count += semantics.package.symbols.symbol_count();
      type_count += semantics.package.types.size();
      procedure_count += bodies.checked_procedures;
    }
    if (checked_all) {
      const draft::WorkspacePackage &root = loaded.graph.package(loaded.graph.root_package);
      std::cout << "checked package graph rooted at " << root.loaded.short_name << ": "
                << loaded.graph.packages.size() << " packages, "
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

void print_usage() {
  std::cerr << "usage:\n"
            << "  draftc lex <file.draft>\n"
            << "  draftc syntax <file.draft>\n"
            << "  draftc check <package-directory>\n"
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
    return check_package(argv[2]);
  }
  if (argc == 2 && std::string_view(argv[1]) == "target") {
    return print_target();
  }
  print_usage();
  return 2;
}
