// Complete native test/benchmark execution and evidence recording.

#pragma once

#include "backend/toolchain.h"
#include "sema/foreign_summary.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "validation/discovery.h"
#include "validation/instrumentation.h"
#include "workspace/workspace.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace draft {

struct ValidationCommandOptions {
  std::filesystem::path package_directory;
  TargetProfile target;
  WorkspaceLoadOptions workspace;
  ValidationKind kind = ValidationKind::None;
  // Requests are checked before semantic compilation or native tool probing.
  // When a target first supports one, its complete selection identity must
  // also enter ValidationEvidence before this boundary may accept it.
  std::vector<ValidationInstrumentationKind> instrumentation;
  std::vector<ForeignProviderInput> foreign_providers;
  // The native harness does not read these files directly, but a manifest-
  // bearing run must verify every external runtime identity before execution.
  std::vector<RuntimeAssetInput> runtime_assets;
  std::vector<ForeignProviderAudit> foreign_provider_audits;
  // Command-owned timing recorder shared across compilation, native harness
  // construction, execution, and evidence commit. It is observation only.
  TimingRecorder *timings = nullptr;
};

struct ValidationCommandResult {
  bool completed = false;
  bool passed = false;
  std::size_t selected_procedures = 0;
  Sha256Digest evidence_key;
  Sha256Digest evidence_digest;
  std::uint64_t attempt = 0;
  int exit_code = 0;
  int signal = 0;
};

// Compiles one command-selected validation graph, emits its native harness,
// executes the fixed test or benchmark policy, and commits an immutable attempt
// plus active/revoked state. A launch failure is not an execution attempt and
// therefore does not modify evidence.
[[nodiscard]] ValidationCommandResult execute_validation_command(
    SourceManager &sources,
    ValidationCommandOptions options,
    DiagnosticSink &diagnostics);

} // namespace draft
