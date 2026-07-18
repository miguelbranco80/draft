// Semantic denial contracts supplied by exact foreign provider artifacts.

#pragma once

#include "base/sha256.h"
#include "sema/analyzer.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// Provider audit rows intentionally mirror only the stable, source-independent
// part of SemanticEffect. Process-local SymbolIds never cross this boundary.
struct ForeignAuditEffect {
  EffectKind kind = EffectKind::UnknownCall;
  std::string root_identity;
  std::string root_relative_path;
  std::string declaration;
  std::string detail;
  std::uint32_t flow_parameter =
      std::numeric_limits<std::uint32_t>::max();
};

struct ForeignAuditSymbol {
  std::string linker_name;
  std::vector<ForeignAuditEffect> effects;
};

// Both content digests are retained after filesystem verification. The
// artifact digest binds semantics to code; the summary digest lets the native
// adapter prove that every manifest ProviderSummary row was actually consumed
// by semantic compilation.
struct ForeignProviderAudit {
  std::string provider;
  Sha256Digest artifact_content_digest;
  Sha256Digest summary_content_digest;
  std::vector<ForeignAuditSymbol> symbols;

  [[nodiscard]] const ForeignAuditSymbol *find_symbol(
      std::string_view linker_name) const;
};

} // namespace draft
