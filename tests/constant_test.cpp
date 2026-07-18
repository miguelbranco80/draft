// Compile-time evaluation and fixed-point declaration `when` tests.

#include "sema/constant.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "workspace/package.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "constant_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct AnalyzedSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticAnalysisResult analysis;

  explicit AnalyzedSource(std::string text) {
    loaded.short_name = "conditions";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));

    draft::TargetFacts target;
    target.identity = "draft-aarch64-macos-v5";
    target.arch = "aarch64";
    target.os = "macos";
    target.abi = "darwin";
    target.byte_order = "little";
    target.object_format = "macho";
    target.file_tag = "aarch64-macos";
    target.pointer_bits = 64;
    target.page_size = 16384;
    target.known_features = {"crc", "neon"};
    target.simd_shapes = {{"u32", 4}};
    target.features = {"neon"};
    analysis = draft::analyze_package_semantics(
        sources, loaded, target, diagnostics);
  }
};

[[nodiscard]] std::optional<draft::SymbolId> find_symbol(
    const draft::SemanticPackage &package, std::string_view name) {
  return package.symbols.lookup_direct(package.package_scope, name);
}

void test_constants_and_conditional_rounds(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Mode :: enum {
    Off,
    On,
}

Outcome :: union {
    empty,
    value: u32,
}

Compile_Header :: struct {
    tag: u8,
    value: u32,
}

make_mask :: proc(bits: uint) -> uint {
    result: uint
    for i: uint = 0; i < bits; i += 1 {
        if i % 2 == 0 {
            continue
        }
        result |= 1 << i
    }
    return result
}

factorial :: proc(value: uint) -> uint {
    if value <= 1 {
        return 1
    }
    return value * factorial(value - 1)
}

classify :: proc(value: int) -> int {
    switch value {
    case 0:
        return 10
    case 1, 2:
        return 20
    case:
        return 30
    }
}

classify_mode :: proc(value: Mode) -> u32 {
    switch value {
    case .On:
        return 1
    case .Off:
        return 0
    }
}

classify_outcome :: proc(value: Outcome) -> u32 {
    switch value {
    case .value(payload):
        return payload
    case .empty:
        return 0
    }
}

target_word :: proc() -> uint {
    when target.pointer_bits == 64 {
        static_assert(target.arch == .aarch64, "unexpected architecture")
        return 64
    } else {
        return 32
    }
}

increment_byte :: proc(value: u8) -> u8 {
    return value + 1
}

measure[T: type, Extra: usize] :: proc(value: T) -> usize {
    static_assert(size_of(T) > 0)
    return size_of(T) + Extra
}

make_table :: proc() -> [4]u32 {
    result: [4]u32
    for i: usize = 0; i < len(result); i += 1 {
        result[i] = cast[u32](i * i)
    }
    return result
}

sum_table :: proc(values: [4]u32) -> u32 {
    result: u32
    for value, index in values {
        static_assert(index < 4)
        result += value
    }
    return result
}

increment_value :: proc(value: u32) -> u32 {
    return value + 1
}

decrement_value :: proc(value: u32) -> u32 {
    return value - 1
}

apply_operation :: proc(
    operation: proc(value: u32) -> u32,
    value: u32,
) -> u32 {
    return operation(value)
}

statement_forms :: proc() -> u32 {
    grouped_left, grouped_right: u32 = 5
    (first, second): (u32, u32) = (10, 32)
    first, second = second, first
    scratch: u32 = ---
    scratch = first + second
    _, grouped_left = grouped_right, first
    (first, second) = (second, first)
    (_, second) = (999, 10)
    return scratch + grouped_left + second
}

initialize_members :: proc() -> Compile_Header {
    result: Compile_Header = ---
    result.tag = 1
    result.value = 42
    return result
}

round_each_step :: proc() -> f32 {
    value: f32 = 16777216.0
    return value + 1.0
}

round_contextual_tree :: proc() -> f32 {
    return (16777216.0 + 1.0) - 16777216.0
}

make_infinity :: proc() -> f32 {
    zero: f32
    return 1.0 / zero
}

make_nan :: proc() -> f32 {
    zero: f32
    return zero / zero
}

make_negative_zero :: proc() -> f32 {
    return -1.0 / make_infinity()
}

Bits :: target.pointer_bits
Base :: 40
Derived :: Base + 2
Has_Neon :: target.has_feature("neon")
Bit_Value :: (2 << 5) | 1
Huge :: 340282366920938463463374607431768211456 + 7
Fraction :: 1.25 + 0.75
Mixed_Untyped_Fraction :: 1 + 1.5
Accent :: 'é'
Odd_Mask :: make_mask(8)
Factorial_10 :: factorial(10)
Class_2 :: classify(2)
Class_Default :: classify(9)
Class_Mode :: classify_mode(.On)
Class_Outcome :: classify_outcome(.value(42))
Target_Word :: target_word()
Wrapped_Byte :: increment_byte(255)
Measured_U32 :: measure[u32, 7](0)
Table :: make_table()
Table_Sum :: sum_table(Table)
Vector_Type :: #simd[4]u32
Vector :: Vector_Type{1, 2, 3, 4}
Vector_Value :: Vector[2]
Increment_Procedure :: increment_value
Applied_Procedure :: apply_operation(Increment_Procedure, 41)
Same_Procedure :: Increment_Procedure == increment_value
Different_Procedure :: Increment_Procedure != decrement_value
Statement_Forms :: statement_forms()
Initialized_Member :: initialize_members().value
Rounded_F32 :: round_each_step()
Contextual_Rounded_F32 :: round_contextual_tree()
Sibling_Context_Left :: Contextual_Rounded_F32 ==
    ((16777216.0 + 1.0) - 16777216.0)
Sibling_Context_Right :: ((16777216.0 + 1.0) - 16777216.0) ==
    Contextual_Rounded_F32
Forward_Sibling_Context :: ((16777216.0 + 1.0) - 16777216.0) ==
    Forward_Zero_F32
Forward_Zero_F32 :: round_contextual_tree()
Infinity_F32 :: make_infinity()
Nan_Differs :: make_nan() != make_nan()
Negative_Zero_F32 :: make_negative_zero()
Minimum_I64 :: cast[i64](-9223372036854775807 - 1)
Minimum_Remainder :: Minimum_I64 % -1

Header :: struct {
    tag: u8,
    value: u64,
}

Header_Size :: size_of(Header)
Header_Alignment :: align_of(Header)
Header_Value :: Header{tag = 1, value = 42}.value
Tuple_Value :: (10, 32).1

Conditional :: struct {
    when target.pointer_bits == 64 {
        wide: u64,
    } else {
        narrow: u32,
    }
}

when target.pointer_bits == 32 {
    Word :: u32
} else {
    Word :: u64
}

when target.os == .macos && Has_Neon {
    Platform_Value :: Derived
}

when false {
    Never :: 1
} else when Derived == 42 {
    Nested :: 9
}

when false {
    Broken :: struct { value: Does_Not_Exist, }
} else {
    Good :: u8
}

when Huge > 340282366920938463463374607431768211456 {
    Big_Selected :: bool
}

when Fraction == 2.0 {
    Float_Selected :: bool
}

when size_of(Header) == 16 && align_of(Header) == 8 {
    Layout_Selected :: bool
}

when Accent == '\u{e9}' {
    Rune_Selected :: bool
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.analysis.selections.entries.size() == 11);
  EXPECT(state, find_symbol(source.analysis.package, "Word").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Platform_Value").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Nested").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Good").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Big_Selected").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Float_Selected").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Layout_Selected").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Rune_Selected").has_value());
  EXPECT(state, !find_symbol(source.analysis.package, "Never").has_value());
  EXPECT(state, !find_symbol(source.analysis.package, "Broken").has_value());

  const std::optional<draft::SymbolId> conditional =
      find_symbol(source.analysis.package, "Conditional");
  EXPECT(state, conditional.has_value());
  if (conditional.has_value()) {
    const draft::Symbol &symbol = source.analysis.package.symbols.symbol(*conditional);
    EXPECT(state, source.analysis.package.types.type(symbol.type).layout ==
                      draft::TypeLayout({true, 8, 8}));
    bool saw_wide = false;
    bool saw_narrow = false;
    for (const draft::OwnedSemanticScope &owned :
         source.analysis.package.owned_scopes) {
      if (owned.owner != *conditional ||
          source.analysis.package.symbols.scope(owned.scope).kind !=
              draft::ScopeKind::Type) {
        continue;
      }
      saw_wide = source.analysis.package.symbols.lookup_direct(
          owned.scope, "wide").has_value();
      saw_narrow = source.analysis.package.symbols.lookup_direct(
          owned.scope, "narrow").has_value();
    }
    EXPECT(state, saw_wide);
    EXPECT(state, !saw_narrow);
  }

  const std::optional<draft::SymbolId> bits =
      find_symbol(source.analysis.package, "Bits");
  const std::optional<draft::SymbolId> derived =
      find_symbol(source.analysis.package, "Derived");
  const std::optional<draft::SymbolId> feature =
      find_symbol(source.analysis.package, "Has_Neon");
  const std::optional<draft::SymbolId> bit_value =
      find_symbol(source.analysis.package, "Bit_Value");
  const std::optional<draft::SymbolId> huge =
      find_symbol(source.analysis.package, "Huge");
  const std::optional<draft::SymbolId> fraction =
      find_symbol(source.analysis.package, "Fraction");
  const std::optional<draft::SymbolId> mixed_untyped_fraction =
      find_symbol(source.analysis.package, "Mixed_Untyped_Fraction");
  const std::optional<draft::SymbolId> header_size =
      find_symbol(source.analysis.package, "Header_Size");
  const std::optional<draft::SymbolId> header_alignment =
      find_symbol(source.analysis.package, "Header_Alignment");
  const std::optional<draft::SymbolId> accent =
      find_symbol(source.analysis.package, "Accent");
  const std::optional<draft::SymbolId> odd_mask =
      find_symbol(source.analysis.package, "Odd_Mask");
  const std::optional<draft::SymbolId> factorial =
      find_symbol(source.analysis.package, "Factorial_10");
  const std::optional<draft::SymbolId> class_two =
      find_symbol(source.analysis.package, "Class_2");
  const std::optional<draft::SymbolId> class_default =
      find_symbol(source.analysis.package, "Class_Default");
  const std::optional<draft::SymbolId> class_mode =
      find_symbol(source.analysis.package, "Class_Mode");
  const std::optional<draft::SymbolId> class_outcome =
      find_symbol(source.analysis.package, "Class_Outcome");
  const std::optional<draft::SymbolId> target_word =
      find_symbol(source.analysis.package, "Target_Word");
  const std::optional<draft::SymbolId> wrapped_byte =
      find_symbol(source.analysis.package, "Wrapped_Byte");
  const std::optional<draft::SymbolId> measured_u32 =
      find_symbol(source.analysis.package, "Measured_U32");
  const std::optional<draft::SymbolId> table =
      find_symbol(source.analysis.package, "Table");
  const std::optional<draft::SymbolId> table_sum =
      find_symbol(source.analysis.package, "Table_Sum");
  const std::optional<draft::SymbolId> vector_value =
      find_symbol(source.analysis.package, "Vector_Value");
  const std::optional<draft::SymbolId> header_value =
      find_symbol(source.analysis.package, "Header_Value");
  const std::optional<draft::SymbolId> tuple_value =
      find_symbol(source.analysis.package, "Tuple_Value");
  const std::optional<draft::SymbolId> increment_procedure =
      find_symbol(source.analysis.package, "Increment_Procedure");
  const std::optional<draft::SymbolId> applied_procedure =
      find_symbol(source.analysis.package, "Applied_Procedure");
  const std::optional<draft::SymbolId> same_procedure =
      find_symbol(source.analysis.package, "Same_Procedure");
  const std::optional<draft::SymbolId> different_procedure =
      find_symbol(source.analysis.package, "Different_Procedure");
  const std::optional<draft::SymbolId> statement_forms =
      find_symbol(source.analysis.package, "Statement_Forms");
  const std::optional<draft::SymbolId> initialized_member =
      find_symbol(source.analysis.package, "Initialized_Member");
  const std::optional<draft::SymbolId> rounded_f32 =
      find_symbol(source.analysis.package, "Rounded_F32");
  const std::optional<draft::SymbolId> contextual_rounded_f32 =
      find_symbol(source.analysis.package, "Contextual_Rounded_F32");
  const std::optional<draft::SymbolId> sibling_context_left =
      find_symbol(source.analysis.package, "Sibling_Context_Left");
  const std::optional<draft::SymbolId> sibling_context_right =
      find_symbol(source.analysis.package, "Sibling_Context_Right");
  const std::optional<draft::SymbolId> forward_sibling_context =
      find_symbol(source.analysis.package, "Forward_Sibling_Context");
  const std::optional<draft::SymbolId> infinity_f32 =
      find_symbol(source.analysis.package, "Infinity_F32");
  const std::optional<draft::SymbolId> nan_differs =
      find_symbol(source.analysis.package, "Nan_Differs");
  const std::optional<draft::SymbolId> negative_zero_f32 =
      find_symbol(source.analysis.package, "Negative_Zero_F32");
  EXPECT(state, bits.has_value());
  EXPECT(state, derived.has_value());
  EXPECT(state, feature.has_value());
  EXPECT(state, bit_value.has_value());
  EXPECT(state, huge.has_value());
  EXPECT(state, fraction.has_value());
  EXPECT(state, mixed_untyped_fraction.has_value());
  EXPECT(state, header_size.has_value());
  EXPECT(state, header_alignment.has_value());
  EXPECT(state, accent.has_value());
  EXPECT(state, odd_mask.has_value());
  EXPECT(state, factorial.has_value());
  EXPECT(state, class_two.has_value());
  EXPECT(state, class_default.has_value());
  EXPECT(state, class_mode.has_value());
  EXPECT(state, class_outcome.has_value());
  EXPECT(state, target_word.has_value());
  EXPECT(state, wrapped_byte.has_value());
  EXPECT(state, measured_u32.has_value());
  EXPECT(state, table.has_value());
  EXPECT(state, table_sum.has_value());
  EXPECT(state, vector_value.has_value());
  EXPECT(state, header_value.has_value());
  EXPECT(state, tuple_value.has_value());
  EXPECT(state, increment_procedure.has_value());
  EXPECT(state, applied_procedure.has_value());
  EXPECT(state, same_procedure.has_value());
  EXPECT(state, different_procedure.has_value());
  EXPECT(state, statement_forms.has_value());
  EXPECT(state, initialized_member.has_value());
  EXPECT(state, rounded_f32.has_value());
  EXPECT(state, contextual_rounded_f32.has_value());
  EXPECT(state, sibling_context_left.has_value());
  EXPECT(state, sibling_context_right.has_value());
  EXPECT(state, forward_sibling_context.has_value());
  EXPECT(state, infinity_f32.has_value());
  EXPECT(state, nan_differs.has_value());
  EXPECT(state, negative_zero_f32.has_value());
  if (bits && derived && feature && bit_value && huge && fraction &&
      mixed_untyped_fraction &&
      header_size && header_alignment && accent && odd_mask && factorial &&
      class_two && class_default && target_word && wrapped_byte && measured_u32 &&
      table && table_sum && header_value && tuple_value) {
    const draft::ConstantValue *bits_value = source.analysis.constants.find(*bits);
    const draft::ConstantValue *derived_value = source.analysis.constants.find(*derived);
    const draft::ConstantValue *feature_value = source.analysis.constants.find(*feature);
    const draft::ConstantValue *bit_result = source.analysis.constants.find(*bit_value);
    const draft::ConstantValue *huge_result = source.analysis.constants.find(*huge);
    const draft::ConstantValue *fraction_result = source.analysis.constants.find(*fraction);
    const draft::ConstantValue *mixed_untyped_fraction_result =
        source.analysis.constants.find(*mixed_untyped_fraction);
    const draft::ConstantValue *size_result =
        source.analysis.constants.find(*header_size);
    const draft::ConstantValue *alignment_result =
        source.analysis.constants.find(*header_alignment);
    const draft::ConstantValue *accent_result =
        source.analysis.constants.find(*accent);
    const draft::ConstantValue *mask_result =
        source.analysis.constants.find(*odd_mask);
    const draft::ConstantValue *factorial_result =
        source.analysis.constants.find(*factorial);
    const draft::ConstantValue *class_two_result =
        source.analysis.constants.find(*class_two);
    const draft::ConstantValue *class_default_result =
        source.analysis.constants.find(*class_default);
    const draft::ConstantValue *class_mode_result = class_mode
        ? source.analysis.constants.find(*class_mode)
        : nullptr;
    const draft::ConstantValue *class_outcome_result = class_outcome
        ? source.analysis.constants.find(*class_outcome)
        : nullptr;
    const draft::ConstantValue *target_word_result =
        source.analysis.constants.find(*target_word);
    const draft::ConstantValue *wrapped_byte_result =
        source.analysis.constants.find(*wrapped_byte);
    const draft::ConstantValue *measured_u32_result =
        source.analysis.constants.find(*measured_u32);
    const draft::ConstantValue *table_result =
        source.analysis.constants.find(*table);
    const draft::ConstantValue *table_sum_result =
        source.analysis.constants.find(*table_sum);
    const draft::ConstantValue *vector_value_result = vector_value
        ? source.analysis.constants.find(*vector_value)
        : nullptr;
    const draft::ConstantValue *header_value_result =
        source.analysis.constants.find(*header_value);
    const draft::ConstantValue *tuple_value_result =
        source.analysis.constants.find(*tuple_value);
    const draft::ConstantValue *increment_procedure_result = increment_procedure
        ? source.analysis.constants.find(*increment_procedure)
        : nullptr;
    const draft::ConstantValue *applied_procedure_result = applied_procedure
        ? source.analysis.constants.find(*applied_procedure)
        : nullptr;
    const draft::ConstantValue *same_procedure_result = same_procedure
        ? source.analysis.constants.find(*same_procedure)
        : nullptr;
    const draft::ConstantValue *different_procedure_result = different_procedure
        ? source.analysis.constants.find(*different_procedure)
        : nullptr;
    const draft::ConstantValue *statement_forms_result = statement_forms
        ? source.analysis.constants.find(*statement_forms)
        : nullptr;
    const draft::ConstantValue *initialized_member_result = initialized_member
        ? source.analysis.constants.find(*initialized_member)
        : nullptr;
    const draft::ConstantValue *rounded_f32_result = rounded_f32
        ? source.analysis.constants.find(*rounded_f32)
        : nullptr;
    const draft::ConstantValue *contextual_rounded_f32_result =
        contextual_rounded_f32
        ? source.analysis.constants.find(*contextual_rounded_f32)
        : nullptr;
    const draft::ConstantValue *sibling_context_left_result =
        sibling_context_left
        ? source.analysis.constants.find(*sibling_context_left)
        : nullptr;
    const draft::ConstantValue *sibling_context_right_result =
        sibling_context_right
        ? source.analysis.constants.find(*sibling_context_right)
        : nullptr;
    const draft::ConstantValue *forward_sibling_context_result =
        forward_sibling_context
        ? source.analysis.constants.find(*forward_sibling_context)
        : nullptr;
    const draft::ConstantValue *infinity_f32_result = infinity_f32
        ? source.analysis.constants.find(*infinity_f32)
        : nullptr;
    const draft::ConstantValue *nan_differs_result = nan_differs
        ? source.analysis.constants.find(*nan_differs)
        : nullptr;
    const draft::ConstantValue *negative_zero_f32_result = negative_zero_f32
        ? source.analysis.constants.find(*negative_zero_f32)
        : nullptr;
    const std::optional<draft::SymbolId> minimum_remainder =
        source.analysis.package.symbols.lookup(
            source.analysis.package.package_scope, "Minimum_Remainder");
    const draft::ConstantValue *minimum_remainder_result = minimum_remainder
        ? source.analysis.constants.find(*minimum_remainder)
        : nullptr;
    EXPECT(state, bits_value != nullptr);
    EXPECT(state, derived_value != nullptr);
    EXPECT(state, feature_value != nullptr);
    EXPECT(state, bit_result != nullptr);
    EXPECT(state, huge_result != nullptr);
    EXPECT(state, fraction_result != nullptr);
    EXPECT(state, mixed_untyped_fraction_result != nullptr);
    EXPECT(state, size_result != nullptr);
    EXPECT(state, alignment_result != nullptr);
    EXPECT(state, accent_result != nullptr);
    EXPECT(state, mask_result != nullptr);
    EXPECT(state, factorial_result != nullptr);
    EXPECT(state, class_two_result != nullptr);
    EXPECT(state, class_default_result != nullptr);
    EXPECT(state, class_mode_result != nullptr);
    EXPECT(state, class_outcome_result != nullptr);
    EXPECT(state, target_word_result != nullptr);
    EXPECT(state, wrapped_byte_result != nullptr);
    EXPECT(state, measured_u32_result != nullptr);
    EXPECT(state, table_result != nullptr);
    EXPECT(state, table_sum_result != nullptr);
    EXPECT(state, vector_value_result != nullptr);
    EXPECT(state, header_value_result != nullptr);
    EXPECT(state, tuple_value_result != nullptr);
    EXPECT(state, increment_procedure_result != nullptr);
    EXPECT(state, applied_procedure_result != nullptr);
    EXPECT(state, same_procedure_result != nullptr);
    EXPECT(state, different_procedure_result != nullptr);
    EXPECT(state, statement_forms_result != nullptr);
    EXPECT(state, initialized_member_result != nullptr);
    EXPECT(state, rounded_f32_result != nullptr);
    EXPECT(state, contextual_rounded_f32_result != nullptr);
    EXPECT(state, sibling_context_left_result != nullptr);
    EXPECT(state, sibling_context_right_result != nullptr);
    EXPECT(state, forward_sibling_context_result != nullptr);
    EXPECT(state, infinity_f32_result != nullptr);
    EXPECT(state, nan_differs_result != nullptr);
    EXPECT(state, negative_zero_f32_result != nullptr);
    EXPECT(state, minimum_remainder_result != nullptr);
    if (bits_value) EXPECT(state, bits_value->integer.to_decimal() == "64");
    if (derived_value) EXPECT(state, derived_value->integer.to_decimal() == "42");
    if (feature_value) EXPECT(state, feature_value->boolean);
    if (bit_result) EXPECT(state, bit_result->integer.to_decimal() == "65");
    if (huge_result) {
      EXPECT(state, huge_result->integer.to_decimal() ==
                        "340282366920938463463374607431768211463");
    }
    if (fraction_result) {
      EXPECT(state, fraction_result->kind == draft::ConstantKind::Float);
      EXPECT(state, fraction_result->floating.to_fraction() == "2/1");
    }
    if (mixed_untyped_fraction_result) {
      EXPECT(state,
             mixed_untyped_fraction_result->kind == draft::ConstantKind::Float);
      EXPECT(state,
             mixed_untyped_fraction_result->floating.to_fraction() == "5/2");
    }
    if (size_result) EXPECT(state, size_result->integer.to_decimal() == "16");
    if (alignment_result) {
      EXPECT(state, alignment_result->integer.to_decimal() == "8");
    }
    if (accent_result) {
      EXPECT(state, accent_result->integer.to_decimal() == "233");
      EXPECT(state, source.analysis.package.symbols.symbol(*accent).type ==
                        source.analysis.package.types.builtins().rune_type);
    }
    if (mask_result) EXPECT(state, mask_result->integer.to_decimal() == "170");
    if (factorial_result) {
      EXPECT(state, factorial_result->integer.to_decimal() == "3628800");
    }
    if (class_two_result) {
      EXPECT(state, class_two_result->integer.to_decimal() == "20");
    }
    if (class_default_result) {
      EXPECT(state, class_default_result->integer.to_decimal() == "30");
    }
    if (class_mode_result) {
      EXPECT(state, class_mode_result->integer.to_decimal() == "1");
    }
    if (class_outcome_result) {
      EXPECT(state, class_outcome_result->integer.to_decimal() == "42");
    }
    if (target_word_result) {
      EXPECT(state, target_word_result->integer.to_decimal() == "64");
    }
    if (wrapped_byte_result) {
      EXPECT(state, wrapped_byte_result->integer.to_decimal() == "0");
    }
    if (measured_u32_result) {
      EXPECT(state, measured_u32_result->integer.to_decimal() == "11");
    }
    if (table_result) {
      EXPECT(state, table_result->kind == draft::ConstantKind::Aggregate);
      EXPECT(state, table_result->elements.size() == 4);
      if (table_result->elements.size() == 4) {
        EXPECT(state, table_result->elements[3].integer.to_decimal() == "9");
      }
    }
    if (table_sum_result) {
      EXPECT(state, table_sum_result->integer.to_decimal() == "14");
    }
    if (vector_value_result) {
      EXPECT(state, vector_value_result->integer.to_decimal() == "3");
    }
    if (header_value_result) {
      EXPECT(state, header_value_result->integer.to_decimal() == "42");
    }
    if (tuple_value_result) {
      EXPECT(state, tuple_value_result->integer.to_decimal() == "32");
    }
    if (increment_procedure_result) {
      EXPECT(state,
             increment_procedure_result->kind == draft::ConstantKind::Procedure);
      EXPECT(state,
             increment_procedure_result->symbol_index <
                 source.analysis.package.symbols.symbol_count());
    }
    if (applied_procedure_result) {
      EXPECT(state, applied_procedure_result->integer.to_decimal() == "42");
    }
    if (same_procedure_result) EXPECT(state, same_procedure_result->boolean);
    if (different_procedure_result) {
      EXPECT(state, different_procedure_result->boolean);
    }
    if (statement_forms_result) {
      EXPECT(state, statement_forms_result->integer.to_decimal() == "84");
    }
    if (initialized_member_result) {
      EXPECT(state, initialized_member_result->integer.to_decimal() == "42");
    }
    if (rounded_f32_result) {
      EXPECT(state, rounded_f32_result->float_bit_width == 32);
      EXPECT(state, rounded_f32_result->float_bits == 0x4b800000U);
    }
    if (contextual_rounded_f32_result) {
      EXPECT(state, contextual_rounded_f32_result->float_bit_width == 32);
      EXPECT(state, contextual_rounded_f32_result->float_bits == 0U);
    }
    if (sibling_context_left_result) {
      EXPECT(state, sibling_context_left_result->boolean);
    }
    if (sibling_context_right_result) {
      EXPECT(state, sibling_context_right_result->boolean);
    }
    if (forward_sibling_context_result) {
      EXPECT(state, forward_sibling_context_result->boolean);
    }
    if (infinity_f32_result) {
      EXPECT(state, infinity_f32_result->float_bit_width == 32);
      EXPECT(state, infinity_f32_result->float_bits == 0x7f800000U);
    }
    if (nan_differs_result) EXPECT(state, nan_differs_result->boolean);
    if (negative_zero_f32_result) {
      EXPECT(state, negative_zero_f32_result->float_bit_width == 32);
      EXPECT(state, negative_zero_f32_result->float_bits == 0x80000000U);
    }
    if (minimum_remainder_result) {
      EXPECT(state, minimum_remainder_result->integer.is_zero());
    }
  }
}

