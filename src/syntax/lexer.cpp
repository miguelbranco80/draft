// Draft 1 lexer and semicolon-insertion pass.
//
// Lexing is intentionally split into two direct passes. The raw scanner retains
// newline tokens while discarding spaces and comments. A second pass uses those
// newlines, delimiter depth, and active attachment groups to insert semicolons.
// Keeping this transformation explicit is important: the parser sees one stable
// grammar regardless of whether a semicolon was written, and tooling can still
// identify synthetic tokens.
//
// The scanner validates literal spelling but does not construct literal values.
// Exact arbitrary-precision integers, rationals, IEEE values, UTF-8 strings, and
// runes are semantic values built later under an expected type. The token range
// remains the authoritative original spelling.
//
// Relevant specification: docs/specification/01-core-language.md, "Source text and literals".

#include "syntax/lexer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

struct Keyword {
  std::string_view spelling;
  TokenKind kind;
};

constexpr std::array kKeywords{
    Keyword{"package", TokenKind::KeywordPackage},
    Keyword{"import", TokenKind::KeywordImport},
    Keyword{"as", TokenKind::KeywordAs},
    Keyword{"pub", TokenKind::KeywordPub},
    Keyword{"proc", TokenKind::KeywordProc},
    Keyword{"c", TokenKind::KeywordC},
    Keyword{"align", TokenKind::KeywordAlign},
    Keyword{"struct", TokenKind::KeywordStruct},
    Keyword{"enum", TokenKind::KeywordEnum},
    Keyword{"variant", TokenKind::KeywordVariant},
    Keyword{"union", TokenKind::KeywordUnion},
    Keyword{"distinct", TokenKind::KeywordDistinct},
    Keyword{"simd", TokenKind::KeywordSimd},
    Keyword{"packed", TokenKind::KeywordPacked},
    Keyword{"bits", TokenKind::KeywordBits},
    Keyword{"thread_local", TokenKind::KeywordThreadLocal},
    Keyword{"foreign", TokenKind::KeywordForeign},
    Keyword{"export", TokenKind::KeywordExport},
    Keyword{"if", TokenKind::KeywordIf},
    Keyword{"else", TokenKind::KeywordElse},
    Keyword{"for", TokenKind::KeywordFor},
    Keyword{"in", TokenKind::KeywordIn},
    Keyword{"out", TokenKind::KeywordOut},
    Keyword{"switch", TokenKind::KeywordSwitch},
    Keyword{"case", TokenKind::KeywordCase},
    Keyword{"break", TokenKind::KeywordBreak},
    Keyword{"continue", TokenKind::KeywordContinue},
    Keyword{"return", TokenKind::KeywordReturn},
    Keyword{"defer", TokenKind::KeywordDefer},
    Keyword{"when", TokenKind::KeywordWhen},
    Keyword{"unchecked", TokenKind::KeywordUnchecked},
    Keyword{"deny", TokenKind::KeywordDeny},
    Keyword{"asm", TokenKind::KeywordAsm},
    Keyword{"clobber", TokenKind::KeywordClobber},
    Keyword{"flags", TokenKind::KeywordFlags},
    Keyword{"memory", TokenKind::KeywordMemory},
    Keyword{"docs", TokenKind::KeywordDocs},
    Keyword{"judge", TokenKind::KeywordJudge},
    Keyword{"file", TokenKind::KeywordFile},
    Keyword{"folder", TokenKind::KeywordFolder},
    Keyword{"type", TokenKind::KeywordType},
    Keyword{"integer", TokenKind::KeywordInteger},
    Keyword{"float", TokenKind::KeywordFloat},
    Keyword{"number", TokenKind::KeywordNumber},
    Keyword{"true", TokenKind::KeywordTrue},
    Keyword{"false", TokenKind::KeywordFalse},
    Keyword{"nil", TokenKind::KeywordNil},
};

[[nodiscard]] bool is_ascii_letter(char byte) {
  return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] bool is_ascii_digit(char byte) {
  return byte >= '0' && byte <= '9';
}

[[nodiscard]] bool is_identifier_start(char byte) {
  return is_ascii_letter(byte) || byte == '_';
}

