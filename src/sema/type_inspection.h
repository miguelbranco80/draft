// Compile-time structural inspection of canonical Draft types.
//
// This module is the single semantic authority for reflection queries shared
// by body checking and the constant interpreter. Its input is one canonical
// TypeId plus an optional compile-time member index. Its output is an ordinary
// compile-time value: another exact type, an integer, a boolean, a string, or a
// member of one of Draft's compiler-defined reflection enums.
//
// The module owns no data. It reads SemanticPackage's type, symbol, aggregate-
// member, and enum-value tables in source order. It never consults LLVM or ABI
// lowering, and it never emits runtime descriptors. Query applicability and
// member bounds are diagnosed by returning an explicit error string to the
// caller, which retains the exact source range.
//
// Relevant specification: docs/specification/01-core-language.md "Constants
// and compile-time evaluation" and docs/specification/02-types-memory-runtime.md
// "Built-ins and type inspection".

#pragma once

#include "sema/analyzer.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace draft {

// TypeInspectionResult is the typed value returned by one successful query.
// Type-valued results use ConstantKind::Type and store a package-local TypeId
// index; interface translation must rewrite that index before publication.
struct TypeInspectionResult {
  ConstantValue value;
  TypeId type;
};

// TypeInspectionAttempt separates an unrecognized name from a recognized query
// that was used on the wrong type or with an invalid member index. Intrinsic
// recognition needs that distinction so an ordinary procedure with another
// name still follows normal lookup.
struct TypeInspectionAttempt {
  bool recognized = false;
  std::optional<TypeInspectionResult> result;
  std::string error;
};

[[nodiscard]] bool is_type_inspection_query(std::string_view name);

[[nodiscard]] TypeInspectionAttempt inspect_type(
    const SemanticPackage &package,
    std::string_view query,
    TypeId queried,
    std::optional<std::uint64_t> index = std::nullopt);

// Compiler-defined reflection enums have no source declaration. These helpers
// supply their stable member vocabulary to contextual-alternative checking and
// constant folding without manufacturing package symbols or source ranges.
[[nodiscard]] std::optional<std::uint64_t> compiler_enum_member_value(
    const SemanticPackage &package,
    TypeId enum_type,
    std::string_view name);

[[nodiscard]] std::optional<std::string_view> compiler_enum_member_name(
    const SemanticPackage &package,
    TypeId enum_type,
    std::uint64_t value);

// Dependent `when` refinement consumes the same source vocabulary as
// type_kind. Keeping both conversions here prevents the refinement checker
// from acquiring a second, subtly different list of visible type categories.
[[nodiscard]] std::optional<TypeKind> inspected_type_kind(
    std::string_view name);

[[nodiscard]] std::span<const TypeKind> inspectable_type_kinds();

} // namespace draft
