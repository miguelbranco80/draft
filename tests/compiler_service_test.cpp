// End-to-end tests for the opaque compiler service consumed by DraftIDE.
//
// These fixtures exercise the public C ABI rather than reaching into the UI:
// create-time path validation, production-lexer spans, successful and failed
// source overlays, current-graph semantic navigation, last-good retention
// through a failed attempt, topology fallback after adding an import, and
// continuation into the native linker. The source checkout is never mutated;
// every workspace and native artifact belongs to one process-unique temporary
// directory or the session's private scratch.

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

[[nodiscard]] std::string_view native_target_name() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return "aarch64-macos";
#elif defined(__linux__) && defined(__aarch64__)
  return "aarch64-linux";
#elif defined(__linux__) && defined(__x86_64__)
  return "x86_64-linux";
#elif defined(_WIN32) && defined(_M_X64)
  return "x86_64-windows";
#else
#error "compiler service test requires an implemented native host"
#endif
}

[[nodiscard]] std::string_view target_name(std::uint8_t ordinal) {
  switch (ordinal) {
  case 0:
    return "aarch64-macos";
  case 1:
    return "aarch64-linux";
  case 2:
    return "x86_64-linux";
  case 3:
    return "x86_64-windows";
  }
  return {};
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
      0,
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
  const std::string canonical_source_text = canonical_source.string();
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

  // Foreground colorization is intentionally a lexical-only service call. It
  // accepts incomplete editor bytes, replaces the span table, and preserves
  // diagnostics plus the retained checked WorkspaceGraph. This is the
  // contract that keeps package semantics off DraftIDE's keystroke path.
  const std::string incomplete =
      "package app\nmain :: proc() { \"unterminated\n";
  const DraftCompilerServiceOverlay incomplete_overlay =
      overlay(canonical_source_text, incomplete);
  DraftCompilerServiceSyntaxResult colored{};
  draft_compiler_session_colorize(session, &incomplete_overlay, &colored);
  EXPECT(state, colored.success == 1);
  EXPECT(state, colored.span_count != 0);
  EXPECT(state,
         draft_compiler_session_copy_diagnostics(session, nullptr, 0) == 0);
  EXPECT(state, draft_compiler_session_package_row_count(session) == 1);
  for (std::size_t index = 0; index < colored.span_count; ++index) {
    DraftCompilerServiceSpan span{};
    draft_compiler_session_span(session, index, &span);
    EXPECT(state, span.start <= span.end);
    EXPECT(state, span.end <= incomplete.size());
  }

  // Navigation consumes the same successful graph and exact overlay bytes as
  // checking. The definition range selects only the authored declaration name,
  // while the usage table contains the resolved reference in main. Source text
  // is copied from SourceManager rather than reread from the deliberately stale
  // file on disk.
  const std::size_t answer_use = valid.find("Answer +");
  EXPECT(state, answer_use != std::string::npos);
  EXPECT(state, draft_compiler_session_prepare_navigation(
                    session, canonical_source_text.data(),
                    canonical_source_text.size(), answer_use + 1) == 5);
  DraftCompilerServiceNavigationLocation answer_definition{};
  draft_compiler_session_navigation_definition(session, &answer_definition);
  const std::size_t answer_declaration = valid.find("Answer ::");
  EXPECT(state, answer_definition.start == answer_declaration);
  EXPECT(state, answer_definition.end == answer_declaration + 6);
  EXPECT(state, answer_definition.line == 4);
  EXPECT(state, answer_definition.column == 1);
  EXPECT(state, draft_compiler_session_navigation_usage_count(session) == 1);
  DraftCompilerServiceNavigationLocation answer_usage{};
  draft_compiler_session_navigation_usage(session, 0, &answer_usage);
  EXPECT(state, answer_usage.start == answer_use);
  EXPECT(state, answer_usage.end == answer_use + 6);
  EXPECT(state, answer_usage.line == 6);
  EXPECT(state, answer_usage.column == 12);
  std::array<std::uint8_t, 4096> navigation_bytes{};
  const std::size_t navigation_path_size =
      draft_compiler_session_copy_navigation_source_path(
          session, answer_definition.source, navigation_bytes.data(),
          navigation_bytes.size());
  EXPECT(state, navigation_path_size == canonical_source_text.size());
  EXPECT(state, std::string_view(
                    reinterpret_cast<const char *>(navigation_bytes.data()),
                    navigation_path_size) == canonical_source_text);
  navigation_bytes.fill(0);
  const std::size_t navigation_text_size =
      draft_compiler_session_copy_navigation_source_text(
          session, answer_definition.source, navigation_bytes.data(),
          navigation_bytes.size());
  EXPECT(state, navigation_text_size == valid.size());
  EXPECT(state, std::string_view(
                    reinterpret_cast<const char *>(navigation_bytes.data()),
                    navigation_text_size) == valid);
  EXPECT(state, draft_compiler_session_navigation_source_editable(
                    session, answer_definition.source) == 1);

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
  EXPECT(state, rejected.elapsed_nanoseconds != 0);
  std::array<std::uint8_t, 2048> diagnostic_bytes{};
  const std::size_t diagnostic_size = draft_compiler_session_copy_diagnostics(
      session, diagnostic_bytes.data(), diagnostic_bytes.size());
  EXPECT(state, diagnostic_size != 0);
  EXPECT(state, std::string_view(
                    reinterpret_cast<const char *>(diagnostic_bytes.data()),
                    std::min(diagnostic_size, diagnostic_bytes.size() - 1))
                        .find("missing_name") != std::string_view::npos);

  // Diagnostics are also exposed as deterministic activatable rows. The row
  // keeps the exact rejected overlay bytes rather than rereading the older
  // source on disk, so a Draft client can open and select the compiler's
  // half-open range without parsing the rendered diagnostic text.
  const std::size_t diagnostic_row_count =
      draft_compiler_session_diagnostic_row_count(session);
  EXPECT(state, diagnostic_row_count != 0);
  DraftCompilerServiceDiagnosticRow diagnostic_row{};
  draft_compiler_session_diagnostic_row(session, 0, &diagnostic_row);
  const std::size_t missing_name = invalid.find("missing_name");
  EXPECT(state, missing_name != std::string::npos);
  EXPECT(state, diagnostic_row.start == missing_name);
  EXPECT(state, diagnostic_row.end == missing_name + 12);
  EXPECT(state, diagnostic_row.line == 3);
  EXPECT(state, diagnostic_row.column == 12);
  EXPECT(state, diagnostic_row.severity == 0);
  EXPECT(state, diagnostic_row.navigable == 1);
  EXPECT(state, diagnostic_row.editable == 1);
  std::array<std::uint8_t, 4096> diagnostic_row_bytes{};
  const std::size_t diagnostic_label_size =
      draft_compiler_session_copy_diagnostic_row_text(
          session, 0, diagnostic_row_bytes.data(),
          diagnostic_row_bytes.size());
  EXPECT(state, diagnostic_label_size != 0);
  EXPECT(state,
         std::string_view(
             reinterpret_cast<const char *>(diagnostic_row_bytes.data()),
             diagnostic_label_size)
                 .find("error: unknown name 'missing_name'") !=
             std::string_view::npos);
  diagnostic_row_bytes.fill(0);
  const std::size_t diagnostic_path_size =
      draft_compiler_session_copy_diagnostic_source_path(
          session, 0, diagnostic_row_bytes.data(),
          diagnostic_row_bytes.size());
  EXPECT(state, diagnostic_path_size == canonical_source_text.size());
  EXPECT(state,
         std::string_view(
             reinterpret_cast<const char *>(diagnostic_row_bytes.data()),
             diagnostic_path_size) == canonical_source_text);
  diagnostic_row_bytes.fill(0);
  const std::size_t diagnostic_source_size =
      draft_compiler_session_copy_diagnostic_source_text(
          session, 0, diagnostic_row_bytes.data(),
          diagnostic_row_bytes.size());
  EXPECT(state, diagnostic_source_size == invalid.size());
  EXPECT(state,
         std::string_view(
             reinterpret_cast<const char *>(diagnostic_row_bytes.data()),
             diagnostic_source_size) == invalid);

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

  // Textual tooling remains available from last-good for diagnostics, but
  // source navigation must reject it because the visible editor bytes failed
  // their latest check. The rejected request also clears the previous rows.
  EXPECT(state, draft_compiler_session_prepare_navigation(
                    session, canonical_source_text.data(),
                    canonical_source_text.size(), answer_use) == 1);
  EXPECT(state, draft_compiler_session_navigation_usage_count(session) == 0);

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
  EXPECT(state, draft_compiler_session_diagnostic_row_count(session) == 0);

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
  const std::string canonical_library_text = canonical_library.string();
  const std::array<DraftCompilerServiceOverlay, 2> multi_overlays{
      overlay(canonical_source_text, multi_app),
      overlay(canonical_library_text, multi_library),
  };
  DraftCompilerServiceResult multi_checked{};
  draft_compiler_session_check(session, multi_overlays.data(),
                               multi_overlays.size(), 1, &multi_checked);
  EXPECT(state, multi_checked.success == 1);

  // Imported member navigation follows the app-local proxy to the library's
  // canonical public declaration. The returned path and range are sufficient
  // for DraftIDE to open the library buffer and select the exact name without
  // parsing a textual compiler dump.
  const std::size_t imported_use = multi_app.find("answer_two");
  EXPECT(state, imported_use != std::string::npos);
  EXPECT(state, draft_compiler_session_prepare_navigation(
                    session, canonical_source_text.data(),
                    canonical_source_text.size(), imported_use + 2) == 5);
  DraftCompilerServiceNavigationLocation imported_definition{};
  draft_compiler_session_navigation_definition(session, &imported_definition);
  navigation_bytes.fill(0);
  const std::size_t imported_path_size =
      draft_compiler_session_copy_navigation_source_path(
          session, imported_definition.source, navigation_bytes.data(),
          navigation_bytes.size());
  EXPECT(state, std::string_view(
                    reinterpret_cast<const char *>(navigation_bytes.data()),
                    imported_path_size) == canonical_library_text);
  EXPECT(state,
         imported_definition.start == multi_library.find("answer_two ::"));
  EXPECT(state, imported_definition.end == imported_definition.start + 10);
  EXPECT(state, draft_compiler_session_navigation_usage_count(session) == 1);

  DraftCompilerServiceResult built{};
  draft_compiler_session_build(session, multi_overlays.data(),
                               multi_overlays.size(), 0, &built);
  EXPECT(state, built.success == 1);
  EXPECT(state, built.diagnostic_count == 0);
  EXPECT(state, built.elapsed_nanoseconds != 0);
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

void test_manifest_program_build_and_run_configuration(TestState &state) {
  draft::test::TemporaryDirectory temporary{
      "draft-compiler-program-configuration-test"};
  const std::filesystem::path workspace = temporary.path() / "workspace";
  const std::filesystem::path app = workspace / "app";
  const std::filesystem::path working_directory = workspace / "run-here";
  const std::filesystem::path runtime_asset = workspace / "assets";
  std::error_code directory_error;
  std::filesystem::create_directories(working_directory, directory_error);
  EXPECT(state, !directory_error);
  directory_error.clear();
  std::filesystem::create_directories(runtime_asset, directory_error);
  EXPECT(state, !directory_error);
  const std::string source = "package app\n"
                             "main :: proc() -> int {\n"
                             "    return 0\n"
                             "}\n";
  write_file(app / "package.draft", source);
  write_file(workspace / "draft.workspace",
             "draft-workspace-v1\n"
             "default = configured\n"
             "[program configured]\n"
             "root = app\n"
             "target = " +
                 std::string(native_target_name()) +
                 "\n"
                 "optimization = O0\n"
                 "kind = object\n"
                 "output = outputs/configured.o\n"
                 "debug-symbols = off\n"
                 "assertions = off\n"
                 "runtime-asset = data:assets\n"
                 "argument = first argument\n"
                 "argument = --second\n"
                 "environment = DRAFT_SERVICE_TEST=expected\n"
                 "working-directory = run-here\n");

  const std::string workspace_text = workspace.string();
  const std::string root;
  const std::string source_name = "package.draft";
  DraftCompilerServiceConfiguration input =
      configuration(workspace_text, root, source_name);
  input.target = static_cast<std::uint8_t>((native_target_ordinal() + 1) % 4);
  input.target_is_explicit = 0;
  std::array<std::uint8_t, 512> error{};
  void *session =
      draft_compiler_session_create(&input, error.data(), error.size());
  if (session == nullptr)
    std::cerr << reinterpret_cast<const char *>(error.data()) << '\n';
  EXPECT(state, session != nullptr);
  if (session == nullptr)
    return;
  const std::string canonical_working_directory =
      std::filesystem::canonical(working_directory).string();

  // The IDE receives one authoritative compiler-policy projection rather than
  // reconstructing manifest precedence in Draft. Structured run settings are
  // copied through the typed operations tested below.
  std::array<std::uint8_t, 8192> configuration_bytes{};
  const std::size_t configuration_size =
      draft_compiler_session_copy_build_configuration(
          session, configuration_bytes.data(), configuration_bytes.size());
  const std::string_view configuration_text{
      reinterpret_cast<const char *>(configuration_bytes.data()),
      configuration_size,
  };
  EXPECT(state, configuration_text.find("root package: app\n") !=
                    std::string_view::npos);
  EXPECT(state, configuration_text.find(
                    "target: " + std::string(native_target_name()) + "\n") !=
                    std::string_view::npos);
  EXPECT(state, configuration_text.find("optimization: O0\n") !=
                    std::string_view::npos);
  EXPECT(state, configuration_text.find("artifact: object\n") !=
                    std::string_view::npos);
  EXPECT(state, configuration_text.find("argument:") == std::string_view::npos);
  EXPECT(state,
         configuration_text.find("environment:") == std::string_view::npos);
  EXPECT(state, configuration_text.find("working directory:") ==
                    std::string_view::npos);
  std::array<std::uint8_t, 1024> summary_bytes{};
  const std::size_t summary_size = draft_compiler_session_copy_session_summary(
      session, summary_bytes.data(), summary_bytes.size());
  const std::string_view summary_text{
      reinterpret_cast<const char *>(summary_bytes.data()), summary_size};
  EXPECT(state, summary_text.find("Workspace: ") == 0);
  EXPECT(state, summary_text.find("  Root: app  ") != std::string_view::npos);
  EXPECT(state, summary_text.find(std::string(native_target_name())) !=
                    std::string_view::npos);
  EXPECT(state, summary_text.ends_with("  O0"));

  // With no explicit IDE override, the selected program target wins over the
  // deliberately different create-time host fallback.
  EXPECT(state,
         draft_compiler_session_target(session) == native_target_ordinal());
  EXPECT(state, draft_compiler_session_run_argument_count(session) == 2);
  std::array<std::uint8_t, 128> copied{};
  EXPECT(state, draft_compiler_session_copy_run_argument(
                    session, 0, copied.data(), copied.size()) == 14);
  EXPECT(state, std::string_view(reinterpret_cast<const char *>(
                    copied.data())) == "first argument");
  EXPECT(state, draft_compiler_session_run_environment_count(session) == 1);
  copied.fill(0);
  EXPECT(state, draft_compiler_session_copy_run_environment(
                    session, 0, copied.data(), copied.size()) == 27);
  EXPECT(state, std::string_view(reinterpret_cast<const char *>(
                    copied.data())) == "DRAFT_SERVICE_TEST=expected");
  std::array<std::uint8_t, 4096> directory_bytes{};
  const std::size_t directory_size =
      draft_compiler_session_copy_run_working_directory(
          session, directory_bytes.data(), directory_bytes.size());
  EXPECT(state, directory_size == canonical_working_directory.size());
  EXPECT(state, std::string_view(
                    reinterpret_cast<const char *>(directory_bytes.data()),
                    directory_size) == canonical_working_directory);

  const std::filesystem::path source_path =
      std::filesystem::canonical(app / source_name);
  const std::string source_path_text = source_path.string();
  const DraftCompilerServiceOverlay source_overlay =
      overlay(source_path_text, source);
  DraftCompilerServiceResult built{};
  draft_compiler_session_build(session, &source_overlay, 1, 0, &built);
  if (built.success == 0) {
    std::array<std::uint8_t, 4096> diagnostics{};
    draft_compiler_session_copy_diagnostics(session, diagnostics.data(),
                                            diagnostics.size());
    std::cerr << reinterpret_cast<const char *>(diagnostics.data());
  }
  EXPECT(state, built.success == 1);
  EXPECT(state, draft_compiler_session_artifact_kind(session) == 1);
  std::array<std::uint8_t, 4096> artifact{};
  const std::size_t artifact_size = draft_compiler_session_copy_artifact_path(
      session, artifact.data(), artifact.size());
  const std::filesystem::path expected_output =
      std::filesystem::canonical(workspace / "outputs/configured.o");
  const std::string artifact_text(
      reinterpret_cast<const char *>(artifact.data()), artifact_size);
  if (artifact_text != expected_output.string()) {
    std::cerr << "configured artifact: " << artifact_text
              << "\nexpected artifact: " << expected_output.string() << '\n';
  }
  EXPECT(state, artifact_text == expected_output.string());
  EXPECT(state, std::filesystem::is_regular_file(expected_output));

  // A saved manifest is visible to the next foreground build without reopening
  // the session. This also proves that run rows and configured output are read
  // from one refreshed selected-root record rather than an initialization copy.
  write_file(workspace / "draft.workspace",
             "draft-workspace-v1\n"
             "default = configured\n"
             "[program configured]\n"
             "root = app\n"
             "target = " +
                 std::string(native_target_name()) +
                 "\n"
                 "optimization = O0\n"
                 "kind = object\n"
                 "output = outputs/refreshed.o\n"
                 "argument = refreshed\n");
  draft_compiler_session_build(session, &source_overlay, 1, 0, &built);
  EXPECT(state, built.success == 1);
  EXPECT(state, draft_compiler_session_run_argument_count(session) == 1);
  copied.fill(0);
  EXPECT(state, draft_compiler_session_copy_run_argument(
                    session, 0, copied.data(), copied.size()) == 9);
  EXPECT(state, std::string_view(reinterpret_cast<const char *>(
                    copied.data())) == "refreshed");
  artifact.fill(0);
  const std::size_t refreshed_size = draft_compiler_session_copy_artifact_path(
      session, artifact.data(), artifact.size());
  const std::filesystem::path refreshed_output =
      std::filesystem::canonical(workspace / "outputs/refreshed.o");
  EXPECT(state,
         std::string_view(reinterpret_cast<const char *>(artifact.data()),
                          refreshed_size) == refreshed_output.string());
  draft_compiler_session_destroy(session);

  // The same manifest target is only a default. An explicit create-time
  // target, as supplied by --target or Shift-F12, must replace it for discovery
  // and subsequent checking without rewriting draft.workspace.
  input.target_is_explicit = 1;
  session = draft_compiler_session_create(&input, error.data(), error.size());
  EXPECT(state, session != nullptr);
  if (session != nullptr) {
    EXPECT(state, draft_compiler_session_target(session) == input.target);
  }
  draft_compiler_session_destroy(session);
}

void test_named_program_is_discovered_only_under_its_target(TestState &state) {
  draft::test::TemporaryDirectory temporary{
      "draft-compiler-service-program-target-test"};
  const std::filesystem::path workspace = temporary.path() / "workspace";
  const std::filesystem::path app = workspace / "app";
  const std::uint8_t configured_target =
      static_cast<std::uint8_t>((native_target_ordinal() + 1) % 4);
  write_file(
      app / ("entry@" + std::string(target_name(configured_target)) + ".draft"),
      "package app\n"
      "main :: proc() -> int {\n"
      "    return 0\n"
      "}\n");
  write_file(app / ("broken@" + std::string(native_target_name()) + ".draft"),
             "package app\nmain :: proc(\n");
  write_file(workspace / "draft.workspace",
             "draft-workspace-v1\n"
             "default = configured\n"
             "[program configured]\n"
             "root = app\n"
             "target = " +
                 std::string(target_name(configured_target)) + "\n");

  const std::string workspace_text = workspace.string();
  const std::string root;
  const std::string source;
  DraftCompilerServiceConfiguration input =
      configuration(workspace_text, root, source);
  input.target = native_target_ordinal();
  input.target_is_explicit = 0;
  std::array<std::uint8_t, 512> error{};
  void *session =
      draft_compiler_session_create(&input, error.data(), error.size());
  if (session == nullptr)
    std::cerr << reinterpret_cast<const char *>(error.data()) << '\n';
  EXPECT(state, session != nullptr);
  if (session != nullptr) {
    EXPECT(state, draft_compiler_session_target(session) == configured_target);
  }
  draft_compiler_session_destroy(session);
}

} // namespace

int main() {
  TestState state;
  test_service_transactions_and_native_build(state);
  test_create_rejects_escaping_paths(state);
  test_create_discovers_root_when_omitted(state);
  test_manifest_program_build_and_run_configuration(state);
  test_named_program_is_discovered_only_under_its_target(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
