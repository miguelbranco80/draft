// Synthetic Mach-O dependency-closure tests, independent of host tools.

#include "backend/macho_dependencies.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "macho_dependencies_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

constexpr std::uint32_t kMachOMagic64 = 0xfeedfacfU;
constexpr std::uint32_t kCpuTypeArm64 = 0x0100000cU;
constexpr std::uint32_t kMachOExecute = 2;
constexpr std::uint32_t kMachODylib = 6;
constexpr std::uint32_t kLoadDylib = 0x0000000cU;
constexpr std::uint32_t kIdDylib = 0x0000000dU;
constexpr std::uint32_t kLoadDylinker = 0x0000000eU;
constexpr std::uint32_t kRpath = 0x8000001cU;
constexpr std::uint32_t kDyldEnvironment = 0x00000027U;

void append_u32(std::uint32_t value, std::vector<unsigned char> &bytes) {
  bytes.push_back(static_cast<unsigned char>(value & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

[[nodiscard]] std::vector<unsigned char> string_command(
    std::uint32_t command,
    std::string_view value,
    std::uint32_t string_offset) {
  const std::size_t unaligned =
      static_cast<std::size_t>(string_offset) + value.size() + 1;
  const std::uint32_t size = static_cast<std::uint32_t>(
      (unaligned + 7U) & ~static_cast<std::size_t>(7U));
  std::vector<unsigned char> bytes;
  append_u32(command, bytes);
  append_u32(size, bytes);
  append_u32(string_offset, bytes);
  while (bytes.size() < string_offset) bytes.push_back(0);
  bytes.insert(bytes.end(), value.begin(), value.end());
  bytes.push_back(0);
  bytes.resize(size, 0);
  return bytes;
}

[[nodiscard]] bool write_macho(
    const std::filesystem::path &path,
    std::uint32_t file_type,
    std::vector<std::vector<unsigned char>> commands) {
  std::uint32_t command_bytes = 0;
  for (const std::vector<unsigned char> &command : commands) {
    command_bytes += static_cast<std::uint32_t>(command.size());
  }
  std::vector<unsigned char> bytes;
  append_u32(kMachOMagic64, bytes);
  append_u32(kCpuTypeArm64, bytes);
  append_u32(0, bytes); // CPU subtype is not a dependency-closure input.
  append_u32(file_type, bytes);
  append_u32(static_cast<std::uint32_t>(commands.size()), bytes);
  append_u32(command_bytes, bytes);
  append_u32(0, bytes); // Flags.
  append_u32(0, bytes); // Reserved.
  for (std::vector<unsigned char> &command : commands) {
    bytes.insert(bytes.end(), command.begin(), command.end());
  }
  std::ofstream output(path, std::ios::binary);
  output.write(
      reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  output.close();
  return output.good();
}

void test_relocatable_closure(TestState &state) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) /
      "draft-macho-dependency-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "bin", error);
  std::filesystem::create_directories(root / "lib", error);
  EXPECT(state, !error);
  if (error) return;

  const std::filesystem::path helper = root / "lib" / "libhelper.dylib";
  EXPECT(state, write_macho(
      helper,
      kMachODylib,
      {string_command(kIdDylib, "@rpath/libhelper.dylib", 24)}));
  const std::filesystem::path tool = root / "bin" / "tool";
  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {
          string_command(kRpath, "@loader_path/../lib", 12),
          string_command(kLoadDylib, "@rpath/libhelper.dylib", 24),
          string_command(kLoadDylib, "/usr/lib/libSystem.B.dylib", 24),
      }));

  draft::DiagnosticSink accepted_diagnostics;
  EXPECT(state, draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      accepted_diagnostics));
  EXPECT(state, !accepted_diagnostics.has_errors());

  // A compiler runtime is itself a closure entry and therefore must be a
  // dylib, not an executable that merely has an acceptable dependency list.
  draft::DiagnosticSink dylib_entry_diagnostics;
  EXPECT(state, draft::validate_macho_dylib_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&helper, 1),
      dylib_entry_diagnostics));
  EXPECT(state, !dylib_entry_diagnostics.has_errors());
  draft::DiagnosticSink wrong_entry_type_diagnostics;
  EXPECT(state, !draft::validate_macho_dylib_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      wrong_entry_type_diagnostics));
  EXPECT(state, wrong_entry_type_diagnostics.has_errors());

  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {string_command(
          kLoadDylib,
          "/opt/homebrew/opt/llvm/lib/libLLVM.dylib",
          24)}));
  draft::DiagnosticSink ambient_diagnostics;
  EXPECT(state, !draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      ambient_diagnostics));
  EXPECT(state, ambient_diagnostics.has_errors());

  EXPECT(state, write_macho(
      helper,
      kMachODylib,
      {string_command(
          kIdDylib,
          "/opt/homebrew/opt/llvm/lib/libhelper.dylib",
          24)}));
  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {
          string_command(kRpath, "@loader_path/../lib", 12),
          string_command(kLoadDylib, "@rpath/libhelper.dylib", 24),
      }));
  draft::DiagnosticSink id_diagnostics;
  EXPECT(state, !draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      id_diagnostics));
  EXPECT(state, id_diagnostics.has_errors());

  // Runpaths are part of the dependency search policy even if a particular
  // fixture has no @rpath load. An ambient directory could change which image
  // a future dependency resolves to, so it is rejected on its own.
  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {string_command(kRpath, "/opt/homebrew/lib", 12)}));
  draft::DiagnosticSink runpath_diagnostics;
  EXPECT(state, !draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      runpath_diagnostics));
  EXPECT(state, runpath_diagnostics.has_errors());

  // A relocatable spelling is not sufficient by itself. It must name exactly
  // one existing regular file inside the selected tree.
  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {
          string_command(kRpath, "@loader_path/../lib", 12),
          string_command(kLoadDylib, "@rpath/libmissing.dylib", 24),
      }));
  draft::DiagnosticSink missing_diagnostics;
  EXPECT(state, !draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      missing_diagnostics));
  EXPECT(state, missing_diagnostics.has_errors());

  // The executable's loader is also an external runtime input. The sealed
  // system path is accepted, while a package-manager loader and an embedded
  // DYLD_* assignment are both ambient search mechanisms.
  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {string_command(kLoadDylinker, "/usr/lib/dyld", 12)}));
  draft::DiagnosticSink system_loader_diagnostics;
  EXPECT(state, draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      system_loader_diagnostics));
  EXPECT(state, !system_loader_diagnostics.has_errors());

  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {string_command(
          kLoadDylinker,
          "/opt/homebrew/lib/dyld",
          12)}));
  draft::DiagnosticSink ambient_loader_diagnostics;
  EXPECT(state, !draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      ambient_loader_diagnostics));
  EXPECT(state, ambient_loader_diagnostics.has_errors());

  EXPECT(state, write_macho(
      tool,
      kMachOExecute,
      {string_command(kDyldEnvironment, "DYLD_LIBRARY_PATH=/tmp", 12)}));
  draft::DiagnosticSink dyld_environment_diagnostics;
  EXPECT(state, !draft::validate_macho_dependency_closure(
      root,
      std::span<const std::filesystem::path>(&tool, 1),
      dyld_environment_diagnostics));
  EXPECT(state, dyld_environment_diagnostics.has_errors());

  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  TestState state;
  test_relocatable_closure(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " Mach-O dependency expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all Mach-O dependency tests passed\n";
  return EXIT_SUCCESS;
}
