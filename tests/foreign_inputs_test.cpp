// Exact content pinning for logical foreign link providers.

#include "backend/foreign_inputs.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct TestState {
  int failures = 0;
  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "foreign_inputs_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_pin_verify_and_relocation(TestState &state) {
  std::error_code error;
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path(error) /
      "draft-foreign-input-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(temporary, error);
  error.clear();
  std::filesystem::create_directories(temporary / "first", error);
  EXPECT(state, !error);
  std::filesystem::create_directories(temporary / "relocated", error);
  EXPECT(state, !error);
  if (error) return;

  const std::filesystem::path first = temporary / "first" / "provider.o";
  const std::filesystem::path relocated =
      temporary / "relocated" / "renamed.o";
  std::ofstream(first, std::ios::binary) << "exact object bytes\n";
  std::ofstream(relocated, std::ios::binary) << "exact object bytes\n";

  draft::ForeignProviderInput input;
  input.provider = "custom_math";
  input.kind = draft::ForeignArtifactKind::Object;
  input.path = first;
  std::vector<draft::ExternalInputPin> pins;
  draft::DiagnosticSink pin_diagnostics;
  EXPECT(state, draft::pin_foreign_provider_inputs(
      std::span<const draft::ForeignProviderInput>(&input, 1),
      pins,
      pin_diagnostics));
  EXPECT(state, !pin_diagnostics.has_errors());
  EXPECT(state, pins.size() == 1);
  if (pins.size() == 1) {
    EXPECT(state, pins[0].kind == draft::ExternalInputKind::Object);
    EXPECT(state, pins[0].name == "custom_math");
    EXPECT(state, pins[0].entry_point.empty());
  }

  input.path = relocated;
  std::vector<draft::VerifiedForeignProviderInput> verified;
  draft::DiagnosticSink verify_diagnostics;
  EXPECT(state, draft::verify_foreign_provider_inputs(
      std::span<const draft::ForeignProviderInput>(&input, 1),
      pins,
      verified,
      verify_diagnostics));
  EXPECT(state, !verify_diagnostics.has_errors());
  EXPECT(state, verified.size() == 1);
  if (verified.size() == 1) {
    EXPECT(state, verified[0].path ==
        std::filesystem::canonical(relocated, error));
  }

  std::ofstream(relocated, std::ios::binary | std::ios::trunc)
      << "changed object bytes\n";
  draft::DiagnosticSink stale_diagnostics;
  EXPECT(state, !draft::verify_foreign_provider_inputs(
      std::span<const draft::ForeignProviderInput>(&input, 1),
      pins,
      verified,
      stale_diagnostics));
  EXPECT(state, stale_diagnostics.has_errors());

  std::vector<draft::ForeignProviderInput> duplicate{input, input};
  draft::DiagnosticSink duplicate_diagnostics;
  EXPECT(state, !draft::pin_foreign_provider_inputs(
      duplicate, pins, duplicate_diagnostics));
  EXPECT(state, duplicate_diagnostics.has_errors());
  std::filesystem::remove_all(temporary, error);
}

} // namespace

int main() {
  TestState state;
  test_pin_verify_and_relocation(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " foreign input expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all foreign input tests passed\n";
  return EXIT_SUCCESS;
}
