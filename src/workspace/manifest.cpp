// Parser for the versioned `draft.workspace` operator configuration.
//
// The format is intentionally line-oriented and closed. The first meaningful
// line is `draft-workspace-v1`; top-level `default` and repeated `exclude`
// assignments may follow. `[build]` supplies workspace-wide defaults and
// `[program name]` supplies one required `root` plus optional overrides and run
// settings. Values are unquoted bytes after `=` with surrounding ASCII space
// removed. There is no interpolation, include mechanism, or hidden host input.

#include "workspace/manifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace draft {
namespace {

enum class SectionKind {
  TopLevel,
  Build,
  Program,
};

[[nodiscard]] std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

[[nodiscard]] bool valid_relative_path(std::string_view text, bool allow_dot) {
  if (allow_dot && text == ".")
    return true;
  if (text.empty() || text.front() == '/' || text.back() == '/' ||
      text.find('\\') != std::string_view::npos ||
      text.find("//") != std::string_view::npos ||
      text.find('\0') != std::string_view::npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t slash = text.find('/', begin);
    const std::size_t end =
        slash == std::string_view::npos ? text.size() : slash;
    const std::string_view component = text.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    begin = end + 1;
  }
  return true;
}

[[nodiscard]] bool valid_name(std::string_view text) {
  if (text.empty())
    return false;
  return std::all_of(text.begin(), text.end(), [](char byte) {
    const unsigned char value = static_cast<unsigned char>(byte);
    return std::isalnum(value) != 0 || byte == '_' || byte == '-';
  });
}

[[nodiscard]] bool parse_boolean(std::string_view text, bool &value) {
  if (text == "on" || text == "true") {
    value = true;
    return true;
  }
  if (text == "off" || text == "false") {
    value = false;
    return true;
  }
  return false;
}

[[nodiscard]] bool assign_once(std::optional<std::string> &destination,
                               std::string_view value, std::string_view key,
                               std::string &reason) {
  if (destination.has_value()) {
    reason = "duplicate '" + std::string(key) + "' assignment";
    return false;
  }
  destination = std::string(value);
  return true;
}

[[nodiscard]] bool assign_boolean(std::optional<bool> &destination,
                                  std::string_view value, std::string_view key,
                                  std::string &reason) {
  if (destination.has_value()) {
    reason = "duplicate '" + std::string(key) + "' assignment";
    return false;
  }
  bool parsed = false;
  if (!parse_boolean(value, parsed)) {
    reason = "'" + std::string(key) + "' must be on or off";
    return false;
  }
  destination = parsed;
  return true;
}

[[nodiscard]] bool parse_build_value(BuildDefaults &build, std::string_view key,
                                     std::string_view value,
                                     std::string &reason) {
  if (key == "target")
    return assign_once(build.target, value, key, reason);
  if (key == "optimization")
    return assign_once(build.optimization, value, key, reason);
  if (key == "kind")
    return assign_once(build.artifact_kind, value, key, reason);
  if (key == "output")
    return assign_once(build.output, value, key, reason);
  if (key == "debug-symbols")
    return assign_boolean(build.debug_symbols, value, key, reason);
  if (key == "assertions")
    return assign_boolean(build.assertions, value, key, reason);
  if (key == "provider") {
    build.providers.emplace_back(value);
    return true;
  }
  if (key == "provider-summary") {
    build.provider_summaries.emplace_back(value);
    return true;
  }
  if (key == "runtime-asset") {
    build.runtime_assets.emplace_back(value);
    return true;
  }
  reason = "unknown build key '" + std::string(key) + "'";
  return false;
}

[[nodiscard]] bool parse_program_value(ProgramConfiguration &program,
                                       std::string_view key,
                                       std::string_view value,
                                       std::string &reason) {
  if (key == "root") {
    if (!program.root.empty()) {
      reason = "duplicate 'root' assignment";
      return false;
    }
    if (!valid_relative_path(value, true)) {
      reason = "program root must be a normalized workspace-relative path";
      return false;
    }
    program.root = std::string(value);
    return true;
  }
  if (key == "argument") {
    program.arguments.emplace_back(value);
    return true;
  }
  if (key == "working-directory") {
    return assign_once(program.working_directory, value, key, reason);
  }
  if (key == "environment") {
    const std::size_t equals = value.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      reason = "environment must be NAME=value";
      return false;
    }
    program.environment.emplace_back(value);
    return true;
  }
  return parse_build_value(program.build, key, value, reason);
}

[[nodiscard]] bool finish_program(const ProgramConfiguration *program,
                                  std::string &reason) {
  if (program != nullptr && program->root.empty()) {
    reason = "program '" + program->name + "' is missing root";
    return false;
  }
  return true;
}

} // namespace

