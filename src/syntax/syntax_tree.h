// Concrete surface syntax tree for parsed Draft source.
//
// SyntaxTree owns the semicolon-normalized token stream and a data-oriented arena
// of SyntaxNode values. Nodes refer to other nodes by stable NodeId and cover a
// half-open token span. The token span preserves every nontrivia source spelling
// and every inserted semicolon; child IDs describe grammar structure without
// duplicating token storage.
//
// This is intentionally a concrete syntax representation, not the typed HIR.
// Ambiguities that require name resolution—most notably `value[index]` versus a
// parametric application—remain neutral syntax nodes until semantic analysis.
// Comments are lexical trivia by language definition and therefore do not appear
// in this tree. Documentation is ordinary syntax and remains represented.
//
// Node child ordering is grammatical source order. Later AST/HIR lowering uses
// NodeKind plus that order; it must never infer structure by rescanning source
// text. Nodes and tokens are append-only after construction, so NodeId and token
// indices are stable for the tree's lifetime but are not persistent identities.

#pragma once

#include "source/source.h"
#include "syntax/token.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

struct NodeId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const NodeId &) const = default;
};

// NodeKind names grammar concepts rather than later semantic concepts. For
// example, BracketExpression is neutral until resolution decides whether it is
// indexing or parametric application, while NameExpression does not imply that
// its spelling resolves successfully.
enum class NodeKind {
  Error,
  SourceFile,
  PackageClause,
  ImportClause,
  ImportPath,
  DeclarationList,
  Declaration,
  NameList,
  BindingPattern,
  TuplePattern,
  ParametricParameterList,
  ParametricParameter,
  CRepresentationSpecifier,
  AlignmentSpecifier,
  LinkNameClause,
  ForeignBlock,
  ExportDeclaration,
  Documentation,
  Judgment,
  Attachment,
  SynthesisDeclaration,
  SynthesisMember,
  SynthesisStatement,
  SynthesisExpression,
  SynthesisAssembly,

  Procedure,
  ParameterList,
  Parameter,
  // ParameterDefault wraps the expression after `=` in a named procedure
  // declaration. Keeping it distinct from the type child prevents semantic
  // clients from depending on a fragile "last child is the type" convention.
  ParameterDefault,
  StaticPackType,
  // CVariadicTail is the bare `..` which may terminate a `c proc` parameter
  // list. It is not a Parameter: it has no name, declared type, default, or
  // runtime binding. Semantic resolution verifies the surrounding calling
  // convention and records the open ABI tail in procedure-type identity.
  CVariadicTail,
  ResultClause,
  MemberList,
  PackedFieldSpecifier,
  BitFieldSpecifier,
  FieldMember,
  EnumMember,
  VariantAlternative,

  NamedType,
  PointerType,
  MultiPointerType,
  SliceType,
  ArrayType,
  SimdType,
  TupleType,
  ProcedureType,
  DistinctType,
  StructType,
  EnumType,
  VariantType,
  UnionType,

  Block,
  StatementList,
  DeclarationStatement,
  ExpressionStatement,
  AssignmentStatement,
  ReturnStatement,
  BreakStatement,
  ContinueStatement,
  DeferStatement,
  IfStatement,
  ForStatement,
  IterationHeader,
  ForClause,
  SwitchStatement,
  SwitchCase,
  WhenDeclaration,
  WhenMember,
  WhenStatement,
  DenyDeclaration,
  DenyMember,
  DenyStatement,
  DenyExpression,
  UncheckedStatement,
  AsmStatement,
  AsmExpression,
  AsmInput,
  AsmOutput,
  AsmClobber,
  AsmInstruction,

  NameExpression,
  LiteralExpression,
  UninitializedExpression,
  GroupExpression,
  TupleExpression,
  UnaryExpression,
  BinaryExpression,
  ConditionalExpression,
  CallExpression,
  // NamedArgument wraps one call operand written `name = expression`. The
  // name remains in the token span and the sole child is the value expression.
  // Assignment is a statement in Draft, so this production is unambiguous in
  // call-argument position.
  NamedArgument,
  BracketExpression,
  SliceExpression,
  MemberExpression,
  DereferenceExpression,
  CompositeExpression,
  CompositeElement,
  ContextualAlternativeExpression,

  // Keep Last as the final entry. Parser conformance tests walk the contiguous
  // range from SourceFile through Last so a newly added concrete production is
  // not silently left without a valid-source fixture. Last aliases the final
  // real kind instead of adding a sentinel value that every semantic switch
  // would need to handle.
  Last = ContextualAlternativeExpression,
};

// SyntaxNode covers tokens [token_begin, token_end). children contains only
// immediate grammar children, in source order. Punctuation and names remain in
// the token span instead of being copied into fields. An Error node records a
// consumed recovery region so downstream tooling can preserve tree shape.
struct SyntaxNode {
  NodeKind kind = NodeKind::Error;
  SourceRange range;
  std::uint32_t token_begin = 0;
  std::uint32_t token_end = 0;
  std::vector<NodeId> children;
};

class SyntaxTree {
public:
  SyntaxTree(FileId file, std::vector<Token> tokens);

  // Adds one complete node. start and end are token indices in source order and
  // end may equal start for a missing/recovery node. The tree computes a stable
  // zero-width range at the current token for an empty node.
  [[nodiscard]] NodeId add_node(
      NodeKind kind,
      std::uint32_t token_begin,
      std::uint32_t token_end,
      std::vector<NodeId> children = {});

  void set_root(NodeId root);

  [[nodiscard]] FileId file() const;
  [[nodiscard]] NodeId root() const;
  [[nodiscard]] const SyntaxNode &node(NodeId id) const;
  [[nodiscard]] const Token &token(std::uint32_t index) const;
  [[nodiscard]] const std::vector<Token> &tokens() const;
  [[nodiscard]] const std::vector<SyntaxNode> &nodes() const;
  [[nodiscard]] std::size_t count(NodeKind kind) const;

private:
  FileId file_;
  std::vector<Token> tokens_;
  std::vector<SyntaxNode> nodes_;
  NodeId root_;
};

[[nodiscard]] std::string_view node_kind_name(NodeKind kind);

// Dumps node hierarchy and token spans in a stable textual form for diagnostics,
// parser tests, and future `draftc syntax` tooling. Source spellings are omitted
// here because callers can slice each node range from SourceManager.
[[nodiscard]] std::string dump_syntax_tree(const SyntaxTree &tree);

} // namespace draft
