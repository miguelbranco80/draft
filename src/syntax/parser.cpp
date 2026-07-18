// Recursive-descent and precedence parser for the complete Draft 1 surface.
//
// The parser selects grammar categories using source position, never resolved
// names. This is central to Draft's design: braces after `if` are statement
// blocks, braces after `struct` are member lists, and braces in `deny` inherit
// the category of the surrounding position. The resulting concrete tree keeps
// neutral forms where only semantics can decide meaning, such as bracket
// application versus indexing.
//
// Error recovery is deliberately local and direct. A missing token reports at
// the current byte boundary; declaration and statement recovery then consumes
// through a semicolon or closing brace. Invalid expressions consume at least one
// token unless already at a caller-owned delimiter. These rules guarantee
// progress without manufacturing source or silently changing program meaning.
//
// Assembly's language-owned in/out/clobber directives become structural child
// nodes and their input values use the ordinary expression grammar. Individual
// instruction rows retain raw token spans. The AArch64 parser later interprets
// those rows under the versioned target profile; this parser does not guess an
// instruction or register grammar.
//
// Relevant specification: 01-core-language.md sections 3-4 and the surface
// constructs in 03-agent-synthesis.md, 04-native-interop.md, and
// 05-denials-validation.md.

#include "syntax/parser.h"

#include "syntax/lexer.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace draft {
namespace {

enum class MemberMode {
  Struct,
  Enum,
  Union,
};

enum class SynthesisPosition {
  Declaration,
  Member,
  Statement,
  Expression,
  Assembly,
};

class Parser {
public:
  Parser(
      FileId file,
      std::vector<Token> tokens,
      DiagnosticSink &diagnostics)
      : tree_(file, std::move(tokens)), diagnostics_(diagnostics) {}

  [[nodiscard]] SyntaxTree parse() {
    const std::uint32_t start = position_;
    std::vector<NodeId> children;

    children.push_back(parse_package_clause());
    while (at(TokenKind::KeywordDocs)) {
      children.push_back(parse_documentation());
    }
    while (at(TokenKind::KeywordImport)) {
      children.push_back(parse_import_clause());
    }
    children.push_back(parse_declaration_list(TokenKind::EndOfFile));

    const NodeId root = tree_.add_node(NodeKind::SourceFile, start, position_, std::move(children));
    tree_.set_root(root);
    return std::move(tree_);
  }

private:
  [[nodiscard]] const Token &current() const {
    return tree_.token(position_);
  }

  [[nodiscard]] const Token &lookahead(std::uint32_t distance) const {
    const std::uint32_t index = position_ + distance;
    if (static_cast<std::size_t>(index) >= tree_.tokens().size()) {
      return tree_.tokens().back();
    }
    return tree_.token(index);
  }

  [[nodiscard]] bool at(TokenKind kind) const {
    return current().kind == kind;
  }

  [[nodiscard]] bool at_end() const {
    return at(TokenKind::EndOfFile);
  }

  [[nodiscard]] bool match(TokenKind kind) {
    if (!at(kind)) {
      return false;
    }
    ++position_;
    return true;
  }

  void advance() {
    if (!at_end()) {
      ++position_;
    }
  }

  [[nodiscard]] SourceRange current_range() const {
    return current().range;
  }

  void error_here(std::string message) {
    diagnostics_.error(current_range(), std::move(message));
  }

  [[nodiscard]] bool expect(TokenKind kind, std::string message) {
    if (match(kind)) {
      return true;
    }
    error_here(std::move(message));
    return false;
  }

  [[nodiscard]] bool is_contextual_name(TokenKind kind) const {
    // `c` is both the calling-convention modifier and the conventional ordinary
    // alias in `import core/c as c`. `memory` and `flags` are directives only
    // after assembly `clobber`, while `core/memory` and ordinary declarations
    // must still be nameable. Constraint spellings are compiler-defined names
    // valid in their parameter position. Treating these as contextual names
    // keeps the lexer useful while preserving the source examples.
    return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
           kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
           kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
           kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory;
  }

  [[nodiscard]] bool at_name() const {
    return is_contextual_name(current().kind);
  }

  [[nodiscard]] bool expect_name(std::string message) {
    if (at_name()) {
      advance();
      return true;
    }
    error_here(std::move(message));
    return false;
  }

  void skip_semicolons() {
    while (match(TokenKind::Semicolon)) {
    }
  }

  // Consumes a required item terminator or advances to one recovery boundary.
  // A closing brace is left for the list parser that owns it.
  void finish_item(std::string message) {
    if (match(TokenKind::Semicolon)) {
      return;
    }
    error_here(std::move(message));
    while (!at_end() && !at(TokenKind::Semicolon) && !at(TokenKind::RightBrace)) {
      advance();
    }
    (void)match(TokenKind::Semicolon);
  }

  // Type member lists conventionally use commas, while semicolon insertion also
  // permits a line-ending member with no comma. Accepting both matches Draft's
  // explicit-semicolon equivalence without forcing one formatting style.
  void finish_member(std::string message) {
    if (match(TokenKind::Comma) || match(TokenKind::Semicolon)) {
      return;
    }
    error_here(std::move(message));
    while (!at_end() && !at(TokenKind::Comma) && !at(TokenKind::Semicolon) &&
           !at(TokenKind::RightBrace)) {
      advance();
    }
    if (!match(TokenKind::Comma)) {
      (void)match(TokenKind::Semicolon);
    }
  }

  [[nodiscard]] NodeId error_node(std::uint32_t start, std::string message) {
    error_here(std::move(message));
    if (!at_end() && !at(TokenKind::Semicolon) && !at(TokenKind::RightBrace) &&
        !at(TokenKind::RightParen) && !at(TokenKind::RightBracket) &&
        !at(TokenKind::Comma)) {
      advance();
    }
    return tree_.add_node(NodeKind::Error, start, position_);
  }

  [[nodiscard]] NodeId parse_package_clause() {
    const std::uint32_t start = position_;
    if (!expect(TokenKind::KeywordPackage, "source file must begin with a package declaration")) {
      while (!at_end() && !at(TokenKind::Semicolon)) {
        advance();
      }
    } else {
      (void)expect_name("expected package name after 'package'");
    }
    finish_item("expected semicolon after package declaration");
    return tree_.add_node(NodeKind::PackageClause, start, position_);
  }

