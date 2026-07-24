// Shared interpretation of workspace build policy at the native boundary.
//
// The workspace manifest owns only source spellings and precedence. This module
// turns one already-merged BuildDefaults value into the typed target, artifact,
// optimization, provider, summary, and runtime-asset inputs consumed by native
// compiler clients. It performs no source loading, semantic checking, summary
// I/O, native input validation, or CLI override handling. Consequently draftc
// and embedded clients cannot acquire subtly different manifest grammars while
// retaining their distinct process-facing override policy.
//
// ResolvedBuildPolicy owns every string and path. Relative manifest paths are
// made process-facing against the workspace directory, but are not
// canonicalized or inspected until their owning backend operation. The value is
// operational configuration and never enters Draft semantic identity or a
// content hash. See docs/operations/command-reference.md, "Workspace and root
// selection".

#pragma once

#include "backend/foreign_inputs.h"
#include "backend/foreign_summaries.h"
#include "backend/llvm_object_emitter.h"
#include "backend/runtime_assets.h"
#include "backend/toolchain.h"
#include "target/profile.h"
#include "workspace/manifest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace draft {

// ResolvedBuildPolicy is the typed result of one merged workspace/program build
// layer. target begins with the caller's fallback and is replaced only when the
// manifest selects another built-in profile. runtime_assertions remains a bool
// because this layer describes operator policy rather than compiler phase
// state; compilation clients convert it to RuntimeAssertionMode at their own
// boundary.
struct ResolvedBuildPolicy {
  TargetProfile target;
  std::optional<std::filesystem::path> output_path;
  NativeArtifactKind artifact_kind = NativeArtifactKind::Executable;
  NativeOptimizationLevel optimization = NativeOptimizationLevel::O0;
  bool emit_debug_symbols = false;
  bool runtime_assertions = true;
  std::vector<ForeignProviderInput> foreign_providers;
  std::vector<ForeignProviderSummaryInput> provider_summaries;
  std::vector<RuntimeAssetInput> runtime_assets;
};

// Resolves one complete merged BuildDefaults value. The operation is
// deterministic and publishes result only after every spelling has parsed, so
// callers never observe a partially converted configuration. reason describes
// the manifest value itself; a command or IDE may prefix the selected root.
[[nodiscard]] bool
resolve_build_policy(const std::filesystem::path &workspace_directory,
                     const BuildDefaults &defaults,
                     const TargetProfile &fallback_target,
                     ResolvedBuildPolicy &result, std::string &reason);

} // namespace draft
