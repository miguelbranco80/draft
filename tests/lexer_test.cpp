// Focused conformance tests for source locations, diagnostics, tokenization, and
// semicolon insertion.
//
// This test uses a deliberately tiny runner so the compiler foundation does not
// acquire a testing-framework dependency. Each test constructs complete source
// bytes, invokes the public lexer, and inspects tokens exactly as the parser will.
// Negative tests assert diagnostic content and source positioning, not merely a
// nonzero error count.

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/lexer.h"
#include "syntax/token.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "lexer_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct LexedSource {
  draft::SourceManager sources;
  draft::FileId file;
  draft::DiagnosticSink diagnostics;
  std::vector<draft::Token> tokens;
};

[[nodiscard]] LexedSource lex(std::string text) {
  LexedSource result;
  result.file = result.sources.add_source("test.draft", std::move(text));
  result.tokens = draft::lex_source(result.sources, result.file, result.diagnostics);
  return result;
}
[[nodiscard]] std::vector<draft::TokenKind> kinds(const LexedSource &source) {
  std::vector<draft::TokenKind> result;
  for (const draft::Token &token : source.tokens) {
    result.push_back(token.kind);
  }
  return result;
}

void test_source_coordinates(TestState &state) {
  draft::SourceManager sources;
  const draft::FileId file = sources.add_source("unicode.draft", "aé\nxyz\n");
  EXPECT(state, sources.file_count() == 1);
  EXPECT(state, sources.line_column({file, 0}).line == 1);
  EXPECT(state, sources.line_column({file, 0}).column == 1);
  EXPECT(state, sources.line_column({file, 3}).column == 3);
  EXPECT(state, sources.line_column({file, 4}).line == 2);
  EXPECT(state, sources.line_column({file, 4}).column == 1);
  EXPECT(state, sources.line_text(file, 2) == "xyz");
}

void test_basic_semicolon_insertion(TestState &state) {
  const LexedSource source = lex("package demo\nvalue := 42\nreturn\n");
  const std::vector expected{
      draft::TokenKind::KeywordPackage,
      draft::TokenKind::Identifier,
      draft::TokenKind::Semicolon,
      draft::TokenKind::Identifier,
      draft::TokenKind::ColonEqual,
      draft::TokenKind::IntegerLiteral,
      draft::TokenKind::Semicolon,
      draft::TokenKind::KeywordReturn,
      draft::TokenKind::Semicolon,
      draft::TokenKind::EndOfFile,
  };
  EXPECT(state, kinds(source) == expected);
  EXPECT(state, source.tokens[2].inserted);
  EXPECT(state, source.tokens[6].inserted);
  EXPECT(state, !source.diagnostics.has_errors());
}

