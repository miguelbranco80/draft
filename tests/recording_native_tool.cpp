// Executable recording companion for the locked native-adapter test.
//
// Locked toolchain entries must be real self-contained Mach-O executables, so
// the old shell-script fixtures are intentionally insufficient. CMake builds
// this one tiny binary and the test copies it under each required tool name.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::filesystem::path temporary_root(
    const std::filesystem::path &executable) {
  return executable.parent_path().parent_path().parent_path();
}

void record_arguments(
    const std::filesystem::path &log,
    std::string_view marker,
    int argc,
    char **argv) {
  std::ofstream output(log, std::ios::binary | std::ios::app);
  output << marker << '\n';
  for (int index = 1; index < argc; ++index) output << argv[index] << '\n';
}

[[nodiscard]] bool has_argument(
    int argc,
    char **argv,
    std::string_view expected) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == expected) return true;
  }
  return false;
}

[[nodiscard]] std::string environment(std::string_view name) {
  const char *value = std::getenv(std::string(name).c_str());
  return value == nullptr ? "unset" : value;
}

int run_clang(
    const std::filesystem::path &log,
    int argc,
    char **argv) {
  if (has_argument(argc, argv, "--version")) {
    std::cout << "clang version 22.1.0\n";
    return EXIT_SUCCESS;
  }
  record_arguments(log, "-- command --", argc, argv);
  {
    std::ofstream output(log, std::ios::binary | std::ios::app);
    output << "ENV:" << environment("PATH") << '|'
           << environment("SDKROOT") << '|'
           << environment("CPATH") << '|'
           << environment("LIBRARY_PATH") << '\n';
  }
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view(argv[index]) == "-o") {
      std::ofstream(argv[index + 1], std::ios::binary).close();
    }
  }
  return EXIT_SUCCESS;
}

int run_archiver(
    const std::filesystem::path &log,
    int argc,
    char **argv) {
  record_arguments(log, "-- archive --", argc, argv);
  if (argc >= 3) std::ofstream(argv[2], std::ios::binary).close();
  return EXIT_SUCCESS;
}

int run_dsymutil(
    const std::filesystem::path &log,
    int argc,
    char **argv) {
  record_arguments(log, "-- dsymutil --", argc, argv);
  std::filesystem::path input;
  std::filesystem::path output;
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "-o" && index + 1 < argc) {
      output = argv[++index];
    } else {
      input = argv[index];
    }
  }
  if (input.empty() || output.empty()) return EXIT_FAILURE;
  const std::filesystem::path dwarf =
      output / "Contents" / "Resources" / "DWARF";
  const std::filesystem::path relocations =
      output / "Contents" / "Resources" / "Relocations" / "aarch64";
  std::error_code error;
  std::filesystem::create_directories(dwarf, error);
  std::filesystem::create_directories(relocations, error);
  if (error) return EXIT_FAILURE;
  std::ofstream(output / "Contents" / "Info.plist", std::ios::binary).close();
  std::ofstream(dwarf / input.filename(), std::ios::binary).close();
  std::ofstream relocation(
      relocations / "program.yml", std::ios::binary);
  relocation << "binary-path: '" << input.string() << "'\n";
  return relocation.good() ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 0 || argv == nullptr || argv[0] == nullptr) {
    return EXIT_FAILURE;
  }
  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::canonical(argv[0], error);
  if (error) return EXIT_FAILURE;
  const std::filesystem::path log =
      temporary_root(executable) / "locked-arguments.log";
  const std::string name = executable.filename().string();
  if (name == "clang") return run_clang(log, argc, argv);
  if (name == "llvm-ar") return run_archiver(log, argc, argv);
  if (name == "dsymutil") return run_dsymutil(log, argc, argv);
  if (name == "ld" || name == "ld-classic") return EXIT_SUCCESS;
  return EXIT_FAILURE;
}
