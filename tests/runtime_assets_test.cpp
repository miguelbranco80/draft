// Runtime-asset path validation without content pinning.

#include "backend/runtime_assets.h"

#include "source/diagnostic.h"

#include "test_directory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "runtime_assets_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryAssets {
  draft::test::TemporaryDirectory directory{"draft-runtime-assets-test"};
  std::filesystem::path root;

  TemporaryAssets() {
    root = directory.path();
  }
};

void write_file(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void test_file_and_directory_paths(TestState &state) {
  TemporaryAssets temporary;
  const std::filesystem::path first_file = temporary.root / "first.data";
  const std::filesystem::path second_file = temporary.root / "relocated.data";
  const std::filesystem::path first_directory = temporary.root / "first-tree";
  const std::filesystem::path second_directory = temporary.root / "second-tree";
  std::error_code error;
  std::filesystem::create_directories(first_directory / "nested", error);
  EXPECT(state, !error);
  std::filesystem::create_directories(second_directory / "nested", error);
  EXPECT(state, !error);
  write_file(first_file, "dictionary bytes\n");
  write_file(second_file, "dictionary bytes\n");
  write_file(first_directory / "nested" / "table", "table bytes\n");
  write_file(second_directory / "nested" / "table", "table bytes\n");

  std::vector<draft::RuntimeAssetInput> inputs{
      {"dictionary", first_file}, {"tables", first_directory}};
  std::vector<draft::VerifiedRuntimeAssetInput> verified;
  draft::DiagnosticSink inspect_diagnostics;
  EXPECT(state,
      draft::inspect_runtime_asset_inputs(
          inputs, verified, inspect_diagnostics));
  EXPECT(state, !inspect_diagnostics.has_errors());
  EXPECT(state, verified.size() == 2);
  if (verified.size() == 2) {
    EXPECT(state, verified[0].name == "dictionary");
    EXPECT(state, verified[0].path == std::filesystem::canonical(first_file));
    EXPECT(state, verified[1].name == "tables");
  }

  inputs[0].path = second_file;
  inputs[1].path = second_directory;
  draft::DiagnosticSink verify_diagnostics;
  EXPECT(state,
      draft::inspect_runtime_asset_inputs(
          inputs, verified, verify_diagnostics));
  EXPECT(state, !verify_diagnostics.has_errors());
  EXPECT(state, verified.size() == 2);
  if (verified.size() == 2) {
    EXPECT(state, verified[0].name == "dictionary");
    EXPECT(state, verified[0].path == std::filesystem::canonical(second_file));
    EXPECT(state, verified[1].name == "tables");
  }

  write_file(second_directory / "nested" / "table", "changed table\n");
  draft::DiagnosticSink changed_diagnostics;
  EXPECT(state,
      draft::inspect_runtime_asset_inputs(
          inputs, verified, changed_diagnostics));
  EXPECT(state, !changed_diagnostics.has_errors());
}

void test_invalid_mappings(TestState &state) {
  TemporaryAssets temporary;
  const std::filesystem::path file = temporary.root / "asset";
  write_file(file, "asset bytes\n");

  draft::RuntimeAssetInput input{"locale", file};
  std::vector<draft::VerifiedRuntimeAssetInput> verified;
  draft::DiagnosticSink inspect_diagnostics;
  EXPECT(state,
      draft::inspect_runtime_asset_inputs(
          std::span<const draft::RuntimeAssetInput>(&input, 1),
          verified,
          inspect_diagnostics));

  std::vector<draft::RuntimeAssetInput> duplicate{input, input};
  draft::DiagnosticSink duplicate_diagnostics;
  EXPECT(state,
      !draft::inspect_runtime_asset_inputs(
          duplicate, verified, duplicate_diagnostics));
  EXPECT(state, duplicate_diagnostics.has_errors());

  std::error_code error;
  const std::filesystem::path link = temporary.root / "asset-link";
  std::filesystem::create_symlink(file.filename(), link, error);
  EXPECT(state, !error);
  input.path = link;
  draft::DiagnosticSink link_diagnostics;
  EXPECT(state,
      !draft::inspect_runtime_asset_inputs(
          std::span<const draft::RuntimeAssetInput>(&input, 1),
          verified,
          link_diagnostics));
  EXPECT(state, link_diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_file_and_directory_paths(state);
  test_invalid_mappings(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " runtime-asset expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all runtime-asset tests passed\n";
  return EXIT_SUCCESS;
}