[[nodiscard]] bool is_identifier_continue(char byte) {
  return is_identifier_start(byte) || is_ascii_digit(byte);
}

[[nodiscard]] bool is_hex_digit(char byte) {
  return is_ascii_digit(byte) || (byte >= 'a' && byte <= 'f') ||
         (byte >= 'A' && byte <= 'F');
}

[[nodiscard]] std::uint32_t hex_value(char byte) {
  if (byte >= '0' && byte <= '9') {
    return static_cast<std::uint32_t>(byte - '0');
  }
  if (byte >= 'a' && byte <= 'f') {
    return static_cast<std::uint32_t>(byte - 'a' + 10);
  }
  return static_cast<std::uint32_t>(byte - 'A' + 10);
}

[[nodiscard]] bool is_digit_for_base(char byte, unsigned base) {
  if (base <= 10) {
    return byte >= '0' && byte < static_cast<char>('0' + base);
  }
  return is_hex_digit(byte);
}

// Returns the width of one valid UTF-8 scalar beginning at offset or zero when
// the byte sequence is invalid. Overlong forms, surrogate scalars, and values
// above U+10FFFF are rejected here so later rune processing can trust width.
[[nodiscard]] std::uint32_t utf8_scalar_width(std::string_view text, std::uint32_t offset) {
  const std::size_t index = offset;
  if (index >= text.size()) {
    return 0;
  }
  const unsigned char first = static_cast<unsigned char>(text[index]);
  if (first < 0x80U) {
    return 1;
  }

  std::uint32_t width = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  if (first >= 0xc2U && first <= 0xdfU) {
    width = 2;
    value = first & 0x1fU;
    minimum = 0x80U;
  } else if (first >= 0xe0U && first <= 0xefU) {
    width = 3;
    value = first & 0x0fU;
    minimum = 0x800U;
  } else if (first >= 0xf0U && first <= 0xf4U) {
    width = 4;
    value = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return 0;
  }

  if (index + width > text.size()) {
    return 0;
  }
  for (std::uint32_t byte_index = 1; byte_index < width; ++byte_index) {
    const unsigned char continuation =
        static_cast<unsigned char>(text[index + byte_index]);
    if ((continuation & 0xc0U) != 0x80U) {
      return 0;
    }
    value = (value << 6U) | (continuation & 0x3fU);
  }

  if (value < minimum || value > 0x10ffffU ||
      (value >= 0xd800U && value <= 0xdfffU)) {
    return 0;
  }
  return width;
}

[[nodiscard]] TokenKind keyword_kind(std::string_view spelling) {
  // The keyword set is small and queried once per identifier. A linear table is
  // deterministic, allocation-free, and easier to audit than a static hash map.
  for (const Keyword &keyword : kKeywords) {
    if (keyword.spelling == spelling) {
      return keyword.kind;
    }
  }
  return TokenKind::Identifier;
}

class RawLexer {
public:
  RawLexer(
      const SourceManager &sources, FileId file, DiagnosticSink &diagnostics,
      bool retain_tooling_tokens = false)
      : file_(file), text_(sources.text(file)), diagnostics_(diagnostics),
        retain_tooling_tokens_(retain_tooling_tokens) {}

  // scan performs UTF-8 validation first, then emits raw tokens in byte order.
  // It always appends one EOF token even after errors, giving the semicolon pass
  // and parser an unconditional sentinel.
  [[nodiscard]] std::vector<Token> scan() {
    validate_utf8();
    while (!at_end()) {
      scan_one();
    }
    add(TokenKind::EndOfFile, offset_, offset_);
    return std::move(tokens_);
  }

  [[nodiscard]] std::vector<ToolingToken> take_tooling_tokens() {
    return std::move(tooling_tokens_);
  }

private:
  [[nodiscard]] bool at_end() const {
    return static_cast<std::size_t>(offset_) >= text_.size();
  }

  [[nodiscard]] char current() const {
    assert(!at_end());
    return text_[offset_];
  }

  [[nodiscard]] bool has(std::uint32_t lookahead) const {
    return static_cast<std::size_t>(offset_) + lookahead < text_.size();
  }