void test_invalid_required_constants(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Not_Bool :: 7
Bad :: 1 / 0

when Not_Bool {
    Value :: 1
}
)draft");

  EXPECT(state, !source.analysis.ok);
  EXPECT(state, source.diagnostics.error_count() == 2);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("must have type bool") != std::string::npos);
  EXPECT(state, rendered.find("division by zero") != std::string::npos);
}

void test_invalid_procedural_constants(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

bad_return :: proc() -> u8 {
    return 256
}

bad_shift :: proc(value: u8) -> u8 {
    return value << 8
}

bad_division :: proc(value: i8) -> i8 {
    return value / -1
}

recursive :: proc(value: uint) -> uint {
    return recursive(value + 1)
}

foreign system {
    external :: c proc() -> i64
}

uses_foreign :: proc() -> i64 {
    return external()
}

reads_uninitialized :: proc() -> u32 {
    value: u32 = ---
    return value
}

Bad_Return :: bad_return()
Bad_Shift :: bad_shift(1)
Bad_Division :: bad_division(-128)
Bad_Recursion :: recursive(0)
Bad_Foreign :: uses_foreign()
Bad_Uninitialized :: reads_uninitialized()
)draft");

  EXPECT(state, !source.analysis.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("not representable") != std::string::npos);
  EXPECT(state, rendered.find("shift count traps") != std::string::npos);
  EXPECT(state, rendered.find("division overflow traps") != std::string::npos);
  EXPECT(state, rendered.find("recursion limit exceeded") != std::string::npos);
  EXPECT(state, rendered.find("foreign calls are unavailable") != std::string::npos);
  EXPECT(state, rendered.find("reads an uninitialized local") != std::string::npos);
}

