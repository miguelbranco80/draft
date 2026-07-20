// Resolution-store commit, reload, validation, and no-partial-write tests.

#include "elaborator/resolution_store.h"

#include "base/sha256.h"
#include "elaborator/resolution.h"
#include "source/diagnostic.h"

#include "test_directory.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "resolution_store_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryWorkspace {
  draft::test::TemporaryDirectory directory;
  std::filesystem::path path;

  explicit TemporaryWorkspace(std::string_view name)
      : directory("draft-resolution-store-test-" + std::string(name)),
        path(directory.path()) {}
};

draft::ResolutionManifest manifest_for(
    const draft::GeneratedExpansion &expansion) {
  draft::ResolutionPin pin;
  pin.site_identity = "site-" + std::string(64, 'a');
  pin.kind = draft::AgentConstructKind::SynthesisExpression;
  pin.input_digest = draft::sha256("obligation-v1");
  pin.expansion_digest = expansion.digest;
  pin.source_map = {
      "workspace", ".", "package.draft", 10, 13,
      static_cast<std::uint64_t>(expansion.source.size()),
  };
  pin.provider_identity = "fake-provider-v1";
  pin.model_identity = "deterministic-model-v1";
  pin.configuration_identity = "resolver-config-v1";

  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("coherent-program-v1");
  manifest.pins.push_back(std::move(pin));
  return manifest;
}

// Tests derive the filesystem namespace from the same persistent identities
// carried by the manifest. This keeps each fixture honest about the invariant
// enforced by commit_resolution: a manifest may only be published at its exact
// selected-root and target key.
draft::ResolutionStoreKey store_key_for(
    const draft::ResolutionManifest &manifest) {
  return {manifest.target_identity, manifest.root_package};
}

std::filesystem::path manifest_path_for(
    const std::filesystem::path &workspace,
    const draft::ResolutionManifest &manifest) {
  std::filesystem::path directory = workspace / ".draft" / "resolutions" /
      manifest.target_identity;
  if (manifest.root_package.root_relative_path == ".") {
    directory /= "workspace";
  } else {
    directory /= "packages";
    directory /= manifest.root_package.root_relative_path;
  }
  return directory / "resolution.json";
}

struct StopAtCheckpoint {
  draft::ResolutionCommitCheckpoint target =
      draft::ResolutionCommitCheckpoint::StagingDirectoryCreated;
  std::size_t visits = 0;
};

bool stop_at_checkpoint(
    draft::ResolutionCommitCheckpoint checkpoint,
    void *opaque) {
  auto &state = *static_cast<StopAtCheckpoint *>(opaque);
  if (checkpoint != state.target) return true;
  ++state.visits;
  return false;
}

void test_commit_and_reload(TestState &state) {
  TemporaryWorkspace workspace("commit");
  draft::GeneratedExpansion expansion;
  expansion.source = "40 + 2";
  expansion.digest = draft::sha256(expansion.source);
  draft::ResolutionManifest manifest = manifest_for(expansion);

  draft::DiagnosticSink diagnostics;
  EXPECT(state,
      draft::commit_resolution(
          workspace.path,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics));
  EXPECT(state, !diagnostics.has_errors());

  const std::filesystem::path generated = workspace.path / ".draft" /
      "generated" / (expansion.digest.hex() + ".draft");
  EXPECT(state, std::filesystem::is_regular_file(generated));
  EXPECT(state,
      std::filesystem::is_regular_file(
          manifest_path_for(workspace.path, manifest)));

  draft::DiagnosticSink load_diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(
          workspace.path, store_key_for(manifest), load_diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, !load_diagnostics.has_errors());
  EXPECT(state, loaded.manifest.pins.size() == 1);
  if (loaded.manifest.pins.size() == 1) {
    EXPECT(state,
        loaded.manifest.pins[0].expansion_digest == expansion.digest);
  }

  std::string source;
  EXPECT(state,
      draft::load_generated_expansion(
          workspace.path, expansion.digest, source, load_diagnostics));
  EXPECT(state, source == expansion.source);

  // Re-pinning an already stored expansion requires no provider output and
  // exercises atomic replacement of an existing resolution.json.
  manifest.pins[0].input_digest = draft::sha256("obligation-v2");
  manifest.resolved_program_digest = draft::sha256("coherent-program-v2");
  draft::DiagnosticSink repin_diagnostics;
  EXPECT(state,
      draft::commit_resolution(
          workspace.path,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(),
          repin_diagnostics));
  EXPECT(state, !repin_diagnostics.has_errors());
  const draft::ResolutionManifestLoadResult repinned =
      draft::load_resolution_manifest(
          workspace.path, store_key_for(manifest), repin_diagnostics);
  EXPECT(state,
      repinned.state == draft::ResolutionManifestLoadState::Loaded);
  if (repinned.manifest.pins.size() == 1) {
    EXPECT(state,
        repinned.manifest.pins[0].input_digest ==
            manifest.pins[0].input_digest);
  }

  // Successful transactions remove their private staging directory while
  // retaining the shared staging root for later commits.
  const std::filesystem::path staging = workspace.path / ".draft" / "staging";
  EXPECT(state, std::filesystem::is_directory(staging));
  EXPECT(state,
      std::filesystem::directory_iterator(staging) ==
          std::filesystem::directory_iterator());
}

