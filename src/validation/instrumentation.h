// Typed validation-instrumentation requirements and target availability.
//
// Instrumentation is validation policy, not Draft language semantics. Keeping
// requests in this small module prevents CLI strings or host compiler flags
// from leaking into semantic checking. A target adapter may support a request
// only after its compiler pass, runtime, options, and tool identities are all
// explicit evidence inputs.

#pragma once

#include "source/diagnostic.h"
#include "target/profile.h"

#include <optional>
#include <span>
#include <string_view>

namespace draft {

enum class ValidationInstrumentationKind {
  Address,
  Lifetime,
  UndefinedOperation,
  AllocatorPoisoning,
  Race,
};

[[nodiscard]] std::string_view validation_instrumentation_name(
    ValidationInstrumentationKind kind);

// Parses only the stable public vocabulary. Options are intentionally absent
// until one supported instrument has a versioned option schema.
[[nodiscard]] std::optional<ValidationInstrumentationKind>
parse_validation_instrumentation(std::string_view spelling);

// Rejects duplicates and any requirement unavailable for the complete target
// profile. An empty set succeeds and represents the existing uninstrumented
// validation policy. The first target currently supports no diagnostic
// instrument; that explicit answer is safer than forwarding ambient Clang
// sanitizer flags whose passes and runtimes are not pinned.
[[nodiscard]] bool validate_validation_instrumentation(
    const TargetProfile &target,
    std::span<const ValidationInstrumentationKind> requirements,
    DiagnosticSink &diagnostics);

} // namespace draft