void test_compile_time_defer(TestState &state) {
  AnalyzedSource valid(R"draft(
package conditions

require_saved_one :: proc(value: int) {
    if value != 1 {
        impossible := 1 / (value - value)
    }
}

consume[T: type] :: proc(value: T) {
}

deferred_result :: proc() -> int {
    value := 1
    cleanup := require_saved_one
    defer cleanup(value)
    defer consume[int](value)
    value = 2
    return value
}

Deferred_Result :: deferred_result()
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.analysis.ok);
  EXPECT(state, !valid.diagnostics.has_errors());
  const std::optional<draft::SymbolId> result =
      find_symbol(valid.analysis.package, "Deferred_Result");
  EXPECT(state, result.has_value());
  const draft::ConstantValue *value = result.has_value()
      ? valid.analysis.constants.find(*result)
      : nullptr;
  EXPECT(state, value != nullptr);
  if (value != nullptr) {
    EXPECT(state, value->kind == draft::ConstantKind::Integer);
    EXPECT(state, value->integer.to_decimal() == "2");
  }

  AnalyzedSource invalid(R"draft(
package conditions

divide_by_saved_zero :: proc(value: int) -> int {
    return 1 / (value - value)
}

shift_by_saved_large_value :: proc(value: int) -> int {
    return 1 << (value + 1000)
}

no_value :: proc() {
}

deferred_order :: proc() -> int {
    defer divide_by_saved_zero(1)
    defer shift_by_saved_large_value(1)
    return 7
}

Bad_Deferred_Order :: deferred_order()
Bad_Void_Value :: no_value()
)draft");
  EXPECT(state, !invalid.analysis.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("shift count traps") != std::string::npos);
  EXPECT(state, rendered.find("division by zero") == std::string::npos);
  EXPECT(state, rendered.find(
                    "void compile-time procedure call does not produce a value") !=
                    std::string::npos);
}

