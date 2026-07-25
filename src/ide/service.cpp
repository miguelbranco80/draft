// Stable C facade for embedding the bootstrap compiler in Draft tools.
//
// The Draft executable owns workspace selection, source buffers, UI state, and
// the lifetime of the opaque handle returned here. This module validates and
// copies create-time paths, owns exactly one CompilerSession behind that
// handle, and borrows all source/destination byte pointers only for a
// synchronous call. No C++ type, allocation, exception, or LLVM handle crosses
// the ABI.
//
// The facade deliberately has no callback into Draft. That keeps ownership and
// control flow one-way: Draft calls a compiler library, just as it would call a
// small C library. The underlying implementation may remain C++ during the
// bootstrap and can later be replaced without changing the Draft application.

#include "ide/service.h"
#include "ide/compiler_session.h"

#include "target/profile.h"
#include "workspace/manifest.h"
#include "workspace/selection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

static_assert(sizeof(DraftCompilerServiceConfiguration) == 56);
static_assert(alignof(DraftCompilerServiceConfiguration) == alignof(void *));
static_assert(sizeof(DraftCompilerServiceResult) == 16);
static_assert(alignof(DraftCompilerServiceResult) == alignof(std::size_t));
static_assert(sizeof(DraftCompilerServiceSyntaxResult) == 16);
static_assert(alignof(DraftCompilerServiceSyntaxResult) ==
              alignof(std::size_t));
static_assert(sizeof(DraftCompilerServiceSpan) == 24);
static_assert(alignof(DraftCompilerServiceSpan) == alignof(std::size_t));
static_assert(sizeof(DraftCompilerServiceOverlay) == 32);
static_assert(alignof(DraftCompilerServiceOverlay) == alignof(void *));
static_assert(sizeof(DraftCompilerServicePackageRow) == 16);
static_assert(alignof(DraftCompilerServicePackageRow) == alignof(std::size_t));
static_assert(sizeof(DraftCompilerServiceNavigationLocation) == 40);
static_assert(alignof(DraftCompilerServiceNavigationLocation) ==
              alignof(std::size_t));

// ServiceSession is the stable opaque C handle. Keeping the compiler behind
// one extra owner lets Open Folder replace a complete workspace session
// transactionally without invalidating the Draft Host_Api.user value.
struct ServiceSession {
  std::unique_ptr<draft::ide::CompilerSession> compiler;
};

[[nodiscard]] draft::ide::CompilerSession *
compiler_session(void *opaque_session) {
  auto *service = static_cast<ServiceSession *>(opaque_session);
  return service == nullptr ? nullptr : service->compiler.get();
}

[[nodiscard]] std::string_view borrowed_text(const void *data,
                                             std::size_t length) {
  if (length == 0)
    return {};
  if (data == nullptr)
    return {};
  return {static_cast<const char *>(data), length};
}

// Copies as much as fits, always NUL-terminates a nonempty destination, and
// returns the complete source size so a caller can distinguish truncation.
[[nodiscard]] std::size_t copy_text(std::string_view source,
                                    std::uint8_t *destination,
                                    std::size_t capacity) {
  if (capacity != 0 && destination != nullptr) {
    const std::size_t count = std::min(source.size(), capacity - 1);
    std::copy_n(reinterpret_cast<const std::uint8_t *>(source.data()), count,
                destination);
    destination[count] = 0;
  }
  return source.size();
}

void publish_create_error(std::string_view message, std::uint8_t *destination,
                          std::size_t capacity) {
  static_cast<void>(copy_text(message, destination, capacity));
}

[[nodiscard]] bool has_parent_component(const std::filesystem::path &path) {
  return std::any_of(
      path.begin(), path.end(),
      [](const std::filesystem::path &component) { return component == ".."; });
}

// Both paths must already be canonical. A component check avoids the textual
// prefix bug where `/workspace-two` appears to be below `/workspace`.
[[nodiscard]] bool is_within(const std::filesystem::path &base,
                             const std::filesystem::path &candidate) {
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(candidate, base, error);
  return !error && !relative.empty() && !relative.is_absolute() &&
         !has_parent_component(relative);
}

