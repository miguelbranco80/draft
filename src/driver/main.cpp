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
#include "sema/body_checker.h"
#include "sema/semantic.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/syntax_tree.h"
#include "syntax/token.h"
#include "target/profile.h"
#include "workspace/package.h"

#include <iostream>
#include <string>
#include <string_view>

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

// Runs the complete provider-free front end currently available for one folder
// package. `check` never invokes an agent, assembler, linker, or network service;
// later backend stages consume the returned semantic graph and HIR in memory.
int check_package(const std::string &directory) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  std::string profile_error;
  if (!draft::validate_target_profile(target, profile_error)) {
    std::cerr << "error: invalid built-in target profile: " << profile_error << '\n';
    return 1;
  }

  draft::PackageLoadOptions options;
  options.file_tag = target.facts.file_tag;
  draft::PackageLoadResult loaded = draft::load_package(
      sources, directory, options, diagnostics);
  if (loaded.ok) {
    draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
        sources, loaded.package, target.facts, diagnostics);
    draft::BodyCheckResult bodies;
    if (semantics.ok) {
      bodies = draft::check_package_bodies(
          sources,
          loaded.package,
          semantics.selections,
          semantics.package,
          semantics.constants,
          diagnostics);
    }
    if (semantics.ok && bodies.ok) {
      std::cout << "checked package " << loaded.package.short_name << ": "
                << semantics.package.symbols.symbol_count() << " symbols, "
                << semantics.package.types.size() << " types, "
                << bodies.checked_procedures << " procedure bodies\n";
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
