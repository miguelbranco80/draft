// External LLVM tool invocation with an explicit version gate.

#include "backend/toolchain.h"

#include "backend/source_correlation.h"
#include "base/content_tree.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
      // variables are inherited. PATH is deliberately empty: every selected
      // Clang, linker, archiver, and debug-linker path is absolute in this mode.
      // LLVM's dsymutil requires writable temporary-state discovery even when
      // the link itself is hermetic. Fixed HOME/TMPDIR values keep that
      // requirement independent of the invoking user. They are scratch
      // locations only and do not enter the published DWARF or companion map.
      std::array<std::string, 5> environment_storage{
          "LANG=C",
          "LC_ALL=C",
          "PATH=",
          "HOME=/",
          "TMPDIR=/tmp",
      };
      std::array<char *, 6> environment{};
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
  arguments.push_back("-isysroot");
  arguments.push_back(inputs.sdk_root.string());
  if (link) {
    arguments.push_back("--ld-path=" + inputs.linker.string());
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

// Clang normally adds `sanitize_address` while translating C-family source.
// Draft hands it already-formed LLVM IR, so the driver flag alone schedules
// global instrumentation but does not opt function bodies into load/store
// checks. Add the standard function attribute to each definition in the
// isolated native-build snapshot. The canonical compiler IR remains unchanged;
// this transformation belongs solely to the versioned validation profile.
[[nodiscard]] bool add_address_sanitizer_function_attributes(
    std::string_view module,
    std::string &instrumented,
    std::string &reason) {
  instrumented.clear();
  instrumented.reserve(module.size() + module.size() / 100U);
  std::size_t cursor = 0;
  while (cursor < module.size()) {
    const std::size_t newline = module.find('\n', cursor);
    const std::size_t end = newline == std::string_view::npos
        ? module.size()
        : newline;
    const std::string_view line = module.substr(cursor, end - cursor);
    if (line.starts_with("define ")) {
      // Draft's emitter keeps each definition header on one line. Parameter
      // types may themselves contain `{`, so the body opener is the last one.
      const std::size_t body = line.rfind('{');
      if (body == std::string_view::npos) {
        reason = "LLVM function definition has no body opener";
        return false;
      }
      // Debug attachments follow function attributes in LLVM grammar. Insert
      // before the first attachment when present, otherwise before the body.
      const std::size_t metadata = line.find(" !", 0);
      const std::size_t insertion =
          metadata != std::string_view::npos && metadata < body
          ? metadata + 1
          : body;
      instrumented.append(line.substr(0, insertion));
      instrumented += "sanitize_address ";
      instrumented.append(line.substr(insertion));
    } else {
      instrumented.append(line);
    }
    if (newline != std::string_view::npos) instrumented.push_back('\n');
    cursor = newline == std::string_view::npos ? module.size() : newline + 1;
  }
  reason.clear();
  return true;
}

// dsymutil owns a conventional directory layout, but a successful process exit
// is not by itself proof that a usable companion was published. Check the two
// files consumed by macOS symbol tooling before hashing or returning the tree.
[[nodiscard]] bool require_regular_file(
    const std::filesystem::path &path,
    std::string_view role,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    diagnostics.error(
        SourceRange::invalid(),
        "Mach-O dSYM does not contain a regular " + std::string(role) +
            " at '" + path.string() + "'");
    return false;
  }
  return true;
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

void add_provider(std::vector<std::string> &providers, std::string_view provider) {
  if (std::find(providers.begin(), providers.end(), provider) == providers.end()) {
    providers.emplace_back(provider);
  }
}

[[nodiscard]] bool has_package_assembly(
    const CompileWorkspaceResult &compiled) {
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (package.has_value() && !package->assembly_sources.empty()) return true;
  }
  return false;
}

[[nodiscard]] bool built_in_provider(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    std::string_view provider) {
  if (provider == "draft_runtime") return true;
  if (provider == "package_assembly") return has_package_assembly(compiled);
  return std::binary_search(
      target.system_link_providers.begin(),
      target.system_link_providers.end(),
      provider);
}

