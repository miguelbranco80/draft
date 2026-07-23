// Semantic denial contracts supplied by exact foreign provider artifacts.

#pragma once

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
  std::vector<std::string> flow_path;
  bool flow_context = false;
};

struct ForeignAuditSymbol {
  std::string linker_name;
  std::vector<ForeignAuditEffect> effects;
};

// One audit is semantic metadata supplied for one provider. It deliberately
// carries no native-artifact identity: the linker mapping is operational input,
// while this parsed contract is checked directly whenever semantic analysis
// needs it. Draft does not claim that a digest of one library controls its
// transitive runtime environment.
struct ForeignProviderAudit {
  std::string provider;
  std::vector<ForeignAuditSymbol> symbols;

  [[nodiscard]] const ForeignAuditSymbol *find_symbol(
      std::string_view linker_name) const;
};

} // namespace draft
