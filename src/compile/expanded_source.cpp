// Deterministic filesystem writer for complete expanded Draft source.
//
// This implementation is the inspection boundary after provider-free semantic
// compilation. It consumes only the final checked WorkspaceGraph and the
// SourceManager buffers named by that graph, then writes an explicitly
// requested directory containing those exact bytes. It owns the temporary
// sibling directory during the transaction and owns no state after return.
//
// The writer assumes package identities and selected filenames were validated
// by workspace loading, but rechecks every component before using it as a host
// path. Publication is one rename from an otherwise private staging tree, so a
// failure cannot expose a partial mixture of source and source maps. It depends
// on compile/source records and the standard filesystem only; it deliberately
// does not depend on the resolution store, provider adapters, semantic phases,
// or native backend. Relevant language rules are in agent-synthesis section 10.

#include "compile/expanded_source.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace draft {
namespace {

// Removes only the private staging directory created by this operation. The
// requested output path is never touched until the final rename succeeds.
class StagingDirectory {
public:
  explicit StagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}

  StagingDirectory(const StagingDirectory &) = delete;
  StagingDirectory &operator=(const StagingDirectory &) = delete;

  ~StagingDirectory() {
    if (committed_) return;
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }
  void committed() { committed_ = true; }

private:
  std::filesystem::path path_;
  bool committed_ = false;
};

void projection_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(
      SourceRange::invalid(), "expanded source: " + std::move(message));
}

[[nodiscard]] bool path_is_missing(
    const std::filesystem::path &path,
    bool &missing,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    missing = true;
    return true;
  }
  if (error) {
    projection_error(
        diagnostics,
        "cannot inspect '" + path.string() + "': " + error.message());
    return false;
  }
  missing = status.type() == std::filesystem::file_type::not_found;
  return true;
}

[[nodiscard]] bool ensure_directory(
    const std::filesystem::path &path,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  if (std::filesystem::create_directories(path, error) || !error) return true;
  projection_error(
      diagnostics,
      "cannot create directory '" + path.string() + "': " + error.message());
  return false;
}

[[nodiscard]] bool write_file(
    const std::filesystem::path &path,
    std::string_view bytes,
    DiagnosticSink &diagnostics) {
  if (!ensure_directory(path.parent_path(), diagnostics)) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    projection_error(
        diagnostics, "cannot create file '" + path.string() + "'");
    return false;
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    projection_error(
        diagnostics, "cannot write complete file '" + path.string() + "'");
    return false;
  }
  return true;
}

void append_field(
    std::string_view name,
    std::string_view value,
    std::string &output) {
  output += name;
  output.push_back(' ');
  output += std::to_string(value.size());
  output.push_back('\n');
  output.append(value);
  output.push_back('\n');
}

[[nodiscard]] std::string padded_index(std::size_t index) {
  std::string digits = std::to_string(index);
  if (digits.size() < 4) digits.insert(0, 4 - digits.size(), '0');
  return digits;
}

[[nodiscard]] std::string root_kind_name(PackageRootKind kind) {
  switch (kind) {
  case PackageRootKind::Workspace: return "workspace";
  case PackageRootKind::Dependency: return "dependency";
  case PackageRootKind::Core: return "core";
  }
  return "unknown";
}

[[nodiscard]] std::string root_directory_name(
    std::size_t index,
    PackageRootKind kind) {
  return "root-" + padded_index(index) + "-" + root_kind_name(kind);
}

// Semantic package paths originate in the checked workspace graph, but this
// output boundary revalidates them before turning them back into host paths.
[[nodiscard]] bool append_safe_relative_path(
    std::filesystem::path &path,
    std::string_view relative,
    DiagnosticSink &diagnostics) {
  if (relative == ".") return true;
  if (relative.empty() || relative.front() == '/' || relative.back() == '/') {
    projection_error(diagnostics, "package path is not canonical");
    return false;
  }
  std::size_t begin = 0;
  while (begin < relative.size()) {
    const std::size_t slash = relative.find('/', begin);
    const std::string_view component = relative.substr(
        begin,
        slash == std::string_view::npos
            ? relative.size() - begin
            : slash - begin);
    if (component.empty() || component == "." || component == ".." ||
        component.find('\\') != std::string_view::npos) {
      projection_error(diagnostics, "package path is not canonical");
      return false;
    }
    path /= std::string(component);
    if (slash == std::string_view::npos) break;
    begin = slash + 1;
  }
  return true;
}

[[nodiscard]] bool append_safe_filename(
    std::filesystem::path &path,
    std::string_view filename,
    DiagnosticSink &diagnostics) {
  if (filename.empty() || filename == "." || filename == ".." ||
      filename.find('/') != std::string_view::npos ||
      filename.find('\\') != std::string_view::npos) {
    projection_error(diagnostics, "source filename is not a safe leaf");
    return false;
  }
  path /= std::string(filename);
  return true;
}

