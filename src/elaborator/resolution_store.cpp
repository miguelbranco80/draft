// Crash-resistant implementation of the resolution store transaction.
//
// Files are staged below the same .draft directory as their destinations, so a
// rename cannot cross filesystems. Generated objects are immutable and may
// become harmless orphans if a process stops before manifest publication. The
// root/target manifest rename is the single visibility point for one resolved
// program. Generated objects remain shared by content hash across namespaces.
// File and directory synchronization makes the ordering durable on the first
// supported platform, AArch64 macOS. Other hosts retain atomic visibility but
// may not provide the same power-loss durability guarantee.

#include "elaborator/resolution_store.h"

#include "base/sha256.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace draft {
namespace {

constexpr std::uintmax_t kMaximumManifestBytes = 16U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumExpansionBytes = 64U * 1024U * 1024U;

class StagingDirectory {
public:
  explicit StagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}

  StagingDirectory(const StagingDirectory &) = delete;
  StagingDirectory &operator=(const StagingDirectory &) = delete;

  ~StagingDirectory() {
    if (path_.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void store_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(SourceRange::invalid(), std::move(message));
}

// Every manifest writer locks the selected workspace directory itself. That
// directory already exists before compilation begins, so invalid transactions
// preserve the store's useful no-write-before-validation property. flock is
// released automatically on every return path and by the kernel after a crash.
class ResolutionLock {
public:
  ResolutionLock(
      const std::filesystem::path &workspace_directory,
      DiagnosticSink &diagnostics) {
#if defined(__APPLE__) || defined(__unix__)
    descriptor_ = ::open(
        workspace_directory.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
    if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX) != 0) {
      if (descriptor_ >= 0) (void)::close(descriptor_);
      descriptor_ = -1;
      store_error(diagnostics, "cannot lock the resolution workspace");
    }
#else
    (void)workspace_directory;
    store_error(
        diagnostics,
        "resolution workspace locking is unavailable on this host");
#endif
  }

  ResolutionLock(const ResolutionLock &) = delete;
  ResolutionLock &operator=(const ResolutionLock &) = delete;

  ~ResolutionLock() {
#if defined(__APPLE__) || defined(__unix__)
    if (descriptor_ >= 0) {
      (void)::flock(descriptor_, LOCK_UN);
      (void)::close(descriptor_);
    }
#endif
  }

  [[nodiscard]] bool ok() const { return descriptor_ >= 0; }

private:
  int descriptor_ = -1;
};

[[nodiscard]] const char *checkpoint_name(
    ResolutionCommitCheckpoint checkpoint) {
  switch (checkpoint) {
  case ResolutionCommitCheckpoint::StagingDirectoryCreated:
    return "staging-directory-created";
  case ResolutionCommitCheckpoint::ExpansionStaged:
    return "expansion-staged";
  case ResolutionCommitCheckpoint::ManifestStaged:
    return "manifest-staged";
  case ResolutionCommitCheckpoint::ExpansionPublished:
    return "expansion-published";
  case ResolutionCommitCheckpoint::ExpansionsSynchronized:
    return "expansions-synchronized";
  case ResolutionCommitCheckpoint::BeforeManifestPublish:
    return "before-manifest-publish";
  case ResolutionCommitCheckpoint::ManifestPublished:
    return "manifest-published";
  }
  return "unknown";
}

[[nodiscard]] bool continue_commit(
    ResolutionCommitControl control,
    ResolutionCommitCheckpoint checkpoint,
    DiagnosticSink &diagnostics) {
  if (control.continue_commit == nullptr ||
      control.continue_commit(checkpoint, control.context)) {
    return true;
  }
  store_error(
      diagnostics,
      "resolution commit stopped at checkpoint '" +
          std::string(checkpoint_name(checkpoint)) + "'");
  return false;
}

// Rejecting symlinks keeps a workspace-controlled .draft entry from redirecting
// compiler writes outside the selected workspace. The caller creates one path
// component at a time, so no uninspected descendant is traversed.
[[nodiscard]] bool ensure_directory(
    const std::filesystem::path &path,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    store_error(diagnostics,
        "cannot inspect resolution directory '" + path.string() + "': " +
            error.message());
    return false;
  }
  if (!error && std::filesystem::exists(status)) {
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      store_error(diagnostics,
          "resolution path is not a real directory: '" + path.string() + "'");
      return false;
    }
    return true;
  }
  error.clear();
  if (std::filesystem::create_directory(path, error) && !error) return true;

  // Another resolver may create the shared directory after our first
  // inspection. Reinspect the entry instead of treating EEXIST as a failed
  // transaction, but retain the same no-symlink/no-nondirectory boundary.
  const std::error_code create_error = error;
  error.clear();
  const std::filesystem::file_status raced_status =
      std::filesystem::symlink_status(path, error);
  if (!error && std::filesystem::exists(raced_status) &&
      !std::filesystem::is_symlink(raced_status) &&
      std::filesystem::is_directory(raced_status)) {
    return true;
  }
  const std::string reason = error
      ? error.message()
      : (create_error ? create_error.message() : "path is not a real directory");
  store_error(diagnostics,
      "cannot create resolution directory '" + path.string() + "': " + reason);
  return false;
}

