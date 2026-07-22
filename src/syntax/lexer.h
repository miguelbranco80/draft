// Public entry point for Draft lexical analysis.
//
// lex_source validates UTF-8, recognizes the complete Draft 1 token vocabulary,
// removes comments and trivia, and applies newline/EOF semicolon insertion. The
// returned stream always ends with EndOfFile and never contains Newline. Invalid
// input may still produce recovery tokens so the parser can continue, but every
// malformed byte sequence or literal is reported to the supplied sink.
//
// The lexer borrows source bytes from SourceManager for the duration of the call
// and stores only SourceRange values in its result. The manager must therefore
// outlive all consumers of the tokens.

#pragma once

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/token.h"

#include <vector>

namespace draft {

// ToolingTokenClass preserves comment trivia without changing the parser's
// grammar token stream. Syntax rows carry their ordinary TokenKind; comment
// rows carry Invalid because comments have no parser kind. Ranges are exact
// authored bytes in source order and omit whitespace/newline trivia.
enum class ToolingTokenClass {
  Syntax,
  LineComment,
  BlockComment,
};

struct ToolingToken {
  ToolingTokenClass token_class = ToolingTokenClass::Syntax;
  TokenKind kind = TokenKind::Invalid;
  SourceRange range;
};

[[nodiscard]] std::vector<Token> lex_source(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics);

// Runs the same RawLexer used by lex_source while retaining comments and raw
// syntax ranges for editors. It deliberately does not run a second language
// scanner or expose inserted semicolons; parser recovery and tooling therefore
// agree on literal validity, UTF-8 diagnostics, keywords, and token boundaries.
[[nodiscard]] std::vector<ToolingToken> lex_source_for_tooling(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics);

} // namespace draft