void test_compile_time_callee_expressions(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Operation_Box :: struct {
    operation: proc(value: int) -> int,
}

increment :: proc(value: int) -> int {
    return value + 1
}

decrement :: proc(value: int) -> int {
    return value - 1
}

Box :: Operation_Box{operation = increment}
From_Field :: Box.operation(41)
From_Group :: (increment)(41)
From_Selection :: (increment if true else decrement)(41)
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  for (std::string_view name : {
           std::string_view("From_Field"),
           std::string_view("From_Group"),
           std::string_view("From_Selection")}) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(source.analysis.package, name);
    EXPECT(state, symbol.has_value());
    const draft::ConstantValue *value = symbol.has_value()
        ? source.analysis.constants.find(*symbol)
        : nullptr;
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Integer);
      EXPECT(state, value->integer.to_decimal() == "42");
    }
  }
}

void test_procedural_layout_constants(TestState &state) {
  AnalyzedSource valid(R"draft(
package conditions

plus_one :: proc(value: usize) -> usize {
    return value + 1
}

choose_size :: proc(wide: bool) -> usize {
    if wide {
        return 4
    }
    return 8
}

enum_value :: proc() -> i16 {
    return 7
}

small_count :: proc() -> u8 {
    return 3
}

Sized[N: usize] :: struct {
    values: [N]u8,
}

Procedural_Sized[N: usize] :: struct {
    values: [plus_one(N)]u8,
}

Procedural_Wrapper[N: usize] :: struct {
    value: Procedural_Sized[plus_one(N)],
}

Procedural_Outer[N: usize] :: struct {
    value: Procedural_Wrapper[plus_one(N)],
}

Procedural_Bytes[N: usize] :: [N]u8

Procedural_Bytes_Outer[N: usize] :: Procedural_Bytes[plus_one(N)]

Small[N: u8] :: struct {
    values: [cast[usize](N)]u8,
}

Computed_Vector :: #simd[choose_size(true)]u32

Procedural_Vector[N: usize] :: struct {
    values: #simd[plus_one(N)]u32,
}

Procedural_Concrete :: struct {
    array: Procedural_Sized[2],
    vector: Procedural_Vector[3],
    nested: Procedural_Wrapper[1],
    outer: Procedural_Outer[1],
    alias_nested: Procedural_Bytes_Outer[2],
}

Buffer :: struct {
    direct: [plus_one(2)]u8,
    selected: [choose_size(true)]u8,
    nested: Sized[plus_one(2)],
    small: Small[small_count()],
}

// This value cannot be known in the first semantic round: Buffer itself needs
// procedure evaluation before it has a layout. It exercises real fixed-point
// progress instead of several independent calls that happen to finish together.
Measured :: struct {
    bytes: [size_of(Buffer)]u8,
}

when size_of(Measured) == 13 {
    Layout_Selected :: bool
} else {
    Wrong_Layout_Selected :: bool
}

Aligned :: @align(choose_size(false)) struct {
    value: u8,
}

Code :: enum i16 {
    Zero,
    Seven = enum_value(),
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.analysis.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  const std::optional<draft::SymbolId> buffer =
      find_symbol(valid.analysis.package, "Buffer");
  const std::optional<draft::SymbolId> aligned =
      find_symbol(valid.analysis.package, "Aligned");
  const std::optional<draft::SymbolId> measured =
      find_symbol(valid.analysis.package, "Measured");
  const std::optional<draft::SymbolId> computed_vector =
      find_symbol(valid.analysis.package, "Computed_Vector");
  const std::optional<draft::SymbolId> procedural_concrete =
      find_symbol(valid.analysis.package, "Procedural_Concrete");
  EXPECT(state, buffer.has_value());
  EXPECT(state, aligned.has_value());
  EXPECT(state, measured.has_value());
  EXPECT(state, computed_vector.has_value());
  EXPECT(state, procedural_concrete.has_value());
  EXPECT(state, find_symbol(
                    valid.analysis.package, "Layout_Selected").has_value());
  EXPECT(state, !find_symbol(
                    valid.analysis.package, "Wrong_Layout_Selected").has_value());
  if (buffer.has_value()) {
    const draft::Type &type = valid.analysis.package.types.type(
        valid.analysis.package.symbols.symbol(*buffer).type);
    EXPECT(state, type.layout == draft::TypeLayout({true, 13, 1}));
  }
  if (aligned.has_value()) {
    const draft::Type &type = valid.analysis.package.types.type(
        valid.analysis.package.symbols.symbol(*aligned).type);
    EXPECT(state, type.layout == draft::TypeLayout({true, 8, 8}));
  }
  if (measured.has_value()) {
    const draft::Type &type = valid.analysis.package.types.type(
        valid.analysis.package.symbols.symbol(*measured).type);
    EXPECT(state, type.layout == draft::TypeLayout({true, 13, 1}));
  }
  if (computed_vector.has_value()) {
    const draft::Type &type = valid.analysis.package.types.type(
        valid.analysis.package.symbols.symbol(*computed_vector).type);
    EXPECT(state, type.kind == draft::TypeKind::Simd);
    EXPECT(state, type.element_count == 4);
  }
  if (procedural_concrete.has_value()) {
    const draft::Type &type = valid.analysis.package.types.type(
        valid.analysis.package.symbols.symbol(*procedural_concrete).type);
    EXPECT(state, type.layout == draft::TypeLayout({true, 48, 16}));
    EXPECT(state, type.members.size() == 5);
    if (type.members.size() == 5) {
      const draft::Type &array =
          valid.analysis.package.types.type(type.members.front());
      const draft::Type &vector =
          valid.analysis.package.types.type(type.members[1]);
      const draft::Type &nested =
          valid.analysis.package.types.type(type.members[2]);
      const draft::Type &outer =
          valid.analysis.package.types.type(type.members[3]);
      const draft::Type &alias_nested =
          valid.analysis.package.types.type(type.members.back());
      EXPECT(state, array.layout == draft::TypeLayout({true, 3, 1}));
      EXPECT(state, vector.layout == draft::TypeLayout({true, 16, 16}));
      EXPECT(state, nested.layout == draft::TypeLayout({true, 3, 1}));
      EXPECT(state, outer.layout == draft::TypeLayout({true, 4, 1}));
      EXPECT(state, alias_nested.layout == draft::TypeLayout({true, 3, 1}));
    }
  }
  EXPECT(state,
         valid.analysis.package.required_integer_expressions.empty());

  bool saw_seven = false;
  for (const draft::EnumMemberValue &member :
       valid.analysis.package.enum_member_values) {
    const draft::Symbol &symbol =
        valid.analysis.package.symbols.symbol(member.member);
    saw_seven = saw_seven ||
        (symbol.name == "Seven" && member.value.to_decimal() == "7");
  }
  EXPECT(state, saw_seven);

  // u64 and usize happen to have the same machine representation on the first
  // target, but Draft value parameters do not gain an implicit conversion from
  // that coincidence. The typed fixed-point result must retain this boundary.
  AnalyzedSource invalid(R"draft(
package conditions

wrong_count :: proc() -> u64 {
    return 4
}

wrong_generic_count :: proc(value: usize) -> u64 {
    return value
}

Bad_Generic[N: usize] :: struct {
    values: [wrong_generic_count(N)]u8,
}

Sized[N: usize] :: struct {
    values: [N]u8,
}

Bad :: struct {
    value: Sized[wrong_count()],
    generic: Bad_Generic[4],
}

Bad_Array :: struct {
    values: [wrong_count()]u8,
}

Bad_Vector :: #simd[wrong_count()]u32

Bad_Aligned :: @align(wrong_count()) struct {
    value: u8,
}
)draft");
  EXPECT(state, !invalid.analysis.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("package.draft:21:18") != std::string::npos);
  EXPECT(state, rendered.find(
                    "compile-time value argument has the wrong concrete integer type") !=
                    std::string::npos);
  EXPECT(state, rendered.find("array length must have type 'usize'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("SIMD lane count must have type 'usize'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("'@align' argument must have type 'usize'") !=
                    std::string::npos);
}

void test_compile_time_string_views(TestState &state) {
  AnalyzedSource valid(R"draft(
package conditions

Text :: distinct string

Whole :: "dr\x61ft"
Middle :: Whole[1:4]
Prefix :: Whole[:2]
Suffix :: Whole[2:]
Copy :: Whole[:]
Middle_Byte :: Middle[1]
Exact_Bytes :: "A\0\u{e9}Z"[1:4]

Low :: cast[usize](1)
High :: cast[usize](4)
Typed_Bounds :: Whole[Low:High]

Wrapped :: cast[Text](Whole)
Wrapped_Tail :: Wrapped[1:]

select_middle :: proc() -> string {
    return "abcdef"[1:4]
}

From_Procedure :: select_middle()
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.analysis.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  const auto expect_string = [&](std::string_view name,
                                 std::string_view expected) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(valid.analysis.package, name);
    EXPECT(state, symbol.has_value());
    const draft::ConstantValue *value = symbol.has_value()
        ? valid.analysis.constants.find(*symbol)
        : nullptr;
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::String);
      EXPECT(state, value->text == expected);
    }
  };
  expect_string("Middle", "raf");
  expect_string("Prefix", "dr");
  expect_string("Suffix", "aft");
  expect_string("Copy", "draft");
  expect_string("Typed_Bounds", "raf");
  expect_string("Wrapped_Tail", "raft");
  expect_string("From_Procedure", "bcd");
  expect_string("Exact_Bytes", std::string_view("\0\xc3\xa9", 3));

  const std::optional<draft::SymbolId> middle_byte =
      find_symbol(valid.analysis.package, "Middle_Byte");
  const draft::ConstantValue *byte_value = middle_byte.has_value()
      ? valid.analysis.constants.find(*middle_byte)
      : nullptr;
  EXPECT(state, byte_value != nullptr);
  if (byte_value != nullptr) {
    EXPECT(state, byte_value->kind == draft::ConstantKind::Integer);
    EXPECT(state, byte_value->integer.to_decimal() == "97");
  }

  const std::optional<draft::SymbolId> wrapped_tail =
      find_symbol(valid.analysis.package, "Wrapped_Tail");
  const std::optional<draft::SymbolId> text =
      find_symbol(valid.analysis.package, "Text");
  EXPECT(state, wrapped_tail.has_value());
  EXPECT(state, text.has_value());
  if (wrapped_tail.has_value() && text.has_value()) {
    EXPECT(state, valid.analysis.package.symbols.symbol(*wrapped_tail).type ==
                      valid.analysis.package.symbols.symbol(*text).type);
  }

  AnalyzedSource invalid(R"draft(
package conditions

Signed_Bound :: cast[i64](1)
Bad_Index_Type :: "draft"[Signed_Bound]
Bad_Bound_Type :: "draft"[Signed_Bound:]
Bad_Index :: "draft"[5]
Bad_High :: "draft"[:6]
Bad_Order :: "draft"[3:2]
Bad_Negative :: "draft"[-1:]
)draft");
  EXPECT(state, !invalid.analysis.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("constant index must have type usize") !=
                    std::string::npos);
  EXPECT(state, rendered.find("constant slice bound must have type usize") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "constant index 5 is out of bounds for length 5") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "constant slice bounds [0:6] are invalid for length 5") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "constant slice bounds [3:2] are invalid for length 5") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "constant slice bound is negative or excessive") !=
                    std::string::npos);
}

