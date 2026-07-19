// Versioned target-profile facts for the bootstrap AArch64 implementations.
//
// The profile is the single authority for facts that would otherwise leak host
// assumptions into semantic analysis, ABI lowering, assembly parsing, object
// emission, or linking. It contains no LLVM objects and performs no tool lookup;
// the LLVM and linker adapters consume these stable strings and independently
// verify compatibility with the selected host toolchain.
//
// Each supported target has one complete constructor.  Shared architectural
// facts are assembled here, while operating-system ABI, object, linker, and
// runtime facts remain explicit in the individual constructor.  This is the
// intended firewall against target conditionals scattered through semantic or
// backend code.
// Relevant specification: docs/specification/02-types-memory-runtime.md "Target profile" and
// docs/specification/04-native-interop.md sections 11-12.

#pragma once

#include "sema/constant.h"

#include <cstdint>
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

// A target-owned system symbol summary is trusted only because the target
// profile selects both the SDK library and this exact linker spelling. Most
// fixed System calls have no route back into Draft. callback_parameters names
// the zero-based C parameter positions that the native function may invoke.
// Arbitrary user artifacts never enter this table; they require a separately
// content-bound provider summary.
struct SystemForeignSummary {
  std::string provider;
  std::string linker_name;
  std::vector<std::uint32_t> callback_parameters;
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
  // Sorted by provider and linker_name. These are semantic denial contracts,
  // not linker availability declarations: an unlisted System symbol still
  // links, but contributes an unknown call edge while a denial is active.
  std::vector<SystemForeignSummary> system_foreign_summaries;
  std::vector<AssemblyFileRule> assembly_files;
};

// Constructs the complete immutable-value profile used by the bootstrap. The
// value return avoids global initialization and makes tests free to validate a
// modified copy without mutating compiler process state.
[[nodiscard]] TargetProfile make_aarch64_macos_profile();

// Constructs the first GNU/Linux profile.  It intentionally selects one
// concrete 4 KiB-page, glibc-based distribution contract rather than claiming
// that one profile describes every valid AArch64 Linux kernel/libc pairing.
[[nodiscard]] TargetProfile make_aarch64_linux_profile();

// Resolves the stable command-line selector for a built-in profile.  The
// default remains aarch64-macos for compatibility.  Returning a value rather
// than a reference keeps profiles immutable-by-convention and lets tests alter
// a copy without process-global state.
[[nodiscard]] bool select_builtin_target_profile(
    std::string_view selector, TargetProfile &profile, std::string &reason);

// Checks internal profile consistency before any semantic or backend work. It
// returns false and writes one direct reason rather than asserting on values
// that can eventually come from a selected distribution artifact.
[[nodiscard]] bool validate_target_profile(
    const TargetProfile &profile, std::string &reason);

[[nodiscard]] std::string_view relocation_model_name(TargetRelocationModel model);
[[nodiscard]] std::string_view code_model_name(TargetCodeModel model);
[[nodiscard]] std::string_view tls_model_name(TargetTlsModel model);

} // namespace draft
