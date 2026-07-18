// Crash-safe content-addressed validation evidence storage.

#include "validation/evidence_store.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace draft {
namespace {

constexpr std::uintmax_t kMaximumEvidenceBytes = 16U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumStateBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumAttempts = 100000;

void store_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(
      SourceRange::invalid(), "validation evidence store: " + std::move(message));
}

[[nodiscard]] std::filesystem::path evidence_directory(
    const std::filesystem::path &workspace_directory) {
  return workspace_directory / ".draft" / "evidence";
}

[[nodiscard]] std::filesystem::path object_path(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &digest) {
  return evidence_directory(workspace_directory) / (digest.hex() + ".json");
}

[[nodiscard]] std::filesystem::path state_path(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key) {
  return evidence_directory(workspace_directory) / (key.hex() + ".state");
}

[[nodiscard]] bool ensure_directory(
    const std::filesystem::path &path,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    status = std::filesystem::file_status(
        std::filesystem::file_type::not_found);
  }
  if (error) {
    store_error(
        diagnostics,
        "cannot inspect directory '" + path.string() + "': " +
            error.message());
    return false;
  }
  if (status.type() != std::filesystem::file_type::not_found) {
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      store_error(
          diagnostics,
          "store path is not a real directory: '" + path.string() + "'");
      return false;
    }
    return true;
  }
  if (!path.parent_path().empty() &&
      !ensure_directory(path.parent_path(), diagnostics)) {
    return false;
  }
  if (!std::filesystem::create_directory(path, error) && error) {
    store_error(
        diagnostics,
        "cannot create directory '" + path.string() + "': " +
            error.message());
    return false;
  }
  return true;
}

[[nodiscard]] bool read_regular_file(
    const std::filesystem::path &path,
    std::uintmax_t limit,
    bool &exists,
    std::string &bytes,
    DiagnosticSink &diagnostics) {
  exists = false;
  bytes.clear();
  std::error_code error;
  std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    status = std::filesystem::file_status(
        std::filesystem::file_type::not_found);
  }
  if (error) {
    store_error(
        diagnostics,
        "cannot inspect '" + path.string() + "': " + error.message());
    return false;
  }
  if (status.type() == std::filesystem::file_type::not_found) return true;
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    store_error(
        diagnostics,
        "store object is not a real regular file: '" + path.string() + "'");
    return false;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > limit) {
    store_error(
        diagnostics,
        error ? "cannot measure '" + path.string() + "': " + error.message()
              : "store object exceeds its size limit: '" + path.string() + "'");
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    store_error(diagnostics, "cannot open '" + path.string() + "'");
    return false;
  }
  bytes.assign(
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (input.bad()) {
    store_error(diagnostics, "cannot read complete file '" + path.string() + "'");
    return false;
  }
  exists = true;
  return true;
}

#if defined(_WIN32)

[[nodiscard]] bool synchronize_path(
    const std::filesystem::path &,
    DiagnosticSink &diagnostics) {
  store_error(diagnostics, "durable evidence writes are not implemented on this host");
  return false;
}

[[nodiscard]] std::string process_suffix() { return "windows"; }

class StoreLock {
public:
  StoreLock(const std::filesystem::path &, DiagnosticSink &diagnostics) {
    store_error(diagnostics, "evidence locking is not implemented on this host");
  }
  [[nodiscard]] bool ok() const { return false; }
};

#else

[[nodiscard]] bool synchronize_path(
    const std::filesystem::path &path,
    DiagnosticSink &diagnostics) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    store_error(diagnostics, "cannot open path for synchronization: '" +
        path.string() + "'");
    return false;
  }
  const bool ok = ::fsync(descriptor) == 0;
  ::close(descriptor);
  if (!ok) {
    store_error(diagnostics, "cannot synchronize path: '" + path.string() + "'");
  }
  return ok;
}

[[nodiscard]] std::string process_suffix() {
  return std::to_string(static_cast<std::uint64_t>(::getpid()));
}

class StoreLock {
public:
  StoreLock(const std::filesystem::path &directory, DiagnosticSink &diagnostics)
      : diagnostics_(diagnostics) {
    descriptor_ = ::open(
        directory.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
    if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX) != 0) {
      if (descriptor_ >= 0) ::close(descriptor_);
      descriptor_ = -1;
      store_error(diagnostics_, "cannot lock evidence store");
    }
  }

  StoreLock(const StoreLock &) = delete;
  StoreLock &operator=(const StoreLock &) = delete;

  ~StoreLock() {
    if (descriptor_ >= 0) {
      (void)::flock(descriptor_, LOCK_UN);
      ::close(descriptor_);
    }
  }

  [[nodiscard]] bool ok() const { return descriptor_ >= 0; }

private:
  DiagnosticSink &diagnostics_;
  int descriptor_ = -1;
};

#endif

