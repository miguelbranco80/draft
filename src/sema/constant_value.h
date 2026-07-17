// Phase-independent representation of a ready compile-time scalar value.
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
#include <string>

namespace draft {

enum class ConstantKind {
  Unavailable,
  Bool,
  Integer,
  Float,
  String,
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

  [[nodiscard]] static ConstantValue make_bool(bool value);
  [[nodiscard]] static ConstantValue make_integer(std::int64_t value);
  [[nodiscard]] static ConstantValue make_integer(BigInteger value);
  [[nodiscard]] static ConstantValue make_float(ExactRational value);
  [[nodiscard]] static ConstantValue make_string(std::string value);
  [[nodiscard]] static ConstantValue make_enum_label(std::string value);
  [[nodiscard]] static ConstantValue make_target();
};

} // namespace draft
