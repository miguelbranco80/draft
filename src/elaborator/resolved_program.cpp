// Framed SHA-256 encoding for the complete resolved-program identity.
//
// This implementation owns no semantic graph and serializes no process-local
// IDs. It consumes the deterministic graph/file ordering established by the
// workspace loader and makes every collection boundary explicit. The resulting
// digest is suitable for a manifest or evidence key but is not a cache of any
// compiler phase.

#include "elaborator/resolved_program.h"

#include "elaborator/obligation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {
namespace {

// Integer fields and collection counts use one fixed big-endian encoding so
// host byte order and native integer width never enter a persistent identity.
void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - 1 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(bytes);
}

// Length framing prevents concatenation ambiguity between adjacent strings and
// makes empty values semantically distinct from omitted fields.
void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

// Target feature vectors are already canonical, but their count is still
// framed to distinguish a final empty string from the end of the collection.
void hash_string_vector(
    Sha256 &hash,
    const std::vector<std::string> &values) {
  hash_u64(hash, static_cast<std::uint64_t>(values.size()));
  for (const std::string &value : values) hash_field(hash, value);
}

// Hashes every versioned target fact, including disabled-feature vocabulary and
// file extension behavior. A profile identity alone is not trusted to stand in
// for inconsistent profile contents supplied by an embedding caller.
void hash_target(Sha256 &hash, const TargetProfile &target) {
  hash_field(hash, target.facts.identity);
  hash_field(hash, target.facts.arch);
  hash_field(hash, target.facts.os);
  hash_field(hash, target.facts.abi);
  hash_field(hash, target.facts.byte_order);
  hash_field(hash, target.facts.object_format);
  hash_field(hash, target.facts.file_tag);
  hash_u64(hash, target.facts.pointer_bits);
  hash_u64(hash, target.facts.page_size);
  hash_string_vector(hash, target.facts.known_features);
  hash_string_vector(hash, target.facts.features);
  hash_u64(hash, static_cast<std::uint64_t>(target.facts.simd_shapes.size()));
  for (const TargetSimdShape &shape : target.facts.simd_shapes) {
    hash_field(hash, shape.element);
    hash_u64(hash, shape.lanes);
  }
  hash_field(hash, target.llvm_triple);
  hash_field(hash, target.llvm_data_layout);
  hash_field(hash, target.llvm_cpu);
  hash_field(hash, target.llvm_feature_string);
  hash_field(hash, target.minimum_os_version);
  hash_field(hash, relocation_model_name(target.relocation_model));
  hash_field(hash, code_model_name(target.code_model));
  hash_field(hash, tls_model_name(target.tls_model));
  hash_field(hash, target.parsed_assembly_architecture);
  hash_field(hash, target.parsed_assembly_dialect);
  hash_string_vector(hash, target.parsed_assembly_instructions);
  hash_string_vector(hash, target.system_link_providers);
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
  hash_u64(hash, static_cast<std::uint64_t>(target.assembly_files.size()));
  for (const AssemblyFileRule &rule : target.assembly_files) {
    hash_field(hash, rule.extension);
    hash_u64(hash, static_cast<std::uint64_t>(rule.preprocessing));
  }
}

} // namespace

Sha256Digest hash_resolved_program(
    const SourceManager &sources,
    const WorkspaceGraph &graph,
    const TargetProfile &target,
    const ResolutionManifest &manifest,
    std::string_view compiler_content_identity) {
  Sha256 hash;
  hash_field(hash, "draft.resolved-program.v3");
  hash_field(hash, compiler_content_identity);
  hash_target(hash, target);

  // Root physical directories are intentionally absent. Kind, identity, and
  // import prefix capture how the same source-visible import is interpreted.
  hash_u64(hash, static_cast<std::uint64_t>(graph.roots.size()));
  for (const PackageRoot &root : graph.roots) {
    hash_u64(hash, static_cast<std::uint64_t>(root.kind));
    hash_field(hash, root.identity);
    hash_field(hash, root.import_prefix);
  }

  hash_u64(hash, static_cast<std::uint64_t>(graph.packages.size()));
  for (const WorkspacePackage &package : graph.packages) {
    hash_field(hash, package.identity.root_identity);
    hash_field(hash, package.identity.root_relative_path);
    hash_field(hash, package.loaded.short_name);
    hash_u64(hash, static_cast<std::uint64_t>(package.loaded.files.size()));
    for (const LoadedPackageFile &file : package.loaded.files) {
      hash_u64(hash, static_cast<std::uint64_t>(file.kind));
      hash_field(hash, file.relative_name);
      hash_field(hash, sources.text(file.source));
    }
  }

  // Import rows make the selected build graph explicit even though their source
  // spellings are also present in file bytes. PackageId indices are translated
  // back to persistent package identities before hashing.
  hash_u64(hash, static_cast<std::uint64_t>(graph.imports.size()));
  for (const PackageImport &import : graph.imports) {
    const WorkspacePackage &importing = graph.package(import.importing_package);
    const WorkspacePackage &imported = graph.package(import.imported_package);
    hash_field(hash, importing.identity.root_identity);
    hash_field(hash, importing.identity.root_relative_path);
    hash_field(hash, imported.identity.root_identity);
    hash_field(hash, imported.identity.root_relative_path);
    hash_field(hash, import.path);
  }

  // Physical roots are supplied again by the build invocation and verified
  // against these content identities. Only semantic role, logical name, exact
  // tree digest, and internal entry path belong to portable program identity.
  std::vector<ExternalInputPin> external_inputs = manifest.external_inputs;
  std::sort(
      external_inputs.begin(), external_inputs.end(),
      [](const ExternalInputPin &left, const ExternalInputPin &right) {
        if (left.kind != right.kind) {
          return static_cast<std::uint32_t>(left.kind) <
              static_cast<std::uint32_t>(right.kind);
        }
        return left.name < right.name;
      });
  hash_u64(hash, static_cast<std::uint64_t>(external_inputs.size()));
  for (const ExternalInputPin &input : external_inputs) {
    hash_field(hash, external_input_kind_name(input.kind));
    hash_field(hash, input.name);
    hash.update(input.content_digest.bytes);
    hash_field(hash, input.entry_point);
  }

  std::vector<ResolutionPin> pins = manifest.pins;
  std::sort(
      pins.begin(), pins.end(),
      [](const ResolutionPin &left, const ResolutionPin &right) {
        return left.site_identity < right.site_identity;
      });
  hash_u64(hash, static_cast<std::uint64_t>(pins.size()));
  for (const ResolutionPin &pin : pins) {
    hash_field(hash, pin.site_identity);
    hash_field(hash, agent_construct_kind_name(pin.kind));
    hash.update(pin.input_digest.bytes);
    hash.update(pin.expansion_digest.bytes);
    hash_field(hash, pin.source_map.root_identity);
    hash_field(hash, pin.source_map.root_relative_path);
    hash_field(hash, pin.source_map.source_relative_path);
    hash_u64(hash, pin.source_map.surface_begin);
    hash_u64(hash, pin.source_map.surface_end);
    hash_u64(hash, pin.source_map.expansion_bytes);
    hash_field(hash, pin.provider_identity);
    hash_field(hash, pin.model_identity);
    hash_field(hash, pin.configuration_identity);
  }
  return hash.finalize();
}

} // namespace draft
