// External-input content-tree identity and unsafe-entry rejection tests.

#include "base/content_tree.h"

#include "source/diagnostic.h"

#include "test_directory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "content_tree_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

struct TemporaryTrees {
  draft::test::TemporaryDirectory directory{"draft-content-tree-test"};
  std::filesystem::path root;

  TemporaryTrees() {
    root = directory.path();
  }
};

bool make_tree(
    const std::filesystem::path &root,
    bool reverse_creation,
    std::error_code &error) {
  if (reverse_creation) {
    std::filesystem::create_directories(root / "lib", error);
    if (error) return false;
    write_file(root / "lib" / "data", "library bytes\n");
    std::filesystem::create_directories(root / "bin", error);
  } else {
    std::filesystem::create_directories(root / "bin", error);
    if (error) return false;
    std::filesystem::create_directories(root / "lib", error);
    if (error) return false;
    write_file(root / "lib" / "data", "library bytes\n");
  }
  if (error) return false;
  write_file(root / "bin" / "tool", "tool bytes\n");
  std::filesystem::permissions(
      root / "bin" / "tool",
      std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::add,
      error);
  if (error) return false;
  std::filesystem::create_symlink("bin/tool", root / "current", error);
  return !error;
}

bool digest_tree(
    const std::filesystem::path &root,
    draft::Sha256Digest &digest,
    draft::DiagnosticSink &diagnostics) {
  return draft::hash_content_tree(root, digest, diagnostics);
}

void test_relocation_and_changes(TestState &state) {
  TemporaryTrees temporary;
  const std::filesystem::path first = temporary.root / "physical-one";
  const std::filesystem::path second = temporary.root / "different-name";
  std::error_code error;
  EXPECT(state, make_tree(first, false, error));
  EXPECT(state, !error);
  error.clear();
  EXPECT(state, make_tree(second, true, error));
  EXPECT(state, !error);
  if (error) return;

  draft::Sha256Digest first_digest;
  draft::Sha256Digest second_digest;
  draft::DiagnosticSink first_diagnostics;
  draft::DiagnosticSink second_diagnostics;
  EXPECT(state, digest_tree(first, first_digest, first_diagnostics));
  EXPECT(state, digest_tree(second, second_digest, second_diagnostics));
  EXPECT(state, !first_diagnostics.has_errors());
  EXPECT(state, !second_diagnostics.has_errors());
  EXPECT(state, first_digest == second_digest);

  write_file(second / "lib" / "data", "changed library bytes\n");
  draft::Sha256Digest content_changed;
  draft::DiagnosticSink content_diagnostics;
  EXPECT(state, digest_tree(second, content_changed, content_diagnostics));
  EXPECT(state, content_changed != first_digest);

  write_file(second / "lib" / "data", "library bytes\n");
  error.clear();
  std::filesystem::permissions(
      second / "lib" / "data",
      std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::add,
      error);
  EXPECT(state, !error);
  draft::Sha256Digest permissions_changed;
  draft::DiagnosticSink permissions_diagnostics;
  EXPECT(state, digest_tree(second, permissions_changed, permissions_diagnostics));
  EXPECT(state, permissions_changed != first_digest);

  error.clear();
  std::filesystem::permissions(
      second / "lib" / "data",
      std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::remove,
      error);
  std::filesystem::remove(second / "current", error);
  error.clear();
  std::filesystem::create_symlink("./bin/tool", second / "current", error);
  EXPECT(state, !error);
  draft::Sha256Digest spelling_changed;
  draft::DiagnosticSink spelling_diagnostics;
  EXPECT(state, digest_tree(second, spelling_changed, spelling_diagnostics));
  EXPECT(state, spelling_changed != first_digest);
}

void test_file_root_and_unsafe_links(TestState &state) {
  TemporaryTrees temporary;
  write_file(temporary.root / "one.bin", "same bytes");
  write_file(temporary.root / "two.bin", "same bytes");

  draft::Sha256Digest first;
  draft::Sha256Digest second;
  draft::DiagnosticSink first_diagnostics;
  draft::DiagnosticSink second_diagnostics;
  EXPECT(state, digest_tree(temporary.root / "one.bin", first, first_diagnostics));
  EXPECT(state, digest_tree(temporary.root / "two.bin", second, second_diagnostics));
  EXPECT(state, first == second);

  std::error_code error;
  const std::filesystem::path escaping = temporary.root / "escaping";
  std::filesystem::create_directories(escaping / "inside", error);
  EXPECT(state, !error);
  std::filesystem::create_symlink("../../outside", escaping / "inside" / "bad", error);
  EXPECT(state, !error);
  draft::Sha256Digest ignored;
  draft::DiagnosticSink escaping_diagnostics;
  EXPECT(state, !digest_tree(escaping, ignored, escaping_diagnostics));
  EXPECT(state, escaping_diagnostics.error_count() == 1);

  const std::filesystem::path absolute = temporary.root / "absolute";
  error.clear();
  std::filesystem::create_directories(absolute, error);
  EXPECT(state, !error);
  std::filesystem::create_symlink("/tmp", absolute / "bad", error);
  EXPECT(state, !error);
  draft::DiagnosticSink absolute_diagnostics;
  EXPECT(state, !digest_tree(absolute, ignored, absolute_diagnostics));
  EXPECT(state, absolute_diagnostics.error_count() == 1);

  const std::filesystem::path root_link = temporary.root / "root-link";
  error.clear();
  std::filesystem::create_symlink("one.bin", root_link, error);
  EXPECT(state, !error);
  draft::DiagnosticSink root_link_diagnostics;
  EXPECT(state, !digest_tree(root_link, ignored, root_link_diagnostics));
  EXPECT(state, root_link_diagnostics.error_count() == 1);
}

} // namespace

int main() {
  TestState state;
  test_relocation_and_changes(state);
  test_file_root_and_unsafe_links(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " content-tree test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all content-tree tests passed\n";
  return EXIT_SUCCESS;
}