  [[nodiscard]] char peek(std::uint32_t lookahead) const {
    assert(has(lookahead));
    return text_[offset_ + lookahead];
  }

  [[nodiscard]] bool starts_with(std::string_view spelling) const {
    return text_.substr(offset_, spelling.size()) == spelling;
  }

  [[nodiscard]] SourceRange range(std::uint32_t begin, std::uint32_t end) const {
    return {{file_, begin}, {file_, end}};
  }

  void add(TokenKind kind, std::uint32_t begin, std::uint32_t end) {
    tokens_.push_back({kind, range(begin, end), false});
    if (retain_tooling_tokens_ && kind != TokenKind::Newline &&
        kind != TokenKind::EndOfFile) {
      tooling_tokens_.push_back(
          {ToolingTokenClass::Syntax, kind, range(begin, end)});
    }
  }

  void add_comment(
      ToolingTokenClass token_class, std::uint32_t begin, std::uint32_t end) {
    if (!retain_tooling_tokens_) return;
    tooling_tokens_.push_back(
        {token_class, TokenKind::Invalid, range(begin, end)});
  }

  void error(std::uint32_t begin, std::uint32_t end, std::string message) {
    diagnostics_.error(range(begin, end), std::move(message));
  }

  // Reports each invalid UTF-8 start byte independently. The scanner later
  // advances one byte for an invalid sequence but does not duplicate the error.
  void validate_utf8() {
    std::uint32_t cursor = 0;
    while (static_cast<std::size_t>(cursor) < text_.size()) {
      const unsigned char byte = static_cast<unsigned char>(text_[cursor]);
      if (byte < 0x80U) {
        ++cursor;
        continue;
      }
      const std::uint32_t width = utf8_scalar_width(text_, cursor);
      if (width == 0) {
        error(cursor, cursor + 1, "source is not valid UTF-8");
        ++cursor;
      } else {
        cursor += width;
      }
    }
  }

  void scan_one() {
    const char byte = current();
    if (byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f') {
      ++offset_;
      return;
    }
    if (byte == '\n' || byte == '\r') {
      scan_newline();
      return;
    }
    if (starts_with("//")) {
      scan_line_comment();
      return;
    }
    if (starts_with("/*")) {
      scan_block_comment();
      return;
    }
    if (is_identifier_start(byte)) {
      scan_identifier();
      return;
    }
    if (is_ascii_digit(byte)) {
      scan_number();
      return;
    }
    if (byte == '"') {
      scan_quoted_literal('"', TokenKind::StringLiteral, false);
      return;
    }
    if (byte == '\'') {
      scan_quoted_literal('\'', TokenKind::RuneLiteral, true);
      return;
    }
    if (byte == '`') {
      scan_raw_string();
      return;
    }
    scan_punctuation_or_invalid();
  }

  void scan_newline() {
    const std::uint32_t begin = offset_;
    if (current() == '\r' && has(1) && peek(1) == '\n') {
      offset_ += 2;
    } else {
      ++offset_;
    }
    add(TokenKind::Newline, begin, offset_);
  }

  void scan_line_comment() {
    const std::uint32_t begin = offset_;
    offset_ += 2;
    while (!at_end() && current() != '\n' && current() != '\r') {
      ++offset_;
    }
    add_comment(ToolingTokenClass::LineComment, begin, offset_);
    // The newline remains for scan_newline so a trailing comment cannot hide a
    // statement boundary, as required by the semicolon insertion rule.
  }

  void scan_block_comment() {
    const std::uint32_t begin = offset_;
    offset_ += 2;
    while (!at_end()) {
      if (starts_with("*/")) {
        offset_ += 2;
        add_comment(ToolingTokenClass::BlockComment, begin, offset_);
        return;
      }
      if (current() == '\n' || current() == '\r') {
        scan_newline();
      } else {
        ++offset_;
      }
    }
    error(begin, offset_, "unterminated block comment");
    add_comment(ToolingTokenClass::BlockComment, begin, offset_);
  }

  void scan_identifier() {
    const std::uint32_t begin = offset_;
    ++offset_;
    while (!at_end() && is_identifier_continue(current())) {
      ++offset_;
    }
    const std::string_view spelling = text_.substr(begin, offset_ - begin);
    add(keyword_kind(spelling), begin, offset_);
  }

