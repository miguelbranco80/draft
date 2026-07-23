// Immutable target runtime objects embedded in the compiler distribution.
//
// CMake cross-compiles runtime/hosted_runtime.c once for every built-in target
// and generates the table implementing this interface. The returned bytes are
// ordinary relocatable object input owned for the process lifetime. Artifact
// planning borrows them directly; no Draft compilation command reads, hashes,
// regenerates, or caches a runtime file.

#pragma once

#include <span>
#include <string_view>

namespace draft {

// One compiler-distributed runtime object. target_identity is the complete
// versioned TargetFacts identity rather than an architecture nickname, so a
// later ABI/profile revision cannot silently reuse an older runtime contract.
struct EmbeddedHostedRuntimeObject {
  std::string_view target_identity;
  std::string_view object_bytes;
  std::string_view assembly_bytes;
};

// Returns the complete canonical bundle in target-identity order.
[[nodiscard]] std::span<const EmbeddedHostedRuntimeObject>
embedded_hosted_runtime_objects();

// Returns null when no exact versioned target object exists.
[[nodiscard]] const EmbeddedHostedRuntimeObject *
embedded_hosted_runtime_object(std::string_view target_identity);

} // namespace draft
