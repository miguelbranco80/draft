// Stable names and lexical classifications for Draft tokens.
//
// Names are intended for diagnostics, token dumps, and tests; changing one is a
// tooling-visible change. Statement-ending classification implements only the
// lexical half of semicolon insertion. Parenthesis/bracket nesting and attachment
// continuation are handled by the lexer pass that has surrounding context.

#include "syntax/token.h"

namespace draft {

std::string_view token_kind_name(TokenKind kind) {
  switch (kind) {
  case TokenKind::Invalid: return "invalid";
  case TokenKind::EndOfFile: return "end of file";
  case TokenKind::Newline: return "newline";
  case TokenKind::Identifier: return "identifier";
  case TokenKind::IntegerLiteral: return "integer literal";
  case TokenKind::FloatLiteral: return "floating literal";
  case TokenKind::StringLiteral: return "string literal";
  case TokenKind::RawStringLiteral: return "raw string literal";
  case TokenKind::RuneLiteral: return "rune literal";
  case TokenKind::KeywordPackage: return "package";
  case TokenKind::KeywordImport: return "import";
  case TokenKind::KeywordAs: return "as";
  case TokenKind::KeywordPub: return "pub";
  case TokenKind::KeywordProc: return "proc";
  case TokenKind::KeywordC: return "c";
  case TokenKind::KeywordAlign: return "align";
  case TokenKind::KeywordStruct: return "struct";
  case TokenKind::KeywordEnum: return "enum";
  case TokenKind::KeywordVariant: return "variant";
  case TokenKind::KeywordUnion: return "union";
  case TokenKind::KeywordDistinct: return "distinct";
  case TokenKind::KeywordThreadLocal: return "thread_local";
  case TokenKind::KeywordForeign: return "foreign";
  case TokenKind::KeywordExport: return "export";
  case TokenKind::KeywordIf: return "if";
  case TokenKind::KeywordElse: return "else";
  case TokenKind::KeywordFor: return "for";
  case TokenKind::KeywordIn: return "in";
  case TokenKind::KeywordOut: return "out";
  case TokenKind::KeywordSwitch: return "switch";
  case TokenKind::KeywordCase: return "case";
  case TokenKind::KeywordBreak: return "break";
  case TokenKind::KeywordContinue: return "continue";
  case TokenKind::KeywordReturn: return "return";
  case TokenKind::KeywordDefer: return "defer";
  case TokenKind::KeywordWhen: return "when";
  case TokenKind::KeywordUnchecked: return "unchecked";
  case TokenKind::KeywordDeny: return "deny";
  case TokenKind::KeywordAsm: return "asm";
  case TokenKind::KeywordClobber: return "clobber";
  case TokenKind::KeywordFlags: return "flags";
  case TokenKind::KeywordMemory: return "memory";
  case TokenKind::KeywordDocs: return "docs";
  case TokenKind::KeywordJudge: return "judge";
  case TokenKind::KeywordFile: return "file";
  case TokenKind::KeywordFolder: return "folder";
  case TokenKind::KeywordType: return "type";
  case TokenKind::KeywordInteger: return "integer";
  case TokenKind::KeywordFloat: return "float";
  case TokenKind::KeywordNumber: return "number";
  case TokenKind::KeywordTrue: return "true";
  case TokenKind::KeywordFalse: return "false";
  case TokenKind::KeywordNil: return "nil";
  case TokenKind::LeftParen: return "(";
  case TokenKind::RightParen: return ")";
  case TokenKind::LeftBracket: return "[";
  case TokenKind::RightBracket: return "]";
  case TokenKind::LeftBrace: return "{";
  case TokenKind::RightBrace: return "}";
  case TokenKind::Comma: return ",";
  case TokenKind::Semicolon: return ";";
  case TokenKind::Colon: return ":";
  case TokenKind::ColonColon: return "::";
  case TokenKind::ColonEqual: return ":=";
  case TokenKind::Dot: return ".";
  case TokenKind::DotDot: return "..";
  case TokenKind::Ellipsis: return "...";
  case TokenKind::Hash: return "#";
  case TokenKind::Plus: return "+";
  case TokenKind::Minus: return "-";
  case TokenKind::Star: return "*";
  case TokenKind::Slash: return "/";
  case TokenKind::Percent: return "%";
  case TokenKind::Ampersand: return "&";
  case TokenKind::Pipe: return "|";
  case TokenKind::Caret: return "^";
  case TokenKind::Bang: return "!";
  case TokenKind::Tilde: return "~";
  case TokenKind::Equal: return "=";
  case TokenKind::Less: return "<";
  case TokenKind::Greater: return ">";
  case TokenKind::PlusEqual: return "+=";
  case TokenKind::MinusEqual: return "-=";
  case TokenKind::StarEqual: return "*=";
  case TokenKind::SlashEqual: return "/=";
  case TokenKind::PercentEqual: return "%=";
  case TokenKind::AmpersandEqual: return "&=";
  case TokenKind::PipeEqual: return "|=";
  case TokenKind::TildeEqual: return "~=";
  case TokenKind::ShiftLeftEqual: return "<<=";
  case TokenKind::ShiftRightEqual: return ">>=";
  case TokenKind::EqualEqual: return "==";
  case TokenKind::BangEqual: return "!=";
  case TokenKind::LessEqual: return "<=";
  case TokenKind::GreaterEqual: return ">=";
  case TokenKind::ShiftLeft: return "<<";
  case TokenKind::ShiftRight: return ">>";
  case TokenKind::LogicalAnd: return "&&";
  case TokenKind::LogicalOr: return "||";
  case TokenKind::Arrow: return "->";
  case TokenKind::Uninitialized: return "---";
  }
  return "unknown token";
}

bool token_is_literal(TokenKind kind) {
  switch (kind) {
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::RawStringLiteral:
  case TokenKind::RuneLiteral:
  case TokenKind::KeywordTrue:
  case TokenKind::KeywordFalse:
  case TokenKind::KeywordNil:
    return true;
  default:
    return false;
  }
}

bool token_can_end_statement(TokenKind kind) {
  if (kind == TokenKind::Identifier || token_is_literal(kind)) {
    return true;
  }
  switch (kind) {
  // These spellings are contextual compiler names rather than unconditional
  // statement operators. `c` is special only before an ABI-qualified type,
  // while flags/memory terminate assembly clobber lines just as register
  // identifiers do. Their ordinary name uses therefore end statements.
  case TokenKind::KeywordC:
  case TokenKind::KeywordType:
  case TokenKind::KeywordInteger:
  case TokenKind::KeywordFloat:
  case TokenKind::KeywordNumber:
  case TokenKind::KeywordFlags:
  case TokenKind::KeywordMemory:
  case TokenKind::KeywordBreak:
  case TokenKind::KeywordContinue:
  case TokenKind::KeywordReturn:
  case TokenKind::RightParen:
  case TokenKind::RightBracket:
  case TokenKind::RightBrace:
  case TokenKind::Caret:
  case TokenKind::Uninitialized:
    return true;
  default:
    return false;
  }
}

bool token_is_contextual_alternative_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
      kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
      kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
      kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory ||
      kind == TokenKind::KeywordStruct || kind == TokenKind::KeywordVariant ||
      kind == TokenKind::KeywordUnion ||
      kind == TokenKind::KeywordDistinct;
}

} // namespace draft
