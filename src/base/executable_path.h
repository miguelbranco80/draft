// Current-process executable discovery for relocatable compiler distributions.
//
// This base utility converts the host kernel's executable identity into one
// absolute normalized filesystem path. It owns no persistent state and knows
// nothing about Draft packages, LLVM, or installation policy. Higher layers may
// use the returned directory to locate sibling distribution resources; failure
// remains explicit so a development build can use its configured fallback.

#pragma once

#include <filesystem>
#include <optional>

namespace draft {

// Returns the physical path of the executable image for this process. The path
// is absolute and weakly canonical when host APIs and the filesystem permit it.
// An unavailable or truncated host result returns nullopt rather than guessing
// from argv[0], PATH, or the current working directory.
[[nodiscard]] std::optional<std::filesystem::path> current_executable_path();

} // namespace draft
