// Operational path validation and semantic summaries for foreign providers.

#include "backend/foreign_inputs.h"
#include "backend/foreign_summaries.h"

#include "test_directory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
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

void test_paths_and_summaries_are_not_content_pins(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-foreign-input-test"};
  const std::filesystem::path &temporary = temporary_directory.path();
  std::string parse_reason;
  draft::ForeignProviderInput parsed_provider;
  EXPECT(state,
      draft::parse_foreign_provider_input(
          "custom_math=object:provider.o", parsed_provider, parse_reason));
  EXPECT(state, parsed_provider.provider == "custom_math");
  EXPECT(state, parsed_provider.kind == draft::ForeignArtifactKind::Object);
  EXPECT(state, parsed_provider.path.is_absolute());
  EXPECT(state, !draft::parse_foreign_provider_input(
      "custom_math:provider.o", parsed_provider, parse_reason));
  draft::ForeignProviderSummaryInput parsed_summary;
  EXPECT(state,
      draft::parse_foreign_provider_summary_input(
          "custom_math:provider.summary", parsed_summary, parse_reason));
  EXPECT(state, parsed_summary.provider == "custom_math");
  EXPECT(state, parsed_summary.path.is_absolute());
  EXPECT(state, !draft::parse_foreign_provider_summary_input(
      "provider.summary", parsed_summary, parse_reason));
  std::error_code error;
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
  std::vector<draft::VerifiedForeignProviderInput> verified;
  draft::DiagnosticSink inspect_diagnostics;
  EXPECT(state, draft::inspect_foreign_provider_inputs(
      std::span<const draft::ForeignProviderInput>(&input, 1),
      verified,
      inspect_diagnostics));
  EXPECT(state, !inspect_diagnostics.has_errors());
  EXPECT(state, verified.size() == 1);
  if (verified.size() == 1) {
    EXPECT(state, verified[0].provider == "custom_math");
    EXPECT(state, verified[0].kind == draft::ForeignArtifactKind::Object);
    EXPECT(state, verified[0].path == std::filesystem::canonical(first));
  }

  const std::filesystem::path first_summary =
      temporary / "first" / "provider.summary";
  const std::filesystem::path relocated_summary =
      temporary / "relocated" / "renamed.summary";
  const std::string summary_bytes =
      "draft-provider-denial-summary-v2\n"
      "provider\tcustom_math\n"
      "symbol\tdraft_custom_math\n"
      "callback\t0\tstate\tprocedure\n"
      "effect\tassembly\n"
      "end\n";
  std::ofstream(first_summary, std::ios::binary) << summary_bytes;
  std::ofstream(relocated_summary, std::ios::binary) << summary_bytes;
  draft::ForeignProviderSummaryInput summary_input;
  summary_input.provider = "custom_math";
  summary_input.path = first_summary;
  std::vector<draft::ForeignProviderAudit> audits;
  draft::DiagnosticSink summary_diagnostics;
  EXPECT(state, draft::load_foreign_provider_summaries(
      std::span<const draft::ForeignProviderSummaryInput>(&summary_input, 1),
      std::span<const draft::ForeignProviderInput>(&input, 1),
      audits,
      summary_diagnostics));
  EXPECT(state, !summary_diagnostics.has_errors());
  EXPECT(state, audits.size() == 1);
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
          (audits[0].symbols[0].effects[0].flow_path ==
              std::vector<std::string>{"state", "procedure"}));
      EXPECT(state,
          audits[0].symbols[0].effects[1].kind ==
              draft::EffectKind::Assembly);
    }
  }

  summary_input.path = relocated_summary;
  draft::DiagnosticSink relocated_summary_diagnostics;
  EXPECT(state, draft::load_foreign_provider_summaries(
      std::span<const draft::ForeignProviderSummaryInput>(&summary_input, 1),
      std::span<const draft::ForeignProviderInput>(&input, 1),
      audits,
      relocated_summary_diagnostics));
  EXPECT(state, !relocated_summary_diagnostics.has_errors());

  // A summary remains an explicit semantic input and therefore requires a
  // provider mapping by the same logical name. It does not pretend to
  // authenticate the implementation bytes behind that mapping.
  draft::DiagnosticSink missing_provider_diagnostics;
  EXPECT(state, !draft::load_foreign_provider_summaries(
      std::span<const draft::ForeignProviderSummaryInput>(&summary_input, 1),
      {},
      audits,
      missing_provider_diagnostics));
  EXPECT(state, missing_provider_diagnostics.has_errors());

  // Relocation and byte replacement are linker-environment changes, not
  // Draft source identity. Path shape is checked again, but no digest is
  // compared with a resolution manifest.
  input.path = relocated;
  draft::DiagnosticSink verify_diagnostics;
  EXPECT(state, draft::inspect_foreign_provider_inputs(
      std::span<const draft::ForeignProviderInput>(&input, 1),
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
  draft::DiagnosticSink changed_diagnostics;
  EXPECT(state, draft::inspect_foreign_provider_inputs(
      std::span<const draft::ForeignProviderInput>(&input, 1),
      verified,
      changed_diagnostics));
  EXPECT(state, !changed_diagnostics.has_errors());

  std::vector<draft::ForeignProviderInput> duplicate{input, input};
  draft::DiagnosticSink duplicate_diagnostics;
  EXPECT(state, !draft::inspect_foreign_provider_inputs(
      duplicate, verified, duplicate_diagnostics));
  EXPECT(state, duplicate_diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_paths_and_summaries_are_not_content_pins(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " foreign input expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all foreign input tests passed\n";
  return EXIT_SUCCESS;
}
