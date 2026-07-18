// Canonical obligation construction for the provider-independent elaborator.

#include "elaborator/obligation.h"

#include "base/sha256.h"
#include "sema/interface.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - 1 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(bytes);
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

[[nodiscard]] bool is_obligation_kind(AgentConstructKind kind) {
  return kind != AgentConstructKind::Documentation;
}

[[nodiscard]] std::string source_relative_path(
    const LoadedPackage &loaded, FileId file) {
  for (const LoadedPackageFile &entry : loaded.files) {
    if (entry.source == file) return entry.relative_name;
  }
  return {};
}

[[nodiscard]] const SyntaxTree *find_tree(
    const LoadedPackage &loaded, FileId file) {
  for (const LoadedPackageFile &entry : loaded.files) {
    if (entry.source == file && entry.syntax.has_value()) return &*entry.syntax;
  }
  return nullptr;
}

[[nodiscard]] std::string anchor_name(
    const SemanticPackage &package, SymbolId anchor) {
  return anchor.is_valid() ? package.symbols.symbol(anchor).name : std::string();
}

[[nodiscard]] bool already_seen(
    const std::vector<std::string> &names, std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

// Walks lexical scopes from inner to outer. The first declaration of a name is
// the visible one; later declarations in the same block are excluded by source
// position. Sorting happens only after shadowing, so it cannot change meaning.
[[nodiscard]] std::vector<AgentVisibleBinding> visible_bindings(
    const PackageIdentity &identity,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AgentRecord &record,
    DiagnosticSink &diagnostics) {
  std::vector<AgentVisibleBinding> result;
  std::vector<std::string> names;
  const SyntaxTree *tree = find_tree(loaded, record.syntax.file);
  const SourceRange site_range = tree == nullptr
      ? SourceRange::invalid()
      : tree->node(record.syntax.node).range;
  ScopeId scope = record.scope;
  while (scope.is_valid()) {
    const Scope current = package.symbols.scope(scope);
    for (SymbolId symbol_id : current.symbols) {
      const Symbol &symbol = package.symbols.symbol(symbol_id);
      if (already_seen(names, symbol.name)) continue;
      if (site_range.is_valid() && symbol.name_range.is_valid() &&
          symbol.name_range.begin.file == site_range.begin.file &&
          symbol.name_range.begin.offset > site_range.begin.offset) {
        continue;
      }
      names.push_back(symbol.name);
      if (!symbol.type.is_valid() ||
          package.types.type(symbol.type).kind == TypeKind::Invalid ||
          symbol.kind == SymbolKind::Import) {
        continue;
      }
      const InterfaceTypeGraph type = export_interface_type(
          identity, package, symbol.type, diagnostics);
      result.push_back({
          symbol.name,
          symbol.kind,
          hash_interface_type_graph(type),
      });
    }
    scope = current.parent;
  }
  std::sort(
      result.begin(), result.end(),
      [](const AgentVisibleBinding &left, const AgentVisibleBinding &right) {
        if (left.name != right.name) return left.name < right.name;
        return static_cast<std::uint32_t>(left.kind) <
            static_cast<std::uint32_t>(right.kind);
      });
  return result;
}

[[nodiscard]] std::uint64_t occurrence_for(
    const AgentMetadataResult &metadata,
    std::size_t current_index,
    const AgentRecord &current) {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < current_index; ++index) {
    const AgentRecord &candidate = metadata.records[index];
    if (candidate.kind == current.kind &&
        candidate.syntax.file == current.syntax.file &&
        candidate.anchor == current.anchor) {
      ++result;
    }
  }
  return result;
}

[[nodiscard]] Sha256Digest site_identity_digest(
    const AgentObligation &obligation) {
  Sha256 hash;
  hash_field(hash, "draft-agent-site-v1");
  hash_field(hash, obligation.root_identity);
  hash_field(hash, obligation.root_relative_path);
  hash_field(hash, obligation.source_relative_path);
  hash_field(hash, obligation.anchor_name);
  hash_u64(hash, static_cast<std::uint64_t>(obligation.kind));
  hash_u64(hash, obligation.occurrence);
  return hash.finalize();
}

