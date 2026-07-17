// Draft lexical token vocabulary and token/source relationships.
//
// TokenKind contains every fixed spelling used by the Draft 1 surface grammar.
// Identifiers, literals, and punctuation retain their original byte ranges; the
// lexer does not decode values because exact numeric and string interpretation
// belongs to parsing and constant evaluation. Inserted semicolons are marked so
// diagnostics and formatting tools can distinguish them from explicit source.
//
// Newline is an internal staging token used while applying semicolon insertion.
// It is never present in the vector returned by lex_source.
//
// Relevant specification: 01-core-language.md, "Source text and literals".

#pragma once

#include "source/source.h"

#include <string_view>

namespace draft {

enum class TokenKind {
  Invalid,
  EndOfFile,
  Newline,

  Identifier,
  IntegerLiteral,
  FloatLiteral,
  StringLiteral,
  RawStringLiteral,
  RuneLiteral,

  KeywordPackage,
  KeywordImport,
  KeywordAs,
  KeywordPub,
  KeywordProc,
  KeywordC,
  KeywordStruct,
  KeywordEnum,
  KeywordUnion,
  KeywordRaw,
  KeywordDistinct,
  KeywordThreadLocal,
  KeywordForeign,
  KeywordExport,
  KeywordIf,
  KeywordElse,
  KeywordFor,
  KeywordIn,
  KeywordOut,
  KeywordSwitch,
  KeywordCase,
  KeywordBreak,
  KeywordContinue,
  KeywordReturn,
  KeywordDefer,
  KeywordWhen,
  KeywordUnchecked,
  KeywordDeny,
  KeywordAsm,
  KeywordClobber,
  KeywordFlags,
  KeywordMemory,
  KeywordDocs,
  KeywordJudge,
  KeywordFile,
  KeywordFolder,
  KeywordType,
  KeywordInteger,
  KeywordFloat,
  KeywordNumber,
  KeywordTrue,
  KeywordFalse,
  KeywordNil,

  LeftParen,
  RightParen,
  LeftBracket,
  RightBracket,
  LeftBrace,
  RightBrace,
  Comma,
  Semicolon,
  Colon,
  ColonColon,
  ColonEqual,
  Dot,
  Ellipsis,
  At,
  Hash,

  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Ampersand,
  Pipe,
  Caret,
  Bang,
  Tilde,
  Equal,
  Less,
  Greater,

  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  PercentEqual,
  AmpersandEqual,
  PipeEqual,
  CaretEqual,
  ShiftLeftEqual,
  ShiftRightEqual,
  EqualEqual,
  BangEqual,
  LessEqual,
  GreaterEqual,
  ShiftLeft,
  ShiftRight,
  LogicalAnd,
  LogicalOr,
  Arrow,
  Uninitialized,
};

// Token is a lightweight reference into SourceManager. Inserted semicolons use
// the zero-width byte position at the triggering newline or EOF. All other
// tokens cover their exact source spelling.
struct Token {
  TokenKind kind = TokenKind::Invalid;
  SourceRange range;
  bool inserted = false;
};

[[nodiscard]] std::string_view token_kind_name(TokenKind kind);
[[nodiscard]] bool token_is_literal(TokenKind kind);
[[nodiscard]] bool token_can_end_statement(TokenKind kind);

} // namespace draft
