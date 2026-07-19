// Deterministic, non-following traversal for external program inputs.

#include "base/content_tree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace draft {
namespace {

enum class EntryKind : std::uint64_t {
  RegularFile = 1,
  Directory = 2,
  Symlink = 3,
};

struct TreeEntry {
  std::string relative_path;
  EntryKind kind = EntryKind::RegularFile;
  std::uint32_t permissions = 0;
  std::uint64_t file_size = 0;
  Sha256Digest file_digest;
  std::string symlink_target;
};

struct Child {
  std::string name;
  std::filesystem::path physical_path;
};

void report_filesystem_error(
    DiagnosticSink &diagnostics,
    std::string operation,
    const std::error_code &error) {
  diagnostics.error(
      SourceRange::invalid(),
      "cannot hash external input: " + std::move(operation) + ": " +
          error.message());
}

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - index - 1] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(bytes);
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

[[nodiscard]] std::uint32_t permission_bits(
    std::filesystem::perms permissions) {
  using PermissionInteger = std::underlying_type_t<std::filesystem::perms>;
  const PermissionInteger masked = static_cast<PermissionInteger>(
      permissions & std::filesystem::perms::mask);
  return static_cast<std::uint32_t>(masked);
}

[[nodiscard]] bool hash_regular_file(
    const std::filesystem::path &path,
    std::uint64_t &size,
    Sha256Digest &digest,
    DiagnosticSink &diagnostics) {
  std::error_code size_error;
  const std::uintmax_t expected_size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    report_filesystem_error(diagnostics, "read regular-file size", size_error);
    return false;
  }
  if (expected_size > std::numeric_limits<std::uint64_t>::max()) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot hash external input: regular file is too large");
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot hash external input: cannot open regular file");
    return false;
  }

  Sha256 file_hash;
  std::array<char, 64U * 1024U> buffer{};
  std::uint64_t observed_size = 0;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count <= 0) continue;
    const std::size_t byte_count = static_cast<std::size_t>(count);
    if (observed_size >
        std::numeric_limits<std::uint64_t>::max() - byte_count) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot hash external input: regular file is too large");
      return false;
    }
    file_hash.update(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(buffer.data()), byte_count));
    observed_size += static_cast<std::uint64_t>(byte_count);
  }
  if (!input.eof()) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot hash external input: failed while reading regular file");
    return false;
  }
  if (observed_size != static_cast<std::uint64_t>(expected_size)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot hash external input: regular file changed while hashing");
    return false;
  }

  size = observed_size;
  digest = file_hash.finalize();
  return true;
}

// A symlink is safe only when lexical resolution from its containing directory
// never moves above the selected root. Every link in a chain is validated as a
// separate tree record, so an apparently internal first hop cannot conceal an
// escaping later hop.
[[nodiscard]] bool symlink_stays_inside(
    std::string_view containing_directory,
    const std::filesystem::path &target) {
  if (target.empty() || target.is_absolute() || target.has_root_name() ||
      target.has_root_directory()) {
    return false;
  }

  std::size_t depth = 0;
  for (char byte : containing_directory) {
    if (byte == '/') ++depth;
  }
  if (!containing_directory.empty()) ++depth;

  for (const std::filesystem::path &component_path : target) {
    const std::string component = component_path.generic_string();
    if (component.empty() || component == ".") continue;
    if (component == "..") {
      if (depth == 0) return false;
      --depth;
      continue;
    }
    ++depth;
  }
  return true;
}

[[nodiscard]] bool collect_entry(
    const std::filesystem::path &physical_path,
    std::string relative_path,
    bool is_root,
    std::vector<TreeEntry> &entries,
    DiagnosticSink &diagnostics);

