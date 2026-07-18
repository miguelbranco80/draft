// Agent record decoding, secure attachment collection, and content hashing.
//
// This phase owns decoded prompts plus exact bounded attachment bytes for the
// lifetime of AgentMetadataResult. It separately retains content identities for
// package interfaces and obligation hashes. Paths are resolved beneath the
// canonical package root with symlinks rejected; canonical bytewise path order
// is established before either hashing or provider request construction.
//
// No provider is invoked here and no attachment path or byte enters runtime
// lowering. Relevant specification: 03-agent-synthesis.md sections 8-10.

#include "sema/agent_metadata.h"

#include "syntax/token.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

[[nodiscard]] std::optional<AgentConstructKind> agent_kind(SemanticSiteKind kind) {
  switch (kind) {
  case SemanticSiteKind::Documentation: return AgentConstructKind::Documentation;
  case SemanticSiteKind::Judgment: return AgentConstructKind::Judgment;
  case SemanticSiteKind::SynthesisDeclaration:
    return AgentConstructKind::SynthesisDeclaration;
  case SemanticSiteKind::SynthesisMember: return AgentConstructKind::SynthesisMember;
  case SemanticSiteKind::SynthesisStatement: return AgentConstructKind::SynthesisStatement;
  case SemanticSiteKind::SynthesisExpression: return AgentConstructKind::SynthesisExpression;
  case SemanticSiteKind::SynthesisAssembly: return AgentConstructKind::SynthesisAssembly;
  default: return std::nullopt;
  }
}

[[nodiscard]] std::uint8_t hex_digit(char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<std::uint8_t>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<std::uint8_t>(character - 'a') + 10U;
  }
  if (character >= 'A' && character <= 'F') {
    return static_cast<std::uint8_t>(character - 'A') + 10U;
  }
  return 0xffU;
}