void test_validation_precedes_writes(TestState &state) {
  TemporaryWorkspace workspace("invalid");
  draft::GeneratedExpansion expansion;
  expansion.source = "valid bytes";
  expansion.digest = draft::sha256("different bytes");
  const draft::ResolutionManifest manifest = manifest_for(expansion);

  draft::DiagnosticSink diagnostics;
  EXPECT(state,
      !draft::commit_resolution(
          workspace.path,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics));
  EXPECT(state, diagnostics.has_errors());
  EXPECT(state, !std::filesystem::exists(workspace.path / ".draft"));

  TemporaryWorkspace wrong_namespace("wrong-namespace");
  draft::ResolutionStoreKey mismatched_key = store_key_for(manifest);
  mismatched_key.root_package.root_relative_path = "another-root";
  draft::DiagnosticSink namespace_diagnostics;
  EXPECT(state,
      !draft::commit_resolution(
          wrong_namespace.path,
          mismatched_key,
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          namespace_diagnostics));
  EXPECT(state, namespace_diagnostics.has_errors());
  EXPECT(state, !std::filesystem::exists(wrong_namespace.path / ".draft"));

  // A correct content digest is insufficient when the persistent source map
  // claims a different generated interval length. Reject that mismatch before
  // the object or manifest becomes visible.
  TemporaryWorkspace wrong_map("wrong-map");
  expansion.digest = draft::sha256(expansion.source);
  draft::ResolutionManifest wrong_map_manifest = manifest_for(expansion);
  ++wrong_map_manifest.pins[0].source_map.expansion_bytes;
  draft::DiagnosticSink wrong_map_diagnostics;
  EXPECT(state,
      !draft::commit_resolution(
          wrong_map.path,
          store_key_for(wrong_map_manifest),
          wrong_map_manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          wrong_map_diagnostics));
  EXPECT(state, wrong_map_diagnostics.has_errors());
  EXPECT(state, !std::filesystem::exists(wrong_map.path / ".draft"));

  // A manifest which references neither supplied nor stored source also fails
  // before creating store directories.
  TemporaryWorkspace missing("missing");
  draft::GeneratedExpansion named_only;
  named_only.source = "not supplied";
  named_only.digest = draft::sha256(named_only.source);
  const draft::ResolutionManifest missing_manifest = manifest_for(named_only);
  draft::DiagnosticSink missing_diagnostics;
  EXPECT(state,
      !draft::commit_resolution(
          missing.path,
          store_key_for(missing_manifest),
          missing_manifest,
          std::span<const draft::GeneratedExpansion>(),
          missing_diagnostics));
  EXPECT(state, missing_diagnostics.has_errors());
  EXPECT(state, !std::filesystem::exists(missing.path / ".draft"));
}

