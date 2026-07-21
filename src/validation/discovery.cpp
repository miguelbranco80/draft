// Checked validation procedure discovery and canonical ordering.

#include "validation/discovery.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace draft {
namespace {

[[nodiscard]] bool ends_with(
    std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
      value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] ValidationKind file_validation_kind(
    std::string_view relative_name) {
  const std::size_t dot = relative_name.rfind('.');
  if (dot == std::string_view::npos ||
      relative_name.substr(dot) != ".draft") {
    return ValidationKind::None;
  }
  std::string_view stem = relative_name.substr(0, dot);
  const std::size_t qualifier = stem.rfind('@');
  if (qualifier != std::string_view::npos) {
    stem = stem.substr(0, qualifier);
  }
  if (ends_with(stem, "_test")) return ValidationKind::Test;
  if (ends_with(stem, "_bench")) return ValidationKind::Benchmark;
  return ValidationKind::None;
}

[[nodiscard]] bool declaration_is_in_kind(
    const LoadedPackage &loaded, FileId file, ValidationKind kind) {
  for (const LoadedPackageFile &candidate : loaded.files) {
    if (candidate.source == file) {
      return file_validation_kind(candidate.relative_name) == kind;
    }
  }
  return false;
}

[[nodiscard]] bool has_body(const HirProgram &hir, SymbolId procedure) {
  for (const HirProcedure &candidate : hir.procedures()) {
    if (candidate.symbol == procedure && candidate.valid &&
        !candidate.parametric_template && !candidate.compile_time_only) {
      return true;
    }
  }
  return false;
}

struct ExpectedState {
  std::string_view package_path;
  std::string_view public_name;
  std::string_view prefix;
  std::size_t failure_member = 0;
};

[[nodiscard]] ExpectedState expected_state(ValidationKind kind) {
  if (kind == ValidationKind::Test) {
    return {"testing", "Test", "test_", 1};
  }
  return {"benchmark", "Benchmark", "bench_", 3};
}

[[nodiscard]] bool is_expected_nominal(
    std::string_view core_root_identity,
    const PackageIdentity &identity,
    const SemanticPackage &semantic,
    TypeId type,
    const ExpectedState &expected) {
  for (const ImportedType &imported : semantic.imported_types_for_read()) {
    if (imported.type == type &&
        imported.root_identity == core_root_identity &&
        imported.root_relative_path == expected.package_path &&
        imported.public_name == expected.public_name) {
      return true;
    }
  }
  if (identity.root_identity != core_root_identity ||
      identity.root_relative_path != expected.package_path) {
    return false;
  }
  const std::optional<SymbolId> local = semantic.symbols.lookup_direct(
      semantic.package_scope, expected.public_name);
  return local.has_value() && semantic.symbols.symbol(*local).type == type;
}

[[nodiscard]] std::optional<ValidationEntry> validate_candidate(
    ValidationKind kind,
    std::string_view core_root_identity,
    const PackageIdentity &identity,
    const SemanticPackage &semantic,
    const HirProgram &hir,
    SymbolId symbol_id,
    DiagnosticSink &diagnostics) {
  const Symbol &symbol = semantic.symbols.symbol(symbol_id);
  const ExpectedState expected = expected_state(kind);
  auto reject = [&](std::string detail) -> std::optional<ValidationEntry> {
    diagnostics.error(
        symbol.name_range,
        std::string(validation_kind_name(kind)) + " procedure '" +
            symbol.name + "' " + std::move(detail));
    return std::nullopt;
  };

  if (symbol.flags.foreign || symbol.flags.parametric || !has_body(hir, symbol_id)) {
    return reject("must be a defined non-parametric Draft procedure");
  }
  if (!symbol.type.is_valid()) return reject("has no checked procedure type");
  const Type &signature = semantic.types.type(symbol.type);
  if (signature.kind != TypeKind::Procedure ||
      signature.c_calling_convention || signature.members.size() != 2) {
    return reject("must accept exactly one ordinary Draft parameter");
  }
  if (signature.members.back() != semantic.types.builtins().void_type) {
    return reject("must return void");
  }
  const Type &parameter = semantic.types.type(signature.members.front());
  if (parameter.kind != TypeKind::Pointer ||
      !is_expected_nominal(
          core_root_identity,
          identity,
          semantic,
          parameter.element,
          expected)) {
    return reject(
        "must accept ^" + std::string(expected.package_path) + "." +
        std::string(expected.public_name));
  }
  const Type &state = semantic.types.type(parameter.element);
  if (!state.layout.known ||
      expected.failure_member >= state.member_offsets.size()) {
    return reject("uses a validation state type without complete target layout");
  }
  const std::uint64_t failure_offset =
      state.member_offsets[expected.failure_member];
  if (state.layout.size < 8 || failure_offset > state.layout.size - 8 ||
      state.layout.alignment == 0) {
    return reject("uses an invalid validation state layout");
  }

  ValidationEntry result;
  result.kind = kind;
  result.package = identity;
  result.procedure = symbol.name;
  result.state_size = state.layout.size;
  result.state_alignment = state.layout.alignment;
  result.failure_offset = failure_offset;
  result.report_size = failure_offset + 8;
  return result;
}

} // namespace

std::string_view validation_kind_name(ValidationKind kind) {
  switch (kind) {
  case ValidationKind::None: return "validation";
  case ValidationKind::Test: return "test";
  case ValidationKind::Benchmark: return "benchmark";
  }
  return "validation";
}

std::vector<ValidationEntry> discover_validation_entries(
    ValidationKind kind,
    std::string_view core_root_identity,
    const PackageIdentity &identity,
    const LoadedPackage &loaded,
    const SemanticPackage &semantic,
    const HirProgram &hir,
    DiagnosticSink &diagnostics) {
  std::vector<ValidationEntry> result;
  if (kind == ValidationKind::None) return result;
  const ExpectedState expected = expected_state(kind);
  const Scope &scope = semantic.symbols.scope(semantic.package_scope);
  for (SymbolId symbol_id : scope.symbols) {
    const Symbol &symbol = semantic.symbols.symbol(symbol_id);
    if (symbol.kind != SymbolKind::Procedure ||
        !symbol.name.starts_with(expected.prefix) ||
        !declaration_is_in_kind(loaded, symbol.syntax.file, kind)) {
      continue;
    }
    std::optional<ValidationEntry> entry = validate_candidate(
        kind,
        core_root_identity,
        identity,
        semantic,
        hir,
        symbol_id,
        diagnostics);
    if (entry.has_value()) result.push_back(std::move(*entry));
  }
  return result;
}

void sort_validation_entries(std::vector<ValidationEntry> &entries) {
  std::stable_sort(
      entries.begin(), entries.end(),
      [](const ValidationEntry &left, const ValidationEntry &right) {
        if (left.package.root_identity != right.package.root_identity) {
          return left.package.root_identity < right.package.root_identity;
        }
        if (left.package.root_relative_path !=
            right.package.root_relative_path) {
          return left.package.root_relative_path <
              right.package.root_relative_path;
        }
        return left.procedure < right.procedure;
      });
}

} // namespace draft