// Read operations also inspect each store directory itself. symlink_status on
// only the final file would otherwise follow a symlinked .draft parent and let
// a provider-free build consume a store outside the selected workspace.
[[nodiscard]] bool inspect_store_directory(
    const std::filesystem::path &path,
    bool &exists,
    DiagnosticSink &diagnostics) {
  exists = false;
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(status))) {
    return true;
  }
  if (error) {
    store_error(diagnostics,
        "cannot inspect resolution directory '" + path.string() + "': " +
            error.message());
    return false;
  }
  exists = true;
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_directory(status)) {
    store_error(diagnostics,
        "resolution path is not a real directory: '" + path.string() + "'");
    return false;
  }
  return true;
}

// Reads a bounded regular file. exists distinguishes a missing optional
// manifest from an empty or unreadable file; all other failures are diagnosed.
[[nodiscard]] bool read_store_file(
    const std::filesystem::path &path,
    std::uintmax_t maximum_bytes,
    bool &exists,
    std::string &contents,
    DiagnosticSink &diagnostics) {
  exists = false;
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(status))) {
    return true;
  }
  if (error) {
    store_error(diagnostics,
        "cannot inspect resolution file '" + path.string() + "': " +
            error.message());
    return false;
  }
  exists = true;
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    store_error(diagnostics,
        "resolution file is not a real regular file: '" + path.string() + "'");
    return false;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    store_error(diagnostics,
        "cannot size resolution file '" + path.string() + "': " +
            error.message());
    return false;
  }
  if (size > maximum_bytes ||
      size > static_cast<std::uintmax_t>(
          std::numeric_limits<std::size_t>::max())) {
    store_error(diagnostics,
        "resolution file is too large: '" + path.string() + "'");
    return false;
  }
  contents.resize(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    store_error(diagnostics,
        "cannot open resolution file '" + path.string() + "'");
    return false;
  }
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  if (!input || input.peek() != std::ifstream::traits_type::eof()) {
    store_error(diagnostics,
        "cannot read exact resolution file bytes from '" + path.string() + "'");
    return false;
  }
  return true;
}

[[nodiscard]] bool write_file(
    const std::filesystem::path &path,
    std::string_view contents,
    DiagnosticSink &diagnostics) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    store_error(diagnostics,
        "cannot create staged resolution file '" + path.string() + "'");
    return false;
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output) {
    store_error(diagnostics,
        "cannot write staged resolution file '" + path.string() + "'");
    return false;
  }
  output.close();
  return true;
}

// Generated objects are immutable. A normal POSIX rename would silently
// replace an existing destination, which is the wrong primitive when two
// resolver processes publish the same digest concurrently. Creating a hard
// link is atomic, fails if the destination name already exists, and is safe
// here because staging and generated are deliberately on the same filesystem.
// Removing the private staging name afterwards leaves the destination as the
// object's sole link; a crash between the two operations merely leaves an
// extra link inside a transaction directory that recovery already ignores.
[[nodiscard]] bool install_immutable_file(
    const std::filesystem::path &staged,
    const std::filesystem::path &destination,
    std::error_code &error) {
  error.clear();
  std::filesystem::create_hard_link(staged, destination, error);
  if (error) return false;
  std::filesystem::remove(staged, error);
  return !error;
}