[[nodiscard]] std::optional<draft::TargetProfile>
target_profile(std::uint8_t value) {
  switch (value) {
  case 0:
    return draft::make_aarch64_macos_profile();
  case 1:
    return draft::make_aarch64_linux_profile();
  case 2:
    return draft::make_x86_64_linux_profile();
  case 3:
    return draft::make_x86_64_windows_profile();
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::uint8_t target_ordinal(const draft::TargetProfile &target) {
  if (target.facts.file_tag == "aarch64-macos")
    return 0;
  if (target.facts.file_tag == "aarch64-linux")
    return 1;
  if (target.facts.file_tag == "x86_64-linux")
    return 2;
  if (target.facts.file_tag == "x86_64-windows")
    return 3;
  return std::numeric_limits<std::uint8_t>::max();
}

[[nodiscard]] std::unique_ptr<draft::ide::CompilerSession>
create_session(const DraftCompilerServiceConfiguration &input,
               std::string &error_message) {
  if ((input.workspace_length != 0 && input.workspace_data == nullptr) ||
      (input.root_length != 0 && input.root_data == nullptr) ||
      (input.source_length != 0 && input.source_data == nullptr)) {
    error_message = "compiler configuration contains a nil byte range";
    return nullptr;
  }
  const std::string workspace_text(
      borrowed_text(input.workspace_data, input.workspace_length));
  std::string root_text(borrowed_text(input.root_data, input.root_length));
  std::string source_text(
      borrowed_text(input.source_data, input.source_length));
  if (workspace_text.empty() ||
      workspace_text.find('\0') != std::string::npos ||
      root_text.find('\0') != std::string::npos ||
      source_text.find('\0') != std::string::npos) {
    error_message = "workspace and source must be valid paths";
    return nullptr;
  }
  const std::filesystem::path source_spelling(source_text);
  if (source_spelling.is_absolute() || has_parent_component(source_spelling) ||
      source_spelling.has_parent_path()) {
    error_message = "root and source must be workspace-relative package paths";
    return nullptr;
  }

  const std::optional<draft::TargetProfile> target =
      target_profile(input.target);
  if (!target.has_value()) {
    error_message = "unknown Draft target";
    return nullptr;
  }

  draft::DiagnosticSink location_diagnostics;
  std::filesystem::path workspace;
  std::filesystem::path opened_directory;
  std::filesystem::path manifest_path;
  if (!draft::locate_command_scope(workspace_text, workspace, opened_directory,
                                   manifest_path, location_diagnostics)) {
    error_message = "workspace directory is unavailable";
    if (!location_diagnostics.diagnostics().empty()) {
      error_message += ": ";
      error_message += location_diagnostics.diagnostics().front().message;
    }
    return nullptr;
  }

  draft::WorkspaceManifest manifest;
  if (!draft::load_workspace_manifest(manifest_path, manifest, error_message)) {
    return nullptr;
  }
  if (root_text.empty()) {
    if (opened_directory != workspace) {
      const std::filesystem::path relative =
          opened_directory.lexically_relative(workspace);
      root_text = relative.empty() ? "." : relative.generic_string();
    } else if (manifest.default_program.has_value()) {
      const draft::ProgramConfiguration *program =
          draft::find_program_by_name(manifest, *manifest.default_program);
      if (program != nullptr)
        root_text = program->root;
    }
  }
  const std::filesystem::path root_spelling(root_text);
  if (root_spelling.is_absolute() || has_parent_component(root_spelling)) {
    error_message = "root must be a workspace-relative package path";
    return nullptr;
  }

  std::error_code path_error;
  draft::ide::CompilerConfiguration configuration;
  configuration.workspace_directory = workspace;
  configuration.source_relative_name = source_spelling.generic_string();
  configuration.target = *target;
  configuration.target_is_explicit = input.target_is_explicit != 0;
  if (!root_text.empty()) {
    const std::filesystem::path root = std::filesystem::weakly_canonical(
        workspace / root_spelling, path_error);
    const bool root_is_directory =
        !path_error && std::filesystem::is_directory(root, path_error);
    if (path_error || !root_is_directory || !is_within(workspace, root)) {
      error_message = "root package is unavailable or escapes the workspace";
      return nullptr;
    }
    if (!source_text.empty()) {
      const std::filesystem::path source =
          std::filesystem::weakly_canonical(root / source_spelling, path_error);
      const bool source_is_file =
          !path_error && std::filesystem::is_regular_file(source, path_error);
      if (path_error || !source_is_file || !is_within(root, source)) {
        error_message =
            "active source is unavailable or escapes the root package";
        return nullptr;
      }
    }

    const std::filesystem::path relative_root =
        std::filesystem::relative(root, workspace, path_error);
    if (path_error || relative_root.empty()) {
      error_message = "cannot identify the selected root package";
      return nullptr;
    }
    configuration.root_package_directory = root;
    configuration.root_relative_path =
        relative_root == "." ? "." : relative_root.generic_string();
  }

  std::unique_ptr<draft::ide::CompilerSession> session{
      new (std::nothrow) draft::ide::CompilerSession(std::move(configuration))};
  if (session == nullptr) {
    error_message = "cannot allocate compiler session";
    return nullptr;
  }
  draft::DiagnosticSink initialization_diagnostics;
  if (!session->initialize(initialization_diagnostics)) {
    error_message = "cannot discover workspace roots";
    if (!initialization_diagnostics.diagnostics().empty()) {
      error_message += ": ";
      error_message += initialization_diagnostics.diagnostics().front().message;
    }
    return nullptr;
  }
  return session;
}

[[nodiscard]] DraftCompilerServiceResult
service_result(draft::ide::CompilerSession &session,
               draft::ide::CheckResult result) {
  const std::size_t spans = session.syntax_spans().size();
  return {
      static_cast<std::uint8_t>(result.ok ? 1 : 0),
      result.diagnostic_count,
      spans,
  };
}

[[nodiscard]] std::optional<std::vector<draft::ide::SourceOverlay>>
checked_overlays(const DraftCompilerServiceOverlay *overlays, std::size_t count,
                 std::size_t active) {
  if (overlays == nullptr || count == 0 || active >= count)
    return std::nullopt;
  std::vector<draft::ide::SourceOverlay> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const DraftCompilerServiceOverlay &overlay = overlays[index];
    if (overlay.path_data == nullptr || overlay.path_length == 0 ||
        (overlay.source_data == nullptr && overlay.source_length != 0)) {
      return std::nullopt;
    }
    const std::string_view path =
        borrowed_text(overlay.path_data, overlay.path_length);
    if (path.find('\0') != std::string_view::npos)
      return std::nullopt;
    result.push_back({
        std::filesystem::path(path),
        borrowed_text(overlay.source_data, overlay.source_length),
    });
  }
  return result;
}

// checked_overlay validates the raw C ranges without consulting the current
// WorkspaceGraph. Syntax coloring is useful for a new or otherwise
// editing-only file, so imposing semantic package membership here would put
// filesystem discovery back on the keystroke path.
[[nodiscard]] std::optional<draft::ide::SourceOverlay>
checked_overlay(const DraftCompilerServiceOverlay *overlay) {
  if (overlay == nullptr || overlay->path_data == nullptr ||
      overlay->path_length == 0 ||
      (overlay->source_data == nullptr && overlay->source_length != 0)) {
    return std::nullopt;
  }
  const std::string_view path =
      borrowed_text(overlay->path_data, overlay->path_length);
  if (path.find('\0') != std::string_view::npos)
    return std::nullopt;
  return draft::ide::SourceOverlay{
      std::filesystem::path(path),
      borrowed_text(overlay->source_data, overlay->source_length),
  };
}

// navigation_location copies the compiler-owned value into its C ABI twin.
// The all-zero record is the single out-of-range/unavailable sentinel; valid
// FileId zero remains distinguishable because every valid range has one-based
// line and column coordinates.
[[nodiscard]] DraftCompilerServiceNavigationLocation navigation_location(
    const draft::ide::NavigationLocation &location) {
  return {
      location.source,
      location.start,
      location.end,
      location.line,
      location.column,
  };
}

} // namespace

