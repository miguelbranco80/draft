// Persistent storage for checked synthesis expansions and their manifest.
//
// The elaborator passes this module an already checked coherent program. The
// store verifies content identities again, stages immutable generated source,
// and publishes resolution.json only after every expansion is durable and in
// its final content-addressed location. A failed operation therefore leaves the
// previously visible manifest authoritative; newly installed but unreferenced
// expansion files are harmless immutable objects.
//
// This is deliberately a filesystem boundary, not a provider or semantic
// boundary. It does not invoke Codex, parse Draft, decide whether a pin is
// stale, or calculate the resolved-program identity. See
// 03-agent-synthesis.md section 10.

#pragma once

#include "base/sha256.h"
#include "elaborator/resolution.h"
#include "source/diagnostic.h"

#include <filesystem>
#include <span>
#include <string>

namespace draft {

// Named transaction boundaries make the crash-safety contract executable in
// tests without replacing the filesystem with a mock. They are also suitable
// for a caller that must stop a long store operation cleanly. Every checkpoint
// before ManifestPublished leaves the previous manifest authoritative;
// ManifestPublished is intentionally after the atomic visibility point.
enum class ResolutionCommitCheckpoint {
  StagingDirectoryCreated,
  ExpansionStaged,
  ManifestStaged,
  ExpansionPublished,
  ExpansionsSynchronized,
  BeforeManifestPublish,
  ManifestPublished,
};

using ResolutionCommitContinuation = bool (*)(
    ResolutionCommitCheckpoint checkpoint,
    void *context);

// A null continuation means an uninterrupted production commit. Returning
// false records one diagnostic and stops at that exact boundary. The opaque
// context keeps the store independent from std::function allocation and lets a
// test use an ordinary small state record.
struct ResolutionCommitControl {
  ResolutionCommitContinuation continue_commit = nullptr;
  void *context = nullptr;
};

// One proposed persistent object. source contains the exact UTF-8 bytes which
// were parsed and checked by the elaborator; no newline or normalization is
// added here. digest must equal SHA-256(source).
struct GeneratedExpansion {
  Sha256Digest digest;
  std::string source;
};

enum class ResolutionManifestLoadState {
  Missing,
  Loaded,
  Invalid,
};

// Missing is not itself an error because a workspace may never have been
// resolved. Invalid always has a corresponding diagnostic. manifest is useful
// only in the Loaded state.
struct ResolutionManifestLoadResult {
  ResolutionManifestLoadState state = ResolutionManifestLoadState::Missing;
  ResolutionManifest manifest;
};

// Reads .draft/resolution.json without modifying the workspace. Symlinks,
// non-regular files, excessive files, malformed JSON, and I/O failures are
// invalid. A nonexistent .draft directory or manifest returns Missing.
[[nodiscard]] ResolutionManifestLoadResult load_resolution_manifest(
    const std::filesystem::path &workspace_directory,
    DiagnosticSink &diagnostics);

// Loads one exact generated object and verifies its name against its contents.
// Missing or corrupt source is an error because callers use this operation only
// after selecting a pin which claims the object exists.
[[nodiscard]] bool load_generated_expansion(
    const std::filesystem::path &workspace_directory,
    const Sha256Digest &digest,
    std::string &source,
    DiagnosticSink &diagnostics);

// Atomically publishes one coherent resolution. Every pin must reference an
// expansion supplied here or an already valid object in the store. Supplied
// objects must be referenced, unique, and correctly hashed. Validation occurs
// before any directory is created. During commit, source objects are installed
// first and resolution.json is renamed last. The function never edits surface
// source and never removes a previously committed object.
[[nodiscard]] bool commit_resolution(
    const std::filesystem::path &workspace_directory,
    const ResolutionManifest &manifest,
    std::span<const GeneratedExpansion> expansions,
    DiagnosticSink &diagnostics,
    ResolutionCommitControl control = {});

} // namespace draft