// fsync is a durability operation, not part of semantic identity. It is kept in
// this filesystem module and used only where the host exposes the POSIX
// primitive. The compiler reports a real transaction failure if macOS refuses
// to make a staged file durable.
[[nodiscard]] bool synchronize_path(
    const std::filesystem::path &path,
    DiagnosticSink &diagnostics) {
#if defined(__APPLE__) || defined(__unix__)
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    store_error(diagnostics,
        "cannot open resolution path for synchronization '" + path.string() +
            "': " + std::strerror(errno));
    return false;
  }
  const int sync_result = ::fsync(descriptor);
  const int sync_error = errno;
  const int close_result = ::close(descriptor);
  if (sync_result != 0) {
    store_error(diagnostics,
        "cannot synchronize resolution path '" + path.string() + "': " +
            std::strerror(sync_error));
    return false;
  }
  if (close_result != 0) {
    store_error(diagnostics,
        "cannot close synchronized resolution path '" + path.string() + "': " +
            std::strerror(errno));
    return false;
  }
#else
  (void)path;
  (void)diagnostics;
#endif
  return true;
}

[[nodiscard]] const GeneratedExpansion *find_expansion(
    std::span<const GeneratedExpansion> expansions,
    const Sha256Digest &digest) {
  for (const GeneratedExpansion &expansion : expansions) {
    if (expansion.digest == digest) return &expansion;
  }
  return nullptr;
}

[[nodiscard]] bool manifest_references(
    const ResolutionManifest &manifest,
    const Sha256Digest &digest) {
  for (const ResolutionPin &pin : manifest.pins) {
    if (pin.expansion_digest == digest) return true;
  }
  return false;
}

// Every pin sharing a content object must agree on its byte length. The digest
// already protects contents; this independent check protects the manifest map
// from claiming a generated interval that cannot contain those contents.
[[nodiscard]] bool manifest_maps_expansion_size(
    const ResolutionManifest &manifest,
    const Sha256Digest &digest,
    std::size_t size) {
  bool referenced = false;
  for (const ResolutionPin &pin : manifest.pins) {
    if (pin.expansion_digest != digest) continue;
    referenced = true;
    if (pin.source_map.expansion_bytes !=
        static_cast<std::uint64_t>(size)) {
      return false;
    }
  }
  return referenced;
}

