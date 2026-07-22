// Syntax tree arena, stable node names, and deterministic tree dumping.
//
// Nodes are appended only after all their children have been parsed. This means
// child IDs are commonly lower than parent IDs but no semantic behavior depends
// on that incidental ordering. The root is assigned exactly once by the parser.

#include "syntax/syntax_tree.h"

#include <cassert>
#include <sstream>
#include <utility>

namespace draft {

bool NodeId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

SyntaxTree::SyntaxTree(FileId file, std::vector<Token> tokens)
    : file_(file), tokens_(std::move(tokens)) {
  assert(file_.is_valid());
  assert(!tokens_.empty());
  assert(tokens_.back().kind == TokenKind::EndOfFile);
}

NodeId SyntaxTree::add_node(
    NodeKind kind,
    std::uint32_t token_begin,
    std::uint32_t token_end,
    std::vector<NodeId> children) {
  assert(token_begin <= token_end);
  assert(static_cast<std::size_t>(token_end) <= tokens_.size());
  assert(nodes_.size() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));

  SourceRange range;
  if (token_begin < token_end) {
    range = {tokens_[token_begin].range.begin, tokens_[token_end - 1].range.end};
  } else {
    const std::uint32_t position =
        token_begin < tokens_.size() ? tokens_[token_begin].range.begin.offset
                                     : tokens_.back().range.end.offset;
    range = SourceRange::at(file_, position);
  }

  const NodeId id{static_cast<std::uint32_t>(nodes_.size())};
  nodes_.push_back({kind, range, token_begin, token_end, std::move(children)});
  return id;
}

void SyntaxTree::set_root(NodeId root) {
  assert(root.is_valid());
  assert(!root_.is_valid());
  root_ = root;
}

FileId SyntaxTree::file() const {
  return file_;
}

NodeId SyntaxTree::root() const {
  return root_;
}

const SyntaxNode &SyntaxTree::node(NodeId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < nodes_.size());
  return nodes_[id.value];
}

const Token &SyntaxTree::token(std::uint32_t index) const {
  assert(static_cast<std::size_t>(index) < tokens_.size());
  return tokens_[index];
}

const std::vector<Token> &SyntaxTree::tokens() const {
  return tokens_;
}

const std::vector<SyntaxNode> &SyntaxTree::nodes() const {
  return nodes_;
}

std::size_t SyntaxTree::count(NodeKind kind) const {
  std::size_t result = 0;
  for (const SyntaxNode &node : nodes_) {
    if (node.kind == kind) {
      ++result;
    }
  }
  return result;
}

