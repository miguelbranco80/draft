// Bootstrap compiler command-line entry point.
//
// The driver is intentionally thin: it owns process-facing argument and stream
// behavior, while source loading, diagnostics, syntax, semantics, and codegen
// remain reusable modules. The initial `lex` command is an inspectable front-end
// probe and will remain useful after `check`, `build`, `resolve`, and `judge` are
// added. It prints exact token spellings and marks inserted semicolons.

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/syntax_tree.h"
#include "syntax/token.h"

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

void print_usage() {
  std::cerr << "usage:\n"
            << "  draftc lex <file.draft>\n"
            << "  draftc syntax <file.draft>\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "lex") {
    return lex_file(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "syntax") {
    return parse_file(argv[2]);
  }
  print_usage();
  return 2;
}