[[nodiscard]] Sha256Digest input_digest(
    const AgentObligation &obligation,
    const TargetProfile &target) {
  Sha256 hash;
  hash_field(hash, "draft-agent-obligation-v1");
  hash_field(hash, obligation.site_identity);
  hash.update(obligation.record_digest.bytes);
  hash.update(obligation.expected_type_digest.bytes);
  hash_field(hash, target.facts.identity);
  hash_field(hash, target.llvm_triple);
  hash_field(hash, target.llvm_data_layout);
  hash_field(hash, target.llvm_cpu);
  hash_field(hash, target.llvm_feature_string);
  hash_field(hash, target.parsed_assembly_architecture);
  hash_field(hash, target.parsed_assembly_dialect);
  hash_u64(
      hash,
      static_cast<std::uint64_t>(target.parsed_assembly_instructions.size()));
  for (const std::string &instruction : target.parsed_assembly_instructions) {
    hash_field(hash, instruction);
  }
  hash_u64(
      hash,
      static_cast<std::uint64_t>(target.system_link_providers.size()));
  for (const std::string &provider : target.system_link_providers) {
    hash_field(hash, provider);
  }
  hash_field(hash, target.system_link_library);
  hash_u64(
      hash,
      static_cast<std::uint64_t>(target.system_foreign_summaries.size()));
  for (const SystemForeignSummary &summary :
       target.system_foreign_summaries) {
    hash_field(hash, summary.provider);
    hash_field(hash, summary.linker_name);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(summary.callback_parameters.size()));
    for (std::uint32_t parameter : summary.callback_parameters) {
      hash_u64(hash, parameter);
    }
  }
  hash_u64(hash, static_cast<std::uint64_t>(obligation.visible_bindings.size()));
  for (const AgentVisibleBinding &binding : obligation.visible_bindings) {
    hash_field(hash, binding.name);
    hash_u64(hash, static_cast<std::uint64_t>(binding.kind));
    hash.update(binding.type_digest.bytes);
  }
  return hash.finalize();
}

} // namespace

std::string_view agent_construct_kind_name(AgentConstructKind kind) {
  switch (kind) {
  case AgentConstructKind::Documentation: return "documentation";
  case AgentConstructKind::Judgment: return "judgment";
  case AgentConstructKind::SynthesisDeclaration: return "declaration";
  case AgentConstructKind::SynthesisMember: return "member";
  case AgentConstructKind::SynthesisStatement: return "statement";
  case AgentConstructKind::SynthesisExpression: return "expression";
  case AgentConstructKind::SynthesisAssembly: return "assembly";
  }
  return "invalid";
}

AgentObligationResult build_agent_obligations(
    const PackageIdentity &identity,
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    const AgentMetadataResult &metadata,
    const TargetProfile &target,
    DiagnosticSink &diagnostics) {
  (void)sources;
  AgentObligationResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  for (std::size_t index = 0; index < metadata.records.size(); ++index) {
    const AgentRecord &record = metadata.records[index];
    if (!is_obligation_kind(record.kind)) continue;
    AgentObligation obligation;
    obligation.kind = record.kind;
    obligation.syntax = record.syntax;
    obligation.root_identity = identity.root_identity;
    obligation.root_relative_path = identity.root_relative_path;
    obligation.source_relative_path =
        source_relative_path(loaded, record.syntax.file);
    obligation.anchor_name = anchor_name(package, record.anchor);
    obligation.occurrence = occurrence_for(metadata, index, record);
    obligation.record_digest = record.record_digest;
    if (obligation.source_relative_path.empty()) {
      diagnostics.error(
          SourceRange::invalid(), "agent obligation has no package-relative source");
      continue;
    }
    if (record.expected_type.is_valid()) {
      const InterfaceTypeGraph expected = export_interface_type(
          identity, package, record.expected_type, diagnostics);
      obligation.expected_type_digest = hash_interface_type_graph(expected);
    }
    obligation.visible_bindings = visible_bindings(
        identity, loaded, package, record, diagnostics);
    obligation.site_identity = "site-" + site_identity_digest(obligation).hex();
    obligation.input_digest = input_digest(obligation, target);
    result.obligations.push_back(std::move(obligation));
  }
  result.ok = diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