void test_operator_type_boundaries(TestState &state) {
  AnalyzedSource valid(R"draft(
package conditions

Distance :: distinct i64
Good :: cast[Distance](cast[i64](40)) + 2

Truth :: distinct bool
Truth_Value :: cast[Truth](true)
Negated_Truth :: !Truth_Value
Combined_Truth :: Truth_Value && !Negated_Truth

identity[T: type] :: proc(value: T) -> T {
    return value
}
Generic_Selected :: identity[bool](false && true)

callback :: proc() {
}
Good_Nil :: callback != nil
Short_Circuit_Division :: false && ((1 / 0) == 0)
Selected_Conditional :: 42 if true else (1 / 0)
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.analysis.ok);
  EXPECT(state, !valid.diagnostics.has_errors());
  EXPECT(state, valid.analysis.package.parametric_instances.empty());

  AnalyzedSource invalid(R"draft(
package conditions

Mode :: enum {
    Zero,
    One,
}

bad_compound :: proc() -> rune {
    value := 'a'
    value += 'b'
    return value
}

Bad_String :: "draft" == "draft"
Bad_Target_String :: target.file_tag == "aarch64-macos"
Bad_Mixed_Numeric :: cast[u32](1) + cast[u64](2)
Bad_Endian_Order ::
    cast[u32be](cast[u32](1)) < cast[u32be](cast[u32](2))
Bad_Enum_Arithmetic ::
    cast[Mode](cast[int](1)) + cast[Mode](cast[int](1))
Bad_Rune_Arithmetic :: 'a' + 'b'
Bad_Rune_Negate :: -'a'
Bad_Endian_Not :: ~cast[u32be](cast[u32](1))
Bad_Enum_Negate :: -cast[Mode](cast[int](1))
Bad_Compound :: bad_compound()
Bad_Nil :: nil == nil
Bad_Dead_Logical :: false && 1
Bad_Dead_Conditional :: 42 if true else "wrong"
)draft");
  if (invalid.diagnostics.error_count() < 11) {
    std::cerr << draft::render_diagnostics(
        invalid.sources, invalid.diagnostics);
  }
  EXPECT(state, !invalid.analysis.ok);
  EXPECT(state, invalid.diagnostics.error_count() >= 11);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find(
                    "compile-time operator is not defined for operand types") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "compile-time unary operator is not defined for operand type") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "compile-time nil comparison requires a pointer or procedure type") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "logical operators require matching bool operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find("numeric operands require one common type") !=
                    std::string::npos);
}

