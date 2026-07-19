// Content pinning and relocation-safe verification of foreign provider files.

#include "backend/foreign_inputs.h"

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

[[nodiscard]] ExternalInputKind external_kind(ForeignArtifactKind kind) {
  switch (kind) {
  case ForeignArtifactKind::Object: return ExternalInputKind::Object;
  case ForeignArtifactKind::Archive: return ExternalInputKind::Archive;
  case ForeignArtifactKind::SharedLibrary:
    return ExternalInputKind::SharedLibrary;
  }
  return ExternalInputKind::ForeignArtifact;
}

[[nodiscard]] bool foreign_kind(
    ExternalInputKind kind, ForeignArtifactKind &result) {
  if (kind == ExternalInputKind::Object) {
    result = ForeignArtifactKind::Object;
    return true;
  }
  if (kind == ExternalInputKind::Archive) {
    result = ForeignArtifactKind::Archive;
    return true;
  }
  if (kind == ExternalInputKind::SharedLibrary) {
    result = ForeignArtifactKind::SharedLibrary;
    return true;
  }
  return false;
}

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

[[nodiscard]] bool inspect_input(
    const ForeignProviderInput &input,
    std::filesystem::path &canonical,
    DiagnosticSink &diagnostics) {
  if (input.provider.empty()) {
    diagnostics.error(
        SourceRange::invalid(), "foreign provider identity must not be empty");
    return false;
  }
  if (input.path.empty() || !input.path.is_absolute()) {
    diagnostics.error(
        SourceRange::invalid(),
        "foreign provider '" + input.provider +
            "' artifact path must be absolute");
    return false;
  }
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(input.path, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    diagnostics.error(
        SourceRange::invalid(),
        "foreign provider '" + input.provider +
            "' artifact must be a real regular file, not a symlink");
    return false;
  }
  canonical = std::filesystem::canonical(input.path, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize foreign provider '" + input.provider +
            "' artifact: " + error.message());
    return false;
  }
  return true;
}

} // namespace

std::string_view foreign_artifact_kind_name(ForeignArtifactKind kind) {
  switch (kind) {
  case ForeignArtifactKind::Object: return "object";
  case ForeignArtifactKind::Archive: return "archive";
  case ForeignArtifactKind::SharedLibrary: return "shared-library";
  }
  return "unknown";
}

bool pin_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  std::vector<ExternalInputPin> additions;
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (inputs[previous].provider == inputs[index].provider) {
        diagnostics.error(
            SourceRange::invalid(),
            "foreign provider '" + inputs[index].provider +
                "' is mapped more than once");
        return false;
      }
    }
    std::filesystem::path canonical;
    if (!inspect_input(inputs[index], canonical, diagnostics)) return false;
    ExternalInputPin pin;
    pin.kind = external_kind(inputs[index].kind);
    pin.name = inputs[index].provider;
    if (!hash_content_tree(canonical, pin.content_digest, diagnostics)) {
      return false;
    }
    additions.push_back(std::move(pin));
  }
  pins.insert(
      pins.end(),
      std::make_move_iterator(additions.begin()),
      std::make_move_iterator(additions.end()));
  std::sort(pins.begin(), pins.end(), input_less);
  for (std::size_t index = 1; index < pins.size(); ++index) {
    if (pins[index - 1].kind == pins[index].kind &&
        pins[index - 1].name == pins[index].name) {
      diagnostics.error(
          SourceRange::invalid(),
          "external input key is duplicated while pinning foreign providers");
      return false;
    }
  }
  return diagnostics.error_count() == initial_errors;
}

bool verify_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::span<const ExternalInputPin> manifest_pins,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics) {
  std::vector<ExternalInputPin> actual;
  if (!pin_foreign_provider_inputs(inputs, actual, diagnostics)) return false;
  std::vector<ExternalInputPin> expected;
  for (const ExternalInputPin &pin : manifest_pins) {
    ForeignArtifactKind ignored;
    if (foreign_kind(pin.kind, ignored)) expected.push_back(pin);
  }
  std::sort(expected.begin(), expected.end(), input_less);
  if (actual.size() != expected.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "configured foreign providers do not match the complete resolved "
        "object/archive/shared-library manifest set");
    return false;
  }
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (!pins_equal(actual[index], expected[index])) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider artifact does not match the resolution manifest: '" +
              actual[index].name + "'");
      return false;
    }
  }

  std::vector<VerifiedForeignProviderInput> result;
  for (const ForeignProviderInput &input : inputs) {
    std::filesystem::path canonical;
    if (!inspect_input(input, canonical, diagnostics)) return false;
    result.push_back({input.provider, input.kind, std::move(canonical)});
  }
  verified = std::move(result);
  return true;
}

bool inspect_foreign_provider_inputs(
    std::span<const ForeignProviderInput> inputs,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics) {
  std::vector<ExternalInputPin> ignored;
  if (!pin_foreign_provider_inputs(inputs, ignored, diagnostics)) return false;
  std::vector<VerifiedForeignProviderInput> result;
  for (const ForeignProviderInput &input : inputs) {
    std::filesystem::path canonical;
    if (!inspect_input(input, canonical, diagnostics)) return false;
    result.push_back({input.provider, input.kind, std::move(canonical)});
  }
  verified = std::move(result);
  return true;
}

} // namespace draft