[[nodiscard]] bool write_atomic(
    const std::filesystem::path &path,
    std::string_view bytes,
    DiagnosticSink &diagnostics) {
  const std::filesystem::path temporary =
      path.string() + ".tmp-" + process_suffix();
  std::error_code error;
  std::filesystem::file_status temporary_status =
      std::filesystem::symlink_status(temporary, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    temporary_status = std::filesystem::file_status(
        std::filesystem::file_type::not_found);
  }
  if (error || temporary_status.type() != std::filesystem::file_type::not_found) {
    store_error(
        diagnostics,
        error ? "cannot inspect temporary evidence path: " + error.message()
              : "temporary evidence path already exists: '" +
                    temporary.string() + "'");
    return false;
  }
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) {
    store_error(diagnostics, "cannot open temporary file '" + temporary.string() + "'");
    return false;
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    store_error(diagnostics, "cannot write temporary file '" + temporary.string() + "'");
    std::filesystem::remove(temporary, error);
    return false;
  }
  if (!synchronize_path(temporary, diagnostics)) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    store_error(
        diagnostics,
        "cannot publish '" + path.string() + "': " + error.message());
    std::filesystem::remove(temporary, error);
    return false;
  }
  return synchronize_path(path.parent_path(), diagnostics);
}

struct StoredState {
  Sha256Digest key;
  std::optional<Sha256Digest> active;
  std::vector<Sha256Digest> attempts;
};

[[nodiscard]] std::string serialize_state(const StoredState &state) {
  std::string result = "draft-validation-state-v1\nkey " + state.key.hex() +
      "\nactive ";
  result += state.active.has_value() ? state.active->hex() : "-";
  result += '\n';
  for (const Sha256Digest &attempt : state.attempts) {
    result += "attempt " + attempt.hex() + "\n";
  }
  return result;
}

[[nodiscard]] bool parse_digest_line(
    std::string_view line,
    std::string_view prefix,
    Sha256Digest &digest,
    DiagnosticSink &diagnostics) {
  if (!line.starts_with(prefix)) {
    store_error(diagnostics, "state file has an unexpected field");
    return false;
  }
  const std::optional<Sha256Digest> parsed =
      Sha256Digest::from_hex(line.substr(prefix.size()));
  if (!parsed.has_value()) {
    store_error(diagnostics, "state file contains an invalid digest");
    return false;
  }
  digest = *parsed;
  return true;
}

[[nodiscard]] bool parse_state(
    std::string_view bytes,
    StoredState &state,
    DiagnosticSink &diagnostics) {
  if (bytes.empty() || bytes.back() != '\n') {
    store_error(diagnostics, "state file is empty or lacks its final newline");
    return false;
  }
  std::vector<std::string_view> lines;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    const std::size_t newline = bytes.find('\n', begin);
    lines.push_back(bytes.substr(begin, newline - begin));
    begin = newline + 1;
  }
  if (lines.size() < 4 || lines[0] != "draft-validation-state-v1") {
    store_error(diagnostics, "state file has an unsupported format");
    return false;
  }
  StoredState parsed;
  if (!parse_digest_line(lines[1], "key ", parsed.key, diagnostics)) {
    return false;
  }
  if (lines[2] == "active -") {
    parsed.active.reset();
  } else {
    Sha256Digest active;
    if (!parse_digest_line(lines[2], "active ", active, diagnostics)) {
      return false;
    }
    parsed.active = active;
  }
  if (lines.size() - 3 > kMaximumAttempts) {
    store_error(diagnostics, "state file exceeds the attempt-count limit");
    return false;
  }
  for (std::size_t index = 3; index < lines.size(); ++index) {
    Sha256Digest attempt;
    if (!parse_digest_line(lines[index], "attempt ", attempt, diagnostics)) {
      return false;
    }
    parsed.attempts.push_back(attempt);
  }
  if (parsed.attempts.empty()) {
    store_error(diagnostics, "state file contains no validation attempt");
    return false;
  }
  state = std::move(parsed);
  return true;
}

[[nodiscard]] bool read_evidence_object(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &digest,
    ValidationEvidence &evidence,
    DiagnosticSink &diagnostics) {
  bool exists = false;
  std::string bytes;
  if (!read_regular_file(
          object_path(workspace_directory, digest),
          kMaximumEvidenceBytes,
          exists,
          bytes,
          diagnostics)) {
    return false;
  }
  if (!exists) {
    store_error(
        diagnostics,
        "state references missing evidence object " + digest.hex());
    return false;
  }
  if (sha256(bytes) != digest) {
    store_error(
        diagnostics,
        "evidence object content does not match filename " + digest.hex());
    return false;
  }
  ValidationEvidence parsed;
  if (!parse_validation_evidence(bytes, parsed, diagnostics)) return false;
  if (serialize_validation_evidence(parsed) != bytes) {
    store_error(diagnostics, "evidence object is valid but noncanonical");
    return false;
  }
  evidence = std::move(parsed);
  return true;
}

