// Small deterministic lookups for parsed foreign provider denial summaries.

#include "sema/foreign_summary.h"

namespace draft {

const ForeignAuditSymbol *ForeignProviderAudit::find_symbol(
    std::string_view linker_name) const {
  for (const ForeignAuditSymbol &symbol : symbols) {
    if (symbol.linker_name == linker_name) return &symbol;
  }
  return nullptr;
}

} // namespace draft
