// Decoding of lexer-validated Draft string and rune literal spellings.
//
// The lexer owns token boundaries and reports malformed source. Semantic phases
// call this module later to construct owned byte strings and Unicode scalar
// values without duplicating escape rules or retaining SourceManager views.
// Decoders also validate standalone input defensively, return no partial value,
// and have no dependency on semantic types or target layout.
//
// Relevant specification: docs/specification/01-core-language.md, "Source text and literals".

#pragma once

#include "syntax/token.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace draft {

// Decodes quoted and raw string tokens into their runtime byte sequence. The
// function remains defensive and returns nullopt for malformed standalone input
// even though normal callers receive spellings already validated by the lexer.
[[nodiscard]] std::optional<std::string> decode_string_literal(
    std::string_view spelling, TokenKind kind);

// Decodes one quoted rune token to its Unicode scalar value. Byte escapes map
// directly to U+0000..U+00FF; Unicode escapes and source UTF-8 are validated
// against the scalar domain. The function remains defensive for standalone
// callers even though the lexer already enforces exactly one scalar per token.
[[nodiscard]] std::optional<std::uint32_t> decode_rune_literal(
    std::string_view spelling);

} // namespace draft
