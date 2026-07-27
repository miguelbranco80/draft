// Cross-platform current-process executable discovery.
//
// Darwin, Linux, and Windows expose different process-image operations. This
// module owns their temporary buffers and returns one normalized path without
// leaking platform encodings or handles. It deliberately does not inspect
// argv[0] or PATH: both describe invocation spelling rather than the loaded
// executable and would make a relocated distribution depend on the caller's
// working directory.

#include "base/executable_path.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#error "current_executable_path requires a supported Draft host"
#endif

namespace draft {
namespace {

[[nodiscard]] std::optional<std::filesystem::path>
normalized_executable_path(std::filesystem::path path) {
  if (path.empty() || !path.is_absolute()) return std::nullopt;
  std::error_code error;
  const std::filesystem::path normalized =
      std::filesystem::weakly_canonical(path, error);
  return error ? std::optional<std::filesystem::path>{std::move(path)}
               : std::optional<std::filesystem::path>{normalized};
}

} // namespace

std::optional<std::filesystem::path> current_executable_path() {
#if defined(__APPLE__)
  std::uint32_t capacity = 0;
  if (_NSGetExecutablePath(nullptr, &capacity) != -1 || capacity == 0) {
    return std::nullopt;
  }
  std::vector<char> storage(capacity);
  if (_NSGetExecutablePath(storage.data(), &capacity) != 0) {
    return std::nullopt;
  }
  return normalized_executable_path(std::filesystem::path(storage.data()));
#elif defined(__linux__)
  std::vector<char> storage(1024);
  while (storage.size() <= 1024U * 1024U) {
    const ssize_t count =
        ::readlink("/proc/self/exe", storage.data(), storage.size());
    if (count < 0) return std::nullopt;
    const std::size_t length = static_cast<std::size_t>(count);
    if (length < storage.size()) {
      return normalized_executable_path(
          std::filesystem::path(std::string_view(storage.data(), length)));
    }
    storage.resize(storage.size() * 2U);
  }
  return std::nullopt;
#elif defined(_WIN32)
  std::vector<wchar_t> storage(1024);
  while (storage.size() <= 32768U) {
    SetLastError(ERROR_SUCCESS);
    const DWORD capacity = static_cast<DWORD>(storage.size());
    const DWORD count = GetModuleFileNameW(
        nullptr, storage.data(), capacity);
    if (count == 0) return std::nullopt;
    if (count < capacity - 1U ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return normalized_executable_path(
          std::filesystem::path(std::wstring_view(storage.data(), count)));
    }
    storage.resize(storage.size() * 2U);
  }
  return std::nullopt;
#endif
}

} // namespace draft