void test_corrupt_object_is_rejected(TestState &state) {
  TemporaryWorkspace workspace("corrupt");
  draft::GeneratedExpansion expansion;
  expansion.source = "checked expression";
  expansion.digest = draft::sha256(expansion.source);
  const draft::ResolutionManifest manifest = manifest_for(expansion);
  draft::DiagnosticSink diagnostics;
  if (!draft::commit_resolution(
          workspace.path,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics)) {
    EXPECT(state, false);
    return;
  }

  const std::filesystem::path generated = workspace.path / ".draft" /
      "generated" / (expansion.digest.hex() + ".draft");
  std::ofstream corrupt(generated, std::ios::binary | std::ios::trunc);
  corrupt << "tampered";
  corrupt.close();

  std::string source;
  draft::DiagnosticSink corrupt_diagnostics;
  EXPECT(state,
      !draft::load_generated_expansion(
          workspace.path,
          expansion.digest,
          source,
          corrupt_diagnostics));
  EXPECT(state, corrupt_diagnostics.has_errors());
}

void test_root_and_target_namespaces_are_independent(TestState &state) {
  TemporaryWorkspace workspace("namespaces");
  draft::GeneratedExpansion expansion;
  expansion.source = "one shared generated object";
  expansion.digest = draft::sha256(expansion.source);

  draft::ResolutionManifest app = manifest_for(expansion);
  app.root_package = {"workspace", "app"};
  app.resolved_program_digest = draft::sha256("app program");
  draft::ResolutionManifest admin = manifest_for(expansion);
  admin.root_package = {"workspace", "tools/admin"};
  admin.resolved_program_digest = draft::sha256("admin program");
  draft::ResolutionManifest linux = app;
  linux.target_identity = "aarch64-linux-gnu";
  linux.resolved_program_digest = draft::sha256("linux app program");

  draft::DiagnosticSink diagnostics;
  EXPECT(state, draft::commit_resolution(
      workspace.path,
      store_key_for(app),
      app,
      std::span<const draft::GeneratedExpansion>(&expansion, 1),
      diagnostics));
  EXPECT(state, draft::commit_resolution(
      workspace.path,
      store_key_for(admin),
      admin,
      std::span<const draft::GeneratedExpansion>(),
      diagnostics));
  EXPECT(state, draft::commit_resolution(
      workspace.path,
      store_key_for(linux),
      linux,
      std::span<const draft::GeneratedExpansion>(),
      diagnostics));
  EXPECT(state, !diagnostics.has_errors());

  for (const draft::ResolutionManifest *expected : {&app, &admin, &linux}) {
    draft::DiagnosticSink load_diagnostics;
    const draft::ResolutionManifestLoadResult loaded =
        draft::load_resolution_manifest(
            workspace.path, store_key_for(*expected), load_diagnostics);
    EXPECT(state, loaded.state == draft::ResolutionManifestLoadState::Loaded);
    EXPECT(state, !load_diagnostics.has_errors());
    EXPECT(state,
        loaded.manifest.resolved_program_digest ==
            expected->resolved_program_digest);
    EXPECT(state,
        std::filesystem::is_regular_file(
            manifest_path_for(workspace.path, *expected)));
  }

  // Content addressing deliberately remains workspace-wide: all three
  // manifests reference one immutable generated object rather than copying it
  // into each root/target namespace.
  const std::filesystem::path generated = workspace.path / ".draft" /
      "generated" / (expansion.digest.hex() + ".draft");
  EXPECT(state, std::filesystem::is_regular_file(generated));
}

