// Process-unique temporary-directory ownership for compiler tests.
//
// Many Draft tests construct complete workspaces, fake toolchains, or evidence
// stores. Those trees are test resources rather than compiler data: one test
// process owns a tree from construction until scope exit, and no path may be
// shared with a concurrent invocation from another build directory or Git
// worktree. This header claims a directory atomically below the platform temp
// root and removes only the directory that the object successfully claimed.
//
// The physical path is deliberately non-semantic. A process identifier and a
// process-local sequence distinguish concurrent owners; an atomic filesystem
// create handles stale paths after PID reuse. Tests must never include the path
// in a content identity or expected diagnostic.

#pragma once

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace draft::test {

// Owns one atomically claimed test directory.
//
// Construction is intentionally fail-fast because inability to create an
// isolated fixture is a test-runner failure, not a compiler result that an
// individual assertion can usefully recover from. The directory remains at a
// stable path until destruction. The type is neither copyable nor movable so a
// reader can identify its one lexical owner and the destructor can remove
// exactly that owner's tree.
class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string_view label) {
    if (!valid_label(label)) {
      std::cerr << "invalid temporary-directory label: " << label << '\n';
      std::exit(EXIT_FAILURE);
    }

    std::error_code error;
    const std::filesystem::path base =
        std::filesystem::temp_directory_path(error);
    if (error) fail("find the platform temporary directory", error);

    const std::uint64_t process = process_id();
    for (std::uint64_t attempt = 0; attempt != 256; ++attempt) {
      const std::uint64_t serial =
          next_serial_.fetch_add(1, std::memory_order_relaxed);
      path_ = base /
          (std::string(label) + "-" + std::to_string(process) + "-" +
           std::to_string(serial));
      const bool created = std::filesystem::create_directory(path_, error);
      if (created) return;
      if (error == std::errc::file_exists) {
        error.clear();
        continue;
      }
      fail("claim temporary directory", error);
    }

    std::cerr << "could not claim a unique temporary directory for " << label
              << " after 256 attempts\n";
    std::exit(EXIT_FAILURE);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  TemporaryDirectory(TemporaryDirectory &&) = delete;
  TemporaryDirectory &operator=(TemporaryDirectory &&) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

 private:
  // Labels become one filename component. Restricting them to visible ASCII
  // prevents a test name from accidentally introducing a parent traversal or
  // platform-specific separator into the cleanup target.
  [[nodiscard]] static bool valid_label(std::string_view label) {
    if (label.empty()) return false;
    for (const char raw_byte : label) {
      const unsigned char byte = static_cast<unsigned char>(raw_byte);
      if (!std::isalnum(byte) && byte != '-' && byte != '_') return false;
    }
    return true;
  }

  [[nodiscard]] static std::uint64_t process_id() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
  }

  [[noreturn]] static void fail(
      std::string_view operation,
      const std::error_code &error) {
    std::cerr << "cannot " << operation << ": " << error.message() << '\n';
    std::exit(EXIT_FAILURE);
  }

  inline static std::atomic<std::uint64_t> next_serial_{0};
  std::filesystem::path path_;
};

}  // namespace draft::test
