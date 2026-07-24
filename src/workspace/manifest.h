// Durable Draft workspace and build/run configuration.
//
// `draft.workspace` establishes the filesystem boundary used for imports and
// `.draft/` state. Its optional contents name programs and record operator
// defaults; it never enumerates source files, downloads dependencies, or
// changes language semantics. This module owns the small parsed value model and
// deterministic line parser. The driver and IDE may consume that model, while
// the syntax/workspace graph layers remain independent of command policy.
//
// Values are copied from the manifest because the configuration survives the
// input stream. Paths remain workspace-relative spellings until a consumer
// canonicalizes them at an I/O boundary. Repeated list fields preserve source
// order. Relevant language semantics remain in specification section 3; this
// is the operational contract documented in command-reference.md.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// BuildDefaults contains only spellings also accepted by the public driver.
// Empty optionals mean "use the command default". List values are repeated
// command inputs in manifest order; parsing their provider-specific grammar is
// deliberately left to the same driver helpers used for CLI arguments.
struct BuildDefaults {
  std::optional<std::string> target;
  std::optional<std::string> optimization;
  std::optional<std::string> artifact_kind;
  std::optional<std::string> output;
  std::optional<bool> debug_symbols;
  std::optional<bool> assertions;
  std::vector<std::string> providers;
  std::vector<std::string> provider_summaries;
  std::vector<std::string> runtime_assets;
};

// ProgramConfiguration gives one executable root a stable human name plus its
// build overrides and run defaults. arguments excludes argv[0]. environment
// rows are `NAME=value` overrides applied to the inherited environment.
// working_directory and output are relative to the workspace unless absolute;
// root is always a normalized workspace-relative package path.
struct ProgramConfiguration {
  std::string name;
  std::string root;
  BuildDefaults build;
  std::vector<std::string> arguments;
  std::optional<std::string> working_directory;
  std::vector<std::string> environment;
};

// WorkspaceManifest is empty but valid when no marker exists. A present marker
// must carry the version header even when it is used only to establish a
// boundary. exclude entries are normalized relative directory paths pruned by
// recursive program discovery; a directly named package remains selectable.
struct WorkspaceManifest {
  bool present = false;
  std::filesystem::path path;
  std::optional<std::string> default_program;
  std::vector<std::string> excludes;
  BuildDefaults build;
  std::vector<ProgramConfiguration> programs;
};

// Applies one more-specific build layer to an already copied less-specific
// layer. Scalar options replace earlier values; repeated native inputs append
// in source order so a workspace can supply common inputs and one program can
// add its own. The operation owns no command defaults and parses no backend
// spelling, making it the shared policy primitive for the CLI and embeddings.
void merge_build_defaults(BuildDefaults &destination,
                          const BuildDefaults &overrides);

// Returns the workspace build layer followed by the exact program-root layer,
// when one exists. The returned value owns every string and remains valid
// independently of manifest. Explicit command or UI overrides belong to the
// caller and are deliberately not represented here.
[[nodiscard]] BuildDefaults
effective_build_defaults(const WorkspaceManifest &manifest,
                         std::string_view root);

// Resolves only the path suffix of a `name:path` or `name=kind:path` manifest
// mapping against workspace_directory. Absolute suffixes and the logical
// prefix remain exact. The parser intentionally leaves paths lexical; this is
// the shared process-facing conversion used by CLI and IDE consumers.
[[nodiscard]] std::string
resolve_manifest_mapping_path(const std::filesystem::path &workspace_directory,
                              std::string_view spelling);

// Reads and parses one already-located marker. An empty path returns a valid
// absent manifest. Failure reports one stable reason including a one-based line
// where applicable and leaves result in its zero state.
[[nodiscard]] bool load_workspace_manifest(const std::filesystem::path &path,
                                           WorkspaceManifest &result,
                                           std::string &reason);

// Returns a named program or a program whose normalized root matches the given
// package identity. Ambiguity is rejected during parsing, so at most one row is
// returned.
[[nodiscard]] const ProgramConfiguration *
find_program_by_name(const WorkspaceManifest &manifest, std::string_view name);
[[nodiscard]] const ProgramConfiguration *
find_program_by_root(const WorkspaceManifest &manifest, std::string_view root);

} // namespace draft
