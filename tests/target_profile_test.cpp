// Internal consistency tests for the versioned AArch64 macOS target boundary.

#include "target/profile.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "target_profile_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_initial_profile(TestState &state) {
  const draft::TargetProfile profile = draft::make_aarch64_macos_profile();
  std::string reason;
  EXPECT(state, draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.empty());
  EXPECT(state, profile.facts.identity == "draft-aarch64-macos-v1");
  EXPECT(state, profile.facts.pointer_bits == 64);
  EXPECT(state, profile.facts.page_size == 16384);
  EXPECT(state, profile.llvm_triple == "arm64-apple-macosx14.0.0");
  EXPECT(state, profile.llvm_data_layout.find("m:o") != std::string::npos);
  EXPECT(state, profile.parsed_assembly_architecture == "aarch64");
  EXPECT(state, profile.assembly_files.size() == 3);
  EXPECT(state, draft::relocation_model_name(profile.relocation_model) == "pic");
  EXPECT(state, draft::code_model_name(profile.code_model) == "small");
  EXPECT(state, draft::tls_model_name(profile.tls_model) == "general-dynamic");
}

void test_invalid_profile_reports_reason(TestState &state) {
  draft::TargetProfile profile = draft::make_aarch64_macos_profile();
  profile.facts.page_size = 12000;
  std::string reason;
  EXPECT(state, !draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.find("power of two") != std::string::npos);

  profile = draft::make_aarch64_macos_profile();
  profile.facts.features.push_back("unknown");
  EXPECT(state, !draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.find("known feature") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_initial_profile(state);
  test_invalid_profile_reports_reason(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " target profile expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all target profile tests passed\n";
  return EXIT_SUCCESS;
}
