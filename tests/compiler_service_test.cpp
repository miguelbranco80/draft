// End-to-end tests for the opaque compiler service consumed by Turbo Draft.
//
// These fixtures exercise the public C ABI rather than reaching into the UI:
// create-time path validation, production-lexer spans, successful and failed
// source overlays, last-good retention through a failed attempt, topology
// fallback after adding an import, and continuation into the native linker. The
// source checkout is never mutated; every workspace and native artifact belongs
// to one process-unique temporary directory or the session's private scratch.

#include "ide/service.h"

#include "test_directory.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (condition)
      return;
    ++failures;
    std::cerr << "compiler_service_test.cpp:" << line
              << ": expectation failed: " << expression << '\n';
  }
};

#define EXPECT(state, expression)                                              \
  (state).expect((expression), #expression, __LINE__)

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    std::cerr << "cannot create compiler-service fixture: " << error.message()
              << '\n';
    std::exit(EXIT_FAILURE);
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    std::cerr << "cannot write compiler-service fixture: " << path << '\n';
    std::exit(EXIT_FAILURE);
  }
}

[[nodiscard]] std::uint8_t native_target_ordinal() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return 0;
#elif defined(__linux__) && defined(__aarch64__)
  return 1;
#elif defined(__linux__) && defined(__x86_64__)
  return 2;
#elif defined(_WIN32) && defined(_M_X64)
  return 3;
#else
#error "compiler service test requires an implemented native host"
#endif
}

[[nodiscard]] DraftCompilerServiceConfiguration
configuration(const std::string &workspace, const std::string &root,
              const std::string &source) {
  return {
      workspace.data(),
      workspace.size(),
      root.data(),
      root.size(),
      source.data(),
      source.size(),
      native_target_ordinal(),
  };
}

[[nodiscard]] DraftCompilerServiceOverlay overlay(const std::string &path,
                                                  const std::string &source) {
  return {
      path.data(),
      path.size(),
      source.data(),
      source.size(),
  };
}

void check_source(void *session, const std::filesystem::path &path,
                  std::string &source, DraftCompilerServiceResult &result) {
  const std::string path_text = path.string();
  const DraftCompilerServiceOverlay source_overlay = overlay(path_text, source);
  draft_compiler_session_check(session, &source_overlay, 1, 0, &result);
}

