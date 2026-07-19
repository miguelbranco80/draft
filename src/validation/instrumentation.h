// Typed validation-instrumentation requirements and target availability.
//
// Instrumentation is validation policy, not Draft language semantics. Keeping
// requests in this small module prevents CLI strings or host compiler flags
// from leaking into semantic checking. A target adapter may support a request
// only after its compiler pass, runtime behavior, and options form a tested
// host contract. Validation evidence records the selected host tool version;
// instrumentation does not become part of resolved-program identity.

#pragma once

#include "source/diagnostic.h"
#include "target/profile.h"

#include <optional>
#include <span>
#include <string>
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
// validation policy. The first target supports only the address profile; every
// other vocabulary item remains fail-closed.
[[nodiscard]] bool validate_validation_instrumentation(
    const TargetProfile &target,
    std::span<const ValidationInstrumentationKind> requirements,
    DiagnosticSink &diagnostics);

// Returns the exact compiler-owned profile identity placed in validation
// evidence. Callers first validate the request; unsupported combinations never
// receive an identity and therefore cannot alias an uninstrumented claim.
[[nodiscard]] std::string validation_instrumentation_identity(
    std::span<const ValidationInstrumentationKind> requirements);

} // namespace draft