void *draft_compiler_session_create(
    const DraftCompilerServiceConfiguration *configuration,
    std::uint8_t *error_destination, std::size_t error_capacity) {
  if (configuration == nullptr) {
    publish_create_error("compiler configuration is nil", error_destination,
                         error_capacity);
    return nullptr;
  }
  std::string error_message;
  std::unique_ptr<draft::ide::CompilerSession> session =
      create_session(*configuration, error_message);
  if (session == nullptr) {
    publish_create_error(error_message, error_destination, error_capacity);
    return nullptr;
  }
  if (error_capacity != 0 && error_destination != nullptr) {
    error_destination[0] = 0;
  }
  std::unique_ptr<ServiceSession> service{new (std::nothrow) ServiceSession};
  if (service == nullptr) {
    publish_create_error("cannot allocate compiler service handle",
                         error_destination, error_capacity);
    return nullptr;
  }
  service->compiler = std::move(session);
  return service.release();
}

void draft_compiler_session_destroy(void *opaque_session) {
  delete static_cast<ServiceSession *>(opaque_session);
}

std::uint8_t draft_compiler_session_open_workspace(
    void *opaque_session, const void *workspace_data,
    std::size_t workspace_length, std::uint8_t *error_destination,
    std::size_t error_capacity) {
  auto *service = static_cast<ServiceSession *>(opaque_session);
  if (service == nullptr || service->compiler == nullptr) {
    publish_create_error("compiler service session is nil", error_destination,
                         error_capacity);
    return 0;
  }
  const std::string_view workspace =
      borrowed_text(workspace_data, workspace_length);
  if (workspace.empty() || workspace.find('\0') != std::string_view::npos) {
    publish_create_error("workspace path is empty or malformed",
                         error_destination, error_capacity);
    return 0;
  }
  const std::string root;
  const std::string source;
  const std::uint8_t target =
      target_ordinal(service->compiler->target_is_explicit()
                         ? service->compiler->target()
                         : service->compiler->fallback_target());
  const DraftCompilerServiceConfiguration configuration{
      workspace.data(),
      workspace.size(),
      root.data(),
      root.size(),
      source.data(),
      source.size(),
      target,
      static_cast<std::uint8_t>(service->compiler->target_is_explicit()),
  };
  std::string error_message;
  std::unique_ptr<draft::ide::CompilerSession> replacement =
      create_session(configuration, error_message);
  if (replacement == nullptr) {
    publish_create_error(error_message, error_destination, error_capacity);
    return 0;
  }
  service->compiler = std::move(replacement);
  if (error_capacity != 0 && error_destination != nullptr)
    error_destination[0] = 0;
  return 1;
}

