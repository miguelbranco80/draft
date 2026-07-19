// Direct Mach-O load-command parsing for locked toolchain dependency closure.

#include "backend/macho_dependencies.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

constexpr std::uint32_t kMachOMagic64 = 0xfeedfacfU;
constexpr std::uint32_t kCpuTypeArm64 = 0x0100000cU;
constexpr std::uint32_t kMachOExecute = 2;
constexpr std::uint32_t kMachODylib = 6;
constexpr std::uint32_t kLoadDylib = 0x0000000cU;
constexpr std::uint32_t kIdDylib = 0x0000000dU;
constexpr std::uint32_t kLoadDylinker = 0x0000000eU;
constexpr std::uint32_t kLoadWeakDylib = 0x80000018U;
constexpr std::uint32_t kRpath = 0x8000001cU;
constexpr std::uint32_t kReexportDylib = 0x8000001fU;
constexpr std::uint32_t kLazyLoadDylib = 0x00000020U;
constexpr std::uint32_t kLoadUpwardDylib = 0x80000023U;
constexpr std::uint32_t kDyldEnvironment = 0x00000027U;
constexpr std::uint64_t kMaximumLoadCommands = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumCommandCount = 65536;

struct MachOImage {
  std::uint32_t file_type = 0;
  std::vector<std::string> dependencies;
  std::vector<std::string> runpaths;
  std::optional<std::string> dylib_id;
};

