// Target-profiled content-tree pinning and verification for native tools.

#include "backend/locked_inputs.h"

#include "backend/macho_dependencies.h"
#include "base/content_tree.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

constexpr std::string_view kMacosToolchainName = "llvm-aarch64-macos";
constexpr std::string_view kMacosSdkName = "macos-sdk";
constexpr std::string_view kLinuxToolchainName = "llvm-aarch64-linux";
constexpr std::string_view kLinuxSysrootName = "aarch64-linux-sysroot";
constexpr std::string_view kClangEntry = "bin/clang";
constexpr std::string_view kLinkerEntry = "bin/ld";
constexpr std::string_view kElfLinkerEntry = "bin/ld.lld";
constexpr std::string_view kClassicLinkerEntry = "bin/ld-classic";
constexpr std::string_view kArchiverEntry = "bin/llvm-ar";
constexpr std::string_view kDsymutilEntry = "bin/dsymutil";
constexpr std::string_view kAddressSanitizerRuntimeEntry =
    "lib/clang/22/lib/darwin/libclang_rt.asan_osx_dynamic.dylib";
constexpr std::string_view kLlvmSymbolizerEntry = "bin/llvm-symbolizer";

[[nodiscard]] bool inspect_root_directory(
    const std::filesystem::path &root,
    std::string_view role,
    DiagnosticSink &diagnostics) {
  if (root.empty() || !root.is_absolute()) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked " + std::string(role) + " root must be an absolute path");
    return false;
  }
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(root, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot inspect locked " + std::string(role) + " root: " +
            error.message());
    return false;
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_directory(status)) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked " + std::string(role) +
            " root must be a real directory, not a symlink");
    return false;
  }
  return true;
}

[[nodiscard]] bool canonical_roots(
    const LockedNativeInputRoots &roots,
    LockedNativeInputRoots &canonical,
    DiagnosticSink &diagnostics) {
  if (!inspect_root_directory(roots.toolchain_root, "toolchain", diagnostics) ||
      !inspect_root_directory(roots.sdk_root, "SDK", diagnostics)) {
    return false;
  }
  std::error_code error;
  canonical.toolchain_root =
      std::filesystem::canonical(roots.toolchain_root, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize locked toolchain root: " + error.message());
    return false;
  }
  canonical.sdk_root = std::filesystem::canonical(roots.sdk_root, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize locked SDK root: " + error.message());
    return false;
  }
  return true;
}

[[nodiscard]] bool inspect_executable(
    const std::filesystem::path &path,
    std::string_view role,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked toolchain does not contain a regular " + std::string(role) +
            " at the required entry path");
    return false;
  }
  const std::filesystem::perms execute =
      std::filesystem::perms::owner_exec |
      std::filesystem::perms::group_exec |
      std::filesystem::perms::others_exec;
  if ((status.permissions() & execute) == std::filesystem::perms::none) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked toolchain " + std::string(role) + " is not executable");
    return false;
  }
  return true;
}

[[nodiscard]] bool pin_less(
    const ExternalInputPin &left,
    const ExternalInputPin &right) {
  if (left.kind != right.kind) {
    return static_cast<unsigned>(left.kind) <
        static_cast<unsigned>(right.kind);
  }
  return left.name < right.name;
}

[[nodiscard]] bool pins_equal(
    const ExternalInputPin &left,
    const ExternalInputPin &right) {
  return left.kind == right.kind && left.name == right.name &&
      left.content_digest == right.content_digest &&
      left.entry_point == right.entry_point;
}

} // namespace