[[nodiscard]] bool verify_provider_set(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    std::vector<VerifiedForeignProviderInput> &verified,
    DiagnosticSink &diagnostics) {
  std::vector<std::string> required;
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    for (const std::string &provider : package->native_interop.providers) {
      add_provider(required, provider);
    }
  }

  if (options.locked || compiled.resolution_manifest.has_value()) {
    if (!compiled.resolution_manifest.has_value() ||
        !verify_foreign_provider_inputs(
            options.foreign_providers,
            compiled.resolution_manifest->external_inputs,
            verified,
            diagnostics)) {
      return false;
    }
    for (const ExternalInputPin &pin : compiled.resolution_manifest->external_inputs) {
      if (pin.kind == ExternalInputKind::ProviderSummary) {
        const bool consumed = std::any_of(
            compiled.foreign_provider_audits.begin(),
            compiled.foreign_provider_audits.end(),
            [&pin](const ForeignProviderAudit &audit) {
              return audit.provider == pin.name &&
                  audit.summary_content_digest == pin.content_digest;
            });
        if (!consumed) {
          diagnostics.error(
              SourceRange::invalid(),
              "provider summary '" + pin.name +
                  "' was not consumed by semantic compilation");
          return false;
        }
        continue;
      }
      // Runtime assets have their own complete-set verifier. They are not
      // foreign symbol providers and therefore do not enter the link list.
      if (pin.kind == ExternalInputKind::RuntimeAsset) continue;
      if (!options.locked &&
          (pin.kind == ExternalInputKind::Toolchain ||
           pin.kind == ExternalInputKind::Sdk)) {
        continue;
      }
      if (pin.kind != ExternalInputKind::Toolchain &&
          pin.kind != ExternalInputKind::Sdk &&
          pin.kind != ExternalInputKind::Object &&
          pin.kind != ExternalInputKind::Archive &&
          pin.kind != ExternalInputKind::SharedLibrary) {
        diagnostics.error(
            SourceRange::invalid(),
            "locked native adapter does not implement external input role '" +
                std::string(external_input_kind_name(pin.kind)) + "'");
        return false;
      }
    }
  } else if (!inspect_foreign_provider_inputs(
                 options.foreign_providers, verified, diagnostics)) {
    return false;
  }

  for (const VerifiedForeignProviderInput &input : verified) {
    if (std::find(required.begin(), required.end(), input.provider) ==
        required.end()) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider mapping '" + input.provider +
              "' is not required by the compiled program");
      return false;
    }
    if (built_in_provider(target, compiled, input.provider)) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider '" + input.provider +
              "' is owned by the compiler or target profile and cannot be remapped");
      return false;
    }
  }
  for (const std::string &provider : required) {
    if (built_in_provider(target, compiled, provider)) continue;
    const bool configured = std::any_of(
        verified.begin(),
        verified.end(),
        [&provider](const VerifiedForeignProviderInput &input) {
          return input.provider == provider;
        });
    if (!configured) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider '" + provider +
              "' has no exact object, archive, or shared-library mapping");
      return false;
    }
  }
  return true;
}

// A manifest makes runtime assets part of the resolved program even for an
// ordinary non-locked build, so every manifest-bearing native invocation must
// prove the complete mapping. Conversely, an asset passed without a manifest
// would affect no program identity and is rejected instead of being ignored.
[[nodiscard]] bool verify_runtime_asset_set(
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    std::vector<VerifiedRuntimeAssetInput> &verified,
    DiagnosticSink &diagnostics) {
  if (compiled.resolution_manifest.has_value()) {
    return verify_runtime_asset_inputs(
        options.runtime_assets,
        compiled.resolution_manifest->external_inputs,
        verified,
        diagnostics);
  }
  if (!options.runtime_assets.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "runtime assets require a resolution manifest; run 'draftc resolve' "
        "with the mappings first");
    return false;
  }
  verified.clear();
  return true;
}

