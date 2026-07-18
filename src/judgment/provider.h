// Provider-neutral boundary for qualitative judgment validation.
//
// The compiler owns site identity, typed context, artifact identity, policy,
// aggregation, evidence, and persistence. A provider receives one immutable
// request and returns only a verdict plus rationale. It cannot alter source,
// declare evidence active, choose a different claim, or write compiler state.

#pragma once

#include "base/sha256.h"
#include "elaborator/obligation.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <string>
#include <vector>

namespace draft {

struct JudgmentRequestFile {
  std::string relative_path;
  std::uint64_t size = 0;
  Sha256Digest digest;
  std::string contents;
};

// Artifacts are exact compiler-owned bytes, never paths that would let a judge
// inspect ambient state. The first command profile requests none; the boundary
// already supports IR, object, disassembly, or other explicitly named products.
struct JudgmentRequestArtifact {
  std::string kind;
  Sha256Digest digest;
  std::string contents;
};

struct JudgmentRequest {
  std::string format = "draft-judgment-request-v3";
  AgentObligation obligation;
  Sha256Digest resolved_program;
  std::string compiler_identity;
  std::string policy_identity;
  std::string validator_identity;
  std::string claim;
  std::vector<JudgmentRequestFile> attachments;
  std::vector<JudgmentRequestArtifact> artifacts;
};

struct JudgmentResponse {
  bool passed = false;
  std::string rationale;
};

using JudgeFunction = bool (*)(
    void *state,
    const JudgmentRequest &request,
    JudgmentResponse &response,
    DiagnosticSink &diagnostics);

struct JudgmentProvider {
  std::string provider_identity;
  std::string model_identity;
  std::string configuration_identity;
  void *state = nullptr;
  JudgeFunction judge = nullptr;
};

} // namespace draft
