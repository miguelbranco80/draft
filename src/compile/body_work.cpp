// Canonical work identities for demand-driven procedure body specialization.
//
// See body_work.h for the ownership and phase contract. This implementation is
// deliberately a set of direct vector operations: demand sets are normally
// small, already content-addressed, and traversed once per affected package.
// Sorting gives deterministic identity without a hash-table iteration contract;
// the merge walk distinguishes append-only work from removal in linear time.

#include "compile/body_work.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace draft {
namespace {

// Canonical demand order is independent of which consumer first requested an
// instance. The native spelling is the primary human-readable key; the full
// digest is the collision-resistant semantic key.
[[nodiscard]] bool
procedure_demand_less(const ProcedureInstantiationDemand &left,
                      const ProcedureInstantiationDemand &right) {
  if (left.instance_name != right.instance_name) {
    return left.instance_name < right.instance_name;
  }
  return left.digest.bytes < right.digest.bytes;
}

} // namespace

bool canonicalize_procedure_demands(
    std::vector<ProcedureInstantiationDemand> &demands,
    DiagnosticSink &diagnostics) {
  std::sort(demands.begin(), demands.end(), procedure_demand_less);
  std::vector<ProcedureInstantiationDemand> canonical;
  canonical.reserve(demands.size());
  for (ProcedureInstantiationDemand &demand : demands) {
    if (!canonical.empty() &&
        canonical.back().instance_name == demand.instance_name) {
      if (canonical.back().digest != demand.digest) {
        diagnostics.error(
            SourceRange::invalid(),
            "generic procedure instances have a native-name hash collision");
        return false;
      }
      continue;
    }
    canonical.push_back(std::move(demand));
  }
  demands = std::move(canonical);
  return true;
}

bool same_procedure_demands(
    const std::vector<ProcedureInstantiationDemand> &left,
    const std::vector<ProcedureInstantiationDemand> &right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].instance_name != right[index].instance_name ||
        left[index].digest != right[index].digest) {
      return false;
    }
  }
  return true;
}

bool added_procedure_demands(
    const std::vector<ProcedureInstantiationDemand> &previous,
    const std::vector<ProcedureInstantiationDemand> &current,
    std::vector<ProcedureInstantiationDemand> &added) {
  std::size_t previous_index = 0;
  std::size_t current_index = 0;
  while (previous_index < previous.size() && current_index < current.size()) {
    const ProcedureInstantiationDemand &old = previous[previous_index];
    const ProcedureInstantiationDemand &now = current[current_index];
    if (old.instance_name == now.instance_name && old.digest == now.digest) {
      ++previous_index;
      ++current_index;
      continue;
    }
    if (procedure_demand_less(now, old)) {
      added.push_back(now);
      ++current_index;
      continue;
    }
    return false;
  }
  if (previous_index != previous.size())
    return false;
  while (current_index < current.size()) {
    added.push_back(current[current_index]);
    ++current_index;
  }
  return true;
}

std::vector<ProcedureInstantiationSeed> materialize_procedure_demands(
    const std::vector<ProcedureInstantiationDemand> &demands,
    SemanticPackage &owner, DiagnosticSink &diagnostics) {
  std::vector<ProcedureInstantiationSeed> seeds;
  seeds.reserve(demands.size());
  for (const ProcedureInstantiationDemand &demand : demands) {
    ProcedureInstantiationSeed seed;
    seed.public_template_name = demand.public_template_name;
    seed.instance_name = demand.instance_name;
    seed.arguments.reserve(demand.arguments.size());
    for (const ProcedureInstantiationDemandArgument &argument :
         demand.arguments) {
      ParametricArgument materialized;
      materialized.is_type = argument.is_type;
      const TypeId imported =
          import_interface_type(argument.type, owner, diagnostics);
      if (argument.is_type) {
        materialized.type = imported;
      } else {
        materialized.value_type = imported;
        materialized.value = argument.value;
      }
      seed.arguments.push_back(std::move(materialized));
    }
    seed.pack_types.reserve(demand.pack_types.size());
    for (const InterfaceTypeGraph &pack_type : demand.pack_types) {
      seed.pack_types.push_back(
          import_interface_type(pack_type, owner, diagnostics));
    }
    seeds.push_back(std::move(seed));
  }
  return seeds;
}

} // namespace draft