void test_interrupted_publish_recovery(TestState &state) {
  TemporaryWorkspace workspace("interrupted");
  draft::GeneratedExpansion first;
  first.source = "first checked expansion";
  first.digest = draft::sha256(first.source);
  const draft::ResolutionManifest first_manifest = manifest_for(first);
  draft::DiagnosticSink diagnostics;
  EXPECT(state,
      draft::commit_resolution(
          workspace.path,
          store_key_for(first_manifest),
          first_manifest,
          std::span<const draft::GeneratedExpansion>(&first, 1),
          diagnostics));
  if (diagnostics.has_errors()) return;

  draft::GeneratedExpansion second;
  second.source = "second checked expansion";
  second.digest = draft::sha256(second.source);
  draft::ResolutionManifest second_manifest = manifest_for(second);
  second_manifest.resolved_program_digest = draft::sha256("second program");

  // Model a process stop after the immutable object rename but before the
  // manifest rename. The abandoned staged manifest must remain invisible; the
  // old manifest is still authoritative even though the new object is present.
  const std::filesystem::path generated = workspace.path / ".draft" / "generated";
  std::ofstream(
      generated / (second.digest.hex() + ".draft"),
      std::ios::binary) << second.source;
  const std::filesystem::path abandoned =
      workspace.path / ".draft" / "staging" / "abandoned-after-object";
  std::error_code error;
  std::filesystem::create_directories(abandoned, error);
  EXPECT(state, !error);
  std::ofstream(abandoned / "resolution.json", std::ios::binary)
      << draft::serialize_resolution_manifest(second_manifest);

  draft::DiagnosticSink interrupted_diagnostics;
  const draft::ResolutionManifestLoadResult interrupted =
      draft::load_resolution_manifest(
          workspace.path,
          store_key_for(first_manifest),
          interrupted_diagnostics);
  EXPECT(state,
      interrupted.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, !interrupted_diagnostics.has_errors());
  if (interrupted.manifest.pins.size() == 1) {
    EXPECT(state,
        interrupted.manifest.pins[0].expansion_digest == first.digest);
  }

  // A subsequent transaction may reuse the already durable orphan object and
  // publish the new manifest. Stale private staging directories are ignored and
  // cannot affect which coherent program is selected.
  draft::DiagnosticSink recovery_diagnostics;
  EXPECT(state,
      draft::commit_resolution(
          workspace.path,
          store_key_for(second_manifest),
          second_manifest,
          std::span<const draft::GeneratedExpansion>(),
          recovery_diagnostics));
  EXPECT(state, !recovery_diagnostics.has_errors());
  const draft::ResolutionManifestLoadResult recovered =
      draft::load_resolution_manifest(
          workspace.path,
          store_key_for(second_manifest),
          recovery_diagnostics);
  EXPECT(state, recovered.state == draft::ResolutionManifestLoadState::Loaded);
  if (recovered.manifest.pins.size() == 1) {
    EXPECT(state,
        recovered.manifest.pins[0].expansion_digest == second.digest);
  }
}

void test_injected_transaction_boundaries(TestState &state) {
  const std::vector checkpoints{
      draft::ResolutionCommitCheckpoint::StagingDirectoryCreated,
      draft::ResolutionCommitCheckpoint::ExpansionStaged,
      draft::ResolutionCommitCheckpoint::ManifestStaged,
      draft::ResolutionCommitCheckpoint::ExpansionPublished,
      draft::ResolutionCommitCheckpoint::ExpansionsSynchronized,
      draft::ResolutionCommitCheckpoint::BeforeManifestPublish,
      draft::ResolutionCommitCheckpoint::ManifestPublished,
  };

  for (std::size_t index = 0; index < checkpoints.size(); ++index) {
    TemporaryWorkspace workspace("fault-" + std::to_string(index));
    draft::GeneratedExpansion first;
    first.source = "first fault-injection expansion";
    first.digest = draft::sha256(first.source);
    const draft::ResolutionManifest first_manifest = manifest_for(first);
    draft::DiagnosticSink initial_diagnostics;
    EXPECT(state,
        draft::commit_resolution(
            workspace.path,
            store_key_for(first_manifest),
            first_manifest,
            std::span<const draft::GeneratedExpansion>(&first, 1),
            initial_diagnostics));
    if (initial_diagnostics.has_errors()) continue;

    draft::GeneratedExpansion second;
    second.source = "second fault-injection expansion " +
        std::to_string(index);
    second.digest = draft::sha256(second.source);
    draft::ResolutionManifest second_manifest = manifest_for(second);
    second_manifest.resolved_program_digest =
        draft::sha256("fault-injection program " + std::to_string(index));

    StopAtCheckpoint stop{checkpoints[index], 0};
    draft::DiagnosticSink stopped_diagnostics;
    EXPECT(state,
        !draft::commit_resolution(
            workspace.path,
            store_key_for(second_manifest),
            second_manifest,
            std::span<const draft::GeneratedExpansion>(&second, 1),
            stopped_diagnostics,
            {stop_at_checkpoint, &stop}));
    EXPECT(state, stopped_diagnostics.error_count() == 1);
    EXPECT(state, stop.visits == 1);

    draft::DiagnosticSink observed_diagnostics;
    const draft::ResolutionManifestLoadResult observed =
        draft::load_resolution_manifest(
            workspace.path,
            store_key_for(first_manifest),
            observed_diagnostics);
    EXPECT(state,
        observed.state == draft::ResolutionManifestLoadState::Loaded);
    EXPECT(state, !observed_diagnostics.has_errors());
    if (observed.manifest.pins.size() == 1) {
      const bool crossed_visibility = checkpoints[index] ==
          draft::ResolutionCommitCheckpoint::ManifestPublished;
      EXPECT(state,
          observed.manifest.pins[0].expansion_digest ==
              (crossed_visibility ? second.digest : first.digest));
    }

    // RAII removes the transaction-private directory at every return point.
    // An object published before failure may remain as an immutable orphan;
    // the retry is required to verify and reuse it rather than overwrite it.
    const std::filesystem::path staging =
        workspace.path / ".draft" / "staging";
    EXPECT(state,
        std::filesystem::directory_iterator(staging) ==
            std::filesystem::directory_iterator());
    draft::DiagnosticSink retry_diagnostics;
    EXPECT(state,
        draft::commit_resolution(
            workspace.path,
            store_key_for(second_manifest),
            second_manifest,
            std::span<const draft::GeneratedExpansion>(&second, 1),
            retry_diagnostics));
    EXPECT(state, !retry_diagnostics.has_errors());
    const draft::ResolutionManifestLoadResult recovered =
        draft::load_resolution_manifest(
            workspace.path,
            store_key_for(second_manifest),
            retry_diagnostics);
    EXPECT(state,
        recovered.state == draft::ResolutionManifestLoadState::Loaded);
    if (recovered.manifest.pins.size() == 1) {
      EXPECT(state,
          recovered.manifest.pins[0].expansion_digest == second.digest);
    }
  }
}

