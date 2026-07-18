// Resolution-store commit, reload, validation, and no-partial-write tests.

#include "elaborator/resolution_store.h"

#include "base/sha256.h"
#include "elaborator/resolution.h"
#include "source/diagnostic.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

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
  std::filesystem::path path;

  explicit TemporaryWorkspace(std::string_view name) {
    std::error_code error;
    path = std::filesystem::temp_directory_path(error) /
        ("draft-resolution-store-test-" + std::string(name));
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::remove_all(path, error);
    error.clear();
    std::filesystem::create_directory(path, error);
    if (error) std::exit(EXIT_FAILURE);
  }

  ~TemporaryWorkspace() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
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
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics));
  EXPECT(state, !diagnostics.has_errors());

  const std::filesystem::path generated = workspace.path / ".draft" /
      "generated" / (expansion.digest.hex() + ".draft");
  EXPECT(state, std::filesystem::is_regular_file(generated));
  EXPECT(state,
      std::filesystem::is_regular_file(
          workspace.path / ".draft" / "resolution.json"));

  draft::DiagnosticSink load_diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.path, load_diagnostics);
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
          manifest,
          std::span<const draft::GeneratedExpansion>(),
          repin_diagnostics));
  EXPECT(state, !repin_diagnostics.has_errors());
  const draft::ResolutionManifestLoadResult repinned =
      draft::load_resolution_manifest(workspace.path, repin_diagnostics);
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
          manifest,
          std::span<const draft::GeneratedExpansion>(&expansion, 1),
          diagnostics));
  EXPECT(state, diagnostics.has_errors());
  EXPECT(state, !std::filesystem::exists(workspace.path / ".draft"));

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
      draft::load_resolution_manifest(workspace.path, interrupted_diagnostics);
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
          second_manifest,
          std::span<const draft::GeneratedExpansion>(),
          recovery_diagnostics));
  EXPECT(state, !recovery_diagnostics.has_errors());
  const draft::ResolutionManifestLoadResult recovered =
      draft::load_resolution_manifest(workspace.path, recovery_diagnostics);
  EXPECT(state, recovered.state == draft::ResolutionManifestLoadState::Loaded);
  if (recovered.manifest.pins.size() == 1) {
    EXPECT(state,
        recovered.manifest.pins[0].expansion_digest == second.digest);
  }
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
          redirected.path, redirected_load_diagnostics);
  EXPECT(state,
      redirected_load.state == draft::ResolutionManifestLoadState::Invalid);
  EXPECT(state, redirected_load_diagnostics.has_errors());
  draft::DiagnosticSink redirected_commit_diagnostics;
  EXPECT(state,
      !draft::commit_resolution(
          redirected.path,
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
  std::filesystem::create_directory(manifest_link.path / ".draft", error);
  EXPECT(state, !error);
  const std::filesystem::path outside_manifest =
      manifest_link.path / "outside-resolution.json";
  std::ofstream(outside_manifest, std::ios::binary)
      << draft::serialize_resolution_manifest(manifest);
  std::filesystem::create_symlink(
      outside_manifest,
      manifest_link.path / ".draft" / "resolution.json",
      error);
  EXPECT(state, !error);
  draft::DiagnosticSink manifest_link_diagnostics;
  const draft::ResolutionManifestLoadResult linked_manifest =
      draft::load_resolution_manifest(
          manifest_link.path, manifest_link_diagnostics);
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
  std::filesystem::create_directory(
      oversized_manifest.path / ".draft", error);
  EXPECT(state, !error);
  {
    std::ofstream output(
        oversized_manifest.path / ".draft" / "resolution.json",
        std::ios::binary | std::ios::trunc);
    output.seekp(static_cast<std::streamoff>(16U * 1024U * 1024U));
    output.put('x');
  }
  draft::DiagnosticSink oversized_manifest_diagnostics;
  const draft::ResolutionManifestLoadResult too_large_manifest =
      draft::load_resolution_manifest(
          oversized_manifest.path, oversized_manifest_diagnostics);
  EXPECT(state,
      too_large_manifest.state == draft::ResolutionManifestLoadState::Invalid);
  EXPECT(state, oversized_manifest_diagnostics.has_errors());

  TemporaryWorkspace oversized_object("oversized-object");
  draft::DiagnosticSink oversized_commit_diagnostics;
  EXPECT(state,
      draft::commit_resolution(
          oversized_object.path,
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
  test_interrupted_publish_recovery(state);
  test_store_rejects_redirected_and_unbounded_files(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolution-store expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolution store tests passed\n";
  return EXIT_SUCCESS;
}