void test_service_transactions_and_native_build(TestState &state) {
  draft::test::TemporaryDirectory temporary{"draft-compiler-service-test"};
  const std::filesystem::path workspace = temporary.path() / "workspace";
  const std::filesystem::path app = workspace / "app";
  const std::filesystem::path library = workspace / "lib";
  const std::string disk_source = "package app\n"
                                  "main :: proc() -> int {\n"
                                  "    return 0\n"
                                  "}\n";
  write_file(app / "package.draft", disk_source);
  write_file(library / "package.draft", "package lib\n"
                                        "pub answer :: proc() -> int {\n"
                                        "    return 42\n"
                                        "}\n");
  write_file(workspace / "tool" / "package.draft", "package tool\n"
                                                   "main :: proc() -> int {\n"
                                                   "    return 7\n"
                                                   "}\n");

  const std::string workspace_text = workspace.string();
  const std::string root_text = "app";
  const std::string source_name = "package.draft";
  const DraftCompilerServiceConfiguration input =
      configuration(workspace_text, root_text, source_name);
  std::array<std::uint8_t, 256> create_error{};
  void *session = draft_compiler_session_create(&input, create_error.data(),
                                                create_error.size());
  EXPECT(state, session != nullptr);
  if (session == nullptr) {
    std::cerr << reinterpret_cast<const char *>(create_error.data()) << '\n';
    return;
  }

  std::array<std::uint8_t, 4096> path_bytes{};
  const std::size_t path_size = draft_compiler_session_copy_source_path(
      session, path_bytes.data(), path_bytes.size());
  const std::filesystem::path canonical_source =
      std::filesystem::canonical(app / source_name);
  EXPECT(state, path_size == canonical_source.string().size());
  EXPECT(state,
         std::string_view(reinterpret_cast<const char *>(path_bytes.data()),
                          path_size) == canonical_source.string());

  std::string valid =
      "package app\n"
      "// classic syntax colors come from the production lexer\n"
      "Message :: \"hello\"\n"
      "Answer :: 41\n"
      "main :: proc() -> int {\n"
      "    return Answer + 1\n"
      "}\n";
  DraftCompilerServiceResult checked{};
  check_source(session, canonical_source, valid, checked);
  if (checked.success == 0) {
    std::array<std::uint8_t, 4096> bytes{};
    draft_compiler_session_copy_diagnostics(session, bytes.data(),
                                            bytes.size());
    std::cerr << reinterpret_cast<const char *>(bytes.data());
  }
  EXPECT(state, checked.success == 1);
  EXPECT(state, checked.diagnostic_count == 0);
  EXPECT(state, checked.span_count != 0);

  std::array<bool, 7> observed_styles{};
  for (std::size_t index = 0; index < checked.span_count; ++index) {
    DraftCompilerServiceSpan span{};
    draft_compiler_session_span(session, index, &span);
    EXPECT(state, span.start < span.end);
    EXPECT(state, span.end <= valid.size());
    if (span.kind < observed_styles.size())
      observed_styles[span.kind] = true;
  }
  EXPECT(state, observed_styles[1]); // keyword
  EXPECT(state, observed_styles[2]); // comment
  EXPECT(state, observed_styles[3]); // string
  EXPECT(state, observed_styles[4]); // number
  EXPECT(state, observed_styles[5]); // declaration

  const std::array<std::string_view, 5> tooling_needles{
      "workspace:app", "Answer", "main -> Answer", "main", "none",
  };
  for (std::uint8_t section = 0; section < tooling_needles.size(); ++section) {
    std::array<std::uint8_t, 8192> text{};
    const std::size_t size = draft_compiler_session_copy_tooling_section(
        session, section, text.data(), text.size());
    EXPECT(state, size != 0);
    EXPECT(state,
           std::string_view(reinterpret_cast<const char *>(text.data()),
                            std::min(size, text.size() - 1))
                   .find(tooling_needles[section]) != std::string_view::npos);
  }

  // Structured package rows are paired with the same checked graph as the
  // textual sections. The one-package program has one root row and no import
  // child; out-of-range inspection is a deterministic zero record.
  EXPECT(state, draft_compiler_session_package_row_count(session) == 1);
  DraftCompilerServicePackageRow initial_package{};
  draft_compiler_session_package_row(session, 0, &initial_package);
  EXPECT(state, initial_package.package_index == 0);
  EXPECT(state, initial_package.kind == 0 && initial_package.depth == 0);
  EXPECT(state, initial_package.root == 1);
  EXPECT(state, initial_package.has_children == 0);
  DraftCompilerServicePackageRow absent_package{
      99, 99, 99, 99, 99,
  };
  draft_compiler_session_package_row(session, 99, &absent_package);
  EXPECT(state, absent_package.package_index == 0);
  EXPECT(state, absent_package.kind == 0 && absent_package.depth == 0);
  EXPECT(state, absent_package.root == 0 && absent_package.has_children == 0);

  std::string invalid = "package app\n"
                        "main :: proc() -> int {\n"
                        "    return missing_name\n"
                        "}\n";
  DraftCompilerServiceResult rejected{};
  check_source(session, canonical_source, invalid, rejected);
  EXPECT(state, rejected.success == 0);
  EXPECT(state, rejected.diagnostic_count != 0);
  std::array<std::uint8_t, 2048> diagnostic_bytes{};
  const std::size_t diagnostic_size = draft_compiler_session_copy_diagnostics(
      session, diagnostic_bytes.data(), diagnostic_bytes.size());
  EXPECT(state, diagnostic_size != 0);
  EXPECT(state, std::string_view(
                    reinterpret_cast<const char *>(diagnostic_bytes.data()),
                    std::min(diagnostic_size, diagnostic_bytes.size() - 1))
                        .find("missing_name") != std::string_view::npos);

  // A rejected current buffer changes diagnostics and syntax ranges only. The
  // declaration view is still the exact projection built from the previous
  // successful graph; this proves last-good retention through the public ABI
  // without exporting the C++ CompilerSession implementation.
  std::array<std::uint8_t, 8192> retained_declarations{};
  const std::size_t retained_size = draft_compiler_session_copy_tooling_section(
      session, 1, retained_declarations.data(), retained_declarations.size());
  EXPECT(state, retained_size != 0);
  EXPECT(state,
         std::string_view(
             reinterpret_cast<const char *>(retained_declarations.data()),
             std::min(retained_size, retained_declarations.size() - 1))
                 .find("Answer") != std::string_view::npos);
  EXPECT(state, draft_compiler_session_package_row_count(session) == 1);

  // Adding an import cannot preserve PackageIds through the overlay path. A
  // successful result therefore proves that the service used its explicit
  // fresh-topology fallback rather than corrupting the retained graph.
  std::string topology_change = "package app\n"
                                "import lib\n"
                                "main :: proc() -> int {\n"
                                "    return lib.answer()\n"
                                "}\n";
  DraftCompilerServiceResult reloaded{};
  check_source(session, canonical_source, topology_change, reloaded);
  if (reloaded.success == 0) {
    std::array<std::uint8_t, 4096> bytes{};
    draft_compiler_session_copy_diagnostics(session, bytes.data(),
                                            bytes.size());
    std::cerr << reinterpret_cast<const char *>(bytes.data());
  }
  EXPECT(state, reloaded.success == 1);
  EXPECT(state, reloaded.diagnostic_count == 0);

  // The deterministic row order is package followed by its authored imports.
  // Import rows name their parent package index and carry depth one, allowing a
  // Draft client to filter expansion without parsing the formatted tooling
  // text or recreating semantic edges.
  EXPECT(state, draft_compiler_session_package_row_count(session) == 3);
  DraftCompilerServicePackageRow app_package{};
  DraftCompilerServicePackageRow app_import{};
  DraftCompilerServicePackageRow lib_package{};
  draft_compiler_session_package_row(session, 0, &app_package);
  draft_compiler_session_package_row(session, 1, &app_import);
  draft_compiler_session_package_row(session, 2, &lib_package);
  EXPECT(state, app_package.kind == 0 && app_package.depth == 0);
  EXPECT(state, app_package.root == 1 && app_package.has_children == 1);
  EXPECT(state, app_import.kind == 1 && app_import.depth == 1);
  EXPECT(state, app_import.package_index == app_package.package_index);
  EXPECT(state, lib_package.kind == 0 && lib_package.depth == 0);
  std::array<std::uint8_t, 256> package_label{};
  const std::size_t package_label_size =
      draft_compiler_session_copy_package_row_text(
          session, 1, package_label.data(), package_label.size());
  EXPECT(state, package_label_size != 0);
  EXPECT(
      state,
      std::string_view(reinterpret_cast<const char *>(package_label.data()),
                       std::min(package_label_size, package_label.size() - 1))
              .find("lib -> workspace:lib") != std::string_view::npos);

  EXPECT(state, draft_compiler_session_source_count(session) == 2);
  std::array<std::uint8_t, 256> source_name_bytes{};
  EXPECT(state, draft_compiler_session_copy_source_name(
                    session, 0, source_name_bytes.data(),
                    source_name_bytes.size()) == 17);
  EXPECT(state, std::string_view(reinterpret_cast<const char *>(
                    source_name_bytes.data())) == "app/package.draft");

  // Both unsaved files enter one semantic transaction. The app refers to a
  // declaration that exists only in the library overlay, proving the service
  // does not check merely the active editor buffer.
  std::string multi_app = "package app\n"
                          "import lib\n"
                          "main :: proc() -> int {\n"
                          "    return lib.answer_two()\n"
                          "}\n";
  std::string multi_library = "package lib\n"
                              "pub answer_two :: proc() -> int {\n"
                              "    return 42\n"
                              "}\n";
  const std::filesystem::path canonical_library =
      std::filesystem::canonical(library / source_name);
  const std::string canonical_source_text = canonical_source.string();
  const std::string canonical_library_text = canonical_library.string();
  const std::array<DraftCompilerServiceOverlay, 2> multi_overlays{
      overlay(canonical_source_text, multi_app),
      overlay(canonical_library_text, multi_library),
  };
  DraftCompilerServiceResult multi_checked{};
  draft_compiler_session_check(session, multi_overlays.data(),
                               multi_overlays.size(), 1, &multi_checked);
  EXPECT(state, multi_checked.success == 1);

  DraftCompilerServiceResult built{};
  draft_compiler_session_build(session, multi_overlays.data(),
                               multi_overlays.size(), 0, &built);
  EXPECT(state, built.success == 1);
  EXPECT(state, built.diagnostic_count == 0);
  std::array<std::uint8_t, 4096> artifact_bytes{};
  const std::size_t artifact_size = draft_compiler_session_copy_artifact_path(
      session, artifact_bytes.data(), artifact_bytes.size());
  EXPECT(state, artifact_size != 0);
  const std::filesystem::path artifact_path{
      std::string(reinterpret_cast<const char *>(artifact_bytes.data()),
                  std::min(artifact_size, artifact_bytes.size() - 1))};
  EXPECT(state, std::filesystem::is_regular_file(artifact_path));

  std::ifstream disk(app / "package.draft", std::ios::binary);
  const std::string after_build{std::istreambuf_iterator<char>(disk),
                                std::istreambuf_iterator<char>()};
  EXPECT(state, after_build == disk_source);

  // Build must never publish an artifact for stale retained semantics. The
  // previous file may still exist in private scratch, but clearing the service
  // path makes it unreachable to the Draft-owned Run operation.
  DraftCompilerServiceResult invalid_build{};
  const DraftCompilerServiceOverlay invalid_overlay =
      overlay(canonical_source_text, invalid);
  draft_compiler_session_build(session, &invalid_overlay, 1, 0, &invalid_build);
  EXPECT(state, invalid_build.success == 0);
  EXPECT(state,
         draft_compiler_session_copy_artifact_path(
             session, artifact_bytes.data(), artifact_bytes.size()) == 0);

  EXPECT(state, draft_compiler_session_root_count(session) == 2);
  EXPECT(state, draft_compiler_session_selected_root(session) == 0);
  std::array<std::uint8_t, 64> root_name{};
  EXPECT(state, draft_compiler_session_copy_root_name(
                    session, 1, root_name.data(), root_name.size()) == 4);
  EXPECT(state, std::string_view(reinterpret_cast<const char *>(
                    root_name.data())) == "tool");
  EXPECT(state, draft_compiler_session_select_root(session, 1) == 1);
  EXPECT(state, draft_compiler_session_selected_root(session) == 1);
  std::string tool_source = "package tool\n"
                            "main :: proc() -> int {\n"
                            "    return 9\n"
                            "}\n";
  DraftCompilerServiceResult tool_checked{};
  const std::filesystem::path tool_path =
      std::filesystem::canonical(workspace / "tool" / source_name);
  check_source(session, tool_path, tool_source, tool_checked);
  EXPECT(state, tool_checked.success == 1);
  EXPECT(state, draft_compiler_session_select_root(session, 0) == 1);
  EXPECT(state, draft_compiler_session_source_count(session) == 2);

  const std::uint8_t original_target = draft_compiler_session_target(session);
  const std::uint8_t other_target =
      static_cast<std::uint8_t>((original_target + 1) % 4);
  EXPECT(state,
         draft_compiler_session_select_target(session, other_target) == 1);
  EXPECT(state, draft_compiler_session_target(session) == other_target);
  EXPECT(state,
         draft_compiler_session_select_target(session, original_target) == 1);
  EXPECT(state, draft_compiler_session_target(session) == original_target);

  // Workspace replacement is transactional behind the stable opaque handle.
  // A failed replacement leaves the complete old session selectable; a valid
  // workspace discovers its runnable root without a manifest selection.
  std::array<std::uint8_t, 256> workspace_error{};
  const std::string missing_workspace =
      (temporary.path() / "missing-workspace").string();
  EXPECT(state, draft_compiler_session_open_workspace(
                    session, missing_workspace.data(), missing_workspace.size(),
                    workspace_error.data(), workspace_error.size()) == 0);
  EXPECT(state, draft_compiler_session_root_count(session) == 2);

  const std::filesystem::path second_workspace =
      temporary.path() / "second-workspace";
  write_file(second_workspace / "program" / "package.draft",
             "package program\n"
             "main :: proc() -> int {\n"
             "    return 0\n"
             "}\n");
  const std::string second_workspace_text = second_workspace.string();
  const bool workspace_opened =
      draft_compiler_session_open_workspace(
          session, second_workspace_text.data(), second_workspace_text.size(),
          workspace_error.data(), workspace_error.size()) == 1;
  if (!workspace_opened)
    std::cerr << reinterpret_cast<const char *>(workspace_error.data()) << '\n';
  EXPECT(state, workspace_opened);
  EXPECT(state, draft_compiler_session_root_count(session) == 1);
  EXPECT(state, draft_compiler_session_source_count(session) == 1);
  std::array<std::uint8_t, 4096> workspace_bytes{};
  const std::size_t workspace_size = draft_compiler_session_copy_workspace_path(
      session, workspace_bytes.data(), workspace_bytes.size());
  const std::string canonical_second_workspace =
      std::filesystem::canonical(second_workspace).string();
  EXPECT(state, workspace_size == canonical_second_workspace.size());
  EXPECT(state, std::string_view(
                    reinterpret_cast<const char *>(workspace_bytes.data()),
                    workspace_size) == canonical_second_workspace);
  draft_compiler_session_destroy(session);
  EXPECT(state, !std::filesystem::exists(artifact_path.parent_path()));
}

