// Public parser entry points for complete Draft source files.
//
// parse_source_file invokes the normal lexer and constructs a SyntaxTree even
// when recoverable errors occur. The parser never performs name lookup or type
// interpretation. It does, however, select every closed braced grammar category
// required by Draft: declaration/member lists, statement blocks, expression
// regions, switch cases, and assembly bodies.
//
// The returned tree owns its tokens and remains inspectable after diagnostics
// are rendered. Callers must keep SourceManager alive because token and node
// ranges continue to address its source bytes.

#pragma once

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/syntax_tree.h"

namespace draft {

[[nodiscard]] SyntaxTree parse_source_file(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics);

} // namespace draft