std::string_view node_kind_name(NodeKind kind) {
  switch (kind) {
  case NodeKind::Error: return "Error";
  case NodeKind::SourceFile: return "SourceFile";
  case NodeKind::PackageClause: return "PackageClause";
  case NodeKind::ImportClause: return "ImportClause";
  case NodeKind::ImportPath: return "ImportPath";
  case NodeKind::DeclarationList: return "DeclarationList";
  case NodeKind::Declaration: return "Declaration";
  case NodeKind::NameList: return "NameList";
  case NodeKind::BindingPattern: return "BindingPattern";
  case NodeKind::TuplePattern: return "TuplePattern";
  case NodeKind::ParametricParameterList: return "ParametricParameterList";
  case NodeKind::ParametricParameter: return "ParametricParameter";
  case NodeKind::CRepresentationSpecifier: return "CRepresentationSpecifier";
  case NodeKind::AlignmentSpecifier: return "AlignmentSpecifier";
  case NodeKind::LinkNameClause: return "LinkNameClause";
  case NodeKind::ForeignBlock: return "ForeignBlock";
  case NodeKind::ExportDeclaration: return "ExportDeclaration";
  case NodeKind::Documentation: return "Documentation";
  case NodeKind::Judgment: return "Judgment";
  case NodeKind::Attachment: return "Attachment";
  case NodeKind::SynthesisDeclaration: return "SynthesisDeclaration";
  case NodeKind::SynthesisMember: return "SynthesisMember";
  case NodeKind::SynthesisStatement: return "SynthesisStatement";
  case NodeKind::SynthesisExpression: return "SynthesisExpression";
  case NodeKind::SynthesisAssembly: return "SynthesisAssembly";
  case NodeKind::Procedure: return "Procedure";
  case NodeKind::ParameterList: return "ParameterList";
  case NodeKind::Parameter: return "Parameter";
  case NodeKind::ParameterDefault: return "ParameterDefault";
  case NodeKind::StaticPackType: return "StaticPackType";
  case NodeKind::ResultClause: return "ResultClause";
  case NodeKind::MemberList: return "MemberList";
  case NodeKind::FieldMember: return "FieldMember";
  case NodeKind::EnumMember: return "EnumMember";
  case NodeKind::UnionAlternative: return "UnionAlternative";
  case NodeKind::NamedType: return "NamedType";
  case NodeKind::PointerType: return "PointerType";
  case NodeKind::MultiPointerType: return "MultiPointerType";
  case NodeKind::SliceType: return "SliceType";
  case NodeKind::ArrayType: return "ArrayType";
  case NodeKind::SimdType: return "SimdType";
  case NodeKind::TupleType: return "TupleType";
  case NodeKind::ProcedureType: return "ProcedureType";
  case NodeKind::DistinctType: return "DistinctType";
  case NodeKind::StructType: return "StructType";
  case NodeKind::EnumType: return "EnumType";
  case NodeKind::TaggedUnionType: return "TaggedUnionType";
  case NodeKind::RawUnionType: return "RawUnionType";
  case NodeKind::Block: return "Block";
  case NodeKind::StatementList: return "StatementList";
  case NodeKind::DeclarationStatement: return "DeclarationStatement";
  case NodeKind::ExpressionStatement: return "ExpressionStatement";
  case NodeKind::AssignmentStatement: return "AssignmentStatement";
  case NodeKind::ReturnStatement: return "ReturnStatement";
  case NodeKind::BreakStatement: return "BreakStatement";
  case NodeKind::ContinueStatement: return "ContinueStatement";
  case NodeKind::DeferStatement: return "DeferStatement";
  case NodeKind::IfStatement: return "IfStatement";
  case NodeKind::ForStatement: return "ForStatement";
  case NodeKind::IterationHeader: return "IterationHeader";
  case NodeKind::ForClause: return "ForClause";
  case NodeKind::SwitchStatement: return "SwitchStatement";
  case NodeKind::SwitchCase: return "SwitchCase";
  case NodeKind::WhenDeclaration: return "WhenDeclaration";
  case NodeKind::WhenMember: return "WhenMember";
  case NodeKind::WhenStatement: return "WhenStatement";
  case NodeKind::DenyDeclaration: return "DenyDeclaration";
  case NodeKind::DenyMember: return "DenyMember";
  case NodeKind::DenyStatement: return "DenyStatement";
  case NodeKind::DenyExpression: return "DenyExpression";
  case NodeKind::UncheckedStatement: return "UncheckedStatement";
  case NodeKind::AsmStatement: return "AsmStatement";
  case NodeKind::AsmExpression: return "AsmExpression";
  case NodeKind::AsmInput: return "AsmInput";
  case NodeKind::AsmOutput: return "AsmOutput";
  case NodeKind::AsmClobber: return "AsmClobber";
  case NodeKind::AsmInstruction: return "AsmInstruction";
  case NodeKind::NameExpression: return "NameExpression";
  case NodeKind::LiteralExpression: return "LiteralExpression";
  case NodeKind::UninitializedExpression: return "UninitializedExpression";
  case NodeKind::GroupExpression: return "GroupExpression";
  case NodeKind::TupleExpression: return "TupleExpression";
  case NodeKind::UnaryExpression: return "UnaryExpression";
  case NodeKind::BinaryExpression: return "BinaryExpression";
  case NodeKind::ConditionalExpression: return "ConditionalExpression";
  case NodeKind::CallExpression: return "CallExpression";
  case NodeKind::NamedArgument: return "NamedArgument";
  case NodeKind::BracketExpression: return "BracketExpression";
  case NodeKind::SliceExpression: return "SliceExpression";
  case NodeKind::MemberExpression: return "MemberExpression";
  case NodeKind::DereferenceExpression: return "DereferenceExpression";
  case NodeKind::CompositeExpression: return "CompositeExpression";
  case NodeKind::CompositeElement: return "CompositeElement";
  case NodeKind::ContextualAlternativeExpression: return "ContextualAlternativeExpression";
  }
  return "UnknownNode";
}

namespace {

void dump_node(const SyntaxTree &tree, NodeId id, unsigned depth, std::ostringstream &output) {
  const SyntaxNode &node = tree.node(id);
  for (unsigned index = 0; index < depth; ++index) {
    output << "  ";
  }
  output << node_kind_name(node.kind) << " [" << node.token_begin << ','
         << node.token_end << ")\n";
  for (NodeId child : node.children) {
    dump_node(tree, child, depth + 1, output);
  }
}

} // namespace

std::string dump_syntax_tree(const SyntaxTree &tree) {
  std::ostringstream output;
  if (tree.root().is_valid()) {
    dump_node(tree, tree.root(), 0, output);
  }
  return output.str();
}

} // namespace draft