std::size_t draft_compiler_session_copy_workspace_path(
    void *opaque_session, std::uint8_t *destination, std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  return copy_text(session->workspace_directory().string(), destination,
                   capacity);
}

void draft_compiler_session_check(void *opaque_session,
                                  const DraftCompilerServiceOverlay *overlays,
                                  std::size_t overlay_count,
                                  std::size_t active_overlay,
                                  DraftCompilerServiceResult *result) {
  if (result == nullptr)
    return;
  *result = {};
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  const std::optional<std::vector<draft::ide::SourceOverlay>> checked =
      checked_overlays(overlays, overlay_count, active_overlay);
  if (session == nullptr || !checked.has_value())
    return;
  *result = service_result(*session, session->check(*checked, active_overlay));
}

void draft_compiler_session_build(void *opaque_session,
                                  const DraftCompilerServiceOverlay *overlays,
                                  std::size_t overlay_count,
                                  std::size_t active_overlay,
                                  DraftCompilerServiceResult *result) {
  if (result == nullptr)
    return;
  *result = {};
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  const std::optional<std::vector<draft::ide::SourceOverlay>> checked =
      checked_overlays(overlays, overlay_count, active_overlay);
  if (session == nullptr || !checked.has_value())
    return;
  *result = service_result(*session, session->build(*checked, active_overlay));
}

void draft_compiler_session_colorize(
    void *opaque_session, const DraftCompilerServiceOverlay *source,
    DraftCompilerServiceSyntaxResult *result) {
  if (result == nullptr)
    return;
  *result = {};
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  const std::optional<draft::ide::SourceOverlay> checked =
      checked_overlay(source);
  if (session == nullptr || !checked.has_value())
    return;
  session->colorize(*checked);
  *result = {
      1,
      session->syntax_spans().size(),
  };
}

