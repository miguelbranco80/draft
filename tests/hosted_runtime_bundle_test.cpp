// Compiler-distributed hosted runtime bundle contract tests.
//
// The production compiler borrows these embedded bytes directly while
// planning a native artifact. These tests deliberately inspect the public
// bundle rather than the CMake scratch directory: they prove that the linked
// compiler distribution owns exactly one nonempty object and assembly image
// for every versioned target, that exact lookup is deterministic, and that the
// images have the target's expected container format and runtime entry points.
// The tests neither execute cross-target objects nor depend on host tools.

#include "backend/hosted_runtime_bundle.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

namespace {

struct TestState {
  int failures = 0;
};

#define EXPECT(state, condition)                                                \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << __FILE__ << ':' << __LINE__                                  \
                << ": expectation failed: " #condition << '\n';                \
      ++(state).failures;                                                       \
    }                                                                           \
  } while (false)

// Compares object-file magic as unsigned bytes. std::string_view is the right
// ownership type for the embedded binary, but plain char signedness is a host
// implementation detail and must not affect this target-format assertion.
template <std::size_t Size>
bool begins_with(
    std::string_view bytes,
    const std::array<unsigned char, Size> &prefix) {
  if (bytes.size() < prefix.size()) return false;
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (static_cast<unsigned char>(bytes[index]) != prefix[index]) return false;
  }
  return true;
}

// Every hosted target must expose the complete root-runtime surface used by
// direct package LLVM units. Searching assembly is an independent, readable
// symbol oracle which works for Mach-O, ELF, and COFF without adding an object
// parser merely for this bundle test.
void expect_runtime_symbols(
    TestState &state,
    const draft::EmbeddedHostedRuntimeObject &runtime) {
  constexpr std::array<std::string_view, 14> required_symbols{
      "__draft.assert",
      "__draft.bounds",
      "__draft.slice_bounds",
      "__draft.runtime.attach_thread",
      "__draft.runtime.install_thread_context",
      "__draft.runtime.initialize_process",
      "__draft.runtime.shutdown_process",
      "__draft.runtime.default_context",
      "__draft.runtime.reset_temporary_allocator",
      "__draft.runtime.validation_report",
      "__draft.os.args_data",
      "__draft.os.args_count",
      "__draft.os.environment_data",
      "__draft.os.environment_count",
  };
  for (std::string_view symbol : required_symbols) {
    EXPECT(state, runtime.assembly_bytes.find(symbol) != std::string_view::npos);
  }
}

void test_complete_canonical_bundle(TestState &state) {
  constexpr std::array<std::string_view, 4> expected_identities{
      "draft-aarch64-macos-v6",
      "draft-aarch64-linux-gnu-v2",
      "draft-x86_64-linux-gnu-v2",
      "draft-x86_64-windows-msvc-v2",
  };
  const std::span<const draft::EmbeddedHostedRuntimeObject> runtimes =
      draft::embedded_hosted_runtime_objects();
  EXPECT(state, runtimes.size() == expected_identities.size());
  if (runtimes.size() != expected_identities.size()) return;

  for (std::size_t index = 0; index < runtimes.size(); ++index) {
    const draft::EmbeddedHostedRuntimeObject &runtime = runtimes[index];
    EXPECT(state, runtime.target_identity == expected_identities[index]);
    EXPECT(state, !runtime.object_bytes.empty());
    EXPECT(state, !runtime.assembly_bytes.empty());
    EXPECT(state,
        draft::embedded_hosted_runtime_object(runtime.target_identity) ==
            &runtime);
    expect_runtime_symbols(state, runtime);
  }

  EXPECT(state, begins_with(
      runtimes[0].object_bytes,
      std::array<unsigned char, 4>{0xcf, 0xfa, 0xed, 0xfe}));
  for (std::size_t index : {std::size_t{1}, std::size_t{2}}) {
    EXPECT(state, begins_with(
        runtimes[index].object_bytes,
        std::array<unsigned char, 4>{0x7f, 'E', 'L', 'F'}));
  }
  EXPECT(state, begins_with(
      runtimes[3].object_bytes,
      std::array<unsigned char, 2>{0x64, 0x86}));
  EXPECT(state,
      draft::embedded_hosted_runtime_object("draft-missing-target-v1") ==
          nullptr);
}

} // namespace

int main() {
  TestState state;
  test_complete_canonical_bundle(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " hosted runtime bundle expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all hosted runtime bundle tests passed\n";
  return EXIT_SUCCESS;
}