  // Consumes one underscore-separated digit sequence and reports placement or
  // base errors. The caller decides which following bytes belong to another
  // numeric component, such as a fraction or exponent.
  void scan_digit_sequence(unsigned base, bool require_digit, std::string_view component) {
    bool saw_digit = false;
    bool previous_was_digit = false;
    const std::uint32_t begin = offset_;
    while (!at_end()) {
      const char byte = current();
      if (is_digit_for_base(byte, base)) {
        saw_digit = true;
        previous_was_digit = true;
        ++offset_;
        continue;
      }
      if (byte == '_') {
        if (!previous_was_digit || !has(1) || !is_digit_for_base(peek(1), base)) {
          error(offset_, offset_ + 1, "underscore must separate two digits");
        }
        previous_was_digit = false;
        ++offset_;
        continue;
      }
      break;
    }
    if (require_digit && !saw_digit) {
      error(begin, offset_, std::string(component) + " requires at least one digit");
    }
  }

  void scan_number() {
    const std::uint32_t begin = offset_;

    if (current() == '0' && has(1) &&
        (peek(1) == 'b' || peek(1) == 'o' || peek(1) == 'x')) {
      const char prefix = peek(1);
      const unsigned base = prefix == 'b' ? 2U : (prefix == 'o' ? 8U : 16U);
      offset_ += 2;
      scan_digit_sequence(base, true, "base-prefixed integer literal");

      // A decimal digit invalid for the selected base is almost certainly part
      // of the literal rather than a new token. Consume the alphanumeric tail so
      // the user receives one focused diagnostic and the parser can recover.
      if (!at_end() && (is_ascii_letter(current()) || is_ascii_digit(current()))) {
        const std::uint32_t invalid_begin = offset_;
        while (!at_end() &&
               (is_ascii_letter(current()) || is_ascii_digit(current()) || current() == '_')) {
          ++offset_;
        }
        error(invalid_begin, offset_, "digit is not valid for this integer base");
      }
      add(TokenKind::IntegerLiteral, begin, offset_);
      return;
    }

    scan_digit_sequence(10, true, "integer literal");
    bool is_float = false;

    // A decimal point belongs to a float only when it is not the beginning of
    // `..` or `...`. Draft requires digits on both sides, so an isolated `1.`
    // remains one malformed float and receives a lexical diagnostic, while
    // `1..2` is an integer followed by the static-pack marker and another
    // integer. The parser will reject that marker outside its one legal site.
    if (!at_end() && current() == '.' && !starts_with("..")) {
      is_float = true;
      ++offset_;
      scan_digit_sequence(10, true, "fractional part");
    }

    if (!at_end() && (current() == 'e' || current() == 'E')) {
      is_float = true;
      ++offset_;
      if (!at_end() && (current() == '+' || current() == '-')) {
        ++offset_;
      }
      scan_digit_sequence(10, true, "floating exponent");
    }

    add(is_float ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral, begin, offset_);
  }

  // Consumes one escape after the leading backslash and returns one because
  // every valid Draft escape denotes one byte or Unicode scalar in a literal.
  // Errors consume enough input to resume at a likely literal boundary.
  void scan_escape() {
    const std::uint32_t slash = offset_;
    ++offset_;
    if (at_end() || current() == '\n' || current() == '\r') {
      error(slash, offset_, "incomplete escape sequence");
      return;
    }

    const char escape = current();
    if (escape == '\\' || escape == '"' || escape == '\'' || escape == 'n' ||
        escape == 'r' || escape == 't' || escape == '0') {
      ++offset_;
      return;
    }
    if (escape == 'x') {
      ++offset_;
      for (unsigned digit = 0; digit < 2; ++digit) {
        if (at_end() || !is_hex_digit(current())) {
          error(slash, offset_, "\\x escape requires exactly two hexadecimal digits");
          return;
        }
        ++offset_;
      }
      return;
    }
    if (escape == 'u') {
      ++offset_;
      if (at_end() || current() != '{') {
        error(slash, offset_, "\\u escape requires '{' followed by hexadecimal digits");
        return;
      }
      ++offset_;
      std::uint32_t scalar = 0;
      unsigned digits = 0;
      while (!at_end() && is_hex_digit(current())) {
        if (digits < 8) {
          scalar = (scalar << 4U) | hex_value(current());
        }
        ++digits;
        ++offset_;
      }
      if (digits == 0 || at_end() || current() != '}') {
        error(slash, offset_, "\\u escape requires hexadecimal digits and a closing '}'");
        return;
      }
      ++offset_;
      if (digits > 6 || scalar > 0x10ffffU ||
          (scalar >= 0xd800U && scalar <= 0xdfffU)) {
        error(slash, offset_, "\\u escape does not name a Unicode scalar value");
      }
      return;
    }

    ++offset_;
    error(slash, offset_, "unknown escape sequence");
  }