[[nodiscard]] bool valid_store_component(std::string_view value) {
  if (value.empty() || value == "." || value == "..") return false;
  for (const char byte : value) {
    const bool ascii_letter =
        (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
    const bool ascii_digit = byte >= '0' && byte <= '9';
    if (!ascii_letter && !ascii_digit && byte != '-' && byte != '_' &&
        byte != '.') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_root_package_path(std::string_view value) {
  if (value == ".") return true;
  if (value.empty() || value.front() == '/' || value.back() == '/' ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin < value.size()) {
    const std::size_t slash = value.find('/', begin);
    const std::size_t end = slash == std::string_view::npos
        ? value.size()
        : slash;
    const std::string_view component = value.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    begin = end + 1;
  }
  return true;
}

[[nodiscard]] bool validate_store_key(
    const ResolutionStoreKey &key,
    DiagnosticSink &diagnostics) {
  if (!valid_store_component(key.target_identity)) {
    store_error(
        diagnostics,
        "resolution store target identity is not a safe path component");
    return false;
  }
  if (key.root_package.root_identity != "workspace" ||
      !valid_root_package_path(key.root_package.root_relative_path)) {
    store_error(
        diagnostics,
        "resolution store root package is not a canonical workspace identity");
    return false;
  }
  return true;
}

// Root `.` and a child package must never contend for the same filesystem row.
// Separate fixed namespaces make the mapping injective while retaining the
// complete original child-package folder structure below `packages/`.
[[nodiscard]] std::filesystem::path manifest_directory(
    const std::filesystem::path &workspace_directory,
    const ResolutionStoreKey &key) {
  std::filesystem::path result = workspace_directory / ".draft" /
      "resolutions" / key.target_identity;
  if (key.root_package.root_relative_path == ".") {
    return result / "workspace";
  }
  return result / "packages" / key.root_package.root_relative_path;
}

[[nodiscard]] std::vector<std::filesystem::path> manifest_directory_chain(
    const std::filesystem::path &workspace_directory,
    const ResolutionStoreKey &key) {
  const std::filesystem::path store = workspace_directory / ".draft";
  const std::filesystem::path resolutions = store / "resolutions";
  const std::filesystem::path target = resolutions / key.target_identity;
  std::vector<std::filesystem::path> result{store, resolutions, target};
  if (key.root_package.root_relative_path == ".") {
    result.push_back(target / "workspace");
    return result;
  }
  std::filesystem::path current = target / "packages";
  result.push_back(current);
  std::size_t begin = 0;
  const std::string &relative = key.root_package.root_relative_path;
  while (begin < relative.size()) {
    const std::size_t slash = relative.find('/', begin);
    const std::size_t end = slash == std::string::npos
        ? relative.size()
        : slash;
    current /= relative.substr(begin, end - begin);
    result.push_back(current);
    begin = end + 1;
  }
  return result;
}

[[nodiscard]] std::filesystem::path generated_path(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &digest) {
  return workspace_directory / ".draft" / "generated" /
      (digest.hex() + ".draft");
}

[[nodiscard]] bool verify_existing_expansion(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &digest,
    bool required,
    std::string &contents,
    DiagnosticSink &diagnostics) {
  bool exists = false;
  if (!read_store_file(
          generated_path(workspace_directory, digest),
          kMaximumExpansionBytes,
          exists,
          contents,
          diagnostics)) {
    return false;
  }
  if (!exists) {
    if (required) {
      store_error(diagnostics,
          "missing generated expansion " + digest.hex());
    }
    return !required;
  }
  if (sha256(contents) != digest) {
    store_error(diagnostics,
        "generated expansion does not match its content identity " +
            digest.hex());
    return false;
  }
  return true;
}

} // namespace

ResolutionManifestLoadResult load_resolution_manifest(
    const std::filesystem::path &workspace_directory,
    const ResolutionStoreKey &key,
    DiagnosticSink &diagnostics) {
  ResolutionManifestLoadResult result;
  if (!validate_store_key(key, diagnostics)) {
    result.state = ResolutionManifestLoadState::Invalid;
    return result;
  }
  for (const std::filesystem::path &directory :
       manifest_directory_chain(workspace_directory, key)) {
    bool exists = false;
    if (!inspect_store_directory(directory, exists, diagnostics)) {
      result.state = ResolutionManifestLoadState::Invalid;
      return result;
    }
    if (!exists) return result;
  }
  bool exists = false;
  std::string json;
  const std::filesystem::path path =
      manifest_directory(workspace_directory, key) / "resolution.json";
  if (!read_store_file(
          path, kMaximumManifestBytes, exists, json, diagnostics)) {
    result.state = ResolutionManifestLoadState::Invalid;
    return result;
  }
  if (!exists) return result;
  if (!parse_resolution_manifest(json, result.manifest, diagnostics)) {
    result.state = ResolutionManifestLoadState::Invalid;
    return result;
  }
  if (result.manifest.target_identity != key.target_identity ||
      !(result.manifest.root_package == key.root_package)) {
    store_error(
        diagnostics,
        "resolution manifest does not match its root and target namespace");
    result.state = ResolutionManifestLoadState::Invalid;
    return result;
  }
  result.state = ResolutionManifestLoadState::Loaded;
  return result;
}

bool load_generated_expansion(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &digest,
    std::string &source,
    DiagnosticSink &diagnostics) {
  bool store_exists = false;
  bool generated_exists = false;
  if (!inspect_store_directory(
          workspace_directory / ".draft", store_exists, diagnostics) ||
      (store_exists &&
       !inspect_store_directory(
           workspace_directory / ".draft" / "generated",
           generated_exists,
           diagnostics))) {
    return false;
  }
  if (!store_exists || !generated_exists) {
    store_error(diagnostics, "missing generated expansion " + digest.hex());
    return false;
  }
  return verify_existing_expansion(
      workspace_directory, digest, true, source, diagnostics);
}

static bool commit_resolution_unlocked(
    const std::filesystem::path &workspace_directory,
    const ResolutionStoreKey &key,
    const ResolutionManifest &manifest,
    std::span<const GeneratedExpansion> expansions,
    DiagnosticSink &diagnostics,
    ResolutionCommitControl control) {
  const std::size_t initial_errors = diagnostics.error_count();

  if (!validate_store_key(key, diagnostics)) return false;
  if (manifest.target_identity != key.target_identity ||
      !(manifest.root_package == key.root_package)) {
    store_error(
        diagnostics,
        "resolution manifest does not match the selected root and target");
    return false;
  }

  // Round-trip through the strict schema before touching the filesystem. This
  // applies the same duplicate-site and kind invariants to compiler-produced
  // data that the loader applies to disk input.
  const std::string manifest_json = serialize_resolution_manifest(manifest);
  ResolutionManifest checked_manifest;
  if (!parse_resolution_manifest(
          manifest_json, checked_manifest, diagnostics)) {
    return false;
  }

  // Verify supplied objects and reject ambiguous or unused transaction input.
  // Duplicate digests are rejected even when bytes agree so the transaction has
  // one obvious owner for every staged file.
  for (std::size_t index = 0; index < expansions.size(); ++index) {
    const GeneratedExpansion &expansion = expansions[index];
    if (expansion.source.size() > kMaximumExpansionBytes) {
      store_error(diagnostics,
          "supplied generated expansion exceeds the store size limit");
      continue;
    }
    if (sha256(expansion.source) != expansion.digest) {
      store_error(diagnostics,
          "supplied expansion does not match digest " + expansion.digest.hex());
      continue;
    }
    if (!manifest_references(checked_manifest, expansion.digest)) {
      store_error(diagnostics,
          "supplied expansion is not referenced by the resolution manifest");
    } else if (!manifest_maps_expansion_size(
                   checked_manifest,
                   expansion.digest,
                   expansion.source.size())) {
      store_error(
          diagnostics,
          "supplied expansion length does not match its generated-source map");
    }
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (expansions[earlier].digest == expansion.digest) {
        store_error(diagnostics,
            "resolution transaction supplies a duplicate expansion digest");
        break;
      }
    }
  }
  if (diagnostics.error_count() != initial_errors) return false;

  // Every referenced object must be supplied or already exist and verify. This
  // read-only pass happens before creating .draft, preserving the no-write rule
  // for incomplete transactions.
  for (const ResolutionPin &pin : checked_manifest.pins) {
    if (find_expansion(expansions, pin.expansion_digest) != nullptr) continue;
    std::string existing;
    if (!verify_existing_expansion(
            workspace_directory,
            pin.expansion_digest,
            true,
            existing,
            diagnostics)) {
      return false;
    }
    if (pin.source_map.expansion_bytes !=
        static_cast<std::uint64_t>(existing.size())) {
      store_error(
          diagnostics,
          "stored expansion length does not match its generated-source map");
      return false;
    }
  }

  const std::filesystem::path store = workspace_directory / ".draft";
  const std::filesystem::path generated = store / "generated";
  const std::filesystem::path staging_root = store / "staging";
  if (!ensure_directory(store, diagnostics) ||
      !ensure_directory(generated, diagnostics) ||
      !ensure_directory(staging_root, diagnostics)) {
    return false;
  }
  for (const std::filesystem::path &directory :
       manifest_directory_chain(workspace_directory, key)) {
    if (!ensure_directory(directory, diagnostics)) return false;
  }

  // create_directory is the concurrency primitive here: two resolver
  // processes cannot claim the same staging name. The suffix has no semantic
  // meaning and never enters a manifest or diagnostic identity.
  std::filesystem::path staging_path;
  for (std::size_t attempt = 0; attempt < 1024; ++attempt) {
    staging_path = staging_root /
        (checked_manifest.resolved_program_digest.hex().substr(0, 24) + "-" +
         std::to_string(attempt));
    std::error_code create_error;
    if (std::filesystem::create_directory(staging_path, create_error)) break;
    if (create_error) {
      store_error(diagnostics,
          "cannot create resolution staging directory '" +
              staging_path.string() + "': " + create_error.message());
      return false;
    }
    staging_path.clear();
  }
  if (staging_path.empty()) {
    store_error(diagnostics, "too many concurrent resolution transactions");
    return false;
  }
  StagingDirectory staging(staging_path);
  if (!continue_commit(
          control,
          ResolutionCommitCheckpoint::StagingDirectoryCreated,
          diagnostics)) {
    return false;
  }

  // Stage only objects that are not already present. Existing objects are read
  // and hash-verified; a same-digest/different-byte condition is treated as a
  // collision or corruption, never overwritten.
  for (const GeneratedExpansion &expansion : expansions) {
    std::string existing;
    bool exists = false;
    if (!read_store_file(
            generated_path(workspace_directory, expansion.digest),
            kMaximumExpansionBytes,
            exists,
            existing,
            diagnostics)) {
      return false;
    }
    if (exists) {
      if (existing != expansion.source || sha256(existing) != expansion.digest) {
        store_error(diagnostics,
            "existing generated expansion conflicts with digest " +
                expansion.digest.hex());
        return false;
      }
      continue;
    }
    const std::filesystem::path staged =
        staging.path() / (expansion.digest.hex() + ".draft");
    if (!write_file(staged, expansion.source, diagnostics) ||
        !synchronize_path(staged, diagnostics)) {
      return false;
    }
    if (!continue_commit(
            control,
            ResolutionCommitCheckpoint::ExpansionStaged,
            diagnostics)) {
      return false;
    }
  }

  const std::filesystem::path staged_manifest =
      staging.path() / "resolution.json";
  if (!write_file(staged_manifest, manifest_json, diagnostics) ||
      !synchronize_path(staged_manifest, diagnostics) ||
      !synchronize_path(staging.path(), diagnostics)) {
    return false;
  }
  if (!continue_commit(
          control,
          ResolutionCommitCheckpoint::ManifestStaged,
          diagnostics)) {
    return false;
  }

  // Publish immutable source first. If the process stops in this loop, the old
  // manifest still selects the old coherent program and the new source is only
  // an unreferenced content object.
  for (const GeneratedExpansion &expansion : expansions) {
    const std::filesystem::path staged =
        staging.path() / (expansion.digest.hex() + ".draft");
    std::error_code status_error;
    if (!std::filesystem::exists(staged, status_error)) {
      if (status_error) {
        store_error(diagnostics,
            "cannot inspect staged expansion: " + status_error.message());
        return false;
      }
      continue;
    }
    const std::filesystem::path destination =
        generated_path(workspace_directory, expansion.digest);
    std::error_code install_error;
    if (!install_immutable_file(staged, destination, install_error)) {
      // Another identical transaction may have won the race. Accept that only
      // after reading and verifying the exact destination object.
      std::string concurrent;
      if (!verify_existing_expansion(
              workspace_directory,
              expansion.digest,
              true,
              concurrent,
              diagnostics) ||
          concurrent != expansion.source) {
        store_error(diagnostics,
            "cannot publish generated expansion: " + install_error.message());
        return false;
      }
    }
    if (!continue_commit(
            control,
            ResolutionCommitCheckpoint::ExpansionPublished,
            diagnostics)) {
      return false;
    }
  }
  if (!synchronize_path(generated, diagnostics)) return false;
  if (!continue_commit(
          control,
          ResolutionCommitCheckpoint::ExpansionsSynchronized,
          diagnostics) ||
      !continue_commit(
          control,
          ResolutionCommitCheckpoint::BeforeManifestPublish,
          diagnostics)) {
    return false;
  }

  // POSIX rename replaces an existing manifest atomically. This is the only
  // point at which readers can observe the new program identity and pin set.
  const std::filesystem::path destination_manifest =
      manifest_directory(workspace_directory, key) / "resolution.json";
  std::error_code rename_error;
  std::filesystem::rename(staged_manifest, destination_manifest, rename_error);
  if (rename_error) {
    store_error(diagnostics,
        "cannot publish resolution manifest: " + rename_error.message());
    return false;
  }
  if (!continue_commit(
          control,
          ResolutionCommitCheckpoint::ManifestPublished,
          diagnostics)) {
    return false;
  }
  std::vector<std::filesystem::path> manifest_directories =
      manifest_directory_chain(workspace_directory, key);
  for (auto directory = manifest_directories.rbegin();
       directory != manifest_directories.rend(); ++directory) {
    if (!synchronize_path(*directory, diagnostics)) return false;
  }
  return diagnostics.error_count() == initial_errors;
}

bool commit_resolution(
    const std::filesystem::path &workspace_directory,
    const ResolutionStoreKey &key,
    const ResolutionManifest &manifest,
    std::span<const GeneratedExpansion> expansions,
    DiagnosticSink &diagnostics,
    ResolutionCommitControl control) {
  ResolutionLock lock(workspace_directory, diagnostics);
  if (!lock.ok()) return false;
  return commit_resolution_unlocked(
      workspace_directory, key, manifest, expansions, diagnostics, control);
}

} // namespace draft
