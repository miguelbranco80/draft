// Internal consistency tests for the versioned AArch64 target boundaries.

#include "sema/target_validation.h"
#include "target/profile.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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
  EXPECT(state, profile.facts.identity == "draft-aarch64-macos-v5");
  EXPECT(state, profile.facts.pointer_bits == 64);
  EXPECT(state, profile.facts.page_size == 16384);
  EXPECT(state, profile.facts.simd_shapes.size() == 19);
  EXPECT(state, profile.facts.simd_shapes.front() ==
      draft::TargetSimdShape({"f16", 4}));
  EXPECT(state, profile.facts.simd_shapes.back() ==
      draft::TargetSimdShape({"u8", 16}));
  EXPECT(state, profile.llvm_triple == "arm64-apple-macosx14.0.0");
  EXPECT(state, profile.llvm_data_layout.find("m:o") != std::string::npos);
  EXPECT(state, profile.parsed_assembly_architecture == "aarch64");
  EXPECT(state, profile.parsed_assembly_dialect == "draft-aarch64-apple-v2");
  EXPECT(state, profile.parsed_assembly_instructions.size() == 82);
  EXPECT(state, profile.system_link_library == "System");
  EXPECT(state, profile.system_link_providers.size() == 2);
  EXPECT(state, profile.system_foreign_summaries.size() == 26);
  EXPECT(state, profile.system_foreign_summaries[13].linker_name ==
      "pthread_create");
  EXPECT(state,
      profile.system_foreign_summaries[13].callback_parameters ==
          std::vector<std::uint32_t>{2});
  EXPECT(state, profile.assembly_files.size() == 3);
  EXPECT(state, draft::relocation_model_name(profile.relocation_model) == "pic");
  EXPECT(state, draft::code_model_name(profile.code_model) == "small");
  EXPECT(state, draft::tls_model_name(profile.tls_model) == "general-dynamic");
}

void test_linux_profile(TestState &state) {
  const draft::TargetProfile profile = draft::make_aarch64_linux_profile();
  std::string reason;
  EXPECT(state, draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.empty());
  EXPECT(state, profile.facts.identity == "draft-aarch64-linux-gnu-v1");
  EXPECT(state, profile.facts.os == "linux");
  EXPECT(state, profile.facts.abi == "aapcs64_gnu");
  EXPECT(state, profile.facts.object_format == "elf");
  EXPECT(state, profile.facts.file_tag == "aarch64-linux");
  EXPECT(state, profile.facts.page_size == 4096);
  EXPECT(state, profile.llvm_triple == "aarch64-unknown-linux-gnu");
  EXPECT(state, profile.llvm_data_layout.find("m:e") != std::string::npos);
  EXPECT(state, profile.parsed_assembly_dialect ==
      "draft-aarch64-linux-v1");
  EXPECT(state, profile.parsed_assembly_instructions.size() == 82);
  EXPECT(state, profile.system_link_library == "c");
  EXPECT(state, profile.system_link_providers ==
      std::vector<std::string>({"libc", "linux"}));
  EXPECT(state, profile.system_foreign_summaries.size() == 26);
  EXPECT(state, profile.system_foreign_summaries[14].linker_name ==
      "pthread_create");
  EXPECT(state,
      profile.system_foreign_summaries[14].callback_parameters ==
          std::vector<std::uint32_t>{2});

  draft::TargetProfile selected;
  EXPECT(state, draft::select_builtin_target_profile(
      "aarch64-linux", selected, reason));
  EXPECT(state, selected.facts.identity == profile.facts.identity);
  EXPECT(state, !draft::select_builtin_target_profile(
      "x86_64-linux", selected, reason));
  EXPECT(state, reason.find("unknown target") != std::string::npos);
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

  profile = draft::make_aarch64_macos_profile();
  std::swap(profile.facts.simd_shapes[0], profile.facts.simd_shapes[1]);
  EXPECT(state, !draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.find("SIMD shape vocabulary") != std::string::npos);

  profile = draft::make_aarch64_macos_profile();
  profile.facts.simd_shapes[0].lanes = 3;
  EXPECT(state, !draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.find("64-bit or 128-bit vector") != std::string::npos);

  profile = draft::make_aarch64_macos_profile();
  std::swap(
      profile.parsed_assembly_instructions[0],
      profile.parsed_assembly_instructions[1]);
  EXPECT(state, !draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.find("instruction vocabulary") != std::string::npos);

  profile = draft::make_aarch64_linux_profile();
  profile.facts.object_format = "macho";
  EXPECT(state, !draft::validate_target_profile(profile, reason));
  EXPECT(state, reason.find("coherent set") != std::string::npos);
}

void test_simd_semantic_boundary(TestState &state) {
  const draft::TargetProfile profile = draft::make_aarch64_macos_profile();
  draft::TypeStore types;
  const std::optional<draft::TypeId> u32 = types.find_builtin("u32");
  EXPECT(state, u32.has_value());
  if (!u32.has_value()) return;

  (void)types.simd(*u32, 4);
  draft::DiagnosticSink diagnostics;
  EXPECT(state,
      draft::validate_target_types(types, profile.facts, diagnostics));
  EXPECT(state, diagnostics.error_count() == 0);

  (void)types.simd(*u32, 3);
  EXPECT(state,
      !draft::validate_target_types(types, profile.facts, diagnostics));
  EXPECT(state, diagnostics.error_count() == 1);
  EXPECT(state,
      diagnostics.diagnostics().front().message.find("#simd[3]u32") !=
          std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_initial_profile(state);
  test_linux_profile(state);
  test_invalid_profile_reports_reason(state);
  test_simd_semantic_boundary(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " target profile expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all target profile tests passed\n";
  return EXIT_SUCCESS;
}
