// Decoding of lexer-validated Draft literal spellings.

#pragma once

#include "syntax/token.h"

#include <optional>
#include <string>
#include <string_view>

namespace draft {

// Decodes quoted and raw string tokens into their runtime byte sequence. The
// function remains defensive and returns nullopt for malformed standalone input
// even though normal callers receive spellings already validated by the lexer.
[[nodiscard]] std::optional<std::string> decode_string_literal(
    std::string_view spelling, TokenKind kind);

} // namespace draft
