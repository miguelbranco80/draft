// Closed instrumentation vocabulary and fail-closed target selection.

#include "validation/instrumentation.h"

#include <algorithm>
#include <string>
#include <vector>

namespace draft {

std::string_view validation_instrumentation_name(
    ValidationInstrumentationKind kind) {
  switch (kind) {
  case ValidationInstrumentationKind::Address: return "address";
  case ValidationInstrumentationKind::Lifetime: return "lifetime";
  case ValidationInstrumentationKind::UndefinedOperation:
    return "undefined-operation";
  case ValidationInstrumentationKind::AllocatorPoisoning:
    return "allocator-poisoning";
  case ValidationInstrumentationKind::Race: return "race";
  }
  return "unknown";
}

std::optional<ValidationInstrumentationKind> parse_validation_instrumentation(
    std::string_view spelling) {
  if (spelling == "address") {
    return ValidationInstrumentationKind::Address;
  }
  if (spelling == "lifetime") {
    return ValidationInstrumentationKind::Lifetime;
  }
  if (spelling == "undefined-operation") {
    return ValidationInstrumentationKind::UndefinedOperation;
  }
  if (spelling == "allocator-poisoning") {
    return ValidationInstrumentationKind::AllocatorPoisoning;
  }
  if (spelling == "race") {
    return ValidationInstrumentationKind::Race;
  }
  return std::nullopt;
}

bool validate_validation_instrumentation(
    const TargetProfile &target,
    std::span<const ValidationInstrumentationKind> requirements,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  std::vector<ValidationInstrumentationKind> unique;
  unique.reserve(requirements.size());
  for (ValidationInstrumentationKind requirement : requirements) {
    if (std::find(unique.begin(), unique.end(), requirement) != unique.end()) {
      diagnostics.error(
          SourceRange::invalid(),
          "validation instrumentation '" +
              std::string(validation_instrumentation_name(requirement)) +
              "' is requested more than once");
      continue;
    }
    unique.push_back(requirement);
  }

  // draft-aarch64-macos-v5 deliberately owns no diagnostic-instrumentation
  // runtime. Each unique request gets its own exact diagnostic so a profile
  // author can see the complete unavailable set in one command invocation.
  for (ValidationInstrumentationKind requirement : unique) {
    diagnostics.error(
        SourceRange::invalid(),
        "validation instrumentation '" +
            std::string(validation_instrumentation_name(requirement)) +
            "' is unavailable for target '" + target.facts.identity +
            "': no versioned compiler pass, runtime, and evidence contract is "
            "configured");
  }
  return diagnostics.error_count() == initial_errors;
}

} // namespace draft