void draft_compiler_session_span(void *opaque_session, std::size_t index,
                                 DraftCompilerServiceSpan *result) {
  if (result == nullptr)
    return;
  *result = {};
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || index >= session->syntax_spans().size())
    return;
  const draft::ide::SyntaxSpan &span = session->syntax_spans()[index];
  *result = {
      span.start,
      span.end,
      static_cast<std::uint8_t>(span.style),
  };
}

std::size_t draft_compiler_session_copy_diagnostics(void *opaque_session,
                                                    std::uint8_t *destination,
                                                    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  return copy_text(session->diagnostics_text(), destination, capacity);
}

std::size_t draft_compiler_session_copy_tooling_section(
    void *opaque_session, std::uint8_t section, std::uint8_t *destination,
    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr ||
      section >= static_cast<std::uint8_t>(draft::ide::ToolingSection::Count)) {
    if (capacity != 0 && destination != nullptr)
      destination[0] = 0;
    return 0;
  }
  return copy_text(
      session->tooling_text(static_cast<draft::ide::ToolingSection>(section)),
      destination, capacity);
}

std::size_t draft_compiler_session_package_row_count(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session == nullptr ? 0 : session->package_tree_rows().size();
}

void draft_compiler_session_package_row(
    void *opaque_session, std::size_t index,
    DraftCompilerServicePackageRow *result) {
  if (result == nullptr)
    return;
  *result = {};
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || index >= session->package_tree_rows().size())
    return;
  const draft::ide::PackageTreeRow &row = session->package_tree_rows()[index];
  *result = {
      row.package_index,
      static_cast<std::uint8_t>(row.kind),
      static_cast<std::uint8_t>(
          row.kind == draft::ide::PackageTreeRowKind::Import ? 1 : 0),
      static_cast<std::uint8_t>(row.root ? 1 : 0),
      static_cast<std::uint8_t>(row.has_children ? 1 : 0),
  };
}

std::size_t draft_compiler_session_copy_package_row_text(
    void *opaque_session, std::size_t index, std::uint8_t *destination,
    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || index >= session->package_tree_rows().size()) {
    if (capacity != 0 && destination != nullptr)
      destination[0] = 0;
    return 0;
  }
  return copy_text(session->package_tree_rows()[index].label, destination,
                   capacity);
}

std::uint8_t draft_compiler_session_prepare_navigation(
    void *opaque_session, const void *path_data, std::size_t path_length,
    std::size_t byte_offset) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return static_cast<std::uint8_t>(
        draft::ide::NavigationStatus::Unavailable);
  if ((path_data == nullptr && path_length != 0) || path_length == 0) {
    return static_cast<std::uint8_t>(
        draft::ide::NavigationStatus::SourceNotFound);
  }
  const std::string_view path = borrowed_text(path_data, path_length);
  if (path.find('\0') != std::string_view::npos) {
    return static_cast<std::uint8_t>(
        draft::ide::NavigationStatus::SourceNotFound);
  }
  return static_cast<std::uint8_t>(
      session->prepare_navigation(path, byte_offset));
}

void draft_compiler_session_navigation_definition(
    void *opaque_session,
    DraftCompilerServiceNavigationLocation *result) {
  if (result == nullptr)
    return;
  *result = {};
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || !session->navigation_definition().has_value())
    return;
  *result = navigation_location(*session->navigation_definition());
}

std::size_t
draft_compiler_session_navigation_usage_count(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session == nullptr ? 0 : session->navigation_usages().size();
}

void draft_compiler_session_navigation_usage(
    void *opaque_session, std::size_t index,
    DraftCompilerServiceNavigationLocation *result) {
  if (result == nullptr)
    return;
  *result = {};
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || index >= session->navigation_usages().size())
    return;
  *result = navigation_location(session->navigation_usages()[index]);
}

std::size_t draft_compiler_session_copy_navigation_source_path(
    void *opaque_session, std::size_t source, std::uint8_t *destination,
    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  const std::string path = session->navigation_source_path(source);
  return copy_text(path, destination, capacity);
}

std::size_t draft_compiler_session_copy_navigation_source_text(
    void *opaque_session, std::size_t source, std::uint8_t *destination,
    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  return copy_text(session->navigation_source_text(source), destination,
                   capacity);
}

std::uint8_t draft_compiler_session_navigation_source_editable(
    void *opaque_session, std::size_t source) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session != nullptr && session->navigation_source_editable(source)
             ? 1
             : 0;
}