[[nodiscard]] ExternalInputKind provider_external_kind(
    ForeignArtifactKind kind) {
  switch (kind) {
  case ForeignArtifactKind::Object: return ExternalInputKind::Object;
  case ForeignArtifactKind::Archive: return ExternalInputKind::Archive;
  case ForeignArtifactKind::SharedLibrary:
    return ExternalInputKind::SharedLibrary;
  }
  return ExternalInputKind::ForeignArtifact;
}

[[nodiscard]] bool snapshot_locked_providers(
    const std::filesystem::path &build_directory,
    const ResolutionManifest &manifest,
    std::vector<VerifiedForeignProviderInput> &providers,
    DiagnosticSink &diagnostics) {
  for (std::size_t index = 0; index < providers.size(); ++index) {
    VerifiedForeignProviderInput &provider = providers[index];
    std::ifstream input(provider.path, std::ios::binary);
    if (!input) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot snapshot foreign provider '" + provider.provider + "'");
      return false;
    }
    std::string bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot read complete foreign provider '" + provider.provider + "'");
      return false;
    }
    const std::string extension = provider.kind == ForeignArtifactKind::Object
        ? ".o"
        : (provider.kind == ForeignArtifactKind::Archive ? ".a" : ".dylib");
    const std::filesystem::path snapshot =
        build_directory / ("foreign-" + std::to_string(index) + extension);
    std::string reason;
    if (!write_atomic(snapshot, bytes, reason)) {
      diagnostics.error(SourceRange::invalid(), reason);
      return false;
    }
    std::error_code permission_error;
    const std::filesystem::perms permissions =
        std::filesystem::status(provider.path, permission_error).permissions();
    if (permission_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot inspect foreign provider permissions: " +
              permission_error.message());
      return false;
    }
    std::filesystem::permissions(
        snapshot,
        permissions,
        std::filesystem::perm_options::replace,
        permission_error);
    if (permission_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot preserve foreign provider permissions: " +
              permission_error.message());
      return false;
    }
    Sha256Digest digest;
    if (!hash_content_tree(snapshot, digest, diagnostics)) return false;
    const ExternalInputKind expected_kind = provider_external_kind(provider.kind);
    const auto pin = std::find_if(
        manifest.external_inputs.begin(),
        manifest.external_inputs.end(),
        [&](const ExternalInputPin &candidate) {
          return candidate.kind == expected_kind &&
              candidate.name == provider.provider;
        });
    if (pin == manifest.external_inputs.end() ||
        pin->content_digest != digest) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider changed while creating the locked build snapshot: '" +
              provider.provider + "'");
      return false;
    }
    provider.path = snapshot;
  }
  return true;
}

} // namespace

std::string_view native_artifact_kind_name(NativeArtifactKind kind) {
  switch (kind) {
  case NativeArtifactKind::Executable: return "executable";
  case NativeArtifactKind::Object: return "object";
  case NativeArtifactKind::StaticLibrary: return "static-library";
  case NativeArtifactKind::DynamicLibrary: return "dynamic-library";
  case NativeArtifactKind::Assembly: return "assembly";
  }
  return "unknown";
}

