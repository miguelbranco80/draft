// Complete native test/benchmark execution and locked evidence verification.

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
  bool allow_unpinned_toolchain = false;
  bool locked = false;
  LockedNativeInputRoots locked_inputs;
  std::vector<ForeignProviderInput> foreign_providers;
  // The native harness does not read these files directly, but a manifest-
  // bearing run must verify every external runtime identity before execution.
  std::vector<RuntimeAssetInput> runtime_assets;
  std::vector<ForeignProviderAudit> foreign_provider_audits;
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

// Executes an already compiled candidate and records its immutable attempt.
// Resolution uses this before its manifest commit so failures revoke the exact
// key even when pins stay unchanged. A passing attempt becomes selected only
// if the later manifest commit records its returned key and content digest.
[[nodiscard]] ValidationCommandResult execute_precommit_validation(
    const CompileWorkspaceResult &compiled,
    ValidationCommandOptions options,
    DiagnosticSink &diagnostics);

struct ValidationEvidenceRequirement {
  std::filesystem::path package_directory;
  TargetProfile target;
  WorkspaceLoadOptions workspace;
  ValidationKind kind = ValidationKind::None;
  // Locked verification uses the same request vocabulary and availability
  // gate. It never substitutes uninstrumented evidence for a requested kind.
  std::vector<ValidationInstrumentationKind> instrumentation;
  std::vector<ForeignProviderAudit> foreign_provider_audits;
};

// Recompiles only the selected typed definitions and checks exact active
// evidence. It performs no native lowering, provider call, validation process,
// or store mutation, making it safe for locked build verification.
[[nodiscard]] bool verify_active_validation_evidence(
    SourceManager &sources,
    ValidationEvidenceRequirement requirement,
    Sha256Digest &active_digest,
    DiagnosticSink &diagnostics);

} // namespace draft
