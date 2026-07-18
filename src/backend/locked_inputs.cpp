// Content-tree pinning and verification for the first native tool boundary.

#include "backend/locked_inputs.h"

#include "base/content_tree.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

constexpr std::string_view kToolchainName = "llvm-aarch64-macos";
constexpr std::string_view kSdkName = "macos-sdk";
constexpr std::string_view kClangEntry = "bin/clang";
constexpr std::string_view kLinkerEntry = "bin/ld64.lld";
constexpr std::string_view kArchiverEntry = "bin/llvm-ar";

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
    const LockedNativeInputRoots &roots,
    std::vector<ExternalInputPin> &pins,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  LockedNativeInputRoots canonical;
  if (!canonical_roots(roots, canonical, diagnostics)) return false;

  const std::filesystem::path clang = canonical.toolchain_root / kClangEntry;
  const std::filesystem::path linker = canonical.toolchain_root / kLinkerEntry;
  const std::filesystem::path archiver = canonical.toolchain_root / kArchiverEntry;
  if (!inspect_executable(clang, "Clang driver", diagnostics) ||
      !inspect_executable(linker, "Mach-O linker", diagnostics) ||
      !inspect_executable(archiver, "LLVM archiver", diagnostics)) {
    return false;
  }

  ExternalInputPin toolchain;
  toolchain.kind = ExternalInputKind::Toolchain;
  toolchain.name = kToolchainName;
  toolchain.entry_point = kClangEntry;
  if (!hash_content_tree(
          canonical.toolchain_root, toolchain.content_digest, diagnostics)) {
    return false;
  }

  ExternalInputPin sdk;
  sdk.kind = ExternalInputKind::Sdk;
  sdk.name = kSdkName;
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
    const LockedNativeInputRoots &roots,
    std::span<const ExternalInputPin> manifest_pins,
    VerifiedLockedNativeInputs &verified,
    DiagnosticSink &diagnostics) {
  std::vector<ExternalInputPin> actual;
  if (!pin_locked_native_inputs(roots, actual, diagnostics)) return false;
  LockedNativeInputRoots canonical;
  if (!canonical_roots(roots, canonical, diagnostics)) return false;

  std::vector<ExternalInputPin> expected(manifest_pins.begin(), manifest_pins.end());
  std::sort(expected.begin(), expected.end(), pin_less);
  if (expected.size() != actual.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked native build requires exactly one pinned LLVM toolchain and "
        "one pinned macOS SDK and supports no other external inputs yet");
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
  result.linker = canonical.toolchain_root / kLinkerEntry;
  result.archiver = canonical.toolchain_root / kArchiverEntry;
  result.toolchain_root = canonical.toolchain_root;
  result.sdk_root = canonical.sdk_root;
  verified = std::move(result);
  return true;
}

} // namespace draft
