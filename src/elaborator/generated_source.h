// Lexical safety boundary for proposed and stored generated Draft source.
//
// An expansion is ordinary source for parsing, name resolution, typing, and
// denial checking, but it is not allowed to schedule another provider action
// or create a new judgment claim. This small precheck runs before an expansion
// can influence a later elaboration stage. It owns no source after returning;
// SourceManager retains the diagnostic buffer and the caller retains the bytes.
//
// The check is deliberately lexical. Strings and comments are already excluded
// by the Draft lexer, while every actual `...` and `judge` construct begins with
// one unambiguous token. Grammar-category validity remains the ordinary parser
// and semantic compiler's responsibility. Relevant specification:
// 03-agent-synthesis.md sections 9-10.

#pragma once

#include "source/diagnostic.h"
#include "source/source.h"

#include <string_view>

namespace draft {

// Adds source to the caller's SourceManager under display_name, lexes it, and
// rejects provider-operation tokens. Invalid UTF-8 and malformed literals are
// reported by the normal lexer with exact generated-source ranges. The source
// buffer remains available for diagnostic rendering after a false return.
[[nodiscard]] bool validate_generated_source_boundary(
    SourceManager &sources,
    std::string_view display_name,
    std::string_view source,
    DiagnosticSink &diagnostics);

} // namespace draft