  void scan_quoted_literal(char delimiter, TokenKind kind, bool require_one_scalar) {
    const std::uint32_t begin = offset_;
    ++offset_; // Opening quote.
    std::uint32_t scalar_count = 0;
    bool terminated = false;

    while (!at_end()) {
      if (current() == delimiter) {
        ++offset_;
        terminated = true;
        break;
      }
      if (current() == '\n' || current() == '\r') {
        error(begin, offset_, "quoted literal cannot contain a newline");
        break;
      }
      if (current() == '\\') {
        scan_escape();
        ++scalar_count;
        continue;
      }

      const unsigned char byte = static_cast<unsigned char>(current());
      if (byte < 0x80U) {
        ++offset_;
      } else {
        const std::uint32_t width = utf8_scalar_width(text_, offset_);
        offset_ += width == 0 ? 1 : width;
      }
      ++scalar_count;
    }

    if (!terminated && (at_end() || current() != delimiter)) {
      error(begin, offset_, "unterminated quoted literal");
    }
    if (require_one_scalar && scalar_count != 1) {
      error(begin, offset_, "rune literal must contain exactly one Unicode scalar");
    }
    add(kind, begin, offset_);
  }

  void scan_raw_string() {
    const std::uint32_t begin = offset_;
    ++offset_;
    while (!at_end() && current() != '`') {
      ++offset_;
    }
    if (at_end()) {
      error(begin, offset_, "unterminated raw string literal");
    } else {
      ++offset_;
    }
    add(TokenKind::RawStringLiteral, begin, offset_);
  }