  [[nodiscard]] NodeId parse_import_clause() {
    const std::uint32_t start = position_;
    advance(); // `import`.
    std::vector<NodeId> children;

    const std::uint32_t path_start = position_;
    if (!expect_name("expected package path after 'import'")) {
      finish_item("expected semicolon after import");
      return tree_.add_node(NodeKind::ImportClause, start, position_);
    }
    while (match(TokenKind::Slash)) {
      if (!expect_name("expected package path component after '/'")) {
        break;
      }
    }
    children.push_back(tree_.add_node(NodeKind::ImportPath, path_start, position_));

    if (match(TokenKind::KeywordAs)) {
      (void)expect_name("expected local package alias after 'as'");
    }
    finish_item("expected semicolon after import");
    return tree_.add_node(NodeKind::ImportClause, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_declaration_list(TokenKind terminator) {
    const std::uint32_t start = position_;
    std::vector<NodeId> declarations;
    skip_semicolons();
    while (!at_end() && !at(terminator)) {
      const std::uint32_t before = position_;
      declarations.push_back(parse_declaration_item());
      if (position_ == before) {
        // This assertion-equivalent recovery protects malformed input from an
        // infinite loop without turning a user error into a compiler crash.
        error_here("parser made no progress in declaration list");
        advance();
      }
      skip_semicolons();
    }
    return tree_.add_node(NodeKind::DeclarationList, start, position_, std::move(declarations));
  }

  [[nodiscard]] NodeId parse_declaration_item() {
    if (at(TokenKind::KeywordDocs)) return parse_documentation();
    if (at(TokenKind::KeywordJudge)) return parse_judgment();
    if (at(TokenKind::Ellipsis)) return parse_synthesis(SynthesisPosition::Declaration, true);
    if (at(TokenKind::KeywordWhen)) return parse_when_declaration();
    if (at(TokenKind::KeywordDeny)) return parse_deny_declaration();
    if (at(TokenKind::KeywordForeign)) return parse_foreign_block();
    if (at(TokenKind::KeywordExport)) return parse_export_declaration();
    return parse_declaration();
  }

  [[nodiscard]] NodeId parse_documentation() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    bool has_content = false;
    if (at(TokenKind::StringLiteral) || at(TokenKind::RawStringLiteral)) {
      advance();
      has_content = true;
    }
    while (at(TokenKind::KeywordFile) || at(TokenKind::KeywordFolder)) {
      children.push_back(parse_attachment());
      has_content = true;
    }
    if (!has_content) {
      error_here("docs requires an inline string, file, or folder attachment");
    }
    finish_item("expected semicolon after docs attachment group");
    return tree_.add_node(NodeKind::Documentation, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_judgment() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    if (at(TokenKind::StringLiteral) || at(TokenKind::RawStringLiteral)) {
      advance();
    } else {
      error_here("judge requires a claim string");
    }
    while (at(TokenKind::KeywordFile) || at(TokenKind::KeywordFolder)) {
      children.push_back(parse_attachment());
    }
    finish_item("expected semicolon after judge");
    return tree_.add_node(NodeKind::Judgment, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_attachment() {
    const std::uint32_t start = position_;
    advance(); // `file` or `folder`.
    if (!expect(TokenKind::StringLiteral, "attachment path must be a quoted string")) {
      if (at(TokenKind::RawStringLiteral)) {
        advance();
      }
    }
    return tree_.add_node(NodeKind::Attachment, start, position_);
  }

  [[nodiscard]] NodeId parse_synthesis(SynthesisPosition position, bool finish) {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    if (at(TokenKind::StringLiteral) || at(TokenKind::RawStringLiteral)) {
      advance();
    }
    while (at(TokenKind::KeywordFile) || at(TokenKind::KeywordFolder)) {
      children.push_back(parse_attachment());
    }
    if (finish) {
      finish_item("expected semicolon after synthesis site");
    }

    NodeKind kind = NodeKind::SynthesisExpression;
    if (position == SynthesisPosition::Declaration) kind = NodeKind::SynthesisDeclaration;
    if (position == SynthesisPosition::Member) kind = NodeKind::SynthesisMember;
    if (position == SynthesisPosition::Statement) kind = NodeKind::SynthesisStatement;
    if (position == SynthesisPosition::Assembly) kind = NodeKind::SynthesisAssembly;
    return tree_.add_node(kind, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_declaration_region() {
    const std::uint32_t start = position_;
    (void)expect(TokenKind::LeftBrace, "expected '{' to begin declaration region");
    NodeId list = parse_declaration_list(TokenKind::RightBrace);
    (void)expect(TokenKind::RightBrace, "expected '}' to end declaration region");
    std::vector<NodeId> children{list};
    return tree_.add_node(NodeKind::DeclarationList, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_when_declaration() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    children.push_back(parse_expression(1, false));
    children.push_back(parse_declaration_region());
    if (match(TokenKind::KeywordElse)) {
      if (at(TokenKind::KeywordWhen)) {
        children.push_back(parse_when_declaration());
      } else {
        children.push_back(parse_declaration_region());
      }
    }
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::WhenDeclaration, start, position_, std::move(children));
  }

  [[nodiscard]] std::vector<NodeId> parse_deny_selectors() {
    std::vector<NodeId> selectors;
    selectors.push_back(parse_deny_selector());
    while (match(TokenKind::Comma)) {
      selectors.push_back(parse_deny_selector());
    }
    return selectors;
  }

  [[nodiscard]] NodeId parse_deny_selector() {
    if (at(TokenKind::KeywordAsm) || at(TokenKind::KeywordUnchecked)) {
      const std::uint32_t start = position_;
      advance();
      return tree_.add_node(NodeKind::NameExpression, start, position_);
    }
    return parse_expression(1, false);
  }

  [[nodiscard]] NodeId parse_deny_declaration() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children = parse_deny_selectors();
    children.push_back(parse_declaration_region());
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::DenyDeclaration, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_foreign_block() {
    const std::uint32_t start = position_;
    advance();
    (void)expect_name("expected link-provider name after 'foreign'");
    std::vector<NodeId> children;
    children.push_back(parse_declaration_region());
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::ForeignBlock, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_export_declaration() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    children.push_back(parse_declaration());
    return tree_.add_node(NodeKind::ExportDeclaration, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_name_list() {
    const std::uint32_t start = position_;
    if (!expect_name("expected declaration name")) {
      return tree_.add_node(NodeKind::NameList, start, position_);
    }
    while (match(TokenKind::Comma)) {
      if (!expect_name("expected declaration name after ','")) {
        break;
      }
    }
    return tree_.add_node(NodeKind::NameList, start, position_);
  }

  // Declaration patterns are syntactically known before type checking. A plain
  // pattern may contain grouped names (`a, b: T`), while a parenthesized pattern
  // destructures one tuple value (`(a, b): (T, U)`). Keeping those forms distinct
  // prevents assignment comma rules from leaking into declaration parsing.
  [[nodiscard]] NodeId parse_binding_pattern() {
    const std::uint32_t start = position_;
    if (!match(TokenKind::LeftParen)) {
      std::vector<NodeId> children{parse_name_list()};
      return tree_.add_node(NodeKind::BindingPattern, start, position_, std::move(children));
    }

    std::vector<NodeId> children;
    const std::uint32_t names_start = position_;
    (void)expect_name("expected binding name in tuple pattern");
    while (match(TokenKind::Comma)) {
      if (at(TokenKind::RightParen)) {
        break;
      }
      (void)expect_name("expected binding name after ',' in tuple pattern");
    }
    children.push_back(tree_.add_node(NodeKind::NameList, names_start, position_));
    (void)expect(TokenKind::RightParen, "expected ')' after tuple binding pattern");
    return tree_.add_node(NodeKind::TuplePattern, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_parametric_parameters() {
    const std::uint32_t start = position_;
    std::vector<NodeId> parameters;
    (void)expect(TokenKind::LeftBracket, "expected '['");
    while (!at_end() && !at(TokenKind::RightBracket)) {
      const std::uint32_t parameter_start = position_;
      std::vector<NodeId> children;
      (void)expect_name("expected parametric parameter name");
      (void)expect(TokenKind::Colon, "expected ':' after parametric parameter name");
      children.push_back(parse_type());
      parameters.push_back(tree_.add_node(
          NodeKind::ParametricParameter, parameter_start, position_, std::move(children)));
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
    (void)expect(TokenKind::RightBracket, "expected ']' after parametric parameters");
    return tree_.add_node(
        NodeKind::ParametricParameterList, start, position_, std::move(parameters));
  }

  [[nodiscard]] bool begins_type() const {
    switch (current().kind) {
    case TokenKind::At:
    case TokenKind::Caret:
    case TokenKind::LeftBracket:
    case TokenKind::Hash:
    case TokenKind::KeywordProc:
    case TokenKind::KeywordStruct:
    case TokenKind::KeywordEnum:
    case TokenKind::KeywordUnion:
    case TokenKind::KeywordRaw:
    case TokenKind::KeywordDistinct:
      return true;
    case TokenKind::KeywordC:
      return lookahead(1).kind == TokenKind::KeywordProc;
    default:
      return false;
    }
  }

  [[nodiscard]] NodeId parse_declaration(bool finish = true) {
    const std::uint32_t start = position_;
    std::vector<NodeId> children;
    (void)match(TokenKind::KeywordPub);
    (void)match(TokenKind::KeywordThreadLocal);

    if (!at_name() && !at(TokenKind::LeftParen)) {
      NodeId error = error_node(start, "expected declaration");
      if (finish) {
        finish_item("expected semicolon after invalid declaration");
      }
      return error;
    }
    NodeId pattern = parse_binding_pattern();
    children.push_back(pattern);
    if (tree_.node(pattern).kind == NodeKind::BindingPattern && at(TokenKind::LeftBracket)) {
      children.push_back(parse_parametric_parameters());
    }

    if (match(TokenKind::Colon)) {
      if (!at(TokenKind::Equal) && !at(TokenKind::Semicolon)) {
        children.push_back(parse_type());
      }
      if (match(TokenKind::Equal)) {
        children.push_back(parse_expression());
      }
    } else if (match(TokenKind::ColonEqual)) {
      children.push_back(parse_expression());
    } else if (match(TokenKind::ColonColon)) {
      if (at(TokenKind::KeywordProc) ||
          (at(TokenKind::KeywordC) &&
           (lookahead(1).kind == TokenKind::KeywordProc ||
            lookahead(1).kind == TokenKind::StringLiteral))) {
        children.push_back(parse_procedure(true));
      } else if (begins_type()) {
        children.push_back(parse_type());
      } else {
        children.push_back(parse_expression());
      }
    } else {
      error_here("expected ':', ':=', or '::' after declaration name");
    }

    if (finish) {
      finish_item("expected semicolon after declaration");
    }
    return tree_.add_node(NodeKind::Declaration, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_parameter_list() {
    const std::uint32_t start = position_;
    std::vector<NodeId> parameters;
    (void)expect(TokenKind::LeftParen, "expected '('");
    while (!at_end() && !at(TokenKind::RightParen)) {
      const std::uint32_t parameter_start = position_;
      std::vector<NodeId> children;
      children.push_back(parse_name_list());
      (void)expect(TokenKind::Colon, "expected ':' after parameter name");
      children.push_back(parse_type());
      parameters.push_back(tree_.add_node(
          NodeKind::Parameter, parameter_start, position_, std::move(children)));
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
    (void)expect(TokenKind::RightParen, "expected ')' after parameters");
    return tree_.add_node(NodeKind::ParameterList, start, position_, std::move(parameters));
  }

  [[nodiscard]] NodeId parse_procedure(bool allow_body) {
    return parse_procedure(allow_body, position_, {});
  }

  // Type attributes are parsed before the constructor is known. Retaining the
  // prefix on a procedure type lets semantic analysis reject the unsupported
  // attribute instead of silently orphaning its syntax node.
  [[nodiscard]] NodeId parse_procedure(
      bool allow_body,
      std::uint32_t start,
      std::vector<NodeId> children) {
    (void)match(TokenKind::KeywordC);
    if (at(TokenKind::StringLiteral)) {
      advance(); // Optional exact linker symbol on foreign/export declarations.
    }
    (void)expect(TokenKind::KeywordProc, "expected 'proc'");
    children.push_back(parse_parameter_list());
    if (match(TokenKind::Arrow)) {
      const std::uint32_t result_start = position_ - 1;
      std::vector<NodeId> result_children{parse_type()};
      children.push_back(tree_.add_node(
          NodeKind::ResultClause, result_start, position_, std::move(result_children)));
    }
    if (allow_body && at(TokenKind::LeftBrace)) {
      children.push_back(parse_block());
      return tree_.add_node(NodeKind::Procedure, start, position_, std::move(children));
    }
    return tree_.add_node(NodeKind::ProcedureType, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_attribute_list() {
    const std::uint32_t start = position_;
    std::vector<NodeId> attributes;
    while (at(TokenKind::At)) {
      const std::uint32_t attribute_start = position_;
      advance();
      (void)expect_name("expected attribute name after '@'");
      std::vector<NodeId> children;
      if (match(TokenKind::LeftParen)) {
        if (!at(TokenKind::RightParen)) {
          children.push_back(parse_expression());
          while (match(TokenKind::Comma)) {
            children.push_back(parse_expression());
          }
        }
        (void)expect(TokenKind::RightParen, "expected ')' after attribute arguments");
      }
      attributes.push_back(tree_.add_node(
          NodeKind::Attribute, attribute_start, position_, std::move(children)));
    }
    return tree_.add_node(NodeKind::AttributeList, start, position_, std::move(attributes));
  }

  [[nodiscard]] NodeId parse_type() {
    const std::uint32_t start = position_;
    std::vector<NodeId> children;

    if (at(TokenKind::At)) {
      children.push_back(parse_attribute_list());
    }
    if (match(TokenKind::Caret)) {
      children.push_back(parse_type());
      return tree_.add_node(NodeKind::PointerType, start, position_, std::move(children));
    }
    if (match(TokenKind::LeftBracket)) {
      if (match(TokenKind::Caret)) {
        (void)expect(TokenKind::RightBracket, "expected ']' in multi-pointer type");
        children.push_back(parse_type());
        return tree_.add_node(
            NodeKind::MultiPointerType, start, position_, std::move(children));
      }
      if (match(TokenKind::RightBracket)) {
        children.push_back(parse_type());
        return tree_.add_node(NodeKind::SliceType, start, position_, std::move(children));
      }
      children.push_back(parse_expression());
      (void)expect(TokenKind::RightBracket, "expected ']' after array length");
      children.push_back(parse_type());
      return tree_.add_node(NodeKind::ArrayType, start, position_, std::move(children));
    }
    if (match(TokenKind::Hash)) {
      (void)expect_name("expected 'simd' after '#'");
      (void)expect(TokenKind::LeftBracket, "expected '[' before SIMD lane count");
      children.push_back(parse_expression());
      (void)expect(TokenKind::RightBracket, "expected ']' after SIMD lane count");
      children.push_back(parse_type());
      return tree_.add_node(NodeKind::SimdType, start, position_, std::move(children));
    }
    if (at(TokenKind::KeywordProc) ||
        (at(TokenKind::KeywordC) && lookahead(1).kind == TokenKind::KeywordProc)) {
      return parse_procedure(false, start, std::move(children));
    }
    if (match(TokenKind::KeywordDistinct)) {
      children.push_back(parse_type());
      return tree_.add_node(NodeKind::DistinctType, start, position_, std::move(children));
    }
    if (match(TokenKind::LeftParen)) {
      children.push_back(parse_type());
      if (!match(TokenKind::Comma)) {
        (void)expect(TokenKind::RightParen, "expected ')' after grouped type");
        return tree_.add_node(NodeKind::NamedType, start, position_, std::move(children));
      }
      do {
        children.push_back(parse_type());
      } while (match(TokenKind::Comma) && !at(TokenKind::RightParen));
      (void)expect(TokenKind::RightParen, "expected ')' after tuple type");
      return tree_.add_node(NodeKind::TupleType, start, position_, std::move(children));
    }

    NodeKind aggregate_kind = NodeKind::Error;
    MemberMode member_mode = MemberMode::Struct;
    if (match(TokenKind::KeywordStruct)) {
      aggregate_kind = NodeKind::StructType;
      member_mode = MemberMode::Struct;
    } else if (match(TokenKind::KeywordEnum)) {
      aggregate_kind = NodeKind::EnumType;
      member_mode = MemberMode::Enum;
      if (!at(TokenKind::LeftBrace)) {
        children.push_back(parse_type());
      }
    } else if (match(TokenKind::KeywordUnion)) {
      aggregate_kind = NodeKind::TaggedUnionType;
      member_mode = MemberMode::Union;
      if (!at(TokenKind::LeftBrace)) {
        children.push_back(parse_type());
      }
    } else if (match(TokenKind::KeywordRaw)) {
      (void)expect(TokenKind::KeywordUnion, "expected 'union' after 'raw'");
      aggregate_kind = NodeKind::RawUnionType;
      member_mode = MemberMode::Struct;
    }
    if (aggregate_kind != NodeKind::Error) {
      children.push_back(parse_member_list(member_mode));
      return tree_.add_node(aggregate_kind, start, position_, std::move(children));
    }

    if (!at_name()) {
      return error_node(start, "expected type");
    }
    advance();
    while (match(TokenKind::Dot)) {
      (void)expect_name("expected type name after '.'");
    }
    if (match(TokenKind::LeftBracket)) {
      if (!at(TokenKind::RightBracket)) {
        children.push_back(parse_expression());
        while (match(TokenKind::Comma)) {
          children.push_back(parse_expression());
        }
      }
      (void)expect(TokenKind::RightBracket, "expected ']' after type arguments");
    }
    return tree_.add_node(NodeKind::NamedType, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_member_list(MemberMode mode) {
    const std::uint32_t start = position_;
    std::vector<NodeId> members;
    (void)expect(TokenKind::LeftBrace, "expected '{' to begin type members");
    skip_semicolons();
    while (!at_end() && !at(TokenKind::RightBrace)) {
      const std::uint32_t before = position_;
      if (at(TokenKind::KeywordDocs)) {
        members.push_back(parse_documentation());
      } else if (at(TokenKind::KeywordJudge)) {
        members.push_back(parse_judgment());
      } else if (at(TokenKind::Ellipsis)) {
        members.push_back(parse_synthesis(SynthesisPosition::Member, true));
      } else if (at(TokenKind::KeywordWhen)) {
        members.push_back(parse_when_member(mode));
      } else if (at(TokenKind::KeywordDeny)) {
        members.push_back(parse_deny_member(mode));
      } else {
        members.push_back(parse_member(mode));
      }
      if (position_ == before) {
        error_here("parser made no progress in type member list");
        advance();
      }
      skip_semicolons();
    }
    (void)expect(TokenKind::RightBrace, "expected '}' after type members");
    return tree_.add_node(NodeKind::MemberList, start, position_, std::move(members));
  }

  [[nodiscard]] NodeId parse_member_region(MemberMode mode) {
    return parse_member_list(mode);
  }

  [[nodiscard]] NodeId parse_when_member(MemberMode mode) {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children{parse_expression(1, false), parse_member_region(mode)};
    if (match(TokenKind::KeywordElse)) {
      if (at(TokenKind::KeywordWhen)) {
        children.push_back(parse_when_member(mode));
      } else {
        children.push_back(parse_member_region(mode));
      }
    }
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::WhenMember, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_deny_member(MemberMode mode) {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children = parse_deny_selectors();
    children.push_back(parse_member_region(mode));
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::DenyMember, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_member(MemberMode mode) {
    const std::uint32_t start = position_;
    std::vector<NodeId> children;
    if (!expect_name("expected type member name")) {
      NodeId error = error_node(start, "invalid type member");
      finish_item("expected semicolon after invalid member");
      return error;
    }

    if (mode == MemberMode::Struct) {
      while (match(TokenKind::Comma)) {
        (void)expect_name("expected field name after ','");
      }
      (void)expect(TokenKind::Colon, "expected ':' after field name");
      children.push_back(parse_type());
      finish_member("expected ',' or semicolon after field");
      return tree_.add_node(NodeKind::FieldMember, start, position_, std::move(children));
    }
    if (mode == MemberMode::Enum) {
      if (match(TokenKind::Equal)) {
        children.push_back(parse_expression());
      }
      finish_member("expected ',' or semicolon after enum member");
      return tree_.add_node(NodeKind::EnumMember, start, position_, std::move(children));
    }

    if (match(TokenKind::Colon)) {
      children.push_back(parse_type());
    }
    finish_member("expected ',' or semicolon after union alternative");
    return tree_.add_node(NodeKind::UnionAlternative, start, position_, std::move(children));
  }

  [[nodiscard]] int binary_precedence(TokenKind kind) const {
    switch (kind) {
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::ShiftLeft:
    case TokenKind::ShiftRight:
    case TokenKind::Ampersand:
      return 7;
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Pipe:
    case TokenKind::Caret:
      return 6;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
      return 5;
    case TokenKind::LogicalAnd: return 4;
    case TokenKind::LogicalOr: return 3;
    default: return 0;
    }
  }

  [[nodiscard]] NodeId parse_expression(int minimum_precedence = 1, bool allow_composite = true) {
    const std::uint32_t start = position_;
    NodeId left = parse_unary_expression(allow_composite);
    bool consumed_comparison = false;

    while (true) {
      const int precedence = binary_precedence(current().kind);
      if (precedence < minimum_precedence) {
        break;
      }
      if (precedence == 5 && consumed_comparison) {
        error_here("comparison operators do not associate; parenthesize the intended comparison");
      }
      if (precedence == 5) {
        consumed_comparison = true;
      }
      advance();
      NodeId right = parse_expression(precedence + 1, allow_composite);
      std::vector<NodeId> children{left, right};
      left = tree_.add_node(NodeKind::BinaryExpression, start, position_, std::move(children));
    }

    if (minimum_precedence <= 1 && match(TokenKind::KeywordIf)) {
      NodeId condition = parse_expression(1, false);
      (void)expect(TokenKind::KeywordElse, "conditional expression requires 'else'");
      NodeId alternative = parse_expression(1, allow_composite);
      std::vector<NodeId> children{left, condition, alternative};
      left = tree_.add_node(
          NodeKind::ConditionalExpression, start, position_, std::move(children));
    }
    return left;
  }

  [[nodiscard]] NodeId parse_unary_expression(bool allow_composite) {
    const std::uint32_t start = position_;
    switch (current().kind) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Bang:
    case TokenKind::Tilde:
    case TokenKind::Ampersand: {
      advance();
      std::vector<NodeId> children{parse_unary_expression(allow_composite)};
      return tree_.add_node(NodeKind::UnaryExpression, start, position_, std::move(children));
    }
    default:
      return parse_postfix_expression(allow_composite);
    }
  }

  // `^` is both postfix dereference and binary XOR. Postfix parsing must leave
  // the token for the precedence loop when the next token directly begins an
  // XOR right operand. A following binary operator or postfix continuation
  // instead closes a dereference: `pointer^ + 1`, `pointer^.field`, and
  // `pointer^^` therefore keep their ordinary postfix meaning.
  //
  // `+`, `-`, and `&` are themselves binary operators as well as prefixes, so
  // the unparenthesized form chooses postfix dereference. An XOR with one of
  // those unary operands is written `left ^ (-right)`. `!` and `~` are only
  // prefixes and can begin the XOR operand directly. This rule depends only on
  // token kinds; whitespace other than semicolon insertion never changes it.
  [[nodiscard]] bool caret_begins_binary_operation() const {
    assert(at(TokenKind::Caret));
    const TokenKind next = lookahead(1).kind;
    if (is_contextual_name(next) || token_is_literal(next)) return true;
    switch (next) {
    case TokenKind::Uninitialized:
    case TokenKind::Ellipsis:
    case TokenKind::KeywordAsm:
    case TokenKind::KeywordDeny:
    case TokenKind::LeftParen:
    case TokenKind::Bang:
    case TokenKind::Tilde:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] NodeId parse_postfix_expression(bool allow_composite) {
    const std::uint32_t start = position_;
    NodeId expression = parse_primary_expression();
    while (true) {
      if (match(TokenKind::LeftParen)) {
        std::vector<NodeId> children{expression};
        if (!at(TokenKind::RightParen)) {
          children.push_back(parse_expression());
          while (match(TokenKind::Comma)) {
            if (at(TokenKind::RightParen)) break;
            children.push_back(parse_expression());
          }
        }
        (void)expect(TokenKind::RightParen, "expected ')' after call arguments");
        expression = tree_.add_node(NodeKind::CallExpression, start, position_, std::move(children));
        continue;
      }
      if (match(TokenKind::LeftBracket)) {
        std::vector<NodeId> children{expression};
        bool slice = false;
        if (!at(TokenKind::Colon) && !at(TokenKind::RightBracket)) {
          children.push_back(parse_expression());
        }
        if (match(TokenKind::Colon)) {
          slice = true;
          if (!at(TokenKind::RightBracket)) {
            children.push_back(parse_expression());
          }
        } else {
          while (match(TokenKind::Comma)) {
            children.push_back(parse_expression());
          }
        }
        (void)expect(TokenKind::RightBracket, "expected ']' after bracket expression");
        expression = tree_.add_node(
            slice ? NodeKind::SliceExpression : NodeKind::BracketExpression,
            start,
            position_,
            std::move(children));
        continue;
      }
      if (match(TokenKind::Dot)) {
        // Tuple fields use decimal selectors such as `.0`; named aggregate and
        // package members use contextual names. Semantic analysis rejects a
        // numeric selector on a non-tuple and non-decimal/out-of-range forms.
        if (at(TokenKind::IntegerLiteral)) {
          advance();
        } else {
          (void)expect_name("expected member name or tuple index after '.'");
        }
        std::vector<NodeId> children{expression};
        expression = tree_.add_node(NodeKind::MemberExpression, start, position_, std::move(children));
        continue;
      }
      if (at(TokenKind::Caret) && !caret_begins_binary_operation()) {
        advance();
        std::vector<NodeId> children{expression};
        expression = tree_.add_node(
            NodeKind::DereferenceExpression, start, position_, std::move(children));
        continue;
      }
      if (allow_composite && at(TokenKind::LeftBrace)) {
        expression = parse_composite_expression(start, expression);
        continue;
      }
      break;
    }
    return expression;
  }

  [[nodiscard]] NodeId parse_primary_expression() {
    const std::uint32_t start = position_;
    if (at_name()) {
      advance();
      return tree_.add_node(NodeKind::NameExpression, start, position_);
    }
    if (token_is_literal(current().kind)) {
      advance();
      return tree_.add_node(NodeKind::LiteralExpression, start, position_);
    }
    if (match(TokenKind::Uninitialized)) {
      return tree_.add_node(NodeKind::UninitializedExpression, start, position_);
    }
    if (at(TokenKind::Ellipsis)) {
      return parse_synthesis(SynthesisPosition::Expression, false);
    }
    if (at(TokenKind::KeywordAsm)) {
      return parse_asm(false);
    }
    if (at(TokenKind::KeywordDeny)) {
      return parse_deny_expression();
    }
    if (match(TokenKind::Dot)) {
      (void)expect_name("expected alternative name after '.'");
      std::vector<NodeId> children;
      if (match(TokenKind::LeftParen)) {
        if (!at(TokenKind::RightParen)) {
          children.push_back(parse_expression());
        }
        (void)expect(TokenKind::RightParen, "expected ')' after alternative payload");
      }
      return tree_.add_node(
          NodeKind::ContextualAlternativeExpression, start, position_, std::move(children));
    }
    if (match(TokenKind::LeftParen)) {
      std::vector<NodeId> children;
      children.push_back(parse_expression());
      if (!match(TokenKind::Comma)) {
        (void)expect(TokenKind::RightParen, "expected ')' after expression");
        return tree_.add_node(NodeKind::GroupExpression, start, position_, std::move(children));
      }
      do {
        children.push_back(parse_expression());
      } while (match(TokenKind::Comma) && !at(TokenKind::RightParen));
      (void)expect(TokenKind::RightParen, "expected ')' after tuple expression");
      return tree_.add_node(NodeKind::TupleExpression, start, position_, std::move(children));
    }
    if (begins_type()) {
      return parse_type();
    }
    return error_node(start, "expected expression");
  }

  [[nodiscard]] NodeId parse_composite_expression(std::uint32_t start, NodeId type_expression) {
    std::vector<NodeId> children{type_expression};
    (void)expect(TokenKind::LeftBrace, "expected '{' in composite literal");
    while (!at_end() && !at(TokenKind::RightBrace)) {
      const std::uint32_t element_start = position_;
      std::vector<NodeId> element_children;
      if (at_name() && lookahead(1).kind == TokenKind::Equal) {
        advance();
        advance();
      }
      element_children.push_back(parse_expression());
      children.push_back(tree_.add_node(
          NodeKind::CompositeElement, element_start, position_, std::move(element_children)));
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
    (void)expect(TokenKind::RightBrace, "expected '}' after composite literal");
    return tree_.add_node(NodeKind::CompositeExpression, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_deny_expression() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children = parse_deny_selectors();
    (void)expect(TokenKind::LeftBrace, "expected '{' before denied expression");
    children.push_back(parse_expression());
    (void)expect(TokenKind::RightBrace, "expected '}' after denied expression");
    return tree_.add_node(NodeKind::DenyExpression, start, position_, std::move(children));
  }

  [[nodiscard]] bool looks_like_declaration() const {
    std::uint32_t distance = 0;
    if (lookahead(distance).kind == TokenKind::LeftParen) {
      std::uint32_t depth = 1;
      ++distance;
      while (depth != 0 && lookahead(distance).kind != TokenKind::EndOfFile) {
        if (lookahead(distance).kind == TokenKind::LeftParen) ++depth;
        if (lookahead(distance).kind == TokenKind::RightParen) --depth;
        ++distance;
      }
      const TokenKind after_pattern = lookahead(distance).kind;
      return after_pattern == TokenKind::Colon || after_pattern == TokenKind::ColonEqual;
    }
    if (!is_contextual_name(lookahead(distance).kind)) {
      return false;
    }
    ++distance;
    while (lookahead(distance).kind == TokenKind::Comma &&
           is_contextual_name(lookahead(distance + 1).kind)) {
      distance += 2;
    }
    if (lookahead(distance).kind == TokenKind::LeftBracket) {
      std::uint32_t depth = 1;
      ++distance;
      while (depth != 0 && lookahead(distance).kind != TokenKind::EndOfFile) {
        if (lookahead(distance).kind == TokenKind::LeftBracket) ++depth;
        if (lookahead(distance).kind == TokenKind::RightBracket) --depth;
        ++distance;
      }
    }
    const TokenKind next = lookahead(distance).kind;
    return next == TokenKind::Colon || next == TokenKind::ColonEqual ||
           next == TokenKind::ColonColon;
  }

  [[nodiscard]] NodeId parse_block() {
    const std::uint32_t start = position_;
    (void)expect(TokenKind::LeftBrace, "expected '{' to begin block");
    std::vector<NodeId> statements;
    skip_semicolons();
    while (!at_end() && !at(TokenKind::RightBrace)) {
      const std::uint32_t before = position_;
      statements.push_back(parse_statement());
      if (position_ == before) {
        error_here("parser made no progress in statement block");
        advance();
      }
      skip_semicolons();
    }
    const NodeId list = tree_.add_node(
        NodeKind::StatementList, start, position_, std::move(statements));
    (void)expect(TokenKind::RightBrace, "expected '}' to end block");
    std::vector<NodeId> children{list};
    return tree_.add_node(NodeKind::Block, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_statement() {
    if (at(TokenKind::LeftBrace)) return parse_block();
    if (at(TokenKind::KeywordIf)) return parse_if_statement();
    if (at(TokenKind::KeywordFor)) return parse_for_statement();
    if (at(TokenKind::KeywordSwitch)) return parse_switch_statement();
    if (at(TokenKind::KeywordWhen)) return parse_when_statement();
    if (at(TokenKind::KeywordDeny)) return parse_deny_statement();
    if (at(TokenKind::KeywordUnchecked)) return parse_unchecked_statement();
    if (at(TokenKind::KeywordJudge)) return parse_judgment();
    if (at(TokenKind::Ellipsis)) return parse_synthesis(SynthesisPosition::Statement, true);
    if (at(TokenKind::KeywordReturn)) return parse_return_statement();
    if (at(TokenKind::KeywordBreak)) return parse_simple_statement(NodeKind::BreakStatement);
    if (at(TokenKind::KeywordContinue)) return parse_simple_statement(NodeKind::ContinueStatement);
    if (at(TokenKind::KeywordDefer)) return parse_defer_statement();
    if (at(TokenKind::KeywordAsm)) {
      NodeId assembly = parse_asm(true);
      finish_item("expected semicolon after assembly statement");
      return assembly;
    }
    if (looks_like_declaration()) {
      const std::uint32_t start = position_;
      std::vector<NodeId> children{parse_declaration()};
      return tree_.add_node(
          NodeKind::DeclarationStatement, start, position_, std::move(children));
    }
    return parse_expression_or_assignment_statement(true);
  }

  [[nodiscard]] NodeId parse_simple_statement(NodeKind kind) {
    const std::uint32_t start = position_;
    advance();
    finish_item("expected semicolon after control-flow statement");
    return tree_.add_node(kind, start, position_);
  }

  [[nodiscard]] NodeId parse_return_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    if (!at(TokenKind::Semicolon)) {
      children.push_back(parse_expression());
    }
    finish_item("expected semicolon after return");
    return tree_.add_node(NodeKind::ReturnStatement, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_defer_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children{parse_expression()};
    finish_item("expected semicolon after defer");
    return tree_.add_node(NodeKind::DeferStatement, start, position_, std::move(children));
  }

  [[nodiscard]] bool is_assignment_operator(TokenKind kind) const {
    switch (kind) {
    case TokenKind::Equal:
    case TokenKind::PlusEqual:
    case TokenKind::MinusEqual:
    case TokenKind::StarEqual:
    case TokenKind::SlashEqual:
    case TokenKind::PercentEqual:
    case TokenKind::AmpersandEqual:
    case TokenKind::PipeEqual:
    case TokenKind::CaretEqual:
    case TokenKind::ShiftLeftEqual:
    case TokenKind::ShiftRightEqual:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] NodeId parse_expression_or_assignment_statement(
      bool finish, bool allow_composite = true) {
    const std::uint32_t start = position_;
    std::vector<NodeId> children{parse_expression(1, allow_composite)};
    while (match(TokenKind::Comma)) {
      children.push_back(parse_expression(1, allow_composite));
    }
    NodeKind kind = NodeKind::ExpressionStatement;
    if (is_assignment_operator(current().kind)) {
      kind = NodeKind::AssignmentStatement;
      advance();
      children.push_back(parse_expression(1, allow_composite));
      while (match(TokenKind::Comma)) {
        children.push_back(parse_expression(1, allow_composite));
      }
    }
    if (finish) {
      finish_item("expected semicolon after statement");
    }
    return tree_.add_node(kind, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_if_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children{parse_expression(1, false), parse_block()};
    if (match(TokenKind::KeywordElse)) {
      if (at(TokenKind::KeywordIf)) {
        children.push_back(parse_if_statement());
      } else {
        children.push_back(parse_block());
      }
    }
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::IfStatement, start, position_, std::move(children));
  }

  [[nodiscard]] bool begins_iteration_header() const {
    std::uint32_t distance = 0;
    if (!is_contextual_name(lookahead(distance).kind)) return false;
    ++distance;
    if (lookahead(distance).kind == TokenKind::Comma) {
      ++distance;
      if (!is_contextual_name(lookahead(distance).kind)) return false;
      ++distance;
    }
    return lookahead(distance).kind == TokenKind::KeywordIn;
  }

  [[nodiscard]] NodeId parse_for_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    if (at(TokenKind::LeftBrace)) {
      children.push_back(parse_block());
    } else if (begins_iteration_header()) {
      const std::uint32_t header_start = position_;
      (void)expect_name("expected iteration binding");
      if (match(TokenKind::Comma)) {
        (void)expect_name("expected index binding after ','");
      }
      (void)expect(TokenKind::KeywordIn, "expected 'in' in iteration header");
      std::vector<NodeId> header_children{parse_expression(1, false)};
      children.push_back(tree_.add_node(
          NodeKind::IterationHeader, header_start, position_, std::move(header_children)));
      children.push_back(parse_block());
    } else {
      const std::uint32_t header_start = position_;
      std::vector<NodeId> header_children;
      if (looks_like_declaration()) {
        header_children.push_back(parse_declaration(false));
      } else {
        header_children.push_back(parse_expression_or_assignment_statement(false, false));
      }
      if (match(TokenKind::Semicolon)) {
        if (!at(TokenKind::Semicolon)) {
          header_children.push_back(parse_expression(1, false));
        }
        (void)expect(TokenKind::Semicolon, "expected second ';' in for clause");
        if (!at(TokenKind::LeftBrace)) {
          header_children.push_back(parse_expression_or_assignment_statement(false, false));
        }
        children.push_back(tree_.add_node(
            NodeKind::ForClause, header_start, position_, std::move(header_children)));
      } else {
        children.insert(children.end(), header_children.begin(), header_children.end());
      }
      children.push_back(parse_block());
    }
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::ForStatement, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_switch_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children{parse_expression(1, false)};
    (void)expect(TokenKind::LeftBrace, "expected '{' after switch subject");
    skip_semicolons();
    while (!at_end() && !at(TokenKind::RightBrace)) {
      const std::uint32_t case_start = position_;
      std::vector<NodeId> case_children;
      (void)expect(TokenKind::KeywordCase, "expected 'case' in switch");
      if (!at(TokenKind::Colon)) {
        case_children.push_back(parse_expression());
        while (match(TokenKind::Comma)) {
          case_children.push_back(parse_expression());
        }
      }
      (void)expect(TokenKind::Colon, "expected ':' after switch case labels");
      std::vector<NodeId> statements;
      skip_semicolons();
      while (!at_end() && !at(TokenKind::KeywordCase) && !at(TokenKind::RightBrace)) {
        statements.push_back(parse_statement());
        skip_semicolons();
      }
      case_children.push_back(tree_.add_node(
          NodeKind::StatementList, case_start, position_, std::move(statements)));
      children.push_back(tree_.add_node(
          NodeKind::SwitchCase, case_start, position_, std::move(case_children)));
    }
    (void)expect(TokenKind::RightBrace, "expected '}' after switch cases");
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::SwitchStatement, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_when_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children{parse_expression(1, false), parse_block()};
    if (match(TokenKind::KeywordElse)) {
      if (at(TokenKind::KeywordWhen)) {
        children.push_back(parse_when_statement());
      } else {
        children.push_back(parse_block());
      }
    }
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::WhenStatement, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_deny_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children = parse_deny_selectors();
    children.push_back(parse_block());
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::DenyStatement, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_unchecked_statement() {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children{parse_block()};
    (void)match(TokenKind::Semicolon);
    return tree_.add_node(NodeKind::UncheckedStatement, start, position_, std::move(children));
  }

  [[nodiscard]] NodeId parse_asm(bool statement_position) {
    const std::uint32_t start = position_;
    advance();
    std::vector<NodeId> children;
    (void)expect_name("expected assembly architecture after 'asm'");
    bool has_result = false;
    if (match(TokenKind::Arrow)) {
      has_result = true;
      children.push_back(parse_type());
    }
    (void)expect(TokenKind::LeftBrace, "expected '{' before assembly instructions");

    while (!at_end() && !at(TokenKind::RightBrace)) {
      if (match(TokenKind::Semicolon)) continue;
      if (at(TokenKind::Ellipsis)) {
        children.push_back(parse_synthesis(SynthesisPosition::Assembly, false));
        continue;
      }
      const std::uint32_t row_start = position_;
      if (at(TokenKind::KeywordIn)) {
        advance();
        (void)expect_name("expected fixed register after assembly 'in'");
        (void)expect(TokenKind::Equal, "expected '=' before assembly input value");
        std::vector<NodeId> input_children{parse_expression(1, false)};
        (void)match(TokenKind::Semicolon);
        children.push_back(tree_.add_node(
            NodeKind::AsmInput,
            row_start,
            position_,
            std::move(input_children)));
        continue;
      }
      if (at(TokenKind::KeywordOut)) {
        advance();
        (void)expect_name("expected fixed register after assembly 'out'");
        while (!at_end() && !at(TokenKind::Semicolon) &&
               !at(TokenKind::RightBrace)) {
          advance();
        }
        (void)match(TokenKind::Semicolon);
        children.push_back(tree_.add_node(
            NodeKind::AsmOutput, row_start, position_));
        continue;
      }
      if (at(TokenKind::KeywordClobber)) {
        advance();
        while (!at_end() && !at(TokenKind::Semicolon) &&
               !at(TokenKind::RightBrace)) {
          advance();
        }
        (void)match(TokenKind::Semicolon);
        children.push_back(tree_.add_node(
            NodeKind::AsmClobber, row_start, position_));
        continue;
      }
      // A target instruction is one semicolon-normalized source row. Braces
      // are invalid in Draft 1 straight-line assembly and remain for recovery.
      while (!at_end() && !at(TokenKind::Semicolon) &&
             !at(TokenKind::RightBrace) && !at(TokenKind::Ellipsis)) {
        advance();
      }
      if (position_ == row_start) {
        error_here("expected assembly directive or instruction");
        advance();
      } else {
        (void)match(TokenKind::Semicolon);
        children.push_back(tree_.add_node(
            NodeKind::AsmInstruction, row_start, position_));
      }
    }
    (void)expect(TokenKind::RightBrace, "expected '}' after assembly instructions");
    if (statement_position && has_result) {
      diagnostics_.error(
          tree_.token(start).range,
          "value-producing asm expression cannot be used as an asm statement");
    }
    const NodeKind kind = has_result ? NodeKind::AsmExpression : NodeKind::AsmStatement;
    return tree_.add_node(kind, start, position_, std::move(children));
  }

  SyntaxTree tree_;
  DiagnosticSink &diagnostics_;
  std::uint32_t position_ = 0;
};

} // namespace

SyntaxTree parse_source_file(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics) {
  std::vector<Token> tokens = lex_source(sources, file, diagnostics);
  Parser parser(file, std::move(tokens), diagnostics);
  return parser.parse();
}

} // namespace draft
