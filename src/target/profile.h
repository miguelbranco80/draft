// Versioned target-profile facts for the initial AArch64 macOS implementation.
//
// The profile is the single authority for facts that would otherwise leak host
// assumptions into semantic analysis, ABI lowering, assembly parsing, object
// emission, or linking. It contains no LLVM objects and performs no tool lookup;
// the LLVM and linker adapters consume these stable strings and independently
// verify compatibility with the pinned toolchain.
//
// Draft 1 supports exactly one profile. Additional targets add new complete
// profile constructors rather than conditionals scattered through the compiler.
// Relevant specification: 02-types-memory-runtime.md "Target profile" and
// 04-native-interop.md sections 11-12.

#pragma once

#include "sema/constant.h"

#include <string>
#include <string_view>
#include <vector>

namespace draft {

enum class TargetRelocationModel {
  PositionIndependent,
};

enum class TargetCodeModel {
  Small,
};

enum class TargetTlsModel {
  GeneralDynamic,
};

enum class AssemblyPreprocessing {
  None,
};

// AssemblyFileRule removes ambient host behavior from assembly extensions. In
// particular, `.S` does not imply a C preprocessor: all three extensions contain
// exact source bytes in the profile's declared dialect/tool contract.
struct AssemblyFileRule {
  std::string extension;
  AssemblyPreprocessing preprocessing = AssemblyPreprocessing::None;
};

// TargetProfile owns semantic facts plus backend/link contract strings. The
// fields are serialized as build inputs, so changing any value requires a new
// identity even if source programs happen to produce identical objects.
struct TargetProfile {
  TargetFacts facts;
  std::string llvm_triple;
  std::string llvm_data_layout;
  std::string llvm_cpu;
  std::string llvm_feature_string;
  std::string minimum_os_version;
  TargetRelocationModel relocation_model = TargetRelocationModel::PositionIndependent;
  TargetCodeModel code_model = TargetCodeModel::Small;
  TargetTlsModel tls_model = TargetTlsModel::GeneralDynamic;
  std::string parsed_assembly_architecture;
  std::string parsed_assembly_dialect;
  // This sorted list is the closed mnemonic vocabulary of the parsed dialect.
  // Operand forms remain versioned by parsed_assembly_dialect and are checked
  // by the architecture analyzer.  Keeping the names here makes profile drift
  // visible in target reports, resolved-program hashes, and tests.
  std::vector<std::string> parsed_assembly_instructions;
  // Logical providers satisfied by the target SDK's explicitly selected base
  // system library. Other provider names require an exact external artifact.
  std::vector<std::string> system_link_providers;
  std::string system_link_library;
  std::vector<AssemblyFileRule> assembly_files;
};

// Constructs the complete immutable-value profile used by the bootstrap. The
// value return avoids global initialization and makes tests free to validate a
// modified copy without mutating compiler process state.
[[nodiscard]] TargetProfile make_aarch64_macos_profile();

// Checks internal profile consistency before any semantic or backend work. It
// returns false and writes one direct reason rather than asserting on values
// that can eventually come from a selected distribution artifact.
[[nodiscard]] bool validate_target_profile(
    const TargetProfile &profile, std::string &reason);

[[nodiscard]] std::string_view relocation_model_name(TargetRelocationModel model);
[[nodiscard]] std::string_view code_model_name(TargetCodeModel model);
[[nodiscard]] std::string_view tls_model_name(TargetTlsModel model);

} // namespace draft