// Aggregate and SIMD type constructors are reserved at their exact type-syntax
// boundary. `raw` remains an ordinary identifier: low-level concepts such as
// raw pointers and raw strings do not reserve a general-purpose source modifier.
void test_type_constructor_keywords(TestState &state) {
  const LexedSource source = lex("variant union simd packed raw\n");
  const std::vector expected{
      draft::TokenKind::KeywordVariant,
      draft::TokenKind::KeywordUnion,
      draft::TokenKind::KeywordSimd,
      draft::TokenKind::KeywordPacked,
      draft::TokenKind::Identifier,
      draft::TokenKind::Semicolon,
      draft::TokenKind::EndOfFile,
  };
  EXPECT(state, kinds(source) == expected);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_delimiter_suppression(TestState &state) {
  const LexedSource source = lex("call(\n  first,\n  second\n)\narray[\n0\n]\n");
  int inserted_semicolons = 0;
  for (const draft::Token &token : source.tokens) {
    if (token.kind == draft::TokenKind::Semicolon && token.inserted) {
      ++inserted_semicolons;
    }
  }
  EXPECT(state, inserted_semicolons == 2);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_attachment_continuation(TestState &state) {
  const LexedSource source = lex(
      "docs \"intent\"\n"
      "    file \"DESIGN.md\"\n"
      "    folder \"notes/\"\n"
      "value: u32 = ... \"make it\"\n"
      "    file \"VALUE.md\"\n");

  int inserted_semicolons = 0;
  for (const draft::Token &token : source.tokens) {
    if (token.kind == draft::TokenKind::Semicolon && token.inserted) {
      ++inserted_semicolons;
    }
  }
  // One semicolon ends the complete docs attachment group and one ends the
  // declaration containing the synthesis attachment group.
  EXPECT(state, inserted_semicolons == 2);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_comments_and_eof(TestState &state) {
  const LexedSource source = lex("first // trailing\nsecond /* block\ncomment */");
  const std::vector expected{
      draft::TokenKind::Identifier,
      draft::TokenKind::Semicolon,
      draft::TokenKind::Identifier,
      draft::TokenKind::Semicolon,
      draft::TokenKind::EndOfFile,
  };
  EXPECT(state, kinds(source) == expected);
  EXPECT(state, source.tokens[3].inserted);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_longest_tokens(TestState &state) {
  const LexedSource source = lex(
      ". .. ... :: := -> --- <<= >>= += -= *= /= %= &= |= ~= == != <= >= << >> && ||\n");
  const std::vector expected{
      draft::TokenKind::Dot,
      draft::TokenKind::DotDot,
      draft::TokenKind::Ellipsis,
      draft::TokenKind::ColonColon,
      draft::TokenKind::ColonEqual,
      draft::TokenKind::Arrow,
      draft::TokenKind::Uninitialized,
      draft::TokenKind::ShiftLeftEqual,
      draft::TokenKind::ShiftRightEqual,
      draft::TokenKind::PlusEqual,
      draft::TokenKind::MinusEqual,
      draft::TokenKind::StarEqual,
      draft::TokenKind::SlashEqual,
      draft::TokenKind::PercentEqual,
      draft::TokenKind::AmpersandEqual,
      draft::TokenKind::PipeEqual,
      draft::TokenKind::TildeEqual,
      draft::TokenKind::EqualEqual,
      draft::TokenKind::BangEqual,
      draft::TokenKind::LessEqual,
      draft::TokenKind::GreaterEqual,
      draft::TokenKind::ShiftLeft,
      draft::TokenKind::ShiftRight,
      draft::TokenKind::LogicalAnd,
      draft::TokenKind::LogicalOr,
      draft::TokenKind::EndOfFile,
  };
  EXPECT(state, kinds(source) == expected);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_decimal_and_dot_run_boundaries(TestState &state) {
  const LexedSource source = lex("1.25 1..2 3... 4.\n");
  const std::vector expected{
      draft::TokenKind::FloatLiteral,
      draft::TokenKind::IntegerLiteral,
      draft::TokenKind::DotDot,
      draft::TokenKind::IntegerLiteral,
      draft::TokenKind::IntegerLiteral,
      draft::TokenKind::Ellipsis,
      draft::TokenKind::FloatLiteral,
      draft::TokenKind::Semicolon,
      draft::TokenKind::EndOfFile,
  };
  EXPECT(state, kinds(source) == expected);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, source.diagnostics.error_count() == 1);
  EXPECT(state, source.diagnostics.diagnostics().front().range.is_valid());
  EXPECT(state,
      rendered.find("fractional part requires at least one digit") !=
          std::string::npos);
}

void test_literals(TestState &state) {
  const LexedSource source = lex(
      "0 0b1010 0o755 0xCA_FE 1_000 1.25 2e10 3.5e-2\n"
      "\"text\\n\\x00\\u{1f642}\" 'é' '\\n' `raw\ntext`\n");
  EXPECT(state, !source.diagnostics.has_errors());

  int integer_count = 0;
  int float_count = 0;
  int string_count = 0;
  int rune_count = 0;
  for (const draft::Token &token : source.tokens) {
    if (token.kind == draft::TokenKind::IntegerLiteral) ++integer_count;
    if (token.kind == draft::TokenKind::FloatLiteral) ++float_count;
    if (token.kind == draft::TokenKind::StringLiteral ||
        token.kind == draft::TokenKind::RawStringLiteral) ++string_count;
    if (token.kind == draft::TokenKind::RuneLiteral) ++rune_count;
  }
  EXPECT(state, integer_count == 5);
  EXPECT(state, float_count == 3);
  EXPECT(state, string_count == 2);
  EXPECT(state, rune_count == 2);
}

void test_malformed_literals(TestState &state) {
  const LexedSource source = lex("0b102 1__2 1. '\\u{d800}' 'ab' \"bad\\q\"\n");
  EXPECT(state, source.diagnostics.error_count() >= 6);
  const std::string rendered = draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("digit is not valid") != std::string::npos);
  EXPECT(state, rendered.find("underscore must separate") != std::string::npos);
  EXPECT(state, rendered.find("fractional part requires") != std::string::npos);
  EXPECT(state, rendered.find("does not name a Unicode scalar") != std::string::npos);
  EXPECT(state, rendered.find("exactly one Unicode scalar") != std::string::npos);
  EXPECT(state, rendered.find("unknown escape") != std::string::npos);
}

void test_utf8_and_identifier_rules(TestState &state) {
  std::string invalid = "ok\n";
  invalid.push_back(static_cast<char>(0xff));
  invalid += "\n";
  const LexedSource invalid_source = lex(std::move(invalid));
  EXPECT(state, invalid_source.diagnostics.error_count() == 1);
  EXPECT(state, draft::render_diagnostics(invalid_source.sources, invalid_source.diagnostics)
                    .find("not valid UTF-8") != std::string::npos);

  const LexedSource unicode_identifier = lex("café := 1\n");
  EXPECT(state, unicode_identifier.diagnostics.has_errors());
  EXPECT(state, draft::render_diagnostics(
                    unicode_identifier.sources, unicode_identifier.diagnostics)
                    .find("non-ASCII character") != std::string::npos);
}

void test_removed_annotation_prefix(TestState &state) {
  const LexedSource source = lex("Record :: @repr(C) struct {}\n");
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, source.diagnostics.error_count() == 1);
  EXPECT(state, rendered.find("unexpected character in Draft source") !=
                    std::string::npos);
}

void test_caret_and_uninitialized_end_lines(TestState &state) {
  const LexedSource source = lex("pointer^\nvalue: int = ---\n");
  int inserted_semicolons = 0;
  for (const draft::Token &token : source.tokens) {
    if (token.kind == draft::TokenKind::Semicolon && token.inserted) {
      ++inserted_semicolons;
    }
  }
  EXPECT(state, inserted_semicolons == 2);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_keyword_alternative_end_lines(TestState &state) {
  const LexedSource alternatives = lex(
      "first := .struct\n"
      "second := .distinct\n"
      "third := .variant\n"
      "fourth := .union\n"
      "fifth := .simd\n");
  int inserted_semicolons = 0;
  for (const draft::Token &token : alternatives.tokens) {
    if (token.kind == draft::TokenKind::Semicolon && token.inserted) {
      ++inserted_semicolons;
    }
  }
  EXPECT(state, inserted_semicolons == 5);
  EXPECT(state, !alternatives.diagnostics.has_errors());

  // The same keyword tokens remain non-terminating in type-constructor
  // positions. A newline before the associated type body/target is whitespace,
  // not a declaration boundary.
  const LexedSource constructors = lex("Record :: struct\n{}\nMeters :: distinct\ni64\n");
  int constructor_semicolons = 0;
  for (const draft::Token &token : constructors.tokens) {
    if (token.kind == draft::TokenKind::Semicolon && token.inserted) {
      ++constructor_semicolons;
    }
  }
  EXPECT(state, constructor_semicolons == 2);
  EXPECT(state, !constructors.diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_source_coordinates(state);
  test_basic_semicolon_insertion(state);
  test_type_constructor_keywords(state);
  test_delimiter_suppression(state);
  test_attachment_continuation(state);
  test_comments_and_eof(state);
  test_longest_tokens(state);
  test_decimal_and_dot_run_boundaries(state);
  test_literals(state);
  test_malformed_literals(state);
  test_utf8_and_identifier_rules(state);
  test_removed_annotation_prefix(state);
  test_caret_and_uninitialized_end_lines(state);
  test_keyword_alternative_end_lines(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " lexer test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all lexer tests passed\n";
  return EXIT_SUCCESS;
}
