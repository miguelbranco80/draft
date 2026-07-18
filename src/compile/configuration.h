// Build-time language choices that can change emitted program behavior.
//
// These values are separate from the target profile and from native toolchain
// selection. They affect target-independent lowering, so they are also hashed
// into resolved-program identity. Keeping the surface small and explicit makes
// it difficult for a command-line convenience flag to become an untracked
// semantic input.

#pragma once

#include <string_view>

namespace draft {

enum class RuntimeAssertionMode {
  On,
  Off,
};

[[nodiscard]] constexpr std::string_view runtime_assertion_mode_name(
    RuntimeAssertionMode mode) {
  switch (mode) {
  case RuntimeAssertionMode::On: return "on";
  case RuntimeAssertionMode::Off: return "off";
  }
  return "invalid";
}

struct CompileConfiguration {
  RuntimeAssertionMode runtime_assertions = RuntimeAssertionMode::On;
};

} // namespace draft
