// Private filesystem realization of the embedded Draft coding skill.
//
// See draft_skill.h for ownership and identity. This file performs only direct
// filesystem operations over the trusted build-generated row table. It creates
// no symlinks and accepts no source-authored path, which keeps path validation
// separate from the Codex request-directory link created by the adapter.

#include "elaborator/draft_skill.h"

#include <cerrno>
#include <cstdint>
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

// Frames one byte string with its decimal length. The separators make the
// stream unambiguous without relying on forbidden path characters or NUL-free
// Markdown. The spelling is local to this bundle identity and versioned by the
// leading domain string in embedded_draft_skill_digest.
void hash_framed(std::string_view value, Sha256 &hash) {
  hash.update(std::to_string(value.size()));
  hash.update(":");
  hash.update(value);
  hash.update(";");
}

[[nodiscard]] bool valid_relative_path(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.back() == '/') return false;
  std::size_t component_begin = 0;
  for (std::size_t index = 0; index <= path.size(); ++index) {
    if (index != path.size() && path[index] != '/') continue;
    const std::string_view component = path.substr(
        component_begin, index - component_begin);
    if (component.empty() || component == "." || component == ".." ||
        component.find('\\') != std::string_view::npos) {
      return false;
    }
    component_begin = index + 1;
  }
  return true;
}

void remove_tree(const std::filesystem::path &path) {
  if (path.empty()) return;
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

} // namespace

Sha256Digest embedded_draft_skill_digest() {
  Sha256 hash;
  hash.update("draft.embedded-coding-skill.v1;");
  for (const EmbeddedDraftSkillFile &file : embedded_draft_skill_files()) {
    hash_framed(file.relative_path, hash);
    hash_framed(file.contents, hash);
  }
  return hash.finalize();
}

MaterializedDraftSkill::~MaterializedDraftSkill() {
  remove_tree(root_);
}

bool MaterializedDraftSkill::materialize(DiagnosticSink &diagnostics) {
  if (!root_.empty()) return true;
#if defined(__APPLE__) || defined(__unix__)
  std::error_code error;
  const std::filesystem::path temporary_root =
      std::filesystem::temp_directory_path(error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot locate a temporary directory for the embedded Draft skill: " +
            error.message());
    return false;
  }
  std::string pattern = (temporary_root / "draft-skill-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char *created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot create embedded Draft skill directory: " +
            std::string(std::strerror(errno)));
    return false;
  }
  const std::filesystem::path candidate = created;

  // Build the complete private tree before publishing root_. Generated path
  // rows are still validated here so a build-script mistake cannot escape the
  // command-owned directory at runtime.
  for (const EmbeddedDraftSkillFile &file : embedded_draft_skill_files()) {
    if (!valid_relative_path(file.relative_path)) {
      remove_tree(candidate);
      diagnostics.error(
          SourceRange::invalid(),
          "embedded Draft skill contains an invalid relative path");
      return false;
    }
    const std::filesystem::path destination =
        candidate / std::filesystem::path(file.relative_path);
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
      remove_tree(candidate);
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create embedded Draft skill subdirectory: " +
              error.message());
      return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(
        file.contents.data(),
        static_cast<std::streamsize>(file.contents.size()));
    output.close();
    if (!output) {
      remove_tree(candidate);
      diagnostics.error(
          SourceRange::invalid(), "cannot write embedded Draft skill file");
      return false;
    }
    if (::chmod(destination.c_str(), 0400) != 0) {
      const std::string reason = std::strerror(errno);
      remove_tree(candidate);
      diagnostics.error(
          SourceRange::invalid(),
          "cannot make embedded Draft skill file read-only: " + reason);
      return false;
    }
  }
  root_ = candidate;
  return true;
#else
  diagnostics.error(
      SourceRange::invalid(),
      "embedded Draft skill materialization is unavailable on this host");
  return false;
#endif
}

} // namespace draft
