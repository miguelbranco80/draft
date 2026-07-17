// Phase-independent representation of a ready compile-time scalar value.
//
// Constant values are produced by constant evaluation, embedded in canonical
// package interfaces, and copied into importing packages. Keeping the value
// record separate from the evaluator prevents declaration and interface data
// from depending on the evaluator's traversal machinery.
//
// The current integer payload is the explicitly documented bootstrap boundary:
// it will become arbitrary precision before the semantic core is complete.
// Values own their text so package interfaces and imported bindings do not
// retain views into a SourceManager.

#pragma once

#include <cstdint>
#include <string>

namespace draft {

enum class ConstantKind {
  Unavailable,
  Bool,
  Integer,
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
  std::int64_t integer = 0;
  std::string text;

  [[nodiscard]] static ConstantValue make_bool(bool value);
  [[nodiscard]] static ConstantValue make_integer(std::int64_t value);
  [[nodiscard]] static ConstantValue make_string(std::string value);
  [[nodiscard]] static ConstantValue make_enum_label(std::string value);
  [[nodiscard]] static ConstantValue make_target();
};

} // namespace draft