bool pin_locked_native_inputs(
    const TargetProfile &target,
    const LockedNativeInputRoots &roots,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  LockedNativeInputRoots canonical;
  if (!canonical_roots(roots, canonical, diagnostics)) return false;

  const bool is_elf = target.facts.object_format == "elf";
  if (!is_elf && target.facts.object_format != "macho") {
    diagnostics.error(
        SourceRange::invalid(),
        "locked native inputs do not support target object format '" +
            target.facts.object_format + "'");
    return false;
  }

  const std::filesystem::path clang = canonical.toolchain_root / kClangEntry;
  const std::filesystem::path linker = canonical.toolchain_root /
      (is_elf ? kElfLinkerEntry : kLinkerEntry);
  const std::filesystem::path classic_linker =
      canonical.toolchain_root / kClassicLinkerEntry;
  const std::filesystem::path archiver = canonical.toolchain_root / kArchiverEntry;
  const std::filesystem::path dsymutil = canonical.toolchain_root / kDsymutilEntry;
  if (!inspect_executable(clang, "Clang driver", diagnostics) ||
      !inspect_executable(
          linker, is_elf ? "ELF lld linker" : "Mach-O linker", diagnostics) ||
      !inspect_executable(archiver, "LLVM archiver", diagnostics)) {
    return false;
  }
  if (is_elf) {
    // The bootstrap currently cross-compiles Linux from the qualified macOS
    // host, so these executables are Mach-O even though their output is ELF.
    // The dependency checker proves that their non-platform dylibs remain in
    // the pinned tree. A future Linux-hosted bootstrap needs an equivalent ELF
    // host-tool closure before it can claim locked operation there.
    const std::array tool_entries{clang, linker, archiver};
    if (!validate_macho_dependency_closure(
            canonical.toolchain_root, tool_entries, diagnostics)) {
      return false;
    }
  } else {
    if (!inspect_executable(
            classic_linker, "classic Mach-O linker", diagnostics) ||
        !inspect_executable(dsymutil, "LLVM dsymutil", diagnostics)) {
      return false;
    }
    // Apple's linker delegates relocatable links to its colocated classic
    // implementation, while dsymutil publishes the final debug companion.
    const std::array tool_entries{
        clang, linker, classic_linker, archiver, dsymutil};
    if (!validate_macho_dependency_closure(
            canonical.toolchain_root, tool_entries, diagnostics)) {
      return false;
    }
  }

  // Instrumentation is a distribution capability, not a baseline build
  // requirement. If the fixed runtime entry is present, validate its Mach-O
  // type, relocatable install name, and complete dependency closure now. This
  // ensures a later address-instrumented build cannot introduce Homebrew or
  // another ambient loader input after the tree has been accepted and hashed.
  const std::filesystem::path address_runtime =
      canonical.toolchain_root / kAddressSanitizerRuntimeEntry;
  std::error_code runtime_error;
  const std::filesystem::file_status runtime_status =
      std::filesystem::symlink_status(address_runtime, runtime_error);
  if (!is_elf && !runtime_error && std::filesystem::exists(runtime_status)) {
    const std::filesystem::path symbolizer =
        canonical.toolchain_root / kLlvmSymbolizerEntry;
    if (!inspect_executable(symbolizer, "LLVM symbolizer", diagnostics)) {
      return false;
    }
    const std::array symbolizer_entries{symbolizer};
    if (!validate_macho_dependency_closure(
            canonical.toolchain_root, symbolizer_entries, diagnostics)) {
      return false;
    }
    const std::array runtime_entries{address_runtime};
    if (!validate_macho_dylib_dependency_closure(
            canonical.toolchain_root, runtime_entries, diagnostics)) {
      return false;
    }
  } else if (!is_elf && runtime_error &&
             runtime_error != std::errc::no_such_file_or_directory) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot inspect optional locked address-sanitizer runtime: " +
            runtime_error.message());
    return false;
  }

  ExternalInputPin toolchain;
  toolchain.kind = ExternalInputKind::Toolchain;
  toolchain.name = is_elf ? kLinuxToolchainName : kMacosToolchainName;
  toolchain.entry_point = kClangEntry;
  if (!hash_content_tree(
          canonical.toolchain_root, toolchain.content_digest, diagnostics)) {
    return false;
  }

  ExternalInputPin sdk;
  sdk.kind = ExternalInputKind::Sdk;
  sdk.name = is_elf ? kLinuxSysrootName : kMacosSdkName;
  if (!hash_content_tree(canonical.sdk_root, sdk.content_digest, diagnostics)) {
    return false;
  }

  std::vector<ExternalInputPin> result;
  result.push_back(std::move(toolchain));
  result.push_back(std::move(sdk));
  std::sort(result.begin(), result.end(), pin_less);
  pins = std::move(result);
  return diagnostics.error_count() == initial_errors;
}

bool verify_locked_native_inputs(
    const TargetProfile &target,
    const LockedNativeInputRoots &roots,
    std::span<const ExternalInputPin> manifest_pins,
    VerifiedLockedNativeInputs &verified,
    DiagnosticSink &diagnostics) {
  std::vector<ExternalInputPin> actual;
  if (!pin_locked_native_inputs(target, roots, actual, diagnostics)) return false;
  LockedNativeInputRoots canonical;
  if (!canonical_roots(roots, canonical, diagnostics)) return false;

  std::vector<ExternalInputPin> expected;
  for (const ExternalInputPin &pin : manifest_pins) {
    if (pin.kind == ExternalInputKind::Toolchain ||
        pin.kind == ExternalInputKind::Sdk) {
      expected.push_back(pin);
    }
  }
  std::sort(expected.begin(), expected.end(), pin_less);
  if (expected.size() != actual.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked native build requires exactly one pinned LLVM toolchain and "
        "one pinned target system root");
    return false;
  }
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (!pins_equal(expected[index], actual[index])) {
      diagnostics.error(
          SourceRange::invalid(),
          "locked native build input does not match the resolution manifest: " +
              std::string(external_input_kind_name(actual[index].kind)) +
              " '" + actual[index].name + "'");
      return false;
    }
  }

  VerifiedLockedNativeInputs result;
  result.clang = canonical.toolchain_root / kClangEntry;
  const bool is_elf = target.facts.object_format == "elf";
  result.linker = canonical.toolchain_root /
      (is_elf ? kElfLinkerEntry : kLinkerEntry);
  result.archiver = canonical.toolchain_root / kArchiverEntry;
  if (!is_elf) {
    result.dsymutil = canonical.toolchain_root / kDsymutilEntry;
  }
  result.toolchain_root = canonical.toolchain_root;
  result.sdk_root = canonical.sdk_root;
  const std::filesystem::path address_runtime =
      canonical.toolchain_root / kAddressSanitizerRuntimeEntry;
  std::error_code runtime_error;
  if (!is_elf &&
      std::filesystem::is_regular_file(address_runtime, runtime_error) &&
      !runtime_error) {
    result.address_sanitizer_runtime = address_runtime;
    result.llvm_symbolizer =
        canonical.toolchain_root / kLlvmSymbolizerEntry;
  }
  verified = std::move(result);
  return true;
}

} // namespace draft