[[nodiscard]] std::string serialize_source_map(const SourceFile &source) {
  std::string output = "draft-expanded-source-map-v1\n";
  output += "expansions " + std::to_string(source.expansion_maps.size()) + "\n";
  for (const SourceExpansionMap &map : source.expansion_maps) {
    output += "expansion\n";
    output += "generated_begin " + std::to_string(map.generated_begin) + "\n";
    output += "generated_end " + std::to_string(map.generated_end) + "\n";
    append_field("surface_path", map.surface_display_path, output);
    output += "surface_begin_line " +
        std::to_string(map.surface_begin.line) + "\n";
    output += "surface_begin_column " +
        std::to_string(map.surface_begin.column) + "\n";
    output += "surface_end_line " +
        std::to_string(map.surface_end.line) + "\n";
    output += "surface_end_column " +
        std::to_string(map.surface_end.column) + "\n";
    append_field("site", map.site_identity, output);
  }
  return output;
}

[[nodiscard]] std::string serialize_root_manifest(
    const CompileWorkspaceResult &compiled) {
  std::string output = "draft-expanded-source-v1\n";
  if (compiled.resolved_program_digest.has_value()) {
    append_field(
        "resolved_program",
        compiled.resolved_program_digest->hex(),
        output);
  } else {
    append_field("resolved_program", "", output);
  }
  output += "roots " + std::to_string(compiled.graph.roots.size()) + "\n";
  for (std::size_t index = 0; index < compiled.graph.roots.size(); ++index) {
    const PackageRoot &root = compiled.graph.roots[index];
    output += "root " + std::to_string(index) + "\n";
    append_field("kind", root_kind_name(root.kind), output);
    append_field("directory", root_directory_name(index, root.kind), output);
    append_field("identity", root.identity, output);
    append_field("import_prefix", root.import_prefix, output);
  }
  return output;
}

} // namespace

ExpandedSourceProjectionResult materialize_expanded_source(
    const SourceManager &sources,
    const CompileWorkspaceResult &compiled,
    const std::filesystem::path &output_directory,
    DiagnosticSink &diagnostics) {
  ExpandedSourceProjectionResult result;
  if (!compiled.ok || !compiled.resolved_program_digest.has_value()) {
    projection_error(
        diagnostics, "materialization requires a complete checked program");
    return result;
  }
  if (output_directory.empty()) {
    projection_error(diagnostics, "output directory must not be empty");
    return result;
  }

  std::error_code absolute_error;
  const std::filesystem::path output = std::filesystem::absolute(
      output_directory, absolute_error).lexically_normal();
  if (absolute_error) {
    projection_error(
        diagnostics,
        "cannot make output path absolute: " + absolute_error.message());
    return result;
  }
  bool output_missing = false;
  if (!path_is_missing(output, output_missing, diagnostics)) return result;
  if (!output_missing) {
    projection_error(
        diagnostics,
        "output directory already exists: '" + output.string() + "'");
    return result;
  }
  if (!ensure_directory(output.parent_path(), diagnostics)) return result;

  const std::filesystem::path staging = output.string() + ".tmp";
  bool staging_missing = false;
  if (!path_is_missing(staging, staging_missing, diagnostics)) return result;
  if (!staging_missing) {
    projection_error(
        diagnostics,
        "staging directory already exists: '" + staging.string() + "'");
    return result;
  }
  if (!ensure_directory(staging, diagnostics)) return result;
  StagingDirectory transaction(staging);

  if (!write_file(
          transaction.path() / "draft-expanded-source.map",
          serialize_root_manifest(compiled),
          diagnostics)) {
    return result;
  }

  for (const WorkspacePackage &package : compiled.graph.packages) {
    if (package.root >= compiled.graph.roots.size()) {
      projection_error(diagnostics, "package references an invalid root");
      return result;
    }
    const PackageRoot &root = compiled.graph.roots[package.root];
    std::filesystem::path package_output = transaction.path() /
        root_directory_name(package.root, root.kind);
    if (!append_safe_relative_path(
            package_output,
            package.identity.root_relative_path,
            diagnostics)) {
      return result;
    }
    for (const LoadedPackageFile &file : package.loaded.files) {
      std::filesystem::path source_path = package_output;
      if (!append_safe_filename(
              source_path, file.relative_name, diagnostics)) {
        return result;
      }
      const SourceFile &source = sources.file(file.source);
      if (!write_file(source_path, source.text, diagnostics) ||
          !write_file(
              source_path.string() + ".draft-map",
              serialize_source_map(source),
              diagnostics)) {
        return result;
      }
      result.source_files += 1;
      result.mapped_expansions += source.expansion_maps.size();
    }
  }

  std::error_code rename_error;
  std::filesystem::rename(transaction.path(), output, rename_error);
  if (rename_error) {
    projection_error(
        diagnostics,
        "cannot publish output directory '" + output.string() + "': " +
            rename_error.message());
    return result;
  }
  transaction.committed();
  result.output_directory = output;
  result.ok = true;
  return result;
}

} // namespace draft