void append_utf8(std::uint32_t scalar, std::string &output) {
  if (scalar <= 0x7fU) {
    output.push_back(static_cast<char>(scalar));
  } else if (scalar <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (scalar >> 6U)));
    output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
  } else if (scalar <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (scalar >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (scalar >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
  }
}

// Decodes the complete literal escape vocabulary after lexical validation. It
// still returns nullopt for malformed in-memory syntax so this phase never
// relies on the parser having been reached through the normal driver.
[[nodiscard]] std::optional<std::string> decode_string(
    std::string_view spelling, TokenKind kind) {
  if (spelling.size() < 2) return std::nullopt;
  if (kind == TokenKind::RawStringLiteral) {
    return std::string(spelling.substr(1, spelling.size() - 2));
  }
  if (kind != TokenKind::StringLiteral) return std::nullopt;
  std::string output;
  for (std::size_t index = 1; index + 1 < spelling.size(); ++index) {
    const char character = spelling[index];
    if (character != '\\') {
      output.push_back(character);
      continue;
    }
    ++index;
    if (index + 1 >= spelling.size()) return std::nullopt;
    const char escape = spelling[index];
    switch (escape) {
    case '\\': output.push_back('\\'); break;
    case '"': output.push_back('"'); break;
    case '\'': output.push_back('\''); break;
    case 'n': output.push_back('\n'); break;
    case 'r': output.push_back('\r'); break;
    case 't': output.push_back('\t'); break;
    case '0': output.push_back('\0'); break;
    case 'x': {
      if (index + 2 >= spelling.size() - 1) return std::nullopt;
      const std::uint8_t high = hex_digit(spelling[index + 1]);
      const std::uint8_t low = hex_digit(spelling[index + 2]);
      if (high == 0xffU || low == 0xffU) return std::nullopt;
      output.push_back(static_cast<char>((high << 4U) | low));
      index += 2;
      break;
    }
    case 'u': {
      if (index + 1 >= spelling.size() || spelling[index + 1] != '{') {
        return std::nullopt;
      }
      index += 2;
      std::uint32_t scalar = 0;
      std::size_t digits = 0;
      while (index < spelling.size() && spelling[index] != '}') {
        const std::uint8_t digit = hex_digit(spelling[index]);
        if (digit == 0xffU || scalar > (0x10ffffU - digit) / 16U) {
          return std::nullopt;
        }
        scalar = scalar * 16U + digit;
        ++digits;
        ++index;
      }
      if (digits == 0 || index >= spelling.size() || scalar > 0x10ffffU ||
          (scalar >= 0xd800U && scalar <= 0xdfffU)) {
        return std::nullopt;
      }
      append_utf8(scalar, output);
      break;
    }
    default: return std::nullopt;
    }
  }
  return output;
}

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - 1 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(bytes);
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

class MetadataCollector {
public:
  MetadataCollector(
      const SourceManager &sources,
      const LoadedPackage &loaded,
      const SemanticPackage &package,
      const AttachmentPolicy &policy,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), package_(package), policy_(policy),
        diagnostics_(diagnostics) {}

  [[nodiscard]] AgentMetadataResult run() {
    AgentMetadataResult result;
    const std::size_t initial_errors = diagnostics_.error_count();
    std::error_code error;
    package_root_ = std::filesystem::canonical(loaded_.physical_directory, error);
    if (error) {
      diagnostics_.error(
          SourceRange::invalid(),
          "cannot canonicalize package directory for attachments: " + error.message());
      return result;
    }

    for (const SemanticSite &site : package_.sites) {
      const std::optional<AgentConstructKind> kind = agent_kind(site.kind);
      if (!kind.has_value()) continue;
      const SyntaxTree *tree = find_tree(site.syntax.file);
      if (tree == nullptr || !site.syntax.node.is_valid()) {
        diagnostics_.error(SourceRange::invalid(), "agent site has no owning syntax tree");
        continue;
      }
      const SyntaxNode &node = tree->node(site.syntax.node);
      const bool package_documentation =
          *kind == AgentConstructKind::Documentation &&
          is_package_documentation(site);
      if (*kind == AgentConstructKind::Documentation &&
          !site.anchor.is_valid() && !package_documentation) {
        diagnostics_.error(
            node.range,
            "documentation must be package documentation or immediately "
            "precede a declaration");
      }
      AgentRecord record;
      record.kind = *kind;
      record.syntax = site.syntax;
      record.scope = site.scope;
      record.anchor = site.anchor;
      record.expected_type = site.expected_type;
      record.branch_refinements = site.branch_refinements;
      record.public_interface =
          is_public_documentation(site, package_documentation);
      decode_text(*tree, node, record);
      collect_attachments(*tree, node, record);
      canonicalize_files(record);
      record.record_digest = hash_record(record);
      result.records.push_back(std::move(record));
    }
    result.ok = diagnostics_.error_count() == initial_errors;
    return result;
  }

private:
  [[nodiscard]] const SyntaxTree *find_tree(FileId file) const {
    for (const LoadedPackageFile &entry : loaded_.files) {
      if (entry.source == file && entry.syntax.has_value()) return &*entry.syntax;
    }
    return nullptr;
  }

  [[nodiscard]] bool is_package_documentation(
      const SemanticSite &site) const {
    const SyntaxTree *tree = find_tree(site.syntax.file);
    if (tree == nullptr) return false;
    const SyntaxNode &root = tree->node(tree->root());
    return std::find(
        root.children.begin(), root.children.end(), site.syntax.node) !=
        root.children.end();
  }

  [[nodiscard]] bool is_public_documentation(
      const SemanticSite &site, bool package_documentation) const {
    if (site.kind != SemanticSiteKind::Documentation) return false;
    if (package_documentation) return true;
    if (!site.anchor.is_valid()) {
      return false;
    }
    return package_.symbols.symbol(site.anchor).visibility == Visibility::Public;
  }

  void decode_text(
      const SyntaxTree &tree, const SyntaxNode &node, AgentRecord &record) {
    if (node.token_begin + 1 >= node.token_end) return;
    const Token &token = tree.token(node.token_begin + 1);
    if (token.kind != TokenKind::StringLiteral &&
        token.kind != TokenKind::RawStringLiteral) {
      return;
    }
    const std::optional<std::string> decoded =
        decode_string(sources_.text(token.range), token.kind);
    if (!decoded.has_value()) {
      diagnostics_.error(token.range, "cannot decode agent construct string");
      return;
    }
    record.text = *decoded;
  }

  [[nodiscard]] bool ignored_name(const std::filesystem::path &path) const {
    const std::string name = path.filename().string();
    if (policy_.ignore_hidden_names && !name.empty() && name.front() == '.') return true;
    return std::find(
        policy_.ignored_directory_names.begin(),
        policy_.ignored_directory_names.end(),
        name) != policy_.ignored_directory_names.end();
  }

  [[nodiscard]] bool extension_allowed(const std::filesystem::path &path) const {
    if (policy_.allowed_extensions.empty()) return true;
    const std::string extension = path.extension().string();
    return std::find(
        policy_.allowed_extensions.begin(),
        policy_.allowed_extensions.end(),
        extension) != policy_.allowed_extensions.end();
  }

  [[nodiscard]] std::optional<std::filesystem::path> secure_path(
      const std::string &relative,
      SourceRange range) {
    const std::filesystem::path input(relative);
    if (relative.empty() || input.is_absolute()) {
      diagnostics_.error(range, "attachment path must be nonempty and package-relative");
      return std::nullopt;
    }
    std::filesystem::path current = package_root_;
    for (const std::filesystem::path &component : input) {
      if (component == "..") {
        diagnostics_.error(range, "attachment path may not contain '..'");
        return std::nullopt;
      }
      if (component == ".") continue;
      current /= component;
      std::error_code error;
      const std::filesystem::file_status status =
          std::filesystem::symlink_status(current, error);
      if (error) {
        diagnostics_.error(range, "cannot inspect attachment path: " + error.message());
        return std::nullopt;
      }
      if (std::filesystem::is_symlink(status)) {
        diagnostics_.error(range, "attachment path may not contain a symlink");
        return std::nullopt;
      }
    }
    return current;
  }

  void add_file(
      const std::filesystem::path &physical,
      SourceRange range,
      AgentRecord &record) {
    if (!extension_allowed(physical)) {
      diagnostics_.error(range, "attachment file extension is not allowed by policy");
      return;
    }
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(physical, error);
    if (error) {
      diagnostics_.error(range, "cannot read attachment size: " + error.message());
      return;
    }
    if (size > std::numeric_limits<std::uint64_t>::max()) {
      diagnostics_.error(range, "attachment is too large");
      return;
    }
    const std::uint64_t byte_size = static_cast<std::uint64_t>(size);
    if (record.files.size() >= policy_.maximum_files_per_site ||
        byte_size > policy_.maximum_bytes_per_site ||
        total_bytes_ > policy_.maximum_bytes_per_site - byte_size) {
      diagnostics_.error(range, "attachment site exceeds configured file or byte limit");
      return;
    }

    std::ifstream input(physical, std::ios::binary);
    if (!input) {
      diagnostics_.error(range, "cannot open attachment file");
      return;
    }
    std::vector<char> bytes;
    bytes.assign(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad() || bytes.size() != static_cast<std::size_t>(byte_size)) {
      diagnostics_.error(range, "cannot read complete attachment file");
      return;
    }
    const std::filesystem::path relative = physical.lexically_relative(package_root_);
    const std::span<const std::uint8_t> byte_view(
        reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
    record.files.push_back({relative.generic_string(), byte_size, sha256(byte_view)});
    record.file_contents.emplace_back(bytes.begin(), bytes.end());
    total_bytes_ += byte_size;
  }

  void collect_folder(
      const std::filesystem::path &folder,
      SourceRange range,
      AgentRecord &record) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(folder, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
      diagnostics_.error(range, "cannot enumerate attachment folder: " + error.message());
      return;
    }
    for (; iterator != end; iterator.increment(error)) {
      if (error) {
        diagnostics_.error(range, "cannot enumerate attachment folder: " + error.message());
        return;
      }
      const std::filesystem::file_status status = iterator->symlink_status(error);
      if (error) {
        diagnostics_.error(range, "cannot inspect attachment entry: " + error.message());
        return;
      }
      if (std::filesystem::is_symlink(status)) {
        diagnostics_.error(range, "attachment folders may not contain symlinks");
        continue;
      }
      if (std::filesystem::is_directory(status)) {
        if (ignored_name(iterator->path())) iterator.disable_recursion_pending();
        continue;
      }
      if (std::filesystem::is_regular_file(status)) {
        if (ignored_name(iterator->path())) continue;
        add_file(iterator->path(), range, record);
      }
    }
  }

  void collect_attachments(
      const SyntaxTree &tree, const SyntaxNode &node, AgentRecord &record) {
    total_bytes_ = 0;
    for (NodeId child_id : node.children) {
      const SyntaxNode &attachment = tree.node(child_id);
      if (attachment.kind != NodeKind::Attachment ||
          attachment.token_begin + 1 >= attachment.token_end) {
        continue;
      }
      const Token &kind_token = tree.token(attachment.token_begin);
      const Token &path_token = tree.token(attachment.token_begin + 1);
      const std::optional<std::string> decoded =
          decode_string(sources_.text(path_token.range), path_token.kind);
      if (!decoded.has_value() || decoded->find('\0') != std::string::npos) {
        diagnostics_.error(path_token.range, "attachment path is not a valid string path");
        continue;
      }
      const std::optional<std::filesystem::path> physical =
          secure_path(*decoded, path_token.range);
      if (!physical.has_value()) continue;
      std::error_code error;
      const std::filesystem::file_status status =
          std::filesystem::status(*physical, error);
      if (error) {
        diagnostics_.error(path_token.range, "cannot inspect attachment: " + error.message());
        continue;
      }
      if (kind_token.kind == TokenKind::KeywordFile) {
        if (!std::filesystem::is_regular_file(status)) {
          diagnostics_.error(path_token.range, "file attachment is not a regular file");
        } else {
          add_file(*physical, path_token.range, record);
        }
      } else if (kind_token.kind == TokenKind::KeywordFolder) {
        if (!std::filesystem::is_directory(status)) {
          diagnostics_.error(path_token.range, "folder attachment is not a directory");
        } else {
          collect_folder(*physical, path_token.range, record);
        }
      }
    }
  }

  // Sorts the identity/content parallel arrays together and removes repeated
  // paths. The first duplicate is sufficient because both occurrences were read
  // from the same secured physical path during this collector invocation.
  void canonicalize_files(AgentRecord &record) {
    if (record.files.size() != record.file_contents.size()) {
      diagnostics_.error(
          SourceRange::invalid(),
          "agent attachment identities and bytes are inconsistent");
      return;
    }
    std::vector<std::size_t> order(record.files.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::sort(
        order.begin(), order.end(),
        [&record](std::size_t left, std::size_t right) {
          return record.files[left].relative_path <
              record.files[right].relative_path;
        });
    std::vector<AttachedFile> files;
    std::vector<std::string> contents;
    for (std::size_t index : order) {
      if (!files.empty() &&
          files.back().relative_path == record.files[index].relative_path) {
        continue;
      }
      files.push_back(std::move(record.files[index]));
      contents.push_back(std::move(record.file_contents[index]));
    }
    record.files = std::move(files);
    record.file_contents = std::move(contents);
  }

  [[nodiscard]] Sha256Digest hash_record(const AgentRecord &record) const {
    Sha256 hash;
    hash.update("draft-agent-record-v1");
    hash_u64(hash, static_cast<std::uint64_t>(record.kind));
    hash_field(hash, policy_.identity);
    hash_field(hash, record.text);
    hash_u64(hash, static_cast<std::uint64_t>(record.files.size()));
    for (const AttachedFile &file : record.files) {
      hash_field(hash, file.relative_path);
      hash_u64(hash, file.size);
      hash.update(file.digest.bytes);
    }
    return hash.finalize();
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const SemanticPackage &package_;
  const AttachmentPolicy &policy_;
  DiagnosticSink &diagnostics_;
  std::filesystem::path package_root_;
  std::uint64_t total_bytes_ = 0;
};

} // namespace

AgentMetadataResult collect_agent_metadata(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AttachmentPolicy &policy,
    DiagnosticSink &diagnostics) {
  MetadataCollector collector(sources, loaded, package, policy, diagnostics);
  return collector.run();
}

} // namespace draft