void test_global_initializers(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Mode :: enum i16 {
    Off,
    On = 7,
}

Header :: struct {
    tag: u8,
    value: u64,
}

Outcome :: union {
    empty,
    value: u32,
}

Float_Outcome :: union {
    empty,
    value: f32,
}

Overlay :: raw union {
    byte: u8,
    word: u64,
}

Answer :: 40 + 2
answer_from_procedure :: proc() -> u64 {
    return Answer
}
Answer_Procedure :: answer_from_procedure

make_table :: proc() -> [4]u32 {
    result: [4]u32
    for i: usize = 0; i < len(result); i += 1 {
        result[i] = cast[u32](i + 1)
    }
    return result
}

count: u64 = Answer
computed: u64 = answer_from_procedure()
callback: proc() -> u64 = Answer_Procedure
inferred_callback := answer_from_procedure
inferred := 21
ratio: f64 = 0.5
contextual_rounding: f32 = (16777216.0 + 1.0) - 16777216.0
enabled: bool = true
message: string = "draft"
mode: Mode = .On
pointer: ^u64 = nil
table: [4]u32 = make_table()
header: Header = Header{tag = 1, value = 42}
inferred_header := Header{tag = 2, value = 84}
outcome: Outcome = .value(9)
float_outcome: Float_Outcome =
    .value((16777216.0 + 1.0) - 16777216.0)
overlay: Overlay = Overlay{word = 0x1020304050607080}
thread_local scratch: i32 = -7
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  const std::optional<draft::SymbolId> count =
      find_symbol(source.analysis.package, "count");
  const std::optional<draft::SymbolId> inferred =
      find_symbol(source.analysis.package, "inferred");
  const std::optional<draft::SymbolId> computed =
      find_symbol(source.analysis.package, "computed");
  const std::optional<draft::SymbolId> callback =
      find_symbol(source.analysis.package, "callback");
  const std::optional<draft::SymbolId> inferred_callback =
      find_symbol(source.analysis.package, "inferred_callback");
  const std::optional<draft::SymbolId> contextual_rounding =
      find_symbol(source.analysis.package, "contextual_rounding");
  const std::optional<draft::SymbolId> mode =
      find_symbol(source.analysis.package, "mode");
  const std::optional<draft::SymbolId> pointer =
      find_symbol(source.analysis.package, "pointer");
  const std::optional<draft::SymbolId> table =
      find_symbol(source.analysis.package, "table");
  const std::optional<draft::SymbolId> header =
      find_symbol(source.analysis.package, "header");
  const std::optional<draft::SymbolId> inferred_header =
      find_symbol(source.analysis.package, "inferred_header");
  const std::optional<draft::SymbolId> outcome =
      find_symbol(source.analysis.package, "outcome");
  const std::optional<draft::SymbolId> float_outcome =
      find_symbol(source.analysis.package, "float_outcome");
  const std::optional<draft::SymbolId> overlay =
      find_symbol(source.analysis.package, "overlay");
  EXPECT(state, count.has_value());
  EXPECT(state, inferred.has_value());
  EXPECT(state, computed.has_value());
  EXPECT(state, callback.has_value());
  EXPECT(state, inferred_callback.has_value());
  EXPECT(state, contextual_rounding.has_value());
  EXPECT(state, mode.has_value());
  EXPECT(state, pointer.has_value());
  EXPECT(state, table.has_value());
  EXPECT(state, header.has_value());
  EXPECT(state, inferred_header.has_value());
  EXPECT(state, outcome.has_value());
  EXPECT(state, float_outcome.has_value());
  EXPECT(state, overlay.has_value());
  if (count.has_value()) {
    EXPECT(state, source.analysis.constants.find(*count) == nullptr);
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*count);
    EXPECT(state, value != nullptr);
    if (value != nullptr) EXPECT(state, value->integer.to_decimal() == "42");
  }
  if (inferred.has_value()) {
    EXPECT(state, source.analysis.package.symbols.symbol(*inferred).type ==
                      source.analysis.package.types.builtins().int_type);
  }
  if (computed.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*computed);
    EXPECT(state, value != nullptr);
    if (value != nullptr) EXPECT(state, value->integer.to_decimal() == "42");
  }
  if (callback.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*callback);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Procedure);
    }
  }
  if (inferred_callback.has_value()) {
    const draft::Symbol &symbol =
        source.analysis.package.symbols.symbol(*inferred_callback);
    EXPECT(state,
           source.analysis.package.types.type(symbol.type).kind ==
               draft::TypeKind::Procedure);
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*inferred_callback);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Procedure);
    }
  }
  if (contextual_rounding.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*contextual_rounding);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->float_bit_width == 32);
      EXPECT(state, value->float_bits == 0U);
    }
  }
  if (mode.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*mode);
    EXPECT(state, value != nullptr);
    if (value != nullptr) EXPECT(state, value->integer.to_decimal() == "7");
  }
  if (pointer.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*pointer);
    EXPECT(state, value != nullptr);
    if (value != nullptr) EXPECT(state, value->kind == draft::ConstantKind::Nil);
  }
  if (table.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*table);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Aggregate);
      EXPECT(state, value->elements.size() == 4);
      if (value->elements.size() == 4) {
        EXPECT(state, value->elements[3].integer.to_decimal() == "4");
      }
    }
  }
  if (header.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*header);
    EXPECT(state, value != nullptr);
    if (value != nullptr && value->elements.size() == 2) {
      EXPECT(state, value->elements[1].integer.to_decimal() == "42");
    }
  }
  if (inferred_header.has_value()) {
    const draft::Symbol &symbol =
        source.analysis.package.symbols.symbol(*inferred_header);
    EXPECT(state, source.analysis.package.types.type(symbol.type).kind ==
                      draft::TypeKind::Struct);
  }
  if (outcome.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*outcome);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->variant_index == 1);
      EXPECT(state, value->elements.size() == 1);
    }
  }
  if (float_outcome.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*float_outcome);
    EXPECT(state, value != nullptr);
    if (value != nullptr && value->elements.size() == 1) {
      EXPECT(state, value->elements.front().float_bit_width == 32);
      EXPECT(state, value->elements.front().float_bits == 0U);
    }
  }
  if (overlay.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*overlay);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->variant_index == 1);
      EXPECT(state, value->elements.size() == 1);
    }
  }

  AnalyzedSource invalid(R"draft(
package conditions

Mode :: enum {
    Off,
}

Record :: struct {
    left: i64,
    right: i64,
}

Overlay :: raw union {
    signed: i64,
    unsigned: u64,
}

runtime_value: i64 = 1

runtime :: proc() -> i64 {
    return runtime_value
}

too_large: u8 = 256
not_constant: i64 = runtime()
uninitialized: i64 = ---
wrong_number: i32 = 1.5
wrong_nil: i32 = nil
wrong_mode: Mode = .Missing
duplicate_record: Record = Record{left = 1, left = 2}
positional_record: Record = Record{1, 2}
keyed_array: [2]i64 = [2]i64{first = 1}
empty_overlay: Overlay = Overlay{}
multiple_overlay: Overlay = Overlay{signed = 1, unsigned = 2}
positional_overlay: Overlay = Overlay{1}
dead_logical_type: bool = false && 1
dead_conditional_type: i64 = 42 if true else "wrong"
)draft");
  EXPECT(state, !invalid.analysis.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("not representable") != std::string::npos);
  EXPECT(state, rendered.find("not compile-time evaluable") != std::string::npos);
  EXPECT(state, rendered.find("automatic local") != std::string::npos);
  EXPECT(state, rendered.find("incompatible with global type") != std::string::npos);
  EXPECT(state, rendered.find("names no member") != std::string::npos);
  EXPECT(state, rendered.find("initialized more than once") != std::string::npos);
  EXPECT(state, rendered.find("struct composite elements must name a field") !=
                    std::string::npos);
  EXPECT(state, rendered.find("keyed constant element requires a named aggregate member") !=
                    std::string::npos);
  EXPECT(state, rendered.find("must initialize exactly one field") !=
                    std::string::npos);
  EXPECT(state, rendered.find("raw union composite element must name a field") !=
                    std::string::npos);
  EXPECT(state, rendered.find("logical operators require matching bool operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find("numeric operands require one common type") !=
                    std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_constants_and_conditional_rounds(state);
  test_invalid_required_constants(state);
  test_invalid_procedural_constants(state);
  test_compile_time_defer(state);
  test_compile_time_callee_expressions(state);
  test_procedural_layout_constants(state);
  test_compile_time_string_views(state);
  test_operator_type_boundaries(state);
  test_global_initializers(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " constant evaluation expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all constant evaluation tests passed\n";
  return EXIT_SUCCESS;
}
