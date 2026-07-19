// Content pinning and relocation-safe verification of runtime asset roots.
//
// The driver and embedding API provide physical roots. This backend phase owns
// their filesystem validation, canonical content identity, duplicate handling,
// and exact comparison with manifest RuntimeAsset rows. It returns canonical
// physical paths only after equality is proved; it owns no lasting filesystem
// resource and performs no copying or process execution.
//
// The implementation depends downward on the generic content-tree and
// diagnostic modules and sideways on the manifest record definition. It does
// not depend on semantic packages, LLVM, or the native linker because runtime
// assets have no implicit source-language or link meaning.

#include "backend/runtime_assets.h"

#include "base/content_tree.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

// Manifest serialization uses kind then name as its canonical external-input
// order. Reusing that order here makes direct vector comparison deterministic.
[[nodiscard]] bool input_less(
    const ExternalInputPin &left, const ExternalInputPin &right) {
  if (left.kind != right.kind) {
    return static_cast<unsigned>(left.kind) <
        static_cast<unsigned>(right.kind);
  }
  return left.name < right.name;
}

[[nodiscard]] bool pins_equal(
    const ExternalInputPin &left, const ExternalInputPin &right) {
  return left.kind == right.kind && left.name == right.name &&
      left.content_digest == right.content_digest &&
      left.entry_point == right.entry_point;
}

// Checks only the physical root invariant owned by this module. The content-
// tree traversal performs the deeper entry and symlink validation while
// hashing. canonical is assigned only after the root itself is accepted.
[[nodiscard]] bool inspect_input(
    const RuntimeAssetInput &input,
    std::filesystem::path &canonical,
    DiagnosticSink &diagnostics) {
  if (input.name.empty()) {
    diagnostics.error(
        SourceRange::invalid(), "runtime asset name must not be empty");
    return false;
  }
  if (input.path.empty() || !input.path.is_absolute()) {
    diagnostics.error(
        SourceRange::invalid(),
        "runtime asset '" + input.name + "' path must be absolute");
    return false;
  }

  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(input.path, error);
  if (error || std::filesystem::is_symlink(status) ||
      (!std::filesystem::is_regular_file(status) &&
       !std::filesystem::is_directory(status))) {
    diagnostics.error(
        SourceRange::invalid(),
        "runtime asset '" + input.name +
            "' must be a real regular file or directory, not a symlink");
    return false;
  }

  canonical = std::filesystem::canonical(input.path, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize runtime asset '" + input.name + "': " +
            error.message());
    return false;
  }
  return true;
}

} // namespace

// Builds additions off to the side so an invalid mapping never partially
// appends the new runtime-asset set. Existing rows are retained, then the whole
// vector is sorted and checked because callers compose toolchain, provider, and
// asset pinning operations into one manifest candidate.
bool pin_runtime_asset_inputs(
    std::span<const RuntimeAssetInput> inputs,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  std::vector<ExternalInputPin> additions;
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    // Duplicate logical names are rejected before hashing the later path. One
    // program identity cannot leave deployment code to choose between roots.
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (inputs[previous].name == inputs[index].name) {
        diagnostics.error(
            SourceRange::invalid(),
            "runtime asset '" + inputs[index].name +
                "' is mapped more than once");
        return false;
      }
    }

    std::filesystem::path canonical;
    if (!inspect_input(inputs[index], canonical, diagnostics)) return false;
    ExternalInputPin pin;
    pin.kind = ExternalInputKind::RuntimeAsset;
    pin.name = inputs[index].name;
    if (!hash_content_tree(canonical, pin.content_digest, diagnostics)) {
      return false;
    }
    additions.push_back(std::move(pin));
  }

  std::vector<ExternalInputPin> result = pins;
  result.insert(
      result.end(),
      std::make_move_iterator(additions.begin()),
      std::make_move_iterator(additions.end()));
  std::sort(result.begin(), result.end(), input_less);
  for (std::size_t index = 1; index < result.size(); ++index) {
    if (result[index - 1].kind == result[index].kind &&
        result[index - 1].name == result[index].name) {
      diagnostics.error(
          SourceRange::invalid(),
          "external input key is duplicated while pinning runtime assets");
      return false;
    }
  }
  if (diagnostics.error_count() != initial_errors) return false;
  pins = std::move(result);
  return true;
}

// Reconstructs the actual portable rows from current roots, compares only the
// RuntimeAsset role against the manifest, then canonicalizes caller-order paths
// for the result. Re-inspection after hashing also ensures the returned path
// still names an accepted root shape before it is handed to deployment tooling.
bool verify_runtime_asset_inputs(
    std::span<const RuntimeAssetInput> inputs,
    std::span<const ExternalInputPin> manifest_pins,
    std::vector<VerifiedRuntimeAssetInput> &verified,
    DiagnosticSink &diagnostics) {
  std::vector<ExternalInputPin> actual;
  if (!pin_runtime_asset_inputs(inputs, actual, diagnostics)) return false;

  std::vector<ExternalInputPin> expected;
  for (const ExternalInputPin &pin : manifest_pins) {
    if (pin.kind == ExternalInputKind::RuntimeAsset) expected.push_back(pin);
  }
  std::sort(expected.begin(), expected.end(), input_less);
  if (actual.size() != expected.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "configured runtime assets do not match the complete resolved "
        "runtime-asset manifest set");
    return false;
  }
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (!pins_equal(actual[index], expected[index])) {
      diagnostics.error(
          SourceRange::invalid(),
          "runtime asset does not match the resolution manifest: '" +
              actual[index].name + "'");
      return false;
    }
  }

  std::vector<VerifiedRuntimeAssetInput> result;
  for (const RuntimeAssetInput &input : inputs) {
    std::filesystem::path canonical;
    if (!inspect_input(input, canonical, diagnostics)) return false;
    result.push_back({input.name, std::move(canonical)});
  }
  verified = std::move(result);
  return true;
}

} // namespace draft
