// Initial AArch64 macOS target profile construction and consistency validation.

#include "target/profile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace draft {
namespace {

// Page-size validation uses the same representation-independent power-of-two
// rule as semantic alignment.
[[nodiscard]] bool is_power_of_two(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

// Canonical vectors must be strictly increasing so they are both sorted and
// duplicate-free. std::string comparison is the intended bytewise key.
[[nodiscard]] bool bytewise_sorted_unique(const std::vector<std::string> &values) {
  for (std::size_t index = 1; index < values.size(); ++index) {
    if (values[index - 1] >= values[index]) {
      return false;
    }
  }
  return true;
}

} // namespace

TargetProfile make_aarch64_macos_profile() {
  TargetProfile profile;
  profile.facts.identity = "draft-aarch64-macos-v1";
  profile.facts.arch = "aarch64";
  profile.facts.os = "macos";
  profile.facts.abi = "darwin_arm64";
  profile.facts.byte_order = "little";
  profile.facts.object_format = "macho";
  profile.facts.file_tag = "aarch64-macos";
  profile.facts.pointer_bits = 64;
  profile.facts.page_size = 16384;

  // These names are Draft's stable target vocabulary, not every LLVM feature.
  // Generic Apple arm64 enables only architectural baseline behavior here;
  // optional crypto/dot-product/fp16 instructions require later profiles.
  profile.facts.known_features = {
      "aes", "crc", "dotprod", "fp16", "neon", "sha2"};
  profile.facts.features = {"neon"};

  profile.minimum_os_version = "14.0";
  profile.llvm_triple = "arm64-apple-macosx14.0.0";
  profile.llvm_data_layout =
      "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32";
  profile.llvm_cpu = "generic";
  profile.llvm_feature_string = "+neon";
  profile.relocation_model = TargetRelocationModel::PositionIndependent;
  profile.code_model = TargetCodeModel::Small;
  profile.tls_model = TargetTlsModel::GeneralDynamic;
  profile.parsed_assembly_architecture = "aarch64";
  profile.parsed_assembly_dialect = "draft-aarch64-apple-v1";
  profile.assembly_files = {
      {".S", AssemblyPreprocessing::None},
      {".asm", AssemblyPreprocessing::None},
      {".s", AssemblyPreprocessing::None},
  };
  return profile;
}

bool validate_target_profile(const TargetProfile &profile, std::string &reason) {
  reason.clear();
  if (profile.facts.identity.empty() || profile.facts.file_tag.empty()) {
    reason = "target identity and file tag must not be empty";
    return false;
  }
  if (profile.facts.pointer_bits != 64) {
    reason = "initial AArch64 profile requires 64-bit pointers";
    return false;
  }
  if (!is_power_of_two(profile.facts.page_size)) {
    reason = "target page size must be a nonzero power of two";
    return false;
  }
  if (profile.facts.arch != "aarch64" || profile.facts.os != "macos" ||
      profile.facts.object_format != "macho") {
    reason = "initial profile must consistently name AArch64, macOS, and Mach-O";
    return false;
  }
  if (profile.llvm_triple.empty() || profile.llvm_data_layout.empty() ||
      profile.minimum_os_version.empty()) {
    reason = "LLVM triple, data layout, and deployment target must be fixed";
    return false;
  }
  if (!bytewise_sorted_unique(profile.facts.known_features) ||
      !bytewise_sorted_unique(profile.facts.features)) {
    reason = "target feature lists must be bytewise sorted and unique";
    return false;
  }
  for (const std::string &feature : profile.facts.features) {
    if (!std::binary_search(
            profile.facts.known_features.begin(),
            profile.facts.known_features.end(),
            feature)) {
      reason = "enabled target feature is absent from the known feature vocabulary";
      return false;
    }
  }
  if (profile.parsed_assembly_architecture != profile.facts.arch ||
      profile.parsed_assembly_dialect.empty()) {
    reason = "parsed assembly architecture must match the target architecture";
    return false;
  }
  const std::vector<std::string> expected_extensions = {".S", ".asm", ".s"};
  if (profile.assembly_files.size() != expected_extensions.size()) {
    reason = "target must define exactly .s, .S, and .asm assembly rules";
    return false;
  }
  for (std::size_t index = 0; index < expected_extensions.size(); ++index) {
    if (profile.assembly_files[index].extension != expected_extensions[index] ||
        profile.assembly_files[index].preprocessing != AssemblyPreprocessing::None) {
      reason = "assembly extensions must be sorted and explicitly non-preprocessed";
      return false;
    }
  }
  return true;
}

std::string_view relocation_model_name(TargetRelocationModel model) {
  switch (model) {
  case TargetRelocationModel::PositionIndependent: return "pic";
  }
  return "unknown";
}

std::string_view code_model_name(TargetCodeModel model) {
  switch (model) {
  case TargetCodeModel::Small: return "small";
  }
  return "unknown";
}

std::string_view tls_model_name(TargetTlsModel model) {
  switch (model) {
  case TargetTlsModel::GeneralDynamic: return "general-dynamic";
  }
  return "unknown";
}

} // namespace draft