[[nodiscard]] bool collect_directory(
    const std::filesystem::path &physical_path,
    std::string_view relative_path,
    std::vector<TreeEntry> &entries,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(physical_path, error);
  if (error) {
    report_filesystem_error(diagnostics, "enumerate directory", error);
    return false;
  }

  std::vector<Child> children;
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const std::filesystem::path child_path = iterator->path();
    const std::string name = child_path.filename().generic_string();
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot hash external input: noncanonical directory entry name");
      return false;
    }
    children.push_back({name, child_path});
    iterator.increment(error);
    if (error) {
      report_filesystem_error(diagnostics, "enumerate directory", error);
      return false;
    }
  }

  std::sort(
      children.begin(), children.end(),
      [](const Child &left, const Child &right) { return left.name < right.name; });
  for (Child &child : children) {
    std::string child_relative;
    if (relative_path.empty()) {
      child_relative = std::move(child.name);
    } else {
      child_relative.reserve(relative_path.size() + child.name.size() + 1);
      child_relative.append(relative_path);
      child_relative.push_back('/');
      child_relative.append(child.name);
    }
    if (!collect_entry(
            child.physical_path,
            std::move(child_relative),
            false,
            entries,
            diagnostics)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool collect_entry(
    const std::filesystem::path &physical_path,
    std::string relative_path,
    bool is_root,
    std::vector<TreeEntry> &entries,
    DiagnosticSink &diagnostics) {
  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(physical_path, status_error);
  if (status_error) {
    report_filesystem_error(diagnostics, "inspect filesystem entry", status_error);
    return false;
  }

  TreeEntry entry;
  entry.relative_path = relative_path;
  entry.permissions = permission_bits(status.permissions());
  if (std::filesystem::is_regular_file(status)) {
    entry.kind = EntryKind::RegularFile;
    if (!hash_regular_file(
            physical_path,
            entry.file_size,
            entry.file_digest,
            diagnostics)) {
      return false;
    }
    entries.push_back(std::move(entry));
    return true;
  }
  if (std::filesystem::is_directory(status)) {
    entry.kind = EntryKind::Directory;
    entries.push_back(std::move(entry));
    return collect_directory(
        physical_path, relative_path, entries, diagnostics);
  }
  if (std::filesystem::is_symlink(status)) {
    if (is_root) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot hash external input: selected root must not be a symlink");
      return false;
    }
    std::error_code link_error;
    const std::filesystem::path target =
        std::filesystem::read_symlink(physical_path, link_error);
    if (link_error) {
      report_filesystem_error(diagnostics, "read symbolic link", link_error);
      return false;
    }
    const std::size_t slash = relative_path.rfind('/');
    const std::string_view containing_directory = slash == std::string::npos
        ? std::string_view{}
        : std::string_view(relative_path).substr(0, slash);
    if (!symlink_stays_inside(containing_directory, target)) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot hash external input: symbolic link escapes selected root");
      return false;
    }
    entry.kind = EntryKind::Symlink;
    entry.symlink_target = target.generic_string();
    entries.push_back(std::move(entry));
    return true;
  }

  diagnostics.error(
      SourceRange::invalid(),
      "cannot hash external input: special filesystem entries are not supported");
  return false;
}

} // namespace

bool hash_content_tree(
    const std::filesystem::path &root,
    Sha256Digest &digest,
    DiagnosticSink &diagnostics) {
  std::vector<TreeEntry> entries;
  if (root.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot hash external input: selected root path is empty");
    return false;
  }
  if (!collect_entry(root, "", true, entries, diagnostics)) return false;

  // Traversal already visits siblings in lexical order, but sorting the final
  // records is cheap and makes the identity invariant obvious at this boundary.
  std::sort(
      entries.begin(), entries.end(),
      [](const TreeEntry &left, const TreeEntry &right) {
        return left.relative_path < right.relative_path;
      });

  Sha256 hash;
  hash_field(hash, "draft.content-tree.v1");
  hash_u64(hash, static_cast<std::uint64_t>(entries.size()));
  for (const TreeEntry &entry : entries) {
    hash_field(hash, entry.relative_path);
    hash_u64(hash, static_cast<std::uint64_t>(entry.kind));
    hash_u64(hash, entry.permissions);
    switch (entry.kind) {
    case EntryKind::RegularFile:
      hash_u64(hash, entry.file_size);
      hash.update(entry.file_digest.bytes);
      break;
    case EntryKind::Directory:
      break;
    case EntryKind::Symlink:
      hash_field(hash, entry.symlink_target);
      break;
    }
  }
  digest = hash.finalize();
  return true;
}

} // namespace draft
