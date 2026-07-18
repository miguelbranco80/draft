// Implementation of the generated-source lexical safety boundary.

#include "elaborator/generated_source.h"

#include "syntax/lexer.h"
#include "syntax/token.h"

#include <string>
#include <vector>

namespace draft {

bool validate_generated_source_boundary(
    SourceManager &sources,
    std::string_view display_name,
    std::string_view source,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  const FileId file = sources.add_source(
      std::string(display_name), std::string(source));
  const std::vector<Token> tokens = lex_source(sources, file, diagnostics);
  for (const Token &token : tokens) {
    if (token.kind == TokenKind::Ellipsis) {
      diagnostics.error(
          token.range,
          "generated source may not contain another synthesis site");
    } else if (token.kind == TokenKind::KeywordJudge) {
      diagnostics.error(
          token.range,
          "generated source may not introduce a judgment");
    }
  }
  return diagnostics.error_count() == initial_errors;
}

} // namespace draft