[[nodiscard]] bool load_state_unlocked(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key,
    ValidationEvidenceState &state,
    DiagnosticSink &diagnostics) {
  state = {};
  state.key = key;
  bool exists = false;
  std::string bytes;
  if (!read_regular_file(
          state_path(workspace_directory, key),
          kMaximumStateBytes,
          exists,
          bytes,
          diagnostics)) {
    return false;
  }
  if (!exists) {
    state.status = ValidationEvidenceStateStatus::Missing;
    return true;
  }
  StoredState stored;
  if (!parse_state(bytes, stored, diagnostics)) return false;
  if (stored.key != key) {
    store_error(diagnostics, "state key does not match its filename");
    return false;
  }

  ValidationEvidence last;
  for (std::size_t index = 0; index < stored.attempts.size(); ++index) {
    ValidationEvidence attempt;
    if (!read_evidence_object(
            workspace_directory, stored.attempts[index], attempt, diagnostics)) {
      return false;
    }
    if (attempt.key != key || attempt.attempt != index + 1) {
      store_error(diagnostics, "evidence history key or ordinal is inconsistent");
      return false;
    }
    last = std::move(attempt);
  }
  if (stored.active.has_value()) {
    if (*stored.active != stored.attempts.back() || !last.passed) {
      store_error(diagnostics, "active evidence is not the latest passing attempt");
      return false;
    }
    state.status = ValidationEvidenceStateStatus::Active;
    state.active_digest = stored.active;
    state.active_evidence = std::move(last);
  } else {
    if (last.passed) {
      store_error(diagnostics, "latest passing attempt is incorrectly revoked");
      return false;
    }
    state.status = ValidationEvidenceStateStatus::Revoked;
  }
  state.attempts = std::move(stored.attempts);
  return true;
}

[[nodiscard]] bool publish_evidence_object(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &digest,
    std::string_view bytes,
    DiagnosticSink &diagnostics) {
  const std::filesystem::path path = object_path(workspace_directory, digest);
  bool exists = false;
  std::string existing;
  if (!read_regular_file(
          path, kMaximumEvidenceBytes, exists, existing, diagnostics)) {
    return false;
  }
  if (exists) {
    if (existing != bytes || sha256(existing) != digest) {
      store_error(
          diagnostics,
          "existing evidence object conflicts with digest " + digest.hex());
      return false;
    }
    return true;
  }
  return write_atomic(path, bytes, diagnostics);
}

} // namespace

bool load_validation_evidence_state(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &key,
    ValidationEvidenceState &state,
    DiagnosticSink &diagnostics) {
  const std::filesystem::path directory = evidence_directory(workspace_directory);
  std::error_code error;
  std::filesystem::file_status status =
      std::filesystem::symlink_status(directory, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    status = std::filesystem::file_status(
        std::filesystem::file_type::not_found);
  }
  if (error) {
    store_error(diagnostics, "cannot inspect evidence directory: " + error.message());
    return false;
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    state = {};
    state.key = key;
    return true;
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_directory(status)) {
    store_error(diagnostics, "evidence path is not a real directory");
    return false;
  }
  StoreLock lock(directory, diagnostics);
  if (!lock.ok()) return false;
  return load_state_unlocked(workspace_directory, key, state, diagnostics);
}

ValidationEvidenceCommitResult commit_validation_evidence(
    const std::filesystem::path &workspace_directory,
    ValidationEvidence evidence,
    DiagnosticSink &diagnostics) {
  ValidationEvidenceCommitResult result;
  evidence.key = hash_validation_evidence_key(evidence);
  const std::filesystem::path directory = evidence_directory(workspace_directory);
  if (!ensure_directory(directory, diagnostics)) return result;
  StoreLock lock(directory, diagnostics);
  if (!lock.ok()) return result;

  ValidationEvidenceState current;
  if (!load_state_unlocked(
          workspace_directory, evidence.key, current, diagnostics)) {
    return result;
  }
  if (current.attempts.size() >= kMaximumAttempts) {
    store_error(diagnostics, "attempt-count limit reached for evidence key");
    return result;
  }
  evidence.attempt = static_cast<std::uint64_t>(current.attempts.size()) + 1;

  // The strict parser is also the compiler-produced-object validator. This
  // catches accidental serializer/schema drift before any bytes reach disk.
  const std::string bytes = serialize_validation_evidence(evidence);
  ValidationEvidence checked;
  DiagnosticSink check_diagnostics;
  if (!parse_validation_evidence(bytes, checked, check_diagnostics) ||
      serialize_validation_evidence(checked) != bytes) {
    store_error(diagnostics, "compiler produced invalid validation evidence");
    return result;
  }
  const Sha256Digest digest = sha256(bytes);
  if (!publish_evidence_object(
          workspace_directory, digest, bytes, diagnostics)) {
    return result;
  }

  StoredState next;
  next.key = evidence.key;
  next.attempts = current.attempts;
  next.attempts.push_back(digest);
  if (evidence.passed) next.active = digest;
  const std::string state_bytes = serialize_state(next);
  if (!write_atomic(
          state_path(workspace_directory, evidence.key),
          state_bytes,
          diagnostics)) {
    return result;
  }

  result.ok = true;
  result.key = evidence.key;
  result.evidence_digest = digest;
  result.attempt = evidence.attempt;
  result.active = evidence.passed;
  result.evidence_path = object_path(workspace_directory, digest);
  return result;
}

} // namespace draft