std::size_t draft_compiler_session_copy_source_path(void *opaque_session,
                                                    std::uint8_t *destination,
                                                    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  const std::string source_path = session->source_path().string();
  return copy_text(source_path, destination, capacity);
}

std::size_t draft_compiler_session_source_count(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session == nullptr ? 0 : session->source_count();
}

std::size_t draft_compiler_session_copy_source_name(void *opaque_session,
                                                    std::size_t index,
                                                    std::uint8_t *destination,
                                                    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  return copy_text(session->source_name(index), destination, capacity);
}

std::size_t draft_compiler_session_copy_source_path_at(
    void *opaque_session, std::size_t index, std::uint8_t *destination,
    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  return copy_text(session->source_path(index).string(), destination, capacity);
}

std::size_t draft_compiler_session_copy_artifact_path(void *opaque_session,
                                                      std::uint8_t *destination,
                                                      std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  const std::string output_path = session->built_output_path().string();
  return copy_text(output_path, destination, capacity);
}

std::uint8_t draft_compiler_session_artifact_kind(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return std::numeric_limits<std::uint8_t>::max();
  switch (session->built_artifact_kind()) {
  case draft::NativeArtifactKind::Executable:
    return 0;
  case draft::NativeArtifactKind::Object:
    return 1;
  case draft::NativeArtifactKind::StaticLibrary:
    return 2;
  case draft::NativeArtifactKind::DynamicLibrary:
    return 3;
  case draft::NativeArtifactKind::Assembly:
    return 4;
  }
  return std::numeric_limits<std::uint8_t>::max();
}

std::size_t draft_compiler_session_run_argument_count(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session == nullptr ? 0 : session->run_arguments().size();
}

std::size_t draft_compiler_session_copy_run_argument(void *opaque_session,
                                                     std::size_t index,
                                                     std::uint8_t *destination,
                                                     std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || index >= session->run_arguments().size())
    return 0;
  return copy_text(session->run_arguments()[index], destination, capacity);
}

std::size_t draft_compiler_session_run_environment_count(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session == nullptr ? 0 : session->run_environment().size();
}

std::size_t draft_compiler_session_copy_run_environment(
    void *opaque_session, std::size_t index, std::uint8_t *destination,
    std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || index >= session->run_environment().size())
    return 0;
  return copy_text(session->run_environment()[index], destination, capacity);
}

std::size_t draft_compiler_session_copy_run_working_directory(
    void *opaque_session, std::uint8_t *destination, std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr || !session->run_working_directory().has_value())
    return 0;
  return copy_text(session->run_working_directory()->string(), destination,
                   capacity);
}

std::size_t draft_compiler_session_copy_program_configuration(
    void *opaque_session, std::uint8_t *destination, std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  const std::string configuration = session->program_configuration_text();
  return copy_text(configuration, destination, capacity);
}

std::size_t draft_compiler_session_root_count(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session == nullptr ? 0 : session->root_count();
}

std::size_t draft_compiler_session_selected_root(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  return session == nullptr ? 0 : session->selected_root();
}

std::size_t draft_compiler_session_copy_root_name(void *opaque_session,
                                                  std::size_t index,
                                                  std::uint8_t *destination,
                                                  std::size_t capacity) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  return copy_text(session->root_name(index), destination, capacity);
}

std::uint8_t draft_compiler_session_select_root(void *opaque_session,
                                                std::size_t index) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return 0;
  draft::DiagnosticSink diagnostics;
  return session->select_root(index, diagnostics) ? 1 : 0;
}

std::uint8_t draft_compiler_session_target(void *opaque_session) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  if (session == nullptr)
    return std::numeric_limits<std::uint8_t>::max();
  return target_ordinal(session->target());
}

std::uint8_t draft_compiler_session_select_target(void *opaque_session,
                                                  std::uint8_t target) {
  draft::ide::CompilerSession *session = compiler_session(opaque_session);
  const std::optional<draft::TargetProfile> selected = target_profile(target);
  if (session == nullptr || !selected.has_value())
    return 0;
  draft::DiagnosticSink diagnostics;
  return session->select_target(*selected, diagnostics) ? 1 : 0;
}
