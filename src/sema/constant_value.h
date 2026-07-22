// Phase-independent representation of a ready compile-time value.
//
// Constant values are produced by constant evaluation, embedded in canonical
// package interfaces, and copied into importing packages. Keeping the value
// record separate from the evaluator prevents declaration and interface data
// from depending on the evaluator's traversal machinery.
//
// Integer and decimal-float payloads already use Draft's mathematical domains:
// arbitrary precision and exact rational arithmetic. Values own their text so
// package interfaces and imported bindings do not retain SourceManager views.

#pragma once

#include "sema/big_integer.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace draft {

enum class ConstantKind {
  Unavailable,
  Nil,
  Bool,
  Integer,
  Float,
  String,
  Aggregate,
  EnumLabel,
  Procedure,
  // Type carries one exact package-local TypeId index. Package interfaces
  // translate the index through their canonical type graph before it crosses
  // a package boundary; no process-local number is persistent identity.
  Type,
  Target,
};

// Only the field selected by kind is meaningful. Unavailable is a sentinel and
// never represents a successful evaluation. Target is an evaluator-internal
// predeclared object and must not be published as a package constant.
struct ConstantValue {
  ConstantKind kind = ConstantKind::Unavailable;
  bool boolean = false;
  BigInteger integer;
  ExactRational floating;
  // Untyped decimal constants keep float_bit_width zero and use `floating` as
  // their exact mathematical value. Concrete IEEE constants additionally keep
  // their target-format bits so signed zero, infinity, and NaN survive semantic
  // evaluation and no operation accidentally regains excess precision.
  std::uint32_t float_bit_width = 0;
  std::uint64_t float_bits = 0;
  std::string text;
  // A procedure identity is local while a package is being checked and
  // canonical once it crosses a package interface. symbol_index names the
  // current SemanticPackage entry; the two package fields plus `text` name the
  // source declaration independently of process-local table indices. Interface
  // construction clears symbol_index after filling the canonical fields.
  std::uint32_t symbol_index = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t type_index = std::numeric_limits<std::uint32_t>::max();
  std::string root_identity;
  std::string root_relative_path;
  // Aggregate elements use logical source order. Arrays, tuples, and structs
  // contain every element including recursively constructed zero defaults.
  // Variants and unions use member_index to identify the selected source-order
  // member and contain its optional value as the sole element. A zeroed union
  // may have no element because its bytes do not select an active field.
  std::vector<ConstantValue> elements;
  std::uint64_t member_index = std::numeric_limits<std::uint64_t>::max();

  bool operator==(const ConstantValue &) const = default;

  [[nodiscard]] static ConstantValue make_bool(bool value);
  [[nodiscard]] static ConstantValue make_nil();
  [[nodiscard]] static ConstantValue make_integer(std::int64_t value);
  [[nodiscard]] static ConstantValue make_integer(BigInteger value);
  [[nodiscard]] static ConstantValue make_float(ExactRational value);
  [[nodiscard]] static ConstantValue make_ieee_float(
      std::uint32_t bit_width,
      std::uint64_t bits,
      ExactRational finite_value = {});
  [[nodiscard]] static ConstantValue make_string(std::string value);
  [[nodiscard]] static ConstantValue make_aggregate(
      std::vector<ConstantValue> elements,
      std::uint64_t member_index = std::numeric_limits<std::uint64_t>::max());
  [[nodiscard]] static ConstantValue make_enum_label(
      std::string value, std::vector<ConstantValue> payload = {});
  [[nodiscard]] static ConstantValue make_procedure(
      std::uint32_t symbol_index,
      std::string name,
      std::string root_identity = {},
      std::string root_relative_path = {});
  [[nodiscard]] static ConstantValue make_type(std::uint32_t type_index);
  [[nodiscard]] static ConstantValue make_target();
};

} // namespace draft
