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
  std::string text;
  // Aggregate elements use logical source order. Arrays, tuples, and structs
  // contain every element including recursively constructed zero defaults.
  // Tagged/raw unions use variant_index to identify the selected member and
  // contain its optional payload as the sole element.
  std::vector<ConstantValue> elements;
  std::uint64_t variant_index = std::numeric_limits<std::uint64_t>::max();

  bool operator==(const ConstantValue &) const = default;

  [[nodiscard]] static ConstantValue make_bool(bool value);
  [[nodiscard]] static ConstantValue make_nil();
  [[nodiscard]] static ConstantValue make_integer(std::int64_t value);
  [[nodiscard]] static ConstantValue make_integer(BigInteger value);
  [[nodiscard]] static ConstantValue make_float(ExactRational value);
  [[nodiscard]] static ConstantValue make_string(std::string value);
  [[nodiscard]] static ConstantValue make_aggregate(
      std::vector<ConstantValue> elements,
      std::uint64_t variant_index = std::numeric_limits<std::uint64_t>::max());
  [[nodiscard]] static ConstantValue make_enum_label(
      std::string value, std::vector<ConstantValue> payload = {});
  [[nodiscard]] static ConstantValue make_target();
};

} // namespace draft