NativeBuildResult build_native_artifact(
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
  if (options.instrumentation != NativeInstrumentationProfile::None &&
      options.artifact_kind != NativeArtifactKind::Executable) {
    diagnostics.error(
        SourceRange::invalid(),
        "native instrumentation is currently supported only for validation "
        "executables");
    return result;
  }
  if (options.instrumentation != NativeInstrumentationProfile::None &&
      !options.locked) {
    diagnostics.error(
        SourceRange::invalid(),
        "native instrumentation requires a locked toolchain and SDK so its "
        "compiler pass and runtime have content-pinned identities");
    return result;
  }

  std::vector<VerifiedRuntimeAssetInput> runtime_assets;
  if (!verify_runtime_asset_set(
          compiled, options, runtime_assets, diagnostics)) {
    return result;
  }

  std::vector<VerifiedForeignProviderInput> foreign_providers;
  if (!verify_provider_set(
          target, compiled, options, foreign_providers, diagnostics)) {
    return result;
  }

  VerifiedLockedNativeInputs locked_inputs;
  std::string clang_path = options.clang_path;
  std::string archiver_path = options.archiver_path;
  std::string dsymutil_path = options.dsymutil_path;
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
    archiver_path = locked_inputs.archiver.string();
    dsymutil_path = locked_inputs.dsymutil.string();
  }
  if (options.instrumentation ==
          NativeInstrumentationProfile::AddressSanitizer &&
      !locked_inputs.address_sanitizer_runtime.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked LLVM toolchain does not contain the required versioned "
        "address-sanitizer runtime");
    return result;
  }
  if (options.instrumentation ==
          NativeInstrumentationProfile::AddressSanitizer &&
      !locked_inputs.llvm_symbolizer.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "locked LLVM toolchain does not contain the required versioned "
        "LLVM symbolizer");
    return result;
  }

  std::vector<std::string> version_arguments{clang_path};
  if (options.locked) {
    version_arguments.push_back("--no-default-config");
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
  std::filesystem::path instrumentation_runtime_snapshot;
  if (options.instrumentation ==
      NativeInstrumentationProfile::AddressSanitizer) {
    // Snapshot the already verified runtime before either the linker or the
    // eventual validation process sees it. The snapshot lives in the isolated
    // build directory, is the exact dylib passed to ld, and is later published
    // beside the executable under its fixed relocatable install-name basename.
    instrumentation_runtime_snapshot =
        build_directory / locked_inputs.address_sanitizer_runtime->filename();
    std::filesystem::copy_file(
        *locked_inputs.address_sanitizer_runtime,
        instrumentation_runtime_snapshot,
        std::filesystem::copy_options::overwrite_existing,
        directory_error);
    if (directory_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot snapshot locked address-sanitizer runtime: " +
              directory_error.message());
      return result;
    }
  }
  if (options.locked && !foreign_providers.empty() &&
      !snapshot_locked_providers(
          build_directory,
          *compiled.resolution_manifest,
          foreign_providers,
          diagnostics)) {
    return result;
  }
  const std::filesystem::path output_path(options.output_path);
  if (options.artifact_kind == NativeArtifactKind::Assembly) {
    std::filesystem::create_directories(output_path, directory_error);
    if (directory_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create assembly output directory: " +
              directory_error.message());
      return result;
    }
  }
  std::vector<std::string> objects;
  SourceCorrelationMap source_correlation;
  source_correlation.target_identity = target.facts.identity;
  source_correlation.compiler_identity = compiled.compiler_content_identity;
  if (compiled.resolved_program_digest.has_value()) {
    source_correlation.program_identity = "resolved-program-sha256:" +
        compiled.resolved_program_digest->hex();
  }
  Sha256 direct_module_identity;
  for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
    if (!compiled.packages[index].has_value() ||
        !compiled.packages[index]->llvm.ok) {
      diagnostics.error(
          SourceRange::invalid(), "compiled package has no valid LLVM module");
      return result;
    }
    source_correlation.entries.insert(
        source_correlation.entries.end(),
        compiled.packages[index]->llvm.source_correlations.begin(),
        compiled.packages[index]->llvm.source_correlations.end());
    // Hash each complete module separately before combining its fixed-width
    // digest. This preserves package boundaries without another ad-hoc framing
    // format and gives direct backend users an exact correlation identity.
    direct_module_identity.update(
        sha256(compiled.packages[index]->llvm.text).bytes);
    const std::filesystem::path module =
        build_directory / ("package-" + std::to_string(index) + ".ll");
    const std::string package_stem = "package-" + std::to_string(index);
    const std::filesystem::path object = build_directory / (package_stem + ".o");
    std::string write_error;
    std::string native_module = compiled.packages[index]->llvm.text;
    if (options.instrumentation ==
        NativeInstrumentationProfile::AddressSanitizer) {
      std::string instrumented;
      if (!add_address_sanitizer_function_attributes(
              native_module, instrumented, write_error)) {
        diagnostics.error(
            SourceRange::invalid(),
            "cannot prepare address-instrumented LLVM module: " +
                write_error);
        return result;
      }
      native_module = std::move(instrumented);
    }
    if (!write_atomic(module, native_module, write_error)) {
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
        });
    if (options.instrumentation ==
        NativeInstrumentationProfile::AddressSanitizer) {
      // These are compiler-owned profile options. AddressSanitizer inserts its
      // checks while Clang lowers the emitted LLVM IR; frame pointers make an
      // eventual diagnostic useful without changing Draft language semantics.
      compile_arguments.push_back("-fsanitize=address");
      compile_arguments.push_back("-fno-omit-frame-pointer");
    }
    const bool assembly_output =
        options.artifact_kind == NativeArtifactKind::Assembly;
    const std::filesystem::path compiled_output = assembly_output
        ? output_path / (package_stem + ".s")
        : object;
    compile_arguments.push_back(assembly_output ? "-S" : "-c");
    compile_arguments.push_back(module.string());
    compile_arguments.push_back("-o");
    compile_arguments.push_back(compiled_output.string());
    const ProcessResult compile =
        run_process(compile_arguments, options.locked);
    if (!compile.started || compile.exit_code != 0) {
      diagnostics.error(
          SourceRange::invalid(), phase_failure("LLVM object emission", compile));
      return result;
    }
    if (!assembly_output) objects.push_back(object.string());

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
          (assembly_output ? output_path : build_directory) /
          (stem + rule->extension);
      const std::filesystem::path assembly_object =
          build_directory / (stem + ".o");
      if (!write_atomic(source, input.contents, write_error)) {
        diagnostics.error(SourceRange::invalid(), write_error);
        return result;
      }
      if (assembly_output) continue;
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

  if (source_correlation.program_identity.empty()) {
    source_correlation.program_identity = "llvm-modules-sha256:" +
        direct_module_identity.finalize().hex();
  }

  // Write the map only after every package proved that it has a valid LLVM
  // module. A failed partial build may leave ordinary compiler temporaries in
  // the isolated directory, but it must not publish a complete-looking source
  // correlation artifact for an incomplete graph.
  std::string correlation_error;
  if (!validate_source_correlation_map(
          source_correlation,
          correlation_error)) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot publish native source correlation: " + correlation_error);
    return result;
  }
  const std::string source_correlation_bytes =
      serialize_source_correlation_map(source_correlation);
  const std::filesystem::path source_correlation_path =
      build_directory / "draft-source-correlation.json";
  if (!write_atomic(
          source_correlation_path,
          source_correlation_bytes,
          correlation_error)) {
    diagnostics.error(SourceRange::invalid(), correlation_error);
    return result;
  }
  result.source_correlation_path = source_correlation_path.string();
  result.source_correlation_digest = sha256(source_correlation_bytes);

  if (options.artifact_kind == NativeArtifactKind::Assembly) {
    result.ok = true;
    result.output_path = output_path.string();
    result.runtime_assets = runtime_assets;
    return result;
  }

  for (const VerifiedForeignProviderInput &provider : foreign_providers) {
    if (provider.kind == ForeignArtifactKind::Object) {
      objects.push_back(provider.path.string());
    }
  }

  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path(), directory_error);
    if (directory_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create executable output directory: " + directory_error.message());
      return result;
    }
  }

  if (options.artifact_kind == NativeArtifactKind::StaticLibrary) {
    if (!foreign_providers.empty()) {
      diagnostics.error(
          SourceRange::invalid(),
          "static library output cannot embed a mapped foreign provider; "
          "link that provider when the archive is consumed");
      return result;
    }
    // Archive metadata is part of the emitted artifact, so deterministic mode
    // is not merely a locked-release concern. Apple's host ar has no
    // deterministic switch; its libtool replacement does. A locked build uses
    // the separately verified LLVM ar and therefore has a different interface.
    std::vector<std::string> archive_arguments{archiver_path};
    if (options.locked) {
      archive_arguments.push_back("rcsD");
      archive_arguments.push_back(output_path.string());
    } else {
      archive_arguments.push_back("-static");
      archive_arguments.push_back("-D");
      archive_arguments.push_back("-o");
      archive_arguments.push_back(output_path.string());
    }
    archive_arguments.insert(
        archive_arguments.end(), objects.begin(), objects.end());
    const ProcessResult archive = run_process(archive_arguments, options.locked);
    if (!archive.started || archive.exit_code != 0) {
      diagnostics.error(
          SourceRange::invalid(),
          phase_failure("deterministic static archive emission", archive));
      return result;
    }
    result.ok = true;
    result.output_path = output_path.string();
    result.runtime_assets = runtime_assets;
    return result;
  }

  std::vector<std::string> link_arguments = {
      clang_path,
  };
  if (options.locked) {
    append_locked_arguments(locked_inputs, true, link_arguments);
  }
  // Keep the Mach-O UUID load command. Current macOS loaders require it for an
  // executable, and Apple's linker derives it deterministically from the link
  // result. Reproducible mode additionally prevents debug-map bookkeeping such
  // as input object timestamps from perturbing that result once source-line
  // metadata is present. The native integration gate compares complete output
  // bytes, including the content-derived UUID, across identical rebuilds.
  link_arguments.push_back("-target");
  link_arguments.push_back(target.llvm_triple);
  link_arguments.push_back(
      "-mmacosx-version-min=" + target.minimum_os_version);
  if (options.artifact_kind == NativeArtifactKind::Object) {
    for (const VerifiedForeignProviderInput &provider : foreign_providers) {
      if (provider.kind == ForeignArtifactKind::SharedLibrary) {
        diagnostics.error(
            SourceRange::invalid(),
            "relocatable object output cannot bind shared-library provider '" +
                provider.provider + "'");
        return result;
      }
    }
    link_arguments.push_back("-nostdlib");
    link_arguments.push_back("-Wl,-r");
  } else if (options.artifact_kind == NativeArtifactKind::DynamicLibrary) {
    link_arguments.push_back("-nostdlib");
    link_arguments.push_back("-dynamiclib");
    link_arguments.push_back(
        "-Wl,-install_name,@rpath/" + output_path.filename().string());
  } else if (options.artifact_kind == NativeArtifactKind::Executable) {
    link_arguments.push_back("-nostdlib");
  }
  // Every artifact reaching this point invokes the linker. Relocatable object
  // output also owns a Mach-O debug map, so it needs the same timestamp-
  // ignoring policy as a final executable or dynamic library.
  link_arguments.push_back("-Wl,-reproducible");
  link_arguments.insert(link_arguments.end(), objects.begin(), objects.end());
  if (options.artifact_kind == NativeArtifactKind::Object) {
    for (const VerifiedForeignProviderInput &provider : foreign_providers) {
      if (provider.kind == ForeignArtifactKind::Archive) {
        link_arguments.push_back(provider.path.string());
      }
    }
  }
  if (options.artifact_kind == NativeArtifactKind::Executable ||
      options.artifact_kind == NativeArtifactKind::DynamicLibrary) {
    for (const VerifiedForeignProviderInput &provider : foreign_providers) {
      if (provider.kind != ForeignArtifactKind::Object) {
        link_arguments.push_back(provider.path.string());
      }
    }
    if (options.instrumentation ==
        NativeInstrumentationProfile::AddressSanitizer) {
      link_arguments.push_back(instrumentation_runtime_snapshot.string());
      link_arguments.push_back("-Wl,-rpath,@executable_path");
    }
    link_arguments.push_back("-l" + target.system_link_library);
  }
  link_arguments.push_back("-o");
  link_arguments.push_back(output_path.string());
  const ProcessResult link = run_process(link_arguments, options.locked);
  if (!link.started || link.exit_code != 0) {
    diagnostics.error(
        SourceRange::invalid(),
        phase_failure(
            std::string(native_artifact_kind_name(options.artifact_kind)) +
                " Mach-O link",
            link));
    return result;
  }

  if (options.instrumentation ==
      NativeInstrumentationProfile::AddressSanitizer) {
    const std::filesystem::path published_runtime =
        output_path.parent_path() /
        instrumentation_runtime_snapshot.filename();
    if (published_runtime != instrumentation_runtime_snapshot) {
      directory_error.clear();
      std::filesystem::copy_file(
          instrumentation_runtime_snapshot,
          published_runtime,
          std::filesystem::copy_options::overwrite_existing,
          directory_error);
      if (directory_error) {
        diagnostics.error(
            SourceRange::invalid(),
            "cannot publish address-sanitizer runtime beside executable: " +
                directory_error.message());
        return result;
      }
    }
    if (!hash_content_tree(
            published_runtime,
            result.instrumentation_runtime_digest,
            diagnostics)) {
      return result;
    }
    result.instrumentation_runtime_path = published_runtime.string();
    result.instrumentation_symbolizer_path =
        locked_inputs.llvm_symbolizer->string();
  }

  // A Mach-O link keeps only a debug map in the executable or dylib. Package
  // objects still contain complete DWARF, but ordinary debuggers and
  // source-aware disassemblers expect dsymutil to relocate that data into the
  // conventional sibling bundle. Run it before reporting build success so a
  // "built" native program always has the source information promised by the
  // backend contract.
  if (options.artifact_kind == NativeArtifactKind::Executable ||
      options.artifact_kind == NativeArtifactKind::DynamicLibrary) {
    const std::filesystem::path debug_symbols =
        output_path.string() + ".dSYM";
    std::error_code remove_error;
    std::filesystem::remove_all(debug_symbols, remove_error);
    if (remove_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot replace native debug-symbol bundle: " +
              remove_error.message());
      return result;
    }
    const std::vector<std::string> dsymutil_arguments{
        dsymutil_path,
        "--no-object-timestamp",
        "--no-swiftmodule-timestamp",
        "--num-threads=1",
        "--verify-dwarf=output",
        output_path.string(),
        "-o",
        debug_symbols.string(),
    };
    const ProcessResult debug_link =
        run_process(dsymutil_arguments, options.locked);
    if (!debug_link.started || debug_link.exit_code != 0) {
      diagnostics.error(
          SourceRange::invalid(),
          phase_failure("Mach-O dSYM emission", debug_link));
      return result;
    }

    const std::filesystem::path bundle_plist =
        debug_symbols / "Contents" / "Info.plist";
    const std::filesystem::path linked_dwarf =
        debug_symbols / "Contents" / "Resources" / "DWARF" /
        output_path.filename();
    if (!require_regular_file(
            bundle_plist, "Info.plist", diagnostics) ||
        !require_regular_file(
            linked_dwarf, "linked DWARF payload", diagnostics)) {
      return result;
    }

    // LLVM dsymutil emits a relocation YAML cache for later dSYM rewriting.
    // It is not needed to symbolize this exact binary, and its `binary-path`
    // row names the physical checkout. Remove only that known derived cache;
    // the standard Info.plist and linked DWARF remain intact.
    const std::filesystem::path relocation_cache =
        debug_symbols / "Contents" / "Resources" / "Relocations";
    remove_error.clear();
    std::filesystem::remove_all(relocation_cache, remove_error);
    if (remove_error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot remove path-bearing dSYM relocation cache: " +
              remove_error.message());
      return result;
    }
    if (!hash_content_tree(
            debug_symbols, result.debug_symbols_digest, diagnostics)) {
      return result;
    }
    result.debug_symbols_path = debug_symbols.string();
  }
  result.ok = true;
  result.output_path = output_path.string();
  result.runtime_assets = runtime_assets;
  return result;
}

NativeBuildResult build_native_executable(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    const NativeBuildOptions &options,
    DiagnosticSink &diagnostics) {
  NativeBuildOptions executable_options = options;
  executable_options.artifact_kind = NativeArtifactKind::Executable;
  return build_native_artifact(
      target, compiled, executable_options, diagnostics);
}

} // namespace draft