  void scan_punctuation_or_invalid() {
    const std::uint32_t begin = offset_;

    struct FixedToken {
      std::string_view spelling;
      TokenKind kind;
    };
    // Longest spellings appear first. This table handles every multi-byte token;
    // the switch below then handles one-byte punctuation without allocation.
    constexpr std::array fixed_tokens{
        FixedToken{"<<=", TokenKind::ShiftLeftEqual},
        FixedToken{">>=", TokenKind::ShiftRightEqual},
        FixedToken{"...", TokenKind::Ellipsis},
        FixedToken{"..", TokenKind::DotDot},
        FixedToken{"---", TokenKind::Uninitialized},
        FixedToken{"::", TokenKind::ColonColon},
        FixedToken{":=", TokenKind::ColonEqual},
        FixedToken{"->", TokenKind::Arrow},
        FixedToken{"+=", TokenKind::PlusEqual},
        FixedToken{"-=", TokenKind::MinusEqual},
        FixedToken{"*=", TokenKind::StarEqual},
        FixedToken{"/=", TokenKind::SlashEqual},
        FixedToken{"%=", TokenKind::PercentEqual},
        FixedToken{"&=", TokenKind::AmpersandEqual},
        FixedToken{"|=", TokenKind::PipeEqual},
        FixedToken{"~=", TokenKind::TildeEqual},
        FixedToken{"==", TokenKind::EqualEqual},
        FixedToken{"!=", TokenKind::BangEqual},
        FixedToken{"<=", TokenKind::LessEqual},
        FixedToken{">=", TokenKind::GreaterEqual},
        FixedToken{"<<", TokenKind::ShiftLeft},
        FixedToken{">>", TokenKind::ShiftRight},
        FixedToken{"&&", TokenKind::LogicalAnd},
        FixedToken{"||", TokenKind::LogicalOr},
    };
    for (const FixedToken &token : fixed_tokens) {
      if (starts_with(token.spelling)) {
        offset_ += static_cast<std::uint32_t>(token.spelling.size());
        add(token.kind, begin, offset_);
        return;
      }
    }

    TokenKind kind = TokenKind::Invalid;
    switch (current()) {
    case '(': kind = TokenKind::LeftParen; break;
    case ')': kind = TokenKind::RightParen; break;
    case '[': kind = TokenKind::LeftBracket; break;
    case ']': kind = TokenKind::RightBracket; break;
    case '{': kind = TokenKind::LeftBrace; break;
    case '}': kind = TokenKind::RightBrace; break;
    case ',': kind = TokenKind::Comma; break;
    case ';': kind = TokenKind::Semicolon; break;
    case ':': kind = TokenKind::Colon; break;
    case '.': kind = TokenKind::Dot; break;
    case '#': kind = TokenKind::Hash; break;
    case '+': kind = TokenKind::Plus; break;
    case '-': kind = TokenKind::Minus; break;
    case '*': kind = TokenKind::Star; break;
    case '/': kind = TokenKind::Slash; break;
    case '%': kind = TokenKind::Percent; break;
    case '&': kind = TokenKind::Ampersand; break;
    case '|': kind = TokenKind::Pipe; break;
    case '^': kind = TokenKind::Caret; break;
    case '!': kind = TokenKind::Bang; break;
    case '~': kind = TokenKind::Tilde; break;
    case '=': kind = TokenKind::Equal; break;
    case '<': kind = TokenKind::Less; break;
    case '>': kind = TokenKind::Greater; break;
    default: break;
    }

    if (kind != TokenKind::Invalid) {
      ++offset_;
      add(kind, begin, offset_);
      return;
    }

    const unsigned char byte = static_cast<unsigned char>(current());
    std::uint32_t width = 1;
    if (byte >= 0x80U) {
      const std::uint32_t decoded_width = utf8_scalar_width(text_, offset_);
      if (decoded_width != 0) {
        width = decoded_width;
        error(begin, begin + width, "non-ASCII character is not valid in a Draft identifier");
      }
      // Invalid UTF-8 was already reported by validate_utf8.
    } else {
      error(begin, begin + 1, "unexpected character in Draft source");
    }
    offset_ += width;
    add(TokenKind::Invalid, begin, offset_);
  }

  FileId file_;
  std::string_view text_;
  DiagnosticSink &diagnostics_;
  bool retain_tooling_tokens_ = false;
  std::uint32_t offset_ = 0;
  std::vector<Token> tokens_;
  std::vector<ToolingToken> tooling_tokens_;
};

enum class AttachmentState {
  None,
  AfterConstruct,
  AfterClaim,
  ExpectPath,
  AfterPath,
};

[[nodiscard]] bool is_attachment_keyword(TokenKind kind) {
  return kind == TokenKind::KeywordFile || kind == TokenKind::KeywordFolder;
}

[[nodiscard]] bool is_attachment_string(TokenKind kind) {
  return kind == TokenKind::StringLiteral || kind == TokenKind::RawStringLiteral;
}

// Tracks only the small surface grammar needed to decide whether a newline
// before `file` or `folder` continues docs, judge, or synthesis attachments.
// Full validation remains in the parser. Any unrelated token closes the group.
void advance_attachment_state(AttachmentState &state, TokenKind kind) {
  if (kind == TokenKind::KeywordDocs || kind == TokenKind::KeywordJudge ||
      kind == TokenKind::Ellipsis) {
    state = AttachmentState::AfterConstruct;
    return;
  }
  if (state == AttachmentState::None) {
    return;
  }
  if (kind == TokenKind::Semicolon) {
    state = AttachmentState::None;
    return;
  }
  if (is_attachment_keyword(kind) &&
      (state == AttachmentState::AfterConstruct || state == AttachmentState::AfterClaim ||
       state == AttachmentState::AfterPath)) {
    state = AttachmentState::ExpectPath;
    return;
  }
  if (is_attachment_string(kind) && state == AttachmentState::ExpectPath) {
    state = AttachmentState::AfterPath;
    return;
  }
  if (is_attachment_string(kind) && state == AttachmentState::AfterConstruct) {
    state = AttachmentState::AfterClaim;
    return;
  }
  state = AttachmentState::None;
}

