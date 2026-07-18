// Exact content pinning for logical foreign link providers.

#include "backend/foreign_inputs.h"
#include "backend/foreign_summaries.h"

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
  if (pins.empty()) {
    std::filesystem::remove_all(temporary, error);
    return;
  }

  const std::filesystem::path first_summary =
      temporary / "first" / "provider.summary";
  const std::filesystem::path relocated_summary =
      temporary / "relocated" / "renamed.summary";
  const std::string summary_bytes =
      "draft-provider-denial-summary-v1\n"
      "provider\tcustom_math\n"
      "artifact\t" + pins.front().content_digest.hex() + "\n"
      "symbol\tdraft_custom_math\n"
      "callback\t0\n"
      "effect\tassembly\n"
      "end\n";
  std::ofstream(first_summary, std::ios::binary) << summary_bytes;
  std::ofstream(relocated_summary, std::ios::binary) << summary_bytes;
  draft::ForeignProviderSummaryInput summary_input;
  summary_input.provider = "custom_math";
  summary_input.path = first_summary;
  std::vector<draft::ExternalInputPin> summary_pins;
  std::vector<draft::ForeignProviderAudit> audits;
  draft::DiagnosticSink summary_diagnostics;
  EXPECT(state, draft::pin_foreign_provider_summary_inputs(
      std::span<const draft::ForeignProviderSummaryInput>(&summary_input, 1),
      std::span<const draft::ForeignProviderInput>(&input, 1),
      summary_pins,
      audits,
      summary_diagnostics));
  EXPECT(state, !summary_diagnostics.has_errors());
  EXPECT(state, summary_pins.size() == 1);
  EXPECT(state, audits.size() == 1);
  if (summary_pins.size() == 1) {
    EXPECT(state,
        summary_pins[0].kind == draft::ExternalInputKind::ProviderSummary);
    EXPECT(state, summary_pins[0].name == "custom_math");
  }
  if (audits.size() == 1) {
    EXPECT(state, audits[0].provider == "custom_math");
    EXPECT(state, audits[0].symbols.size() == 1);
    if (audits[0].symbols.size() == 1) {
      EXPECT(state, audits[0].symbols[0].effects.size() == 2);
      EXPECT(state,
          audits[0].symbols[0].effects[0].kind ==
              draft::EffectKind::FlowCall);
      EXPECT(state, audits[0].symbols[0].effects[0].flow_parameter == 0);
      EXPECT(state,
          audits[0].symbols[0].effects[1].kind ==
              draft::EffectKind::Assembly);
    }
  }

  summary_input.path = relocated_summary;
  draft::DiagnosticSink relocated_summary_diagnostics;
  EXPECT(state, draft::verify_foreign_provider_summary_inputs(
      std::span<const draft::ForeignProviderSummaryInput>(&summary_input, 1),
      std::span<const draft::ForeignProviderInput>(&input, 1),
      summary_pins,
      audits,
      relocated_summary_diagnostics));
  EXPECT(state, !relocated_summary_diagnostics.has_errors());

  // A manifest claim cannot survive after the caller omits the physical
  // summary and its artifact. This is the fail-closed path used by ordinary
  // builds and by resolution runs that would otherwise preserve old pins.
  draft::DiagnosticSink missing_summary_diagnostics;
  EXPECT(state, !draft::verify_foreign_provider_summary_inputs(
      {}, {}, summary_pins, audits, missing_summary_diagnostics));
  EXPECT(state, missing_summary_diagnostics.has_errors());

  // Summary semantics are inseparable from the artifact digest written into
  // the summary itself, even if both files are supplied under matching names.
  const std::filesystem::path mismatched_summary =
      temporary / "first" / "mismatched.summary";
  const std::string mismatched_bytes =
      "draft-provider-denial-summary-v1\n"
      "provider\tcustom_math\n"
      "artifact\t" + std::string(64, '0') + "\n"
      "symbol\tdraft_custom_math\n"
      "end\n";
  std::ofstream(mismatched_summary, std::ios::binary) << mismatched_bytes;
  summary_input.path = mismatched_summary;
  std::vector<draft::ExternalInputPin> rejected_pins;
  draft::DiagnosticSink mismatch_diagnostics;
  EXPECT(state, !draft::pin_foreign_provider_summary_inputs(
      std::span<const draft::ForeignProviderSummaryInput>(&summary_input, 1),
      std::span<const draft::ForeignProviderInput>(&input, 1),
      rejected_pins,
      audits,
      mismatch_diagnostics));
  EXPECT(state, mismatch_diagnostics.has_errors());
  EXPECT(state, rejected_pins.empty());

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
