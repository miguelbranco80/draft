// Complete built-in target construction and consistency validation.

#include "target/profile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
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

[[nodiscard]] bool simd_shapes_sorted_unique(
    const std::vector<TargetSimdShape> &shapes) {
  for (std::size_t index = 1; index < shapes.size(); ++index) {
    const TargetSimdShape &previous = shapes[index - 1];
    const TargetSimdShape &current = shapes[index];
    if (previous.element > current.element ||
        (previous.element == current.element &&
         previous.lanes >= current.lanes)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::uint64_t> simd_element_bits(
    std::string_view element) {
  if (element == "i8" || element == "u8") return 8;
  if (element == "i16" || element == "u16" || element == "f16") return 16;
  if (element == "i32" || element == "u32" || element == "f32") return 32;
  if (element == "i64" || element == "u64" || element == "f64") return 64;
  return std::nullopt;
}

// All current profiles expose the same 64-bit and 128-bit source vector shapes.
// Shape legality is a storage/lowering fact; instruction availability remains
// governed by each architecture's separate enabled-feature vocabulary. These
// helpers return values instead of exposing mutable process globals, so each
// TargetProfile remains a complete owned build input.
[[nodiscard]] std::vector<TargetSimdShape> baseline_simd_shapes() {
  return {
      {"f16", 4}, {"f16", 8}, {"f32", 2}, {"f32", 4}, {"f64", 2},
      {"i16", 4}, {"i16", 8}, {"i32", 2}, {"i32", 4}, {"i64", 2},
      {"i8", 8}, {"i8", 16}, {"u16", 4}, {"u16", 8}, {"u32", 2},
      {"u32", 4}, {"u64", 2}, {"u8", 8}, {"u8", 16},
  };
}

[[nodiscard]] std::vector<std::string> baseline_assembly_instructions() {
  return {
      "adc", "adcs", "add", "adds", "and", "ands", "asr", "bic",
      "cls", "clz", "cmn", "cmp", "csel", "cset", "csetm", "csinc",
      "csinv", "csneg", "dmb", "dsb", "dup", "eon", "eor", "fabs",
      "fadd", "fcmp", "fcsel", "fcvt", "fcvtzs", "fcvtzu", "fdiv",
      "fmax", "fmaxnm", "fmin", "fminnm", "fmov", "fmul", "fneg",
      "fsqrt", "fsub", "isb", "ldar", "ldp", "ldr", "ldrb", "ldrh",
      "ldrsb", "ldrsh", "ldrsw", "ldur", "lsl", "lsr", "madd", "mov",
      "msub", "mul", "mvn", "neg", "negs", "nop", "orn", "orr", "rbit",
      "rev", "rev16", "rev32", "ror", "sbc", "sbcs", "scvtf", "sdiv",
      "stlr", "stp", "str", "strb", "strh", "stur", "sub", "subs", "tst",
      "ucvtf", "udiv",
  };
}

[[nodiscard]] std::vector<AssemblyFileRule> assembly_file_rules() {
  return {
      {".S", AssemblyPreprocessing::None},
      {".asm", AssemblyPreprocessing::None},
      {".s", AssemblyPreprocessing::None},
  };
}

// Both GNU/Linux profiles select the same glibc 2.39 symbol boundary. Keep the
// list independent of either machine constructor: sharing a complete target
// profile would obscure which facts are genuinely OS/libc facts and would do
// unnecessary work merely to copy this one semantic denial table.
[[nodiscard]] std::vector<SystemForeignSummary>
linux_system_foreign_summaries() {
  return {
      {"libc", "labs", {}},
      {"linux", "_exit", {}},
      {"linux", "clock_gettime", {}},
      {"linux", "close", {}},
      {"linux", "getpid", {}},
      {"linux", "mmap", {}},
      {"linux", "mprotect", {}},
      {"linux", "munmap", {}},
      {"linux", "nanosleep", {}},
      {"linux", "pthread_cond_broadcast", {}},
      {"linux", "pthread_cond_destroy", {}},
      {"linux", "pthread_cond_init", {}},
      {"linux", "pthread_cond_signal", {}},
      {"linux", "pthread_cond_wait", {}},
      {"linux", "pthread_create", {2}},
      {"linux", "pthread_join", {}},
      {"linux", "pthread_mutex_destroy", {}},
      {"linux", "pthread_mutex_init", {}},
      {"linux", "pthread_mutex_lock", {}},
      {"linux", "pthread_mutex_trylock", {}},
      {"linux", "pthread_mutex_unlock", {}},
      {"linux", "pthread_self", {}},
      {"linux", "read", {}},
      {"linux", "sched_yield", {}},
      {"linux", "unlink", {}},
      {"linux", "write", {}},
  };
}

// The Windows profile deliberately trusts individual UCRT and Kernel32 entry
// points rather than treating either DLL as an opaque provider. The callback
// positions are part of denial analysis: CreateThread may invoke parameter 2,
// while FlsAlloc may invoke its parameter 0 during thread teardown.
[[nodiscard]] std::vector<SystemForeignSummary>
windows_system_foreign_summaries() {
  return {
      {"libc", "_close", {}},
      {"libc", "_exit", {}},
      {"libc", "_get_osfhandle", {}},
      {"libc", "_open", {}},
      {"libc", "_read", {}},
      {"libc", "_setmode", {}},
      {"libc", "_unlink", {}},
      {"libc", "_write", {}},
      {"libc", "calloc", {}},
      {"libc", "free", {}},
      {"libc", "realloc", {}},
      {"libc", "strlen", {}},
      {"windows", "CloseHandle", {}},
      {"windows", "CreateThread", {2}},
      {"windows", "FlsAlloc", {0}},
      {"windows", "FlsFree", {}},
      {"windows", "FlsGetValue", {}},
      {"windows", "FlsSetValue", {}},
      {"windows", "GetConsoleMode", {}},
      {"windows", "GetConsoleScreenBufferInfo", {}},
      {"windows", "GetCurrentProcessId", {}},
      {"windows", "GetCurrentThreadId", {}},
      {"windows", "QueryPerformanceCounter", {}},
      {"windows", "QueryPerformanceFrequency", {}},
      {"windows", "SetConsoleMode", {}},
      {"windows", "Sleep", {}},
      {"windows", "SwitchToThread", {}},
      {"windows", "VirtualAlloc", {}},
      {"windows", "VirtualFree", {}},
      {"windows", "VirtualProtect", {}},
      {"windows", "WaitForSingleObject", {}},
  };
}

} // namespace

TargetProfile make_aarch64_macos_profile() {
  TargetProfile profile;
  profile.facts.identity = "draft-aarch64-macos-v5";
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
  // Draft 1 names only the baseline 64-bit and 128-bit Advanced SIMD register
  // shapes with at least two lanes. f16 is a legal storage/vector shape even
  // though arithmetic requiring optional full-FP16 instructions remains gated
  // by the separate target feature vocabulary.
  profile.facts.simd_shapes = baseline_simd_shapes();

  profile.minimum_os_version = "14.0";
  profile.llvm_triple = "arm64-apple-macosx14.0.0";
  profile.llvm_data_layout =
      "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32";
  profile.llvm_cpu = "generic";
  profile.llvm_feature_string = "+neon";
  profile.relocation_model = TargetRelocationModel::PositionIndependent;
  profile.code_model = TargetCodeModel::Small;
  profile.tls_model = TargetTlsModel::GeneralDynamic;
  profile.supports_parsed_assembly = true;
  profile.parsed_assembly_architecture = "aarch64";
  profile.parsed_assembly_dialect = "draft-aarch64-apple-v2";
  profile.parsed_assembly_instructions = baseline_assembly_instructions();
  profile.system_link_providers = {"darwin", "libc"};
  profile.system_link_library = "System";
  // This closed list covers the first core distribution and native examples.
  // It is deliberately a symbol list rather than provider-wide trust: adding
  // another System API requires deciding whether it calls or stores any Draft
  // procedure pointer and then changing the target profile identity.
  profile.system_foreign_summaries = {
      {"darwin", "_exit", {}},
      {"darwin", "clock_gettime_nsec_np", {}},
      {"darwin", "close", {}},
      {"darwin", "getpid", {}},
      {"darwin", "mmap", {}},
      {"darwin", "mprotect", {}},
      {"darwin", "munmap", {}},
      {"darwin", "nanosleep", {}},
      {"darwin", "pthread_cond_broadcast", {}},
      {"darwin", "pthread_cond_destroy", {}},
      {"darwin", "pthread_cond_init", {}},
      {"darwin", "pthread_cond_signal", {}},
      {"darwin", "pthread_cond_wait", {}},
      {"darwin", "pthread_create", {2}},
      {"darwin", "pthread_join", {}},
      {"darwin", "pthread_mutex_destroy", {}},
      {"darwin", "pthread_mutex_init", {}},
      {"darwin", "pthread_mutex_lock", {}},
      {"darwin", "pthread_mutex_trylock", {}},
      {"darwin", "pthread_mutex_unlock", {}},
      {"darwin", "pthread_self", {}},
      {"darwin", "read", {}},
      {"darwin", "sched_yield", {}},
      {"darwin", "unlink", {}},
      {"darwin", "write", {}},
      {"libc", "labs", {}},
  };
  profile.assembly_files = assembly_file_rules();
  return profile;
}

TargetProfile make_aarch64_linux_profile() {
  TargetProfile profile;
  profile.facts.identity = "draft-aarch64-linux-gnu-v1";
  profile.facts.arch = "aarch64";
  profile.facts.os = "linux";
  profile.facts.abi = "aapcs64_gnu";
  profile.facts.byte_order = "little";
  profile.facts.object_format = "elf";
  profile.facts.file_tag = "aarch64-linux";
  profile.facts.pointer_bits = 64;
  profile.facts.page_size = 4096;
  profile.facts.known_features = {
      "aes", "crc", "dotprod", "fp16", "neon", "sha2"};
  profile.facts.features = {"neon"};
  profile.facts.simd_shapes = baseline_simd_shapes();

  // The first Linux distribution contract is Ubuntu 24.04-class userspace:
  // Linux 6.8, glibc 2.39, the GNU AArch64 ABI, and a 4 KiB base page.  The
  // version string is descriptive profile identity; unlike Darwin's deployment
  // floor it is not handed to Clang as a command-line option.
  profile.minimum_os_version = "linux-6.8-glibc-2.39";
  profile.llvm_triple = "aarch64-unknown-linux-gnu";
  profile.llvm_data_layout =
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32";
  profile.llvm_cpu = "generic";
  profile.llvm_feature_string = "+neon";
  profile.relocation_model = TargetRelocationModel::PositionIndependent;
  profile.code_model = TargetCodeModel::Small;
  profile.tls_model = TargetTlsModel::GeneralDynamic;
  profile.supports_parsed_assembly = true;
  profile.parsed_assembly_architecture = "aarch64";
  profile.parsed_assembly_dialect = "draft-aarch64-linux-v1";
  profile.parsed_assembly_instructions = baseline_assembly_instructions();

  // `linux` names the POSIX/Linux surface supplied by glibc; `libc` retains
  // the portable C provider spelling.  Both resolve to -lc in the initial GNU
  // profile, but remain separate semantic provider identities for denials.
  profile.system_link_providers = {"libc", "linux"};
  profile.system_link_library = "c";
  profile.system_foreign_summaries = linux_system_foreign_summaries();
  profile.assembly_files = assembly_file_rules();
  return profile;
}

TargetProfile make_x86_64_linux_profile() {
  TargetProfile profile;
  profile.facts.identity = "draft-x86_64-linux-gnu-v1";
  profile.facts.arch = "x86_64";
  profile.facts.os = "linux";
  profile.facts.abi = "sysv_amd64";
  profile.facts.byte_order = "little";
  profile.facts.object_format = "elf";
  profile.facts.file_tag = "x86_64-linux";
  profile.facts.pointer_bits = 64;
  profile.facts.page_size = 4096;

  // x86-64 requires SSE2 as architectural baseline. AVX and AVX2 are known
  // profile vocabulary but deliberately disabled so generated code remains
  // runnable on the complete baseline architecture rather than the build host.
  profile.facts.known_features = {"avx", "avx2", "sse2"};
  profile.facts.features = {"sse2"};
  profile.facts.simd_shapes = baseline_simd_shapes();

  profile.minimum_os_version = "linux-6.8-glibc-2.39";
  profile.llvm_triple = "x86_64-unknown-linux-gnu";
  profile.llvm_data_layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-"
                             "i128:128-f80:128-n8:16:32:64-S128";
  profile.llvm_cpu = "x86-64";
  profile.llvm_feature_string = "+sse2";
  profile.relocation_model = TargetRelocationModel::PositionIndependent;
  profile.code_model = TargetCodeModel::Small;
  profile.tls_model = TargetTlsModel::GeneralDynamic;

  // Native code and package assembly are supported, but the first x86-64
  // profile deliberately has no parsed inline-assembly grammar. This absence
  // is explicit target capability rather than an AArch64 parser fallback.
  profile.supports_parsed_assembly = false;

  profile.system_link_providers = {"libc", "linux"};
  profile.system_link_library = "c";
  profile.system_foreign_summaries = linux_system_foreign_summaries();
  profile.assembly_files = assembly_file_rules();
  return profile;
}

TargetProfile make_x86_64_windows_profile() {
  TargetProfile profile;
  profile.facts.identity = "draft-x86_64-windows-msvc-v1";
  profile.facts.arch = "x86_64";
  profile.facts.os = "windows";
  profile.facts.abi = "win64";
  profile.facts.byte_order = "little";
  profile.facts.object_format = "coff";
  profile.facts.file_tag = "x86_64-windows";
  profile.facts.pointer_bits = 64;
  profile.facts.page_size = 4096;
  profile.facts.known_features = {"avx", "avx2", "sse2"};
  profile.facts.features = {"sse2"};
  profile.facts.simd_shapes = baseline_simd_shapes();

  // Windows 10 is the first distribution boundary. The version is semantic
  // profile identity, while LLVM needs only the MSVC environment triple to
  // select Win64 calling convention and COFF lowering.
  profile.minimum_os_version = "windows-10";
  profile.llvm_triple = "x86_64-pc-windows-msvc";
  profile.llvm_data_layout =
      "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-"
      "f80:128-n8:16:32:64-S128";
  profile.llvm_cpu = "x86-64";
  profile.llvm_feature_string = "+sse2";
  profile.relocation_model = TargetRelocationModel::PositionIndependent;
  profile.code_model = TargetCodeModel::Small;
  profile.tls_model = TargetTlsModel::GeneralDynamic;
  profile.supports_parsed_assembly = false;

  // `libc` names the Universal CRT surface. `windows` names Kernel32, whose
  // import library is part of the selected Windows SDK. The native toolchain
  // links the UCRT through the MSVC driver defaults and adds Kernel32
  // explicitly for core/runtime operating-system primitives.
  profile.system_link_providers = {"libc", "windows"};
  profile.system_link_library = "kernel32";
  profile.system_foreign_summaries = windows_system_foreign_summaries();
  profile.assembly_files = assembly_file_rules();
  return profile;
}

bool select_builtin_target_profile(std::string_view selector,
                                   TargetProfile &profile,
                                   std::string &reason) {
  reason.clear();
  if (selector.empty() || selector == "aarch64-macos") {
    profile = make_aarch64_macos_profile();
    return true;
  }
  if (selector == "aarch64-linux") {
    profile = make_aarch64_linux_profile();
    return true;
  }
  if (selector == "x86_64-linux") {
    profile = make_x86_64_linux_profile();
    return true;
  }
  if (selector == "x86_64-windows") {
    profile = make_x86_64_windows_profile();
    return true;
  }
  reason = "unknown target '" + std::string(selector) +
      "'; expected aarch64-macos, aarch64-linux, x86_64-linux, or "
      "x86_64-windows";
  return false;
}

bool validate_target_profile(const TargetProfile &profile,
                             std::string &reason) {
  reason.clear();
  if (profile.facts.identity.empty() || profile.facts.file_tag.empty()) {
    reason = "target identity and file tag must not be empty";
    return false;
  }
  if (profile.facts.pointer_bits != 64) {
    reason = "built-in native profiles require 64-bit pointers";
    return false;
  }
  if (!is_power_of_two(profile.facts.page_size)) {
    reason = "target page size must be a nonzero power of two";
    return false;
  }
  if (profile.facts.arch != "aarch64" && profile.facts.arch != "x86_64") {
    reason = "bootstrap target profile names an unsupported architecture";
    return false;
  }
  const bool macos = profile.facts.arch == "aarch64" &&
                     profile.facts.os == "macos" &&
                     profile.facts.abi == "darwin_arm64" &&
                     profile.facts.object_format == "macho";
  const bool aarch64_linux = profile.facts.arch == "aarch64" &&
                             profile.facts.os == "linux" &&
                             profile.facts.abi == "aapcs64_gnu" &&
                             profile.facts.object_format == "elf";
  const bool x86_64_linux =
      profile.facts.arch == "x86_64" && profile.facts.os == "linux" &&
      profile.facts.abi == "sysv_amd64" && profile.facts.object_format == "elf";
  const bool x86_64_windows =
      profile.facts.arch == "x86_64" && profile.facts.os == "windows" &&
      profile.facts.abi == "win64" && profile.facts.object_format == "coff";
  if (!macos && !aarch64_linux && !x86_64_linux && !x86_64_windows) {
    reason =
        "target OS, ABI, and object format are not a supported coherent set";
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
    if (!std::binary_search(profile.facts.known_features.begin(),
                            profile.facts.known_features.end(), feature)) {
      reason =
          "enabled target feature is absent from the known feature vocabulary";
      return false;
    }
  }
  if (profile.facts.simd_shapes.empty() ||
      !simd_shapes_sorted_unique(profile.facts.simd_shapes)) {
    reason = "target SIMD shape vocabulary must be sorted and unique";
    return false;
  }
  for (const TargetSimdShape &shape : profile.facts.simd_shapes) {
    const std::optional<std::uint64_t> element_bits =
        simd_element_bits(shape.element);
    if (!element_bits.has_value() || shape.lanes < 2 ||
        shape.lanes > 128 / *element_bits ||
        (*element_bits * shape.lanes != 64 &&
         *element_bits * shape.lanes != 128)) {
      reason =
          "target SIMD shape must name a baseline 64-bit or 128-bit vector";
      return false;
    }
  }
  if (profile.supports_parsed_assembly) {
    if (profile.parsed_assembly_architecture != profile.facts.arch ||
        profile.parsed_assembly_dialect.empty()) {
      reason =
          "parsed assembly architecture must match the target architecture";
      return false;
    }
    if (profile.parsed_assembly_instructions.empty() ||
        !bytewise_sorted_unique(profile.parsed_assembly_instructions)) {
      reason =
          "parsed assembly instruction vocabulary must be sorted and unique";
      return false;
    }
  } else if (!profile.parsed_assembly_architecture.empty() ||
             !profile.parsed_assembly_dialect.empty() ||
             !profile.parsed_assembly_instructions.empty()) {
    reason = "unsupported parsed assembly must not publish a partial grammar";
    return false;
  }
  const std::vector<std::string> expected_system_providers =
      macos ? std::vector<std::string>{"darwin", "libc"}
      : x86_64_windows ? std::vector<std::string>{"libc", "windows"}
                       : std::vector<std::string>{"libc", "linux"};
  const std::string_view expected_system_library =
      macos ? "System" : x86_64_windows ? "kernel32" : "c";
  if (profile.system_link_library != expected_system_library ||
      profile.system_link_providers != expected_system_providers ||
      !bytewise_sorted_unique(profile.system_link_providers)) {
    reason = "target system providers do not match its base system library";
    return false;
  }
  for (std::size_t index = 0; index < profile.system_foreign_summaries.size();
       ++index) {
    const SystemForeignSummary &summary =
        profile.system_foreign_summaries[index];
    if (!std::binary_search(
            profile.system_link_providers.begin(),
            profile.system_link_providers.end(),
            summary.provider) || summary.linker_name.empty()) {
      reason = "system foreign summary must name a target-owned provider and symbol";
      return false;
    }
    if (index != 0) {
      const SystemForeignSummary &previous =
          profile.system_foreign_summaries[index - 1];
      if (previous.provider > summary.provider ||
          (previous.provider == summary.provider &&
           previous.linker_name >= summary.linker_name)) {
        reason = "system foreign summaries must be sorted and unique";
        return false;
      }
    }
    for (std::size_t callback = 1;
         callback < summary.callback_parameters.size(); ++callback) {
      if (summary.callback_parameters[callback - 1] >=
          summary.callback_parameters[callback]) {
        reason = "system foreign callback slots must be sorted and unique";
        return false;
      }
    }
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
