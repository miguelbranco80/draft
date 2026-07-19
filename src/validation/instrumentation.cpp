// Closed instrumentation vocabulary and fail-closed target selection.

#include "validation/instrumentation.h"

#include <algorithm>
#include <string>
#include <vector>

namespace draft {

namespace {

// Instrumentation support belongs to a complete target contract, not merely
// to a tool name accepted by the selected Clang binary. If any target fact
// changes, its identity changes and this profile stays unavailable until that
// new target has independently qualified the host pass and runtime behavior.
constexpr std::string_view kAddressSanitizerTargetIdentity =
    "draft-aarch64-macos-v5";

} // namespace

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

  // AddressSanitizer is the first complete target profile: the native adapter
  // owns the Clang options, and validation evidence records the selected host
  // toolchain version. The remaining vocabulary stays explicit and fail-closed
  // until each item has an equally complete contract.
  for (ValidationInstrumentationKind requirement : unique) {
    if (requirement == ValidationInstrumentationKind::Address &&
        target.facts.identity == kAddressSanitizerTargetIdentity) {
      continue;
    }
    diagnostics.error(
        SourceRange::invalid(),
        "validation instrumentation '" +
            std::string(validation_instrumentation_name(requirement)) +
            "' is unavailable for target '" + target.facts.identity +
            "': no qualified compiler pass, runtime, and evidence contract is "
            "configured");
  }
  return diagnostics.error_count() == initial_errors;
}

std::string validation_instrumentation_identity(
    std::span<const ValidationInstrumentationKind> requirements) {
  if (requirements.empty()) {
    return "draft-validation-instrumentation-v1:none";
  }
  if (requirements.size() == 1 &&
      requirements.front() == ValidationInstrumentationKind::Address) {
    // Toolchain identity separately records the selected host Clang version.
    // This field names the compiler-controlled flags and execution policy.
    return "draft-validation-instrumentation-v1:address;"
        "ir-function-attribute=sanitize_address;"
        "compile=-fsanitize=address,-fno-omit-frame-pointer;"
        "link=-fsanitize=address;"
        "runtime=host-clang-address-sanitizer;"
        "runtime-options=abort_on_error=1,symbolize=0;"
        "process-environment=draft-validation-process-environment-v1";
  }
  return "draft-validation-instrumentation-v1:invalid";
}

} // namespace draft
