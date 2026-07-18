// External LLVM tool invocation with an explicit version gate.

#include "backend/toolchain.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace draft {
namespace {

struct ProcessResult {
  bool started = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

// Runs a process without a shell. Capturing both stdout and stderr into one pipe
// is intentional for version probes and concise diagnostics; compilation output
// is normally empty and is attached to the phase error when nonzero.
[[nodiscard]] ProcessResult run_process(
    const std::vector<std::string> &arguments,
    bool clean_environment) {
  ProcessResult result;
  if (arguments.empty()) {
    result.error = "empty process argument vector";
    return result;
  }
  int pipe_descriptors[2] = {-1, -1};
  if (pipe(pipe_descriptors) != 0) {
    result.error = std::string("pipe failed: ") + std::strerror(errno);
    return result;
  }
  const pid_t child = fork();
  if (child < 0) {
    result.error = std::string("fork failed: ") + std::strerror(errno);
    (void)close(pipe_descriptors[0]);
    (void)close(pipe_descriptors[1]);
    return result;
  }
  if (child == 0) {
    (void)close(pipe_descriptors[0]);
    if (dup2(pipe_descriptors[1], STDOUT_FILENO) < 0 ||
        dup2(pipe_descriptors[1], STDERR_FILENO) < 0) {
      _exit(126);
    }
    (void)close(pipe_descriptors[1]);
    std::vector<char *> raw_arguments;
    raw_arguments.reserve(arguments.size() + 1);
    for (const std::string &argument : arguments) {
      raw_arguments.push_back(const_cast<char *>(argument.c_str()));
    }
    raw_arguments.push_back(nullptr);
    if (clean_environment) {
      // No package, SDK, library, compiler-config, locale, or deployment
      // variables are inherited. PATH is deliberately empty: the Clang driver
      // and linker paths are absolute in this mode.
      std::array<std::string, 3> environment_storage{
          "LANG=C",
          "LC_ALL=C",
          "PATH=",
      };
      std::array<char *, 4> environment{};
      for (std::size_t index = 0; index < environment_storage.size(); ++index) {
        environment[index] = environment_storage[index].data();
      }
      execve(
          raw_arguments[0], raw_arguments.data(), environment.data());
    } else {
      execvp(raw_arguments[0], raw_arguments.data());
    }
    _exit(127);
  }

  result.started = true;
  (void)close(pipe_descriptors[1]);
  char buffer[4096];
  while (true) {
    const ssize_t count = read(pipe_descriptors[0], buffer, sizeof(buffer));
    if (count > 0) {
      result.output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  (void)close(pipe_descriptors[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      result.error = std::string("waitpid failed: ") + std::strerror(errno);
      return result;
    }
  }
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

// Appends flags that make every locked Clang phase independent of driver
// configuration files and host SDK discovery. The linker is an absolute member
// of the already hashed toolchain tree.
void append_locked_arguments(
    const VerifiedLockedNativeInputs &inputs,
    bool link,
    std::vector<std::string> &arguments) {
  arguments.push_back("--no-default-config");
  arguments.push_back("--no-xcselect");
  arguments.push_back("-isysroot");
  arguments.push_back(inputs.sdk_root.string());
  if (link) {
    arguments.push_back("--ld-path=" + inputs.linker.string());
    // Mach-O UUID generation is unnecessary for the bootstrap executable and
    // can otherwise introduce a link-specific identity into repeat builds.
    arguments.push_back("-Wl,-no_uuid");
  }
}

[[nodiscard]] bool is_pinned_llvm(std::string_view version) {
  return version.find("clang version 22.1.") != std::string_view::npos ||
      version.find("LLVM version 22.1.") != std::string_view::npos;
}

[[nodiscard]] bool write_atomic(
    const std::filesystem::path &path,
    std::string_view contents,
    std::string &reason) {
  reason.clear();
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      reason = "cannot open temporary file '" + temporary.string() + "'";
      return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.flush();
    if (!stream) {
      reason = "cannot write temporary file '" + temporary.string() + "'";
      return false;
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    reason = "cannot commit file '" + path.string() + "': " +
        rename_error.message();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return true;
}

[[nodiscard]] std::string phase_failure(
    std::string_view phase, const ProcessResult &process) {
  std::string message(phase);
  message += " failed with exit code ";
  message += std::to_string(process.exit_code);
  if (!process.output.empty()) {
    message += ":\n";
    message += process.output;
  }
  if (!process.error.empty()) {
    message += ": ";
    message += process.error;
  }
  return message;
}

// Resolves a selected filename back to the already validated target rule. A
// null result is a compiled-input/profile mismatch and is diagnosed before an
// ambient tool can infer language behavior from the filename.
[[nodiscard]] const AssemblyFileRule *assembly_rule(
    const TargetProfile &target, std::string_view relative_name) {
  const std::string extension =
      std::filesystem::path(relative_name).extension().string();
  for (const AssemblyFileRule &rule : target.assembly_files) {
    if (rule.extension == extension) return &rule;
  }
  return nullptr;
}

} // namespace

NativeBuildResult build_native_executable(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    DiagnosticSink &diagnostics) {
  NativeBuildResult result;
  if (!compiled.ok) {
    diagnostics.error(
        SourceRange::invalid(), "native build requires a successful compiled workspace");
    return result;
  }
  if (options.build_directory.empty() || options.output_path.empty()) {
    diagnostics.error(
        SourceRange::invalid(), "native build directory and output path are required");
    return result;
  }

  if (options.locked && options.allow_unpinned_toolchain) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked native build cannot allow an unpinned host toolchain");
    return result;
  }

  VerifiedLockedNativeInputs locked_inputs;
  std::string clang_path = options.clang_path;
  if (options.locked) {
    if (!std::filesystem::path(options.build_directory).is_absolute() ||
        !std::filesystem::path(options.output_path).is_absolute()) {
      diagnostics.error(
          SourceRange::invalid(),
          "locked native build and output paths must be absolute");
      return result;
    }
    if (!compiled.resolution_manifest.has_value()) {
      diagnostics.error(
          SourceRange::invalid(),
          "locked native build requires a verified resolution manifest; "
          "run 'draftc resolve' with explicit toolchain and SDK roots");
      return result;
    }
    if (compiled.resolution_manifest->target_identity !=
        target.facts.identity) {
      diagnostics.error(
          SourceRange::invalid(),
          "locked native build manifest target does not match selected target");
      return result;
    }
    if (!verify_locked_native_inputs(
            options.locked_inputs,
            compiled.resolution_manifest->external_inputs,
            locked_inputs,
            diagnostics)) {
      return result;
    }
    clang_path = locked_inputs.clang.string();
  }

  std::vector<std::string> version_arguments{clang_path};
  if (options.locked) {
    version_arguments.push_back("--no-default-config");
    version_arguments.push_back("--no-xcselect");
  }
  version_arguments.push_back("--version");
  const ProcessResult version =
      run_process(version_arguments, options.locked);
  if (!version.started || version.exit_code != 0) {
    diagnostics.error(
        SourceRange::invalid(), phase_failure("LLVM toolchain version probe", version));
    return result;
  }
  result.toolchain_version = version.output;
  if (!options.allow_unpinned_toolchain && !is_pinned_llvm(version.output)) {
    diagnostics.error(
        SourceRange::invalid(),
        "toolchain is not the required LLVM/Clang 22.1.x distribution; "
        "use an explicitly pinned toolchain (development builds may opt into "
        "--allow-host-toolchain)");
    return result;
  }

  std::error_code directory_error;
  std::filesystem::create_directories(options.build_directory, directory_error);
  if (directory_error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot create native build directory: " + directory_error.message());
    return result;
  }
  const std::filesystem::path build_directory(options.build_directory);
  std::vector<std::string> objects;
  for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
    if (!compiled.packages[index].has_value() ||
        !compiled.packages[index]->llvm.ok) {
      diagnostics.error(
          SourceRange::invalid(), "compiled package has no valid LLVM module");
      return result;
    }
    const std::filesystem::path module =
        build_directory / ("package-" + std::to_string(index) + ".ll");
    const std::filesystem::path object =
        build_directory / ("package-" + std::to_string(index) + ".o");
    std::string write_error;
    if (!write_atomic(module, compiled.packages[index]->llvm.text, write_error)) {
      diagnostics.error(SourceRange::invalid(), write_error);
      return result;
    }
    std::vector<std::string> compile_arguments{clang_path};
    if (options.locked) {
      append_locked_arguments(locked_inputs, false, compile_arguments);
    }
    compile_arguments.insert(
        compile_arguments.end(),
        {
            "-target",
            target.llvm_triple,
            "-mmacosx-version-min=" + target.minimum_os_version,
            "-x",
            "ir",
            "-c",
            module.string(),
            "-o",
            object.string(),
        });
    const ProcessResult compile =
        run_process(compile_arguments, options.locked);
    if (!compile.started || compile.exit_code != 0) {
      diagnostics.error(
          SourceRange::invalid(), phase_failure("LLVM object emission", compile));
      return result;
    }
    objects.push_back(object.string());

    // Package assembly is an ordinary selected source input, not inline Draft
    // assembly.  Write the captured bytes rather than rereading the workspace,
    // and force Clang's assembler language for every extension.  In particular
    // this prevents Clang's ambient `.S` convention from invoking a C
    // preprocessor when the Draft target profile says preprocessing is None.
    for (std::size_t assembly_index = 0;
         assembly_index < compiled.packages[index]->assembly_sources.size();
         ++assembly_index) {
      const CompiledAssemblySource &input =
          compiled.packages[index]->assembly_sources[assembly_index];
      const AssemblyFileRule *rule = assembly_rule(target, input.relative_name);
      if (rule == nullptr ||
          rule->preprocessing != AssemblyPreprocessing::None) {
        diagnostics.error(
            SourceRange::invalid(),
            "package assembly input '" + input.relative_name +
                "' has no exact non-preprocessed target rule");
        return result;
      }
      const std::string stem = "package-" + std::to_string(index) +
          "-assembly-" + std::to_string(assembly_index);
      const std::filesystem::path source =
          build_directory / (stem + rule->extension);
      const std::filesystem::path assembly_object =
          build_directory / (stem + ".o");
      if (!write_atomic(source, input.contents, write_error)) {
        diagnostics.error(SourceRange::invalid(), write_error);
        return result;
      }
      std::vector<std::string> assemble_arguments{clang_path};
      if (options.locked) {
        append_locked_arguments(locked_inputs, false, assemble_arguments);
      }
      assemble_arguments.insert(
          assemble_arguments.end(),
          {
              "-target",
              target.llvm_triple,
              "-mmacosx-version-min=" + target.minimum_os_version,
              "-x",
              "assembler",
              "-c",
              source.string(),
              "-o",
              assembly_object.string(),
          });
      const ProcessResult assemble =
          run_process(assemble_arguments, options.locked);
      if (!assemble.started || assemble.exit_code != 0) {
        diagnostics.error(
            SourceRange::invalid(),
            phase_failure(
                "assembly object emission for '" + input.relative_name + "'",
                assemble));
        return result;
      }
      objects.push_back(assembly_object.string());
    }
  }

  std::filesystem::path output_path(options.output_path);
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path(), directory_error);
    if (directory_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create executable output directory: " + directory_error.message());
      return result;
    }
  }
  std::vector<std::string> link_arguments = {
      clang_path,
  };
  if (options.locked) {
    append_locked_arguments(locked_inputs, true, link_arguments);
  }
  link_arguments.push_back("-target");
  link_arguments.push_back(target.llvm_triple);
  link_arguments.push_back(
      "-mmacosx-version-min=" + target.minimum_os_version);
  link_arguments.insert(link_arguments.end(), objects.begin(), objects.end());
  link_arguments.push_back("-o");
  link_arguments.push_back(output_path.string());
  const ProcessResult link = run_process(link_arguments, options.locked);
  if (!link.started || link.exit_code != 0) {
    diagnostics.error(
        SourceRange::invalid(), phase_failure("Mach-O link", link));
    return result;
  }
  result.ok = true;
  result.output_path = output_path.string();
  return result;
}

} // namespace draft
