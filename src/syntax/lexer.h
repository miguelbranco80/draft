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

[[nodiscard]] std::vector<Token> lex_source(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics);

} // namespace draft