bool load_workspace_manifest(const std::filesystem::path &path,
                             WorkspaceManifest &result, std::string &reason) {
  result = {};
  reason.clear();
  if (path.empty())
    return true;

  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    reason = "cannot open workspace manifest '" + path.string() + "'";
    return false;
  }

  WorkspaceManifest parsed;
  parsed.present = true;
  parsed.path = path;
  bool header_seen = false;
  SectionKind section = SectionKind::TopLevel;
  ProgramConfiguration *program = nullptr;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    const std::string_view row = trim(line);
    if (row.empty() || row.front() == '#')
      continue;
    if (!header_seen) {
      if (row != "draft-workspace-v1") {
        reason = "line " + std::to_string(line_number) +
                 ": expected draft-workspace-v1 header";
        return false;
      }
      header_seen = true;
      continue;
    }

    if (row.front() == '[') {
      if (row.back() != ']') {
        reason = "line " + std::to_string(line_number) +
                 ": unterminated section header";
        return false;
      }
      if (!finish_program(program, reason)) {
        reason = "line " + std::to_string(line_number) + ": " + reason;
        return false;
      }
      const std::string_view name = trim(row.substr(1, row.size() - 2));
      program = nullptr;
      if (name == "build") {
        section = SectionKind::Build;
        continue;
      }
      constexpr std::string_view prefix = "program ";
      if (!name.starts_with(prefix) ||
          !valid_name(trim(name.substr(prefix.size())))) {
        reason = "line " + std::to_string(line_number) +
                 ": expected [build] or [program name]";
        return false;
      }
      const std::string program_name(trim(name.substr(prefix.size())));
      if (find_program_by_name(parsed, program_name) != nullptr) {
        reason = "line " + std::to_string(line_number) +
                 ": duplicate program name '" + program_name + "'";
        return false;
      }
      parsed.programs.push_back({});
      parsed.programs.back().name = program_name;
      program = &parsed.programs.back();
      section = SectionKind::Program;
      continue;
    }

    const std::size_t equals = row.find('=');
    if (equals == std::string_view::npos) {
      reason = "line " + std::to_string(line_number) + ": expected key = value";
      return false;
    }
    const std::string_view key = trim(row.substr(0, equals));
    const std::string_view value = trim(row.substr(equals + 1));
    if (key.empty() || value.empty()) {
      reason = "line " + std::to_string(line_number) +
               ": key and value must not be empty";
      return false;
    }

    std::string row_reason;
    bool accepted = false;
    if (section == SectionKind::TopLevel) {
      if (key == "default") {
        if (!valid_name(value)) {
          row_reason = "default must name a program";
        } else {
          accepted =
              assign_once(parsed.default_program, value, key, row_reason);
        }
      } else if (key == "exclude") {
        if (!valid_relative_path(value, false)) {
          row_reason = "exclude must be a normalized workspace-relative path";
        } else {
          parsed.excludes.emplace_back(value);
          accepted = true;
        }
      } else {
        row_reason = "unknown top-level key '" + std::string(key) + "'";
      }
    } else if (section == SectionKind::Build) {
      accepted = parse_build_value(parsed.build, key, value, row_reason);
    } else {
      accepted = parse_program_value(*program, key, value, row_reason);
    }
    if (!accepted) {
      reason = "line " + std::to_string(line_number) + ": " + row_reason;
      return false;
    }
  }
  if (!stream.eof()) {
    reason = "cannot read workspace manifest '" + path.string() + "'";
    return false;
  }
  if (!header_seen) {
    reason = "line 1: expected draft-workspace-v1 header";
    return false;
  }
  if (!finish_program(program, reason))
    return false;

  std::sort(parsed.excludes.begin(), parsed.excludes.end());
  if (std::adjacent_find(parsed.excludes.begin(), parsed.excludes.end()) !=
      parsed.excludes.end()) {
    reason = "duplicate exclude path";
    return false;
  }
  for (std::size_t left = 0; left < parsed.programs.size(); ++left) {
    for (std::size_t right = left + 1; right < parsed.programs.size();
         ++right) {
      if (parsed.programs[left].root == parsed.programs[right].root) {
        reason = "programs '" + parsed.programs[left].name + "' and '" +
                 parsed.programs[right].name + "' select the same root";
        return false;
      }
    }
  }
  if (parsed.default_program.has_value() &&
      find_program_by_name(parsed, *parsed.default_program) == nullptr) {
    reason =
        "default names an undeclared program '" + *parsed.default_program + "'";
    return false;
  }
  result = std::move(parsed);
  return true;
}

const ProgramConfiguration *
find_program_by_name(const WorkspaceManifest &manifest, std::string_view name) {
  const auto found =
      std::find_if(manifest.programs.begin(), manifest.programs.end(),
                   [name](const ProgramConfiguration &program) {
                     return program.name == name;
                   });
  return found == manifest.programs.end() ? nullptr : &*found;
}

const ProgramConfiguration *
find_program_by_root(const WorkspaceManifest &manifest, std::string_view root) {
  const auto found =
      std::find_if(manifest.programs.begin(), manifest.programs.end(),
                   [root](const ProgramConfiguration &program) {
                     return program.root == root;
                   });
  return found == manifest.programs.end() ? nullptr : &*found;
}

} // namespace draft