void test_concurrent_process_writers(TestState &state) {
#if defined(__APPLE__) || defined(__unix__)
  static constexpr std::size_t writer_count = 6;
  static constexpr std::size_t rounds = 4;
  for (std::size_t round = 0; round < rounds; ++round) {
    TemporaryWorkspace workspace("writer-race-" + std::to_string(round));
    draft::GeneratedExpansion expansion;
    expansion.source = "shared concurrent expansion " + std::to_string(round);
    expansion.digest = draft::sha256(expansion.source);
    std::vector<draft::ResolutionManifest> manifests;
    manifests.reserve(writer_count);
    for (std::size_t writer = 0; writer < writer_count; ++writer) {
      draft::ResolutionManifest manifest = manifest_for(expansion);
      // Alternate distinct candidate manifests with identical transactions.
      // The former proves the visible file is one complete atomic winner; the
      // latter forces all writers through the numbered staging-name collision
      // path while they race to install the same immutable object.
      if (round % 2 == 0) {
        manifest.pins[0].input_digest = draft::sha256(
            "concurrent obligation " + std::to_string(round) + ":" +
            std::to_string(writer));
        manifest.resolved_program_digest = draft::sha256(
            "concurrent program " + std::to_string(round) + ":" +
            std::to_string(writer));
      }
      manifests.push_back(std::move(manifest));
    }

    // Children block on one shared pipe until every process exists. Releasing
    // one byte per child makes staging, exclusive object installation, and
    // atomic manifest replacement contend on the real host filesystem.
    int start_pipe[2] = {-1, -1};
    EXPECT(state, ::pipe(start_pipe) == 0);
    if (start_pipe[0] < 0 || start_pipe[1] < 0) return;
    std::vector<pid_t> children;
    children.reserve(writer_count);
    for (std::size_t writer = 0; writer < writer_count; ++writer) {
      const pid_t child = ::fork();
      EXPECT(state, child >= 0);
      if (child < 0) break;
      if (child == 0) {
        (void)::close(start_pipe[1]);
        char release = 0;
        ssize_t count = 0;
        do {
          count = ::read(start_pipe[0], &release, 1);
        } while (count < 0 && errno == EINTR);
        (void)::close(start_pipe[0]);
        if (count != 1) _exit(2);
        draft::DiagnosticSink diagnostics;
        const bool committed = draft::commit_resolution(
            workspace.path,
            store_key_for(manifests[writer]),
            manifests[writer],
            std::span<const draft::GeneratedExpansion>(&expansion, 1),
            diagnostics);
        _exit(committed && !diagnostics.has_errors() ? 0 : 3);
      }
      children.push_back(child);
    }
    (void)::close(start_pipe[0]);
    const std::string release(children.size(), 'x');
    std::size_t written = 0;
    while (written < release.size()) {
      const ssize_t count = ::write(
          start_pipe[1], release.data() + written, release.size() - written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      written += static_cast<std::size_t>(count);
    }
    (void)::close(start_pipe[1]);
    EXPECT(state, written == release.size());
    for (pid_t child : children) {
      int status = 0;
      pid_t waited = -1;
      do {
        waited = ::waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      EXPECT(state, waited == child);
      EXPECT(state, WIFEXITED(status));
      if (WIFEXITED(status)) EXPECT(state, WEXITSTATUS(status) == 0);
    }

    draft::DiagnosticSink diagnostics;
    const draft::ResolutionManifestLoadResult loaded =
        draft::load_resolution_manifest(
            workspace.path, store_key_for(manifests[0]), diagnostics);
    EXPECT(state, loaded.state == draft::ResolutionManifestLoadState::Loaded);
    EXPECT(state, !diagnostics.has_errors());
    bool complete_winner = false;
    for (const draft::ResolutionManifest &candidate : manifests) {
      if (draft::serialize_resolution_manifest(candidate) ==
          draft::serialize_resolution_manifest(loaded.manifest)) {
        complete_winner = true;
        break;
      }
    }
    EXPECT(state, complete_winner);
    std::string source;
    EXPECT(state,
        draft::load_generated_expansion(
            workspace.path, expansion.digest, source, diagnostics));
    EXPECT(state, source == expansion.source);
    const std::filesystem::path staging =
        workspace.path / ".draft" / "staging";
    EXPECT(state,
        std::filesystem::directory_iterator(staging) ==
            std::filesystem::directory_iterator());
  }
#else
  (void)state;
#endif
}

void test_store_rejects_redirected_and_unbounded_files(TestState &state) {
  draft::GeneratedExpansion expansion;
  expansion.source = "bounded checked expansion";
  expansion.digest = draft::sha256(expansion.source);
  const draft::ResolutionManifest manifest = manifest_for(expansion);

  // The store root itself must never redirect compiler reads or writes. Keep
  // the target in a separate temporary workspace so a mistaken traversal is
  // observable without touching anything outside the test's ownership.
  TemporaryWorkspace redirected("redirected-root");
  TemporaryWorkspace redirect_target("redirect-target");
  std::error_code error;
  std::filesystem::create_directory_symlink(
      redirect_target.path, redirected.path / ".draft", error);
  EXPECT(state, !error);
  draft::DiagnosticSink redirected_load_diagnostics;
  const draft::ResolutionManifestLoadResult redirected_load =
      draft::load_resolution_manifest(
          redirected.path,
          store_key_for(manifest),
          redirected_load_diagnostics);
  EXPECT(state,
      redirected_load.state == draft::ResolutionManifestLoadState::Invalid);
  EXPECT(state, redirected_load_diagnostics.has_errors());
  draft::DiagnosticSink redirected_commit_diagnostics;
  EXPECT(state,
      !draft::commit_resolution(
          redirected.path,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          redirected_commit_diagnostics));
  EXPECT(state, redirected_commit_diagnostics.has_errors());
  EXPECT(state,
      std::filesystem::directory_iterator(redirect_target.path) ==
          std::filesystem::directory_iterator());

  // Final files receive the same treatment as directories. A valid manifest
  // reached through a symlink is still not part of the selected store.
  TemporaryWorkspace manifest_link("manifest-link");
  const std::filesystem::path linked_manifest_path =
      manifest_path_for(manifest_link.path, manifest);
  std::filesystem::create_directories(linked_manifest_path.parent_path(), error);
  EXPECT(state, !error);
  const std::filesystem::path outside_manifest =
      manifest_link.path / "outside-resolution.json";
  std::ofstream(outside_manifest, std::ios::binary)
      << draft::serialize_resolution_manifest(manifest);
  std::filesystem::create_symlink(
      outside_manifest,
      linked_manifest_path,
      error);
  EXPECT(state, !error);
  draft::DiagnosticSink manifest_link_diagnostics;
  const draft::ResolutionManifestLoadResult linked_manifest =
      draft::load_resolution_manifest(
          manifest_link.path,
          store_key_for(manifest),
          manifest_link_diagnostics);
  EXPECT(state,
      linked_manifest.state == draft::ResolutionManifestLoadState::Invalid);
  EXPECT(state, manifest_link_diagnostics.has_errors());

  // Commit one coherent store, then replace its content object by a symlink.
  // Even identical target bytes are rejected because identity includes the
  // selected regular-file entry, not merely the path's eventual contents.
  TemporaryWorkspace object_link("object-link");
  draft::DiagnosticSink object_commit_diagnostics;
  EXPECT(state,
      draft::commit_resolution(
          object_link.path,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          object_commit_diagnostics));
  const std::filesystem::path object_path = object_link.path / ".draft" /
      "generated" / (expansion.digest.hex() + ".draft");
  const std::filesystem::path outside_object =
      object_link.path / "outside-expansion.draft";
  std::ofstream(outside_object, std::ios::binary) << expansion.source;
  std::filesystem::remove(object_path, error);
  EXPECT(state, !error);
  std::filesystem::create_symlink(outside_object, object_path, error);
  EXPECT(state, !error);
  std::string linked_source;
  draft::DiagnosticSink object_link_diagnostics;
  EXPECT(state,
      !draft::load_generated_expansion(
          object_link.path,
          expansion.digest,
          linked_source,
          object_link_diagnostics));
  EXPECT(state, object_link_diagnostics.has_errors());

  // Sparse files make the size gates cheap to test while still presenting the
  // exact oversized metadata a hostile workspace could create.
  TemporaryWorkspace oversized_manifest("oversized-manifest");
  const std::filesystem::path oversized_manifest_path =
      manifest_path_for(oversized_manifest.path, manifest);
  std::filesystem::create_directories(
      oversized_manifest_path.parent_path(), error);
  EXPECT(state, !error);
  {
    std::ofstream output(
        oversized_manifest_path,
        std::ios::binary | std::ios::trunc);
    output.seekp(static_cast<std::streamoff>(16U * 1024U * 1024U));
    output.put('x');
  }
  draft::DiagnosticSink oversized_manifest_diagnostics;
  const draft::ResolutionManifestLoadResult too_large_manifest =
      draft::load_resolution_manifest(
          oversized_manifest.path,
          store_key_for(manifest),
          oversized_manifest_diagnostics);
  EXPECT(state,
      too_large_manifest.state == draft::ResolutionManifestLoadState::Invalid);
  EXPECT(state, oversized_manifest_diagnostics.has_errors());

  TemporaryWorkspace oversized_object("oversized-object");
  draft::DiagnosticSink oversized_commit_diagnostics;
  EXPECT(state,
      draft::commit_resolution(
          oversized_object.path,
          store_key_for(manifest),
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          oversized_commit_diagnostics));
  const std::filesystem::path oversized_path = oversized_object.path /
      ".draft" / "generated" / (expansion.digest.hex() + ".draft");
  {
    std::ofstream output(
        oversized_path, std::ios::binary | std::ios::trunc);
    output.seekp(static_cast<std::streamoff>(64U * 1024U * 1024U));
    output.put('x');
  }
  std::string oversized_source;
  draft::DiagnosticSink oversized_object_diagnostics;
  EXPECT(state,
      !draft::load_generated_expansion(
          oversized_object.path,
          expansion.digest,
          oversized_source,
          oversized_object_diagnostics));
  EXPECT(state, oversized_object_diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_commit_and_reload(state);
  test_validation_precedes_writes(state);
  test_corrupt_object_is_rejected(state);
  test_root_and_target_namespaces_are_independent(state);
  test_interrupted_publish_recovery(state);
  test_injected_transaction_boundaries(state);
  test_concurrent_process_writers(state);
  test_store_rejects_redirected_and_unbounded_files(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolution-store expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolution store tests passed\n";
  return EXIT_SUCCESS;
}
