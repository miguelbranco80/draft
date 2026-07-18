// Versioned provider-neutral boundary for synthesis proposal generation.
//
// The compiler owns obligations, checking, retries, program identity, and
// commits. A provider receives immutable plain Draft context and returns only a
// proposed source fragment. It cannot manufacture syntax/HIR IDs, write pins,
// declare a proposal valid, or observe compiler-owned transaction paths.
//
// This bootstrap boundary uses a direct function table instead of inheritance,
// SDK types, or callbacks hidden in a framework. A Codex CLI adapter and
// deterministic test providers can implement the same one synchronous call.
// Provider, model, and configuration identities are fixed on the adapter and
// copied into every new pin. Relevant specification: 03-agent-synthesis.md
// sections 9-10 and 06-compiler.md section 15.

#pragma once

#include "base/sha256.h"
#include "elaborator/obligation.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <string>
#include <vector>

namespace draft {

// One exact attached file supplied to synthesis. contents owns binary bytes and
// may contain zero. digest and size duplicate independently checked identity
// facts so an adapter can frame a request without filesystem access.
struct SynthesisRequestFile {
  std::string relative_path;
  std::uint64_t size = 0;
  Sha256Digest digest;
  std::string contents;
};

// One earlier proposal that the ordinary Draft compiler rejected during the
// current resolver transaction. attempt is one-based and rows are ordered by
// attempt. source owns the exact provider bytes; diagnostics owns their
// generated-source-aware compiler rendering. These rows are correction context
// only: they do not alter the stable obligation or its input digest, and no row
// survives after one expansion has been accepted and pinned.
struct SynthesisRejection {
  std::uint32_t attempt = 0;
  std::string source;
  std::string diagnostics;
};

// One complete provider request for the current bootstrap context format.
// obligation includes grammar category, usable canonical type spellings,
// visible binding names/types/canonical constant values, a bounded transitive
// relevant-declaration closure, explicit target and
// assembly facts, enclosing documentation, canonical enclosing declaration
// source and typed semantic skeleton, active denial selectors,
// typed outer-to-inner branch refinements, conservative current loop ranges,
// parametric constraints, complete
// referenced type graphs, positionally
// available imported package interfaces and their explicit public documentation
// bytes, judgment claims, canonical test and benchmark source plus checked
// validation signatures/layouts/references, persistent
// source coordinates, the exact input digest, and stable site identity. prompt
// and attachments contain source-authored bounded context without exposing
// semantic arena IDs. prior_rejections is initially empty; on a bounded retry
// it contains the exact compiler feedback needed to correct earlier proposals.
struct SynthesisRequest {
  std::string format = "draft-synthesis-request-v21";
  AgentObligation obligation;
  std::string prompt;
  std::vector<SynthesisRequestFile> attachments;
  std::vector<SynthesisRejection> prior_rejections;
};

// A provider returns exact fragment bytes only. Empty bytes are not rejected at
// this boundary because grammar-category parsing is the semantic authority.
struct SynthesisResponse {
  std::string source;
};

using SynthesizeFunction = bool (*)(
    void *state,
    const SynthesisRequest &request,
    SynthesisResponse &response,
    DiagnosticSink &diagnostics);

// state is borrowed and must outlive a resolver call. A null synthesize function
// means no provider is configured; it is valid when every selected pin is fresh
// and therefore no invocation is required. Identity strings are validated only
// before an actual call.
struct SynthesisProvider {
  std::string provider_identity;
  std::string model_identity;
  std::string configuration_identity;
  void *state = nullptr;
  SynthesizeFunction synthesize = nullptr;
};

} // namespace draft
