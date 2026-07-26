// Private filesystem realization of the embedded factual Draft references.
//
// See draft_reference.h for ownership and identity. This file performs only
// direct filesystem operations over the trusted build-generated row table. It
// creates no symlinks and accepts no source-authored path, keeping its path
// validation separate from the request-directory link created by an adapter.

#include "elaborator/draft_reference.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace draft {
namespace {

// Frames one byte string with its decimal length. Separators make the stream
// unambiguous without relying on forbidden path characters or NUL-free
// Markdown. The leading domain string versions the complete digest spelling.
void hash_framed(std::string_view value, Sha256 &hash) {
  hash.update(std::to_string(value.size()));
  hash.update(":");
  hash.update(value);
  hash.update(";");
}

// Generated rows are intentionally flat. Rechecking the shape here turns a
// build-script regression into a controlled compiler diagnostic rather than a
// path traversal during materialization.
[[nodiscard]] bool valid_reference_name(std::string_view path) {
  return !path.empty() && path != "." && path != ".." &&
         path.find('/') == std::string_view::npos &&
         path.find('\\') == std::string_view::npos;
}

// Materialization failures call this while the directory is writable; normal
// destruction calls it after publication made the directory read-only. Restore
// owner write permission before recursive removal so cleanup is reliable on
// POSIX rather than depending on filesystem-specific unlink behavior.
void remove_tree(const std::filesystem::path &path) {
  if (path.empty())
    return;
#if defined(__APPLE__) || defined(__unix__)
  (void)::chmod(path.c_str(), 0700);
#endif
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

} // namespace

Sha256Digest embedded_draft_reference_digest() {
  Sha256 hash;
  hash.update("draft.embedded-reference-bundle.v1;");
  for (const EmbeddedDraftReferenceFile &file :
       embedded_draft_reference_files()) {
    hash_framed(file.relative_path, hash);
    hash_framed(file.contents, hash);
  }
  return hash.finalize();
}

MaterializedDraftReference::~MaterializedDraftReference() {
  remove_tree(root_);
}

bool MaterializedDraftReference::materialize(DiagnosticSink &diagnostics) {
  if (!root_.empty())
    return true;
#if defined(__APPLE__) || defined(__unix__)
  std::error_code error;
  const std::filesystem::path temporary_root =
      std::filesystem::temp_directory_path(error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot locate a temporary directory for Draft references: " +
            error.message());
    return false;
  }
  std::string pattern = (temporary_root / "draft-reference-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char *created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    diagnostics.error(SourceRange::invalid(),
                      "cannot create embedded Draft reference directory: " +
                          std::string(std::strerror(errno)));
    return false;
  }
  const std::filesystem::path candidate = created;

  // Build the complete private directory before publishing root_. Generated
  // row names are still validated here so a build-script mistake cannot escape
  // the command-owned directory at runtime.
  for (const EmbeddedDraftReferenceFile &file :
       embedded_draft_reference_files()) {
    if (!valid_reference_name(file.relative_path)) {
      remove_tree(candidate);
      diagnostics.error(
          SourceRange::invalid(),
          "embedded Draft reference contains an invalid filename");
      return false;
    }
    const std::filesystem::path destination = candidate / file.relative_path;
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(file.contents.data(),
                 static_cast<std::streamsize>(file.contents.size()));
    output.close();
    if (!output) {
      remove_tree(candidate);
      diagnostics.error(SourceRange::invalid(),
                        "cannot write embedded Draft reference file");
      return false;
    }
    if (::chmod(destination.c_str(), 0400) != 0) {
      const std::string reason = std::strerror(errno);
      remove_tree(candidate);
      diagnostics.error(SourceRange::invalid(),
                        "cannot make embedded Draft reference read-only: " +
                            reason);
      return false;
    }
  }
  if (::chmod(candidate.c_str(), 0500) != 0) {
    const std::string reason = std::strerror(errno);
    remove_tree(candidate);
    diagnostics.error(
        SourceRange::invalid(),
        "cannot make embedded Draft reference directory read-only: " + reason);
    return false;
  }
  root_ = candidate;
  return true;
#else
  diagnostics.error(
      SourceRange::invalid(),
      "embedded Draft reference materialization is unavailable on this host");
  return false;
#endif
}

} // namespace draft