void test_create_rejects_escaping_paths(TestState &state) {
  draft::test::TemporaryDirectory temporary{"draft-compiler-service-path-test"};
  const std::filesystem::path workspace = temporary.path() / "workspace";
  write_file(workspace / "app" / "package.draft",
             "package app\nmain :: proc() {\n}\n");
  const std::string workspace_text = workspace.string();
  const std::string root = "../outside";
  const std::string source = "package.draft";
  const DraftCompilerServiceConfiguration input =
      configuration(workspace_text, root, source);
  std::array<std::uint8_t, 128> error{};
  void *session =
      draft_compiler_session_create(&input, error.data(), error.size());
  EXPECT(state, session == nullptr);
  EXPECT(state, error[0] != 0);
  draft_compiler_session_destroy(session);
}

void test_create_discovers_root_when_omitted(TestState &state) {
  draft::test::TemporaryDirectory temporary{"draft-compiler-auto-root-test"};
  const std::filesystem::path workspace = temporary.path() / "workspace";
  write_file(workspace / "app" / "package.draft", "package app\n"
                                                  "main :: proc() -> int {\n"
                                                  "    return 0\n"
                                                  "}\n");
  const std::string workspace_text = workspace.string();
  const std::string root;
  const std::string source = "package.draft";
  const DraftCompilerServiceConfiguration input =
      configuration(workspace_text, root, source);
  std::array<std::uint8_t, 256> error{};
  void *session =
      draft_compiler_session_create(&input, error.data(), error.size());
  if (session == nullptr)
    std::cerr << reinterpret_cast<const char *>(error.data()) << '\n';
  EXPECT(state, session != nullptr);
  if (session != nullptr) {
    EXPECT(state, draft_compiler_session_root_count(session) == 1);
    EXPECT(state, draft_compiler_session_source_count(session) == 1);
  }
  draft_compiler_session_destroy(session);
}

} // namespace

int main() {
  TestState state;
  test_service_transactions_and_native_build(state);
  test_create_rejects_escaping_paths(state);
  test_create_discovers_root_when_omitted(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