[[nodiscard]] TokenKind next_non_newline_kind(
    const std::vector<Token> &tokens, std::size_t start) {
  for (std::size_t index = start; index < tokens.size(); ++index) {
    if (tokens[index].kind != TokenKind::Newline) {
      return tokens[index].kind;
    }
  }
  return TokenKind::EndOfFile;
}

// Reserved type-constructor words normally cannot finish a source item. They
// can finish one when used as stable compiler-enum alternatives such as
// `.struct`, `.variant`, `.union`, `.distinct`, and `.simd`. Keeping this
// dot-prefixed exception in the insertion pass avoids declaring any constructor
// keyword a general statement-ending token, which would incorrectly split
// `variant\n{ ... }`, `distinct\nT`, or `simd\n[N]T` type syntax.
[[nodiscard]] bool token_sequence_can_end_statement(
    TokenKind before_previous, TokenKind previous) {
  if (token_can_end_statement(previous)) return true;
  return before_previous == TokenKind::Dot &&
      token_is_contextual_alternative_name(previous);
}

// Converts raw newline tokens into inserted semicolons. Parentheses and brackets
// suppress insertion unconditionally. Braces do not: a closing brace can end a
// declaration or statement, and `} else {` must consequently remain on one line.
[[nodiscard]] std::vector<Token> insert_semicolons(std::vector<Token> raw_tokens) {
  std::vector<Token> tokens;
  tokens.reserve(raw_tokens.size());

  std::uint32_t paren_depth = 0;
  std::uint32_t bracket_depth = 0;
  TokenKind before_previous = TokenKind::Invalid;
  TokenKind previous = TokenKind::Invalid;
  AttachmentState attachment = AttachmentState::None;

  for (std::size_t index = 0; index < raw_tokens.size(); ++index) {
    const Token token = raw_tokens[index];
    if (token.kind == TokenKind::Newline) {
      const TokenKind next = next_non_newline_kind(raw_tokens, index + 1);
      const bool continues_attachment =
          attachment != AttachmentState::None && is_attachment_keyword(next);
      if (paren_depth == 0 && bracket_depth == 0 &&
          token_sequence_can_end_statement(before_previous, previous) &&
          !continues_attachment) {
        tokens.push_back({TokenKind::Semicolon, SourceRange::at(token.range.begin.file, token.range.begin.offset), true});
        before_previous = previous;
        previous = TokenKind::Semicolon;
      }
      if (!continues_attachment) {
        attachment = AttachmentState::None;
      }
      continue;
    }

    if (token.kind == TokenKind::EndOfFile) {
      if (paren_depth == 0 && bracket_depth == 0 &&
          token_sequence_can_end_statement(before_previous, previous)) {
        tokens.push_back({TokenKind::Semicolon, SourceRange::at(token.range.begin.file, token.range.begin.offset), true});
      }
      tokens.push_back(token);
      break;
    }

    switch (token.kind) {
    case TokenKind::LeftParen: ++paren_depth; break;
    case TokenKind::RightParen:
      if (paren_depth > 0) {
        --paren_depth;
      }
      break;
    case TokenKind::LeftBracket: ++bracket_depth; break;
    case TokenKind::RightBracket:
      if (bracket_depth > 0) {
        --bracket_depth;
      }
      break;
    default: break;
    }

    tokens.push_back(token);
    before_previous = previous;
    previous = token.kind;
    advance_attachment_state(attachment, token.kind);
  }
  return tokens;
}

} // namespace

std::vector<Token> lex_source(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics) {
  RawLexer lexer(sources, file, diagnostics);
  return insert_semicolons(lexer.scan());
}

std::vector<ToolingToken> lex_source_for_tooling(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics) {
  RawLexer lexer(sources, file, diagnostics, true);
  // The parser token vector is intentionally discarded only after the shared
  // scanner has completed every normal validation and recovery transition.
  (void)lexer.scan();
  return lexer.take_tooling_tokens();
}

} // namespace draft
