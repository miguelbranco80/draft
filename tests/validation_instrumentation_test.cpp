// Validation-instrument vocabulary and fail-closed availability tests.

#include "source/diagnostic.h"
#include "target/profile.h"
#include "validation/instrumentation.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "validation_instrumentation_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_closed_vocabulary(TestState &state) {
  using Kind = draft::ValidationInstrumentationKind;
  constexpr std::array<std::pair<std::string_view, Kind>, 5> expected{{
      {"address", Kind::Address},
      {"lifetime", Kind::Lifetime},
      {"undefined-operation", Kind::UndefinedOperation},
      {"allocator-poisoning", Kind::AllocatorPoisoning},
      {"race", Kind::Race},
  }};
  for (const auto &[spelling, kind] : expected) {
    const std::optional<Kind> parsed =
        draft::parse_validation_instrumentation(spelling);
    EXPECT(state, parsed.has_value());
    if (parsed.has_value()) EXPECT(state, *parsed == kind);
    EXPECT(state, draft::validation_instrumentation_name(kind) == spelling);
  }
  EXPECT(state, !draft::parse_validation_instrumentation("asan").has_value());
  EXPECT(state, !draft::parse_validation_instrumentation("").has_value());
}

void test_target_availability_is_explicit(TestState &state) {
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::DiagnosticSink empty_diagnostics;
  EXPECT(state, draft::validate_validation_instrumentation(
      target, {}, empty_diagnostics));
  EXPECT(state, !empty_diagnostics.has_errors());
  EXPECT(state, draft::validation_instrumentation_identity({}) ==
      "draft-validation-instrumentation-v1:none");

  const std::array requested{
      draft::ValidationInstrumentationKind::Address,
      draft::ValidationInstrumentationKind::Race,
      draft::ValidationInstrumentationKind::Address,
  };
  draft::DiagnosticSink diagnostics;
  EXPECT(state, !draft::validate_validation_instrumentation(
      target, requested, diagnostics));
  EXPECT(state, diagnostics.error_count() == 2);
  std::string combined;
  for (const draft::Diagnostic &diagnostic : diagnostics.diagnostics()) {
    combined += diagnostic.message;
    combined.push_back('\n');
  }
  EXPECT(state, combined.find("'address' is requested more than once") !=
      std::string::npos);
  EXPECT(state, combined.find("'address' is unavailable for target '") ==
      std::string::npos);
  EXPECT(state, combined.find("'race' is unavailable for target '") !=
      std::string::npos);
  EXPECT(state, combined.find("versioned compiler pass, runtime, and evidence") !=
      std::string::npos);

  const std::array address{
      draft::ValidationInstrumentationKind::Address};
  draft::DiagnosticSink address_diagnostics;
  EXPECT(state, draft::validate_validation_instrumentation(
      target, address, address_diagnostics));
  EXPECT(state, !address_diagnostics.has_errors());
  EXPECT(state, draft::validation_instrumentation_identity(address).find(
      "compile=-fsanitize=address,-fno-omit-frame-pointer") !=
      std::string::npos);
  EXPECT(state, draft::validation_instrumentation_identity(address).find(
      "ir-function-attribute=sanitize_address") != std::string::npos);

  // A future target cannot silently inherit this profile just because it also
  // happens to invoke a Clang with an option named -fsanitize=address.
  draft::TargetProfile unqualified_target = target;
  unqualified_target.facts.identity = "draft-unqualified-target-v1";
  draft::DiagnosticSink unqualified_diagnostics;
  EXPECT(state, !draft::validate_validation_instrumentation(
      unqualified_target, address, unqualified_diagnostics));
  EXPECT(state, unqualified_diagnostics.error_count() == 1);
  if (!unqualified_diagnostics.diagnostics().empty()) {
    EXPECT(state,
        unqualified_diagnostics.diagnostics().front().message.find(
            "'address' is unavailable for target "
            "'draft-unqualified-target-v1'") != std::string::npos);
  }
}

} // namespace

int main() {
  TestState state;
  test_closed_vocabulary(state);
  test_target_availability_is_explicit(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