[[nodiscard]] std::uint32_t read_u32(
    const unsigned char *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
      (static_cast<std::uint32_t>(bytes[1]) << 8U) |
      (static_cast<std::uint32_t>(bytes[2]) << 16U) |
      (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool read_exact(
    std::ifstream &input,
    unsigned char *bytes,
    std::size_t size) {
  input.read(
      reinterpret_cast<char *>(bytes),
      static_cast<std::streamsize>(size));
  return input.good() ||
      input.gcount() == static_cast<std::streamsize>(size);
}

[[nodiscard]] bool command_string(
    const std::vector<unsigned char> &commands,
    std::size_t command_begin,
    std::uint32_t command_size,
    std::uint32_t string_offset,
    std::string &value) {
  if (string_offset >= command_size) return false;
  const std::size_t begin = command_begin + string_offset;
  const std::size_t end = command_begin + command_size;
  std::size_t cursor = begin;
  while (cursor < end && commands[cursor] != 0) ++cursor;
  if (cursor == end || cursor == begin) return false;
  value.assign(
      reinterpret_cast<const char *>(commands.data() + begin),
      cursor - begin);
  return true;
}

[[nodiscard]] bool is_dependency_command(std::uint32_t command) {
  return command == kLoadDylib || command == kLoadWeakDylib ||
      command == kReexportDylib || command == kLazyLoadDylib ||
      command == kLoadUpwardDylib;
}

[[nodiscard]] bool read_image(
    const std::filesystem::path &path,
    MachOImage &image,
    std::string &reason) {
  reason.clear();
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size < 32) {
    reason = "file is too small for a Mach-O 64 header";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    reason = "file cannot be opened";
    return false;
  }
  unsigned char header[32]{};
  if (!read_exact(input, header, sizeof(header))) {
    reason = "Mach-O header is truncated";
    return false;
  }
  if (read_u32(header) != kMachOMagic64) {
    reason = "file is not a thin little-endian Mach-O 64 image";
    return false;
  }
  if (read_u32(header + 4) != kCpuTypeArm64) {
    reason = "Mach-O image is not AArch64";
    return false;
  }
  const std::uint32_t command_count = read_u32(header + 16);
  const std::uint32_t command_bytes = read_u32(header + 20);
  if (command_count > kMaximumCommandCount ||
      command_bytes > kMaximumLoadCommands ||
      static_cast<std::uint64_t>(command_bytes) + sizeof(header) > size) {
    reason = "Mach-O load-command table is invalid or too large";
    return false;
  }
  std::vector<unsigned char> commands(command_bytes);
  if (!commands.empty() &&
      !read_exact(input, commands.data(), commands.size())) {
    reason = "Mach-O load-command table is truncated";
    return false;
  }

  MachOImage parsed;
  parsed.file_type = read_u32(header + 12);
  std::size_t offset = 0;
  for (std::uint32_t index = 0; index < command_count; ++index) {
    if (offset + 8 > commands.size()) {
      reason = "Mach-O load command header is truncated";
      return false;
    }
    const std::uint32_t command = read_u32(commands.data() + offset);
    const std::uint32_t command_size =
        read_u32(commands.data() + offset + 4);
    if (command_size < 8 || (command_size % 8) != 0 ||
        command_size > commands.size() - offset) {
      reason = "Mach-O load command has an invalid size";
      return false;
    }
    if (is_dependency_command(command) || command == kIdDylib) {
      if (command_size < 24) {
        reason = "Mach-O dylib command is truncated";
        return false;
      }
      std::string value;
      if (!command_string(
              commands,
              offset,
              command_size,
              read_u32(commands.data() + offset + 8),
              value)) {
        reason = "Mach-O dylib command has an invalid path";
        return false;
      }
      if (command == kIdDylib) {
        if (parsed.dylib_id.has_value()) {
          reason = "Mach-O image has duplicate dylib IDs";
          return false;
        }
        parsed.dylib_id = std::move(value);
      } else {
        parsed.dependencies.push_back(std::move(value));
      }
    } else if (command == kLoadDylinker) {
      // LC_LOAD_DYLINKER has the same string-offset prefix as an rpath
      // command, not the longer dylib-command structure. Treat its path as a
      // dependency so only the sealed-system loader can leave the pinned tree.
      if (command_size < 12) {
        reason = "Mach-O dynamic-linker command is truncated";
        return false;
      }
      std::string value;
      if (!command_string(
              commands,
              offset,
              command_size,
              read_u32(commands.data() + offset + 8),
              value)) {
        reason = "Mach-O dynamic-linker command has an invalid path";
        return false;
      }
      parsed.dependencies.push_back(std::move(value));
    } else if (command == kDyldEnvironment) {
      // A binary-level DYLD_* assignment would restore ambient loader search
      // state even though the compiler starts every locked child with a clean
      // process environment. None is needed by the selected distribution.
      reason = "Mach-O image embeds a dynamic-loader environment variable";
      return false;
    } else if (command == kRpath) {
      if (command_size < 12) {
        reason = "Mach-O runpath command is truncated";
        return false;
      }
      std::string value;
      if (!command_string(
              commands,
              offset,
              command_size,
              read_u32(commands.data() + offset + 8),
              value)) {
        reason = "Mach-O runpath command has an invalid path";
        return false;
      }
      parsed.runpaths.push_back(std::move(value));
    }
    offset += command_size;
  }
  if (offset != commands.size()) {
    reason = "Mach-O load-command byte count is inconsistent";
    return false;
  }
  image = std::move(parsed);
  return true;
}

[[nodiscard]] bool is_system_dependency(std::string_view path) {
  return path.starts_with("/usr/lib/") ||
      path.starts_with("/System/Library/");
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path &root,
    const std::filesystem::path &candidate) {
  auto root_component = root.begin();
  auto candidate_component = candidate.begin();
  for (; root_component != root.end();
       ++root_component, ++candidate_component) {
    if (candidate_component == candidate.end() ||
        *candidate_component != *root_component) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::filesystem::path> expand_loader_path(
    std::string_view spelling,
    const std::filesystem::path &image,
    const std::filesystem::path &executable) {
  constexpr std::string_view loader = "@loader_path";
  constexpr std::string_view main = "@executable_path";
  if (spelling == loader) return image.parent_path();
  if (spelling.starts_with("@loader_path/")) {
    return image.parent_path() /
        std::string(spelling.substr(loader.size() + 1));
  }
  if (spelling == main) return executable.parent_path();
  if (spelling.starts_with("@executable_path/")) {
    return executable.parent_path() /
        std::string(spelling.substr(main.size() + 1));
  }
  return std::nullopt;
}

[[nodiscard]] bool canonical_internal_path(
    const std::filesystem::path &root,
    const std::filesystem::path &candidate,
    std::filesystem::path &canonical) {
  std::error_code error;
  const std::filesystem::path resolved =
      std::filesystem::canonical(candidate, error);
  if (error || !path_is_within(root, resolved)) return false;
  const std::filesystem::file_status status =
      std::filesystem::status(resolved, error);
  if (error || !std::filesystem::is_regular_file(status)) return false;
  canonical = resolved;
  return true;
}

[[nodiscard]] std::string display_path(
    const std::filesystem::path &root,
    const std::filesystem::path &path) {
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(path, root, error);
  return error ? path.generic_string() : relative.generic_string();
}

struct ClosureState {
  std::filesystem::path root;
  std::vector<std::filesystem::path> visited;
  DiagnosticSink *diagnostics = nullptr;
};

[[nodiscard]] bool validate_image(
    ClosureState &state,
    const std::filesystem::path &image_path,
    const std::filesystem::path &executable_path,
    std::uint32_t expected_type,
    std::vector<std::filesystem::path> inherited_runpaths) {
  if (std::find(
          state.visited.begin(), state.visited.end(), image_path) !=
      state.visited.end()) {
    return true;
  }
  state.visited.push_back(image_path);

  MachOImage image;
  std::string reason;
  if (!read_image(image_path, image, reason)) {
    state.diagnostics->error(
        SourceRange::invalid(),
        "locked toolchain image '" + display_path(state.root, image_path) +
            "' is invalid: " + reason);
    return false;
  }
  if (image.file_type != expected_type) {
    state.diagnostics->error(
        SourceRange::invalid(),
        "locked toolchain image '" + display_path(state.root, image_path) +
            "' has the wrong Mach-O file type");
    return false;
  }
  if (image.dylib_id.has_value() &&
      (image.dylib_id->starts_with('/') ||
       (!image.dylib_id->starts_with("@rpath/") &&
        !image.dylib_id->starts_with("@loader_path/") &&
        !image.dylib_id->starts_with("@executable_path/")))) {
    state.diagnostics->error(
        SourceRange::invalid(),
        "locked toolchain dylib '" + display_path(state.root, image_path) +
            "' has non-relocatable ID '" + *image.dylib_id + "'");
    return false;
  }

  for (const std::string &runpath : image.runpaths) {
    const std::optional<std::filesystem::path> expanded =
        expand_loader_path(runpath, image_path, executable_path);
    if (!expanded.has_value()) {
      state.diagnostics->error(
          SourceRange::invalid(),
          "locked toolchain image '" + display_path(state.root, image_path) +
              "' has non-relocatable runpath '" + runpath + "'");
      return false;
    }
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::canonical(*expanded, error);
    if (error || !path_is_within(state.root, canonical) ||
        !std::filesystem::is_directory(canonical, error) || error) {
      state.diagnostics->error(
          SourceRange::invalid(),
          "locked toolchain image '" + display_path(state.root, image_path) +
              "' has runpath outside the pinned root: '" + runpath + "'");
      return false;
    }
    if (std::find(
            inherited_runpaths.begin(),
            inherited_runpaths.end(),
            canonical) == inherited_runpaths.end()) {
      inherited_runpaths.push_back(canonical);
    }
  }

  for (const std::string &dependency : image.dependencies) {
    if (is_system_dependency(dependency)) continue;
    std::vector<std::filesystem::path> candidates;
    if (dependency.starts_with("@rpath/")) {
      const std::string suffix = dependency.substr(7);
      for (const std::filesystem::path &runpath : inherited_runpaths) {
        std::filesystem::path canonical;
        if (canonical_internal_path(
                state.root, runpath / suffix, canonical)) {
          candidates.push_back(std::move(canonical));
        }
      }
    } else if (dependency.starts_with("@loader_path/") ||
               dependency.starts_with("@executable_path/")) {
      const std::optional<std::filesystem::path> expanded =
          expand_loader_path(dependency, image_path, executable_path);
      std::filesystem::path canonical;
      if (expanded.has_value() && canonical_internal_path(
              state.root, *expanded, canonical)) {
        candidates.push_back(std::move(canonical));
      }
    } else {
      state.diagnostics->error(
          SourceRange::invalid(),
          "locked toolchain image '" + display_path(state.root, image_path) +
              "' loads ambient dependency '" + dependency + "'");
      return false;
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() != 1) {
      state.diagnostics->error(
          SourceRange::invalid(),
          "locked toolchain image '" + display_path(state.root, image_path) +
              "' cannot resolve exactly one pinned dependency for '" +
              dependency + "'");
      return false;
    }
    if (!validate_image(
            state,
            candidates.front(),
            executable_path,
            kMachODylib,
            inherited_runpaths)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool validate_closure(
    const std::filesystem::path &root,
    std::span<const std::filesystem::path> entries,
    std::uint32_t entry_type,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  std::error_code error;
  const std::filesystem::path canonical_root =
      std::filesystem::canonical(root, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize locked Mach-O toolchain root: " +
            error.message());
    return false;
  }
  for (const std::filesystem::path &entry : entries) {
    // @executable_path is relative to each entry. Keep its visited closure
    // separate so a shared dylib is rechecked under the correct main image.
    ClosureState state;
    state.root = canonical_root;
    state.diagnostics = &diagnostics;
    std::filesystem::path canonical_entry;
    if (!canonical_internal_path(
            canonical_root, entry, canonical_entry)) {
      diagnostics.error(
          SourceRange::invalid(),
          "locked Mach-O toolchain entry is outside the pinned root");
      return false;
    }
    if (!validate_image(
            state, canonical_entry, canonical_entry, entry_type, {})) {
      return false;
    }
  }
  return diagnostics.error_count() == initial_errors;
}

} // namespace

bool validate_macho_dependency_closure(
    const std::filesystem::path &root,
    std::span<const std::filesystem::path> entries,
    DiagnosticSink &diagnostics) {
  return validate_closure(root, entries, kMachOExecute, diagnostics);
}

bool validate_macho_dylib_dependency_closure(
    const std::filesystem::path &root,
    std::span<const std::filesystem::path> entries,
    DiagnosticSink &diagnostics) {
  return validate_closure(root, entries, kMachODylib, diagnostics);
}

} // namespace draft
