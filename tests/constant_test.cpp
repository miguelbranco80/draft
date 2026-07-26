// Compile-time evaluation and product-scheduled declaration `when` tests.

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

[[nodiscard]] draft::TargetFacts test_target() {
  draft::TargetFacts target;
  target.identity = "draft-aarch64-macos-v6";
  target.arch = "aarch64";
  target.os = "macos";
  target.abi = "darwin_arm64";
  target.byte_order = "little";
  target.object_format = "macho";
  target.file_tag = "aarch64-macos";
  target.pointer_bits = 64;
  target.page_size = 16384;
  target.known_features = {"crc", "neon"};
  target.simd_shapes = {{"u32", 4}};
  target.features = {"neon"};
  return target;
}

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

    const draft::TargetFacts target = test_target();
    analysis = draft::analyze_package_semantics(
        sources, loaded, target, diagnostics);
  }
};

[[nodiscard]] std::optional<draft::SymbolId> find_symbol(
    const draft::SemanticPackage &package, std::string_view name) {
  return package.symbols.lookup_direct(package.package_scope, name);
}

void test_append_only_constant_overlay(TestState &state) {
  draft::ConstantTable canonical;
  canonical.bindings.push_back(
      {draft::SymbolId{1}, draft::ConstantValue::make_integer(10)});
  canonical.bindings.push_back(
      {draft::SymbolId{3}, draft::ConstantValue::make_bool(true)});
  const std::size_t prefix = canonical.size();

  draft::ConstantTable task = canonical.fork_append_only();
  EXPECT(state, task.size() == prefix);
  EXPECT(state, task.find(draft::SymbolId{1}) != nullptr);
  EXPECT(state, task.find(draft::SymbolId{3}) != nullptr);
  task.bindings.push_back(
      {draft::SymbolId{7}, draft::ConstantValue::make_integer(42)});
  EXPECT(state, task.size() == prefix + 1);
  EXPECT(state, task.find(draft::SymbolId{7}) != nullptr);
  EXPECT(state, canonical.find(draft::SymbolId{7}) == nullptr);
  EXPECT(state, canonical.size() == prefix);

  // Materialization preserves caller-supplied bindings. Generic recipe
  // substitution relies on this precedence before filling the remaining
  // visible package and lexical constants from the overlay.
  draft::ConstantTable materialized;
  materialized.bindings.push_back(
      {draft::SymbolId{1}, draft::ConstantValue::make_integer(99)});
  task.append_missing_bindings_to(materialized);
  EXPECT(state, materialized.size() == 3);
  const draft::ConstantValue *overridden =
      materialized.find(draft::SymbolId{1});
  EXPECT(state, overridden != nullptr);
  if (overridden != nullptr) {
    EXPECT(state,
           overridden->integer == draft::BigInteger::from_u64(99));
  }
  EXPECT(state, materialized.find(draft::SymbolId{3}) != nullptr);
  EXPECT(state, materialized.find(draft::SymbolId{7}) != nullptr);

  std::vector<draft::ConstantBinding> appended =
      task.appended_since(prefix);
  EXPECT(state, appended.size() == 1);
  canonical.append_exact(prefix, std::move(appended));
  EXPECT(state, canonical.size() == prefix + 1);
  EXPECT(state, canonical.find(draft::SymbolId{7}) != nullptr);
}

// Compile-time aggregate values represent the logical result of reading their
// storage, not merely the pre-storage initializer expressions. This must match
// native bit-field truncation for literals, direct assignment, and compound
// assignment, including negative values more than one modulus below zero.
void test_compile_time_bit_field_storage(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Bit_Record :: struct {
    bits(3) kind: u8,
    bits(6) delta: i16,
}

Stored :: Bit_Record{kind = 15, delta = -67}
Stored_Kind :: Stored.kind
Stored_Delta :: Stored.delta

mutate :: proc() -> i16 {
    value := Stored
    value.delta = -129
    value.delta += 64
    return value.delta
}

Mutated_Delta :: mutate()
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const auto expect_integer = [&](std::string_view name,
                                  std::string_view expected) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(source.analysis.package, name);
    EXPECT(state, symbol.has_value());
    if (!symbol.has_value()) return;
    const draft::ConstantValue *value =
        source.analysis.constants.find(*symbol);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Integer);
      EXPECT(state, value->integer.to_decimal() == expected);
    }
  };
  expect_integer("Stored_Kind", "7");
  expect_integer("Stored_Delta", "-3");
  expect_integer("Mutated_Delta", "-1");
}

void test_single_constant_product_dependencies(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Header :: struct {
    tag: u8,
    value: u64,
}

Base :: 40
Derived :: Base + 2
Header_Size :: size_of(Header)
)draft");
  EXPECT(state, source.analysis.ok);
  const std::optional<draft::SymbolId> base =
      find_symbol(source.analysis.package, "Base");
  const std::optional<draft::SymbolId> derived =
      find_symbol(source.analysis.package, "Derived");
  const std::optional<draft::SymbolId> header =
      find_symbol(source.analysis.package, "Header");
  const std::optional<draft::SymbolId> header_size =
      find_symbol(source.analysis.package, "Header_Size");
  EXPECT(state, base.has_value());
  EXPECT(state, derived.has_value());
  EXPECT(state, header.has_value());
  EXPECT(state, header_size.has_value());
  if (!base || !derived || !header || !header_size) return;

  draft::ConstantTable published;
  draft::SemanticPackage blocked_package = source.analysis.package;
  draft::DiagnosticSink blocked_diagnostics;
  const draft::ConstantProductAttempt blocked =
      draft::evaluate_package_constant_product(
          source.sources,
          source.loaded,
          blocked_package,
          test_target(),
          *derived,
          published,
          draft::CompileTimeSynthesisMode::Reject,
          blocked_diagnostics);
  EXPECT(state, blocked.status == draft::CompileTimeProductStatus::Blocked);
  EXPECT(state,
      blocked.constant_dependencies == std::vector<draft::SymbolId>({*base}));
  EXPECT(state, blocked.type_dependencies.empty());
  EXPECT(state, !blocked_diagnostics.has_errors());

  draft::SemanticPackage base_package = source.analysis.package;
  draft::DiagnosticSink base_diagnostics;
  const draft::ConstantProductAttempt base_result =
      draft::evaluate_package_constant_product(
          source.sources,
          source.loaded,
          base_package,
          test_target(),
          *base,
          published,
          draft::CompileTimeSynthesisMode::Reject,
          base_diagnostics);
  EXPECT(state, base_result.status == draft::CompileTimeProductStatus::Complete);
  EXPECT(state, base_result.result.has_value());
  if (!base_result.result.has_value()) return;
  published.bindings.push_back({*base, base_result.result->value});

  draft::SemanticPackage ready_package = source.analysis.package;
  draft::DiagnosticSink ready_diagnostics;
  const draft::ConstantProductAttempt ready =
      draft::evaluate_package_constant_product(
          source.sources,
          source.loaded,
          ready_package,
          test_target(),
          *derived,
          published,
          draft::CompileTimeSynthesisMode::Reject,
          ready_diagnostics);
  EXPECT(state, ready.status == draft::CompileTimeProductStatus::Complete);
  EXPECT(state, ready.result.has_value());
  if (ready.result.has_value()) {
    EXPECT(state,
        ready.result->value.integer == draft::BigInteger::from_u64(42));
  }

  // Replace only the source type symbol with an incomplete nominal identity.
  // The constant producer must name that exact natural-layout facet rather
  // than diagnosing size_of or recursively trying to complete the type.
  draft::SemanticPackage layout_package = source.analysis.package;
  const draft::TypeId waiting_type = layout_package.types.begin_nominal(
      draft::TypeKind::Struct,
      "Waiting_Header",
      draft::SourceRange::invalid());
  layout_package.symbols.symbol_mut(*header).type = waiting_type;
  draft::DiagnosticSink layout_diagnostics;
  const draft::ConstantProductAttempt layout =
      draft::evaluate_package_constant_product(
          source.sources,
          source.loaded,
          layout_package,
          test_target(),
          *header_size,
          published,
          draft::CompileTimeSynthesisMode::Reject,
          layout_diagnostics);
  EXPECT(state, layout.status == draft::CompileTimeProductStatus::Blocked);
  EXPECT(state, layout.constant_dependencies.empty());
  EXPECT(state,
      layout.type_dependencies ==
          std::vector<draft::TypeFacetDependency>({
              {waiting_type, draft::TypeFacet::NaturalLayout},
          }));
  EXPECT(state, !layout_diagnostics.has_errors());
}

// One conditional producer must expose the same dependency vocabulary as a
// named constant without selecting or materializing its branch. This covers a
// local constant edge, an exact natural-layout facet, and a provider wait.
void test_single_conditional_product_dependencies(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Gate :: true
Header :: struct {
    tag: u8,
    value: u64,
}

when Gate {
    Enabled :: 1
}

when size_of(Header) == 16 {
    Sized :: 1
}
)draft");
  EXPECT(state, source.analysis.ok);
  const std::optional<draft::SymbolId> gate =
      find_symbol(source.analysis.package, "Gate");
  const std::optional<draft::SymbolId> header =
      find_symbol(source.analysis.package, "Header");
  EXPECT(state, gate.has_value());
  EXPECT(state, header.has_value());
  if (!gate || !header) return;

  std::vector<draft::SemanticSite> conditions;
  for (const draft::SemanticSite &site : source.analysis.package.sites) {
    if (site.kind == draft::SemanticSiteKind::ConditionalDeclaration) {
      conditions.push_back(site);
    }
  }
  EXPECT(state, conditions.size() == 2);
  if (conditions.size() != 2) return;

  draft::ConstantTable published;
  draft::SemanticPackage blocked_package = source.analysis.package;
  draft::DiagnosticSink blocked_diagnostics;
  const draft::ConditionalProductAttempt blocked =
      draft::evaluate_conditional_product(
          source.sources,
          source.loaded,
          blocked_package,
          test_target(),
          conditions[0],
          published,
          draft::CompileTimeSynthesisMode::Reject,
          blocked_diagnostics);
  EXPECT(state, blocked.status == draft::CompileTimeProductStatus::Blocked);
  EXPECT(state,
      blocked.constant_dependencies == std::vector<draft::SymbolId>({*gate}));
  EXPECT(state, blocked.type_dependencies.empty());
  EXPECT(state, !blocked_diagnostics.has_errors());

  const draft::ConstantValue *gate_value =
      source.analysis.constants.find(*gate);
  EXPECT(state, gate_value != nullptr);
  if (gate_value == nullptr) return;
  published.bindings.push_back({*gate, *gate_value});
  draft::SemanticPackage ready_package = source.analysis.package;
  draft::DiagnosticSink ready_diagnostics;
  const draft::ConditionalProductAttempt ready =
      draft::evaluate_conditional_product(
          source.sources,
          source.loaded,
          ready_package,
          test_target(),
          conditions[0],
          published,
          draft::CompileTimeSynthesisMode::Reject,
          ready_diagnostics);
  EXPECT(state, ready.status == draft::CompileTimeProductStatus::Complete);
  EXPECT(state, ready.selected_true);
  EXPECT(state, !ready_diagnostics.has_errors());

  draft::SemanticPackage layout_package = source.analysis.package;
  const draft::TypeId waiting_type = layout_package.types.begin_nominal(
      draft::TypeKind::Struct,
      "Waiting_Header",
      draft::SourceRange::invalid());
  layout_package.symbols.symbol_mut(*header).type = waiting_type;
  draft::DiagnosticSink layout_diagnostics;
  const draft::ConditionalProductAttempt layout =
      draft::evaluate_conditional_product(
          source.sources,
          source.loaded,
          layout_package,
          test_target(),
          conditions[1],
          published,
          draft::CompileTimeSynthesisMode::Reject,
          layout_diagnostics);
  EXPECT(state, layout.status == draft::CompileTimeProductStatus::Blocked);
  EXPECT(state, layout.constant_dependencies.empty());
  EXPECT(state,
      layout.type_dependencies ==
          std::vector<draft::TypeFacetDependency>({
              {waiting_type, draft::TypeFacet::NaturalLayout},
          }));
  EXPECT(state, !layout_diagnostics.has_errors());

  draft::SourceManager synthesis_sources;
  draft::DiagnosticSink synthesis_diagnostics;
  draft::LoadedPackage synthesis_loaded;
  synthesis_loaded.short_name = "conditions";
  draft::LoadedPackageFile synthesis_file;
  synthesis_file.kind = draft::PackageFileKind::DraftSource;
  synthesis_file.relative_name = "package.draft";
  synthesis_file.source = synthesis_sources.add_source(
      "package.draft",
      "package conditions\n"
      "when ... \"Select the true branch\" {\n"
      "    Selected :: 1\n"
      "}\n");
  synthesis_file.syntax.emplace(draft::parse_source_file(
      synthesis_sources,
      synthesis_file.source,
      synthesis_diagnostics));
  synthesis_loaded.files.push_back(std::move(synthesis_file));
  draft::AvailablePackageImports no_imports;
  draft::DiagnosticSink initial_diagnostics;
  const draft::PackageDeclarationDiscovery initial =
      draft::begin_package_declaration_discovery(
          synthesis_sources,
          synthesis_loaded,
          no_imports,
          initial_diagnostics);
  EXPECT(state, !initial.terminal);
  EXPECT(state, initial.discovery_ok);
  EXPECT(state, initial.package.conditional_declarations.size() == 1);
  EXPECT(
      state,
      !initial.package.symbols
           .lookup_direct(initial.package.package_scope, "Selected")
           .has_value());
  EXPECT(state, !initial_diagnostics.has_errors());

  draft::SourceManager ready_sources;
  draft::DiagnosticSink barrier_diagnostics;
  draft::LoadedPackage ready_loaded;
  ready_loaded.short_name = "ready";
  draft::LoadedPackageFile ready_file;
  ready_file.kind = draft::PackageFileKind::DraftSource;
  ready_file.relative_name = "package.draft";
  ready_file.source = ready_sources.add_source(
      "package.draft", "package ready\nAnswer :: 42\n");
  ready_file.syntax.emplace(draft::parse_source_file(
      ready_sources, ready_file.source, barrier_diagnostics));
  ready_loaded.files.push_back(std::move(ready_file));
  draft::PackageDeclarationDiscovery barrier_ready =
      draft::begin_package_declaration_discovery(
          ready_sources, ready_loaded, no_imports, barrier_diagnostics);
  EXPECT(
      state,
      draft::finish_package_declaration_discovery(
          barrier_ready, barrier_diagnostics));
  EXPECT(state, barrier_ready.terminal);
  EXPECT(state, !barrier_diagnostics.has_errors());

  // The condition task consumes the eager declaration-generation site directly.
  // Interface discovery must not run a separate aggregate semantic fixed point
  // merely to recreate this task input.
  const draft::SemanticSite *synthesis_condition = nullptr;
  for (const draft::SemanticSite &site : initial.package.sites) {
    if (site.kind == draft::SemanticSiteKind::ConditionalDeclaration) {
      synthesis_condition = &site;
      break;
    }
  }
  EXPECT(state, synthesis_condition != nullptr);
  if (synthesis_condition == nullptr) return;
  draft::SemanticPackage synthesis_package = initial.package;
  draft::DiagnosticSink task_diagnostics;
  const draft::ConditionalProductAttempt waiting =
      draft::evaluate_conditional_product(
          synthesis_sources,
          synthesis_loaded,
          synthesis_package,
          test_target(),
          *synthesis_condition,
          {},
          draft::CompileTimeSynthesisMode::Discover,
          task_diagnostics);
  EXPECT(state,
      waiting.status == draft::CompileTimeProductStatus::WaitingForSynthesis);
  EXPECT(state, !task_diagnostics.has_errors());
}

void test_constants_and_conditional_rounds(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Mode :: enum {
    Off,
    On,
}

Outcome :: variant {
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
Vector_Type :: simd[4]u32
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
  // The statement selection in target_word is provisional discovery evidence;
  // BodyChecker still evaluates it again at its lexical program point.
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

Computed_Vector :: simd[choose_size(true)]u32

Procedural_Vector[N: usize] :: struct {
    values: simd[plus_one(N)]u32,
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

// This value waits for Buffer's completed layout, while Buffer's own counts wait
// for procedure declarations and compile-time execution. It exercises a real
// multi-edge product chain rather than several independent calls that happen to
// finish together.
Measured :: struct {
    bytes: [size_of(Buffer)]u8,
}

when size_of(Measured) == 13 {
    Layout_Selected :: bool
} else {
    Wrong_Layout_Selected :: bool
}

Aligned :: align(choose_size(false)) struct {
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
  // that coincidence. Independently scheduled declaration products must retain
  // this boundary for both a generic body and each concrete application.
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

Bad_Value_Application :: struct {
    value: Sized[wrong_count()],
}

Bad_Generic_Application :: struct {
    generic: Bad_Generic[4],
}

Bad_Array :: struct {
    values: [wrong_count()]u8,
}

Bad_Vector :: simd[wrong_count()]u32

Bad_Aligned :: align(wrong_count()) struct {
    value: u8,
}
)draft");
  EXPECT(state, !invalid.analysis.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find(
                    "compile-time value argument has the wrong concrete integer type") !=
                    std::string::npos);
  bool saw_wrong_value_argument_range = false;
  for (const draft::Diagnostic &diagnostic :
       invalid.diagnostics.diagnostics()) {
    saw_wrong_value_argument_range = saw_wrong_value_argument_range ||
        (diagnostic.message ==
             "compile-time value argument has the wrong concrete integer type" &&
         invalid.sources.text(diagnostic.range) == "wrong_count()");
  }
  EXPECT(state, saw_wrong_value_argument_range);
  EXPECT(state, rendered.find("array length must have type 'usize'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("SIMD lane count must have type 'usize'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("'align' argument must have type 'usize'") !=
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

sum_bytes :: proc(value: string) -> usize {
    result: usize
    for byte, offset in value {
        result += cast[usize](byte) + offset
    }
    return result
}

sum_pairs :: proc() -> usize {
    pairs := [2](usize, bool){(10, true), (20, false)}
    result: usize
    for (value, _), index in pairs {
        result += value + index
    }
    return result
}

From_Procedure :: select_middle()
Byte_Sum :: sum_bytes("A\0\u{e9}")
Empty_Byte_Sum :: sum_bytes("")
Tuple_Iteration_Sum :: sum_pairs()
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

  const auto expect_integer = [&](std::string_view name,
                                  std::string_view expected) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(valid.analysis.package, name);
    EXPECT(state, symbol.has_value());
    const draft::ConstantValue *value = symbol.has_value()
        ? valid.analysis.constants.find(*symbol)
        : nullptr;
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Integer);
      EXPECT(state, value->integer.to_decimal() == expected);
    }
  };
  // The four bytes are 65, 0, 195, and 169. Their sum is 429 and the
  // zero-based byte offsets add another 6.
  expect_integer("Byte_Sum", "435");
  expect_integer("Empty_Byte_Sum", "0");
  expect_integer("Tuple_Iteration_Sum", "31");

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

// Structural type syntax can be evaluated inside a required compile-time
// procedure without first passing through the ordinary declaration type
// resolver. That evaluator path must enforce the same Draft 1 fixed-sequence
// count rules and retain the count expression as the diagnostic range.
void test_compile_time_structural_sequence_counts(TestState &state) {
  AnalyzedSource invalid(R"draft(
package conditions

zero_array :: proc() -> bool {
    return [0]u8 == [1]u8
}

typed_array_count :: proc() -> bool {
    return [cast[u64](2)]u8 == [2]u8
}

negative_array_count :: proc() -> bool {
    return [-1]u8 == [1]u8
}

zero_simd :: proc() -> bool {
    return simd[0]u32 == simd[4]u32
}

typed_simd_count :: proc() -> bool {
    return simd[cast[u64](4)]u32 == simd[4]u32
}

negative_simd_count :: proc() -> bool {
    return simd[-1]u32 == simd[4]u32
}

Zero_Array :: zero_array()
Typed_Array_Count :: typed_array_count()
Negative_Array_Count :: negative_array_count()
Zero_SIMD :: zero_simd()
Typed_SIMD_Count :: typed_simd_count()
Negative_SIMD_Count :: negative_simd_count()
)draft");
  EXPECT(state, !invalid.analysis.ok);

  bool saw_zero_array = false;
  bool saw_typed_array = false;
  bool saw_negative_array = false;
  bool saw_zero_simd = false;
  bool saw_typed_simd = false;
  bool saw_negative_simd = false;
  for (const draft::Diagnostic &diagnostic :
       invalid.diagnostics.diagnostics()) {
    const std::string_view spelling =
        invalid.sources.text(diagnostic.range);
    saw_zero_array = saw_zero_array ||
        (diagnostic.message ==
             "array length must be a nonzero compile-time usize" &&
         spelling == "0");
    saw_typed_array = saw_typed_array ||
        (diagnostic.message == "array length must have type 'usize'" &&
         spelling == "cast[u64](2)");
    saw_negative_array = saw_negative_array ||
        (diagnostic.message ==
             "array length must be a nonzero compile-time usize" &&
         spelling == "-1");
    saw_zero_simd = saw_zero_simd ||
        (diagnostic.message ==
             "SIMD lane count must be a nonzero compile-time usize" &&
         spelling == "0");
    saw_typed_simd = saw_typed_simd ||
        (diagnostic.message == "SIMD lane count must have type 'usize'" &&
         spelling == "cast[u64](4)");
    saw_negative_simd = saw_negative_simd ||
        (diagnostic.message ==
             "SIMD lane count must be a nonzero compile-time usize" &&
         spelling == "-1");
  }
  EXPECT(state, saw_zero_array);
  EXPECT(state, saw_typed_array);
  EXPECT(state, saw_negative_array);
  EXPECT(state, saw_zero_simd);
  EXPECT(state, saw_typed_simd);
  EXPECT(state, saw_negative_simd);
  EXPECT(state, invalid.diagnostics.error_count() == 6);
  if (!saw_zero_array || !saw_typed_array || !saw_negative_array ||
      !saw_zero_simd || !saw_typed_simd || !saw_negative_simd ||
      invalid.diagnostics.error_count() != 6) {
    std::cerr << draft::render_diagnostics(
        invalid.sources, invalid.diagnostics);
  }
}

void test_operator_type_boundaries(TestState &state) {
  AnalyzedSource valid(R"draft(
package conditions

Mode :: enum {
    Zero,
    One,
}

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
Inferred_Nil_Callback ::
    ((nil if true else nil)) if true else callback
Inferred_Mode :: (.One) if true else cast[Mode](cast[u8](0))
Short_Circuit_Division :: false && ((1 / 0) == 0)
Selected_Conditional :: 42 if true else (1 / 0)
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.analysis.ok);
  EXPECT(state, !valid.diagnostics.has_errors());
  EXPECT(state, valid.analysis.package.parametric_instances.empty());
  const std::optional<draft::SymbolId> inferred_nil =
      find_symbol(valid.analysis.package, "Inferred_Nil_Callback");
  const std::optional<draft::SymbolId> inferred_mode =
      find_symbol(valid.analysis.package, "Inferred_Mode");
  EXPECT(state, inferred_nil.has_value());
  EXPECT(state, inferred_mode.has_value());
  if (inferred_nil.has_value()) {
    const draft::Symbol &symbol =
        valid.analysis.package.symbols.symbol(*inferred_nil);
    EXPECT(state, symbol.type.is_valid());
    if (symbol.type.is_valid()) {
      EXPECT(state,
          valid.analysis.package.types.type(symbol.type).kind ==
              draft::TypeKind::Procedure);
    }
    const draft::ConstantValue *value =
        valid.analysis.constants.find(*inferred_nil);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Nil);
    }
  }
  if (inferred_mode.has_value()) {
    const draft::Symbol &symbol =
        valid.analysis.package.symbols.symbol(*inferred_mode);
    EXPECT(state, symbol.type.is_valid());
    if (symbol.type.is_valid()) {
      EXPECT(state,
          valid.analysis.package.types.type(symbol.type).kind ==
              draft::TypeKind::Enum);
    }
    const draft::ConstantValue *value =
        valid.analysis.constants.find(*inferred_mode);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::EnumLabel);
    }
  }

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
  EXPECT(state, rendered.find("does not match expected type") !=
                    std::string::npos);
}

// Constant evaluation is a semantic path independent of runtime HIR checking.
// Keep the contextual rune-literal contract covered here as well so package
// constants, compile-time calls and switches, and global initializers cannot
// drift from the ordinary expression checker.
void test_contextual_rune_literals(TestState &state) {
  AnalyzedSource valid(R"draft(
package conditions

Byte_Code :: distinct u8

accept_byte :: proc(value: u8) -> u8 {
    return value
}

classify_byte :: proc(value: u8) -> bool {
    switch value {
    case 'q':
        return true
    case:
        return false
    }
}

Byte_Result :: accept_byte('q')
Left_Comparison :: Byte_Result == 'q'
Right_Comparison :: 'q' == Byte_Result
Conditional_Left :: 'l' if true else Byte_Result
Conditional_Right :: Byte_Result if false else 'r'
Selected :: classify_byte('q')
Rune_Default :: 'é'
Explicit_Byte :: cast[u8](Rune_Default)

byte: u8 = 'q'
distinct_byte: Byte_Code = 'x'
bytes: [2]u8 = [2]u8{'a', 'b'}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.analysis.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  const auto expect_type = [&](std::string_view name, draft::TypeId type) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(valid.analysis.package, name);
    EXPECT(state, symbol.has_value());
    if (symbol.has_value()) {
      EXPECT(state, valid.analysis.package.symbols.symbol(*symbol).type == type);
    }
  };
  expect_type("Byte_Result", valid.analysis.package.types.builtins().u8_type);
  expect_type(
      "Conditional_Left", valid.analysis.package.types.builtins().u8_type);
  expect_type(
      "Conditional_Right", valid.analysis.package.types.builtins().u8_type);
  expect_type(
      "Rune_Default", valid.analysis.package.types.builtins().rune_type);
  expect_type(
      "Explicit_Byte", valid.analysis.package.types.builtins().u8_type);

  for (std::string_view name :
       {"Left_Comparison", "Right_Comparison", "Selected"}) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(valid.analysis.package, name);
    EXPECT(state, symbol.has_value());
    const draft::ConstantValue *value = symbol.has_value()
        ? valid.analysis.constants.find(*symbol)
        : nullptr;
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Bool);
      EXPECT(state, value->boolean);
    }
  }

  AnalyzedSource invalid(R"draft(
package conditions

accept_byte :: proc(value: u8) -> u8 {
    return value
}

Byte_Value :: accept_byte('q')
Rune_Value :: 'q'

Bad_Concrete_Rune_Comparison :: Byte_Value == Rune_Value
Bad_Named_Rune_Argument :: accept_byte(Rune_Value)
Bad_Rune_Arithmetic :: 'a' + 'b'
Bad_Float_Context :: cast[f32](1) == 'q'
too_large: u8 = '😀'
wrong_named_rune: u8 = Rune_Value
wrong_float_literal: f32 = 'q'
)draft");
  EXPECT(state, !invalid.analysis.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 7);

  std::size_t named_rune_context_errors = 0;
  bool saw_arithmetic = false;
  std::size_t float_context_errors = 0;
  bool saw_range = false;
  for (const draft::Diagnostic &diagnostic : invalid.diagnostics.diagnostics()) {
    if (diagnostic.severity != draft::DiagnosticSeverity::Error) continue;
    EXPECT(state, diagnostic.range.is_valid());
    const std::string spelling(
        invalid.sources.text(diagnostic.range));
    if (diagnostic.message.find("value of type 'rune'") !=
            std::string::npos &&
        spelling == "Rune_Value") {
      ++named_rune_context_errors;
    }
    saw_arithmetic = saw_arithmetic ||
        (diagnostic.message ==
             "compile-time operator is not defined for operand types" &&
         spelling == "'a' + 'b'");
    if (diagnostic.message.find("value of type 'rune'") !=
            std::string::npos &&
        spelling == "'q'") {
      ++float_context_errors;
    }
    saw_range = saw_range ||
        (diagnostic.message.find("not representable") != std::string::npos &&
         spelling == "'😀'");
  }
  EXPECT(state, named_rune_context_errors == 3);
  EXPECT(state, saw_arithmetic);
  EXPECT(state, float_context_errors == 2);
  EXPECT(state, saw_range);
  if (named_rune_context_errors != 3 || !saw_arithmetic ||
      float_context_errors != 2 || !saw_range ||
      invalid.diagnostics.error_count() != 7) {
    std::cerr << draft::render_diagnostics(
        invalid.sources, invalid.diagnostics);
  }
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

Outcome :: variant {
    empty,
    value: u32,
}

Float_Outcome :: variant {
    empty,
    value: f32,
}

Overlay :: union {
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
tuple_outcomes: (Outcome, Mode) = (.value(11), .On)
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
  const std::optional<draft::SymbolId> tuple_outcomes =
      find_symbol(source.analysis.package, "tuple_outcomes");
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
  EXPECT(state, tuple_outcomes.has_value());
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
      EXPECT(state, value->member_index == 1);
      EXPECT(state, value->elements.size() == 1);
    }
  }
  if (tuple_outcomes.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*tuple_outcomes);
    EXPECT(state, value != nullptr);
    if (value != nullptr && value->elements.size() == 2) {
      EXPECT(state, value->elements[0].kind == draft::ConstantKind::Aggregate);
      EXPECT(state, value->elements[0].member_index == 1);
      EXPECT(state, value->elements[0].elements.size() == 1);
      EXPECT(state, value->elements[1].kind == draft::ConstantKind::Integer);
      EXPECT(state, value->elements[1].integer.to_decimal() == "7");
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
      EXPECT(state, value->member_index == 1);
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

Overlay :: union {
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
  EXPECT(state, rendered.find("union composite element must name a field") !=
                    std::string::npos);
  EXPECT(state, rendered.find("logical operators require matching bool operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find("does not match expected type") !=
                    std::string::npos);
}

void test_global_type_value_storage_is_rejected(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

inferred := u32
explicit: type = u64
aggregate := (u16, true)
)draft");
  EXPECT(state, !source.analysis.ok);
  for (const draft::Diagnostic &diagnostic : source.diagnostics.diagnostics()) {
    if (diagnostic.severity == draft::DiagnosticSeverity::Error) {
      EXPECT(state, diagnostic.range.is_valid());
    }
  }
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find(
      "global storage cannot contain compile-time 'type' values") !=
      std::string::npos);
}

void test_constant_dependency_chain_is_product_scheduled(TestState &state) {
  std::string text = "package conditions\n\n";
  constexpr std::size_t constant_count = 320;
  for (std::size_t index = 0; index < constant_count; ++index) {
    // The binary expression deliberately bypasses the type resolver's
    // name-only declaration-disambiguation chain. This reaches the constant
    // evaluator's independent dependency graph instead.
    text += "Constant_" + std::to_string(index) + " :: Constant_" +
        std::to_string(index + 1) + " + 0\n";
  }
  text += "Constant_" + std::to_string(constant_count) + " :: 42\n";
  AnalyzedSource source(std::move(text));

  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  const std::optional<draft::SymbolId> first =
      find_symbol(source.analysis.package, "Constant_0");
  EXPECT(state, first.has_value());
  const draft::ConstantValue *value = first.has_value()
      ? source.analysis.constants.find(*first)
      : nullptr;
  EXPECT(state, value != nullptr);
  if (value != nullptr) {
    EXPECT(state, value->kind == draft::ConstantKind::Integer);
    EXPECT(state, value->integer.to_decimal() == "42");
  }
}

void test_type_values_and_structural_queries(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

runtime_storage: int

runtime_only :: proc() -> int {
    return runtime_storage
}

runtime_text :: proc() -> string {
    return "runtime text"
}

Observed_Type :: type_of(runtime_only())
Raw_Data_Type :: type_of(raw_data(runtime_text()))
Sliced_Raw_Data_Type :: type_of(raw_data("abc"[1:]))
Length_Type :: type_of(len(runtime_text()))
Feature_Type :: type_of((target).has_feature("neon"))
Target_OS_Type_Value :: type_of(target.os)
Integer_Type_Value :: type_of(1)
Callback :: proc(value: i32) -> i32
Vector :: simd[4]u32
callback_identity :: proc(value: i32) -> i32 {
    return value
}
Target_MacOS :: cast[Target_Operating_System](cast[u8](0))
Target_Linux :: cast[Target_Operating_System](cast[u8](1))
Target_Windows :: cast[Target_Operating_System](cast[u8](2))
Target_Linux_Through_Type_Value :: cast[Target_OS_Type_Value](cast[u8](1))
Integer_Through_Type_Value :: cast[Integer_Type_Value](42)
Element_Type :: type_element(^u64)
Meta_Type :: type_of(u64)
Array_Name :: type_name([4]u8)
Array_Count :: type_element_count([4]u8)

// A structural type constructor is the type value itself, not runtime syntax.
// Keep this condition direct so the test exercises both constant evaluation
// and the selected-condition type-validation preflight.
when type_of(raw_data(runtime_text())) == [^]u8 && []u8 != [^]u8 {
    Exact_Structural_Type_Selected :: true
}

when type_of(callback_identity) == proc(value: i32) -> i32 &&
     Callback == proc(value: i32) -> i32 &&
     Vector == simd[4]u32 {
    Exact_Procedure_And_SIMD_Selected :: true
}

when .macos == target.os && .aarch64 == target.arch {
    Reverse_Target_Alternatives_Selected :: true
}

when type_kind(Element_Type) == .unsigned_integer {
    Selected :: 1
} else {
    Selected :: Missing
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const std::optional<draft::SymbolId> observed =
      find_symbol(source.analysis.package, "Observed_Type");
  const std::optional<draft::SymbolId> raw_data =
      find_symbol(source.analysis.package, "Raw_Data_Type");
  const std::optional<draft::SymbolId> sliced_raw_data =
      find_symbol(source.analysis.package, "Sliced_Raw_Data_Type");
  const std::optional<draft::SymbolId> length =
      find_symbol(source.analysis.package, "Length_Type");
  const std::optional<draft::SymbolId> feature_type =
      find_symbol(source.analysis.package, "Feature_Type");
  const std::optional<draft::SymbolId> target_macos =
      find_symbol(source.analysis.package, "Target_MacOS");
  const std::optional<draft::SymbolId> target_linux =
      find_symbol(source.analysis.package, "Target_Linux");
  const std::optional<draft::SymbolId> target_linux_through_type_value =
      find_symbol(source.analysis.package, "Target_Linux_Through_Type_Value");
  const std::optional<draft::SymbolId> integer_through_type_value =
      find_symbol(source.analysis.package, "Integer_Through_Type_Value");
  const std::optional<draft::SymbolId> element =
      find_symbol(source.analysis.package, "Element_Type");
  const std::optional<draft::SymbolId> meta =
      find_symbol(source.analysis.package, "Meta_Type");
  const std::optional<draft::SymbolId> name =
      find_symbol(source.analysis.package, "Array_Name");
  const std::optional<draft::SymbolId> count =
      find_symbol(source.analysis.package, "Array_Count");
  const std::optional<draft::SymbolId> selected =
      find_symbol(source.analysis.package, "Selected");
  const std::optional<draft::SymbolId> exact_structural_type_selected =
      find_symbol(source.analysis.package, "Exact_Structural_Type_Selected");
  const std::optional<draft::SymbolId> exact_procedure_and_simd_selected =
      find_symbol(source.analysis.package, "Exact_Procedure_And_SIMD_Selected");
  const std::optional<draft::SymbolId> reverse_target_alternatives_selected =
      find_symbol(source.analysis.package, "Reverse_Target_Alternatives_Selected");
  EXPECT(state, observed.has_value());
  EXPECT(state, raw_data.has_value());
  EXPECT(state, sliced_raw_data.has_value());
  EXPECT(state, length.has_value());
  EXPECT(state, feature_type.has_value());
  EXPECT(state, target_macos.has_value());
  EXPECT(state, target_linux.has_value());
  EXPECT(state, target_linux_through_type_value.has_value());
  EXPECT(state, integer_through_type_value.has_value());
  EXPECT(state, element.has_value());
  EXPECT(state, meta.has_value());
  EXPECT(state, name.has_value());
  EXPECT(state, count.has_value());
  EXPECT(state, selected.has_value());
  EXPECT(state, exact_structural_type_selected.has_value());
  EXPECT(state, exact_procedure_and_simd_selected.has_value());
  EXPECT(state, reverse_target_alternatives_selected.has_value());
  if (!observed.has_value() || !raw_data.has_value() ||
      !sliced_raw_data.has_value() || !length.has_value() ||
      !feature_type.has_value() || !target_macos.has_value() ||
      !target_linux.has_value() ||
      !target_linux_through_type_value.has_value() ||
      !integer_through_type_value.has_value() || !element.has_value() ||
      !meta.has_value() || !name.has_value() ||
      !count.has_value() || !selected.has_value() ||
      !exact_structural_type_selected.has_value() ||
      !exact_procedure_and_simd_selected.has_value() ||
      !reverse_target_alternatives_selected.has_value()) {
    return;
  }

  const draft::ConstantValue *observed_value =
      source.analysis.constants.find(*observed);
  const draft::ConstantValue *raw_data_value =
      source.analysis.constants.find(*raw_data);
  const draft::ConstantValue *sliced_raw_data_value =
      source.analysis.constants.find(*sliced_raw_data);
  const draft::ConstantValue *length_value =
      source.analysis.constants.find(*length);
  const draft::ConstantValue *feature_type_value =
      source.analysis.constants.find(*feature_type);
  const draft::ConstantValue *target_macos_value =
      source.analysis.constants.find(*target_macos);
  const draft::ConstantValue *target_linux_value =
      source.analysis.constants.find(*target_linux);
  const draft::ConstantValue *target_linux_through_type_value_value =
      source.analysis.constants.find(*target_linux_through_type_value);
  const draft::ConstantValue *integer_through_type_value_value =
      source.analysis.constants.find(*integer_through_type_value);
  const draft::ConstantValue *element_value =
      source.analysis.constants.find(*element);
  const draft::ConstantValue *meta_value = source.analysis.constants.find(*meta);
  const draft::ConstantValue *name_value = source.analysis.constants.find(*name);
  const draft::ConstantValue *count_value = source.analysis.constants.find(*count);
  const draft::ConstantValue *selected_value =
      source.analysis.constants.find(*selected);
  EXPECT(state, observed_value != nullptr);
  EXPECT(state, raw_data_value != nullptr);
  EXPECT(state, sliced_raw_data_value != nullptr);
  EXPECT(state, length_value != nullptr);
  EXPECT(state, feature_type_value != nullptr);
  EXPECT(state, target_macos_value != nullptr);
  EXPECT(state, target_linux_value != nullptr);
  EXPECT(state, target_linux_through_type_value_value != nullptr);
  EXPECT(state, integer_through_type_value_value != nullptr);
  EXPECT(state, element_value != nullptr);
  EXPECT(state, meta_value != nullptr);
  EXPECT(state, name_value != nullptr);
  EXPECT(state, count_value != nullptr);
  EXPECT(state, selected_value != nullptr);
  if (observed_value == nullptr || raw_data_value == nullptr ||
      sliced_raw_data_value == nullptr || length_value == nullptr ||
      feature_type_value == nullptr || target_macos_value == nullptr ||
      target_linux_value == nullptr ||
      target_linux_through_type_value_value == nullptr ||
      integer_through_type_value_value == nullptr || element_value == nullptr ||
      meta_value == nullptr || name_value == nullptr ||
      count_value == nullptr || selected_value == nullptr) {
    return;
  }

  EXPECT(state, observed_value->kind == draft::ConstantKind::Type);
  EXPECT(state, raw_data_value->kind == draft::ConstantKind::Type);
  EXPECT(state, sliced_raw_data_value->kind == draft::ConstantKind::Type);
  EXPECT(state, length_value->kind == draft::ConstantKind::Type);
  EXPECT(state, feature_type_value->kind == draft::ConstantKind::Type);
  EXPECT(state, element_value->kind == draft::ConstantKind::Type);
  EXPECT(state, meta_value->kind == draft::ConstantKind::Type);
  if (observed_value->kind == draft::ConstantKind::Type) {
    EXPECT(state,
        draft::type_kind_name(source.analysis.package.types.type(
            draft::TypeId{observed_value->type_index}).kind) ==
            "signed integer");
  }
  if (raw_data_value->kind == draft::ConstantKind::Type) {
    const draft::Type &raw_data_type = source.analysis.package.types.type(
        draft::TypeId{raw_data_value->type_index});
    EXPECT(state, raw_data_type.kind == draft::TypeKind::MultiPointer);
    EXPECT(state,
        raw_data_type.element ==
            source.analysis.package.types.builtins().u8_type);
  }
  if (sliced_raw_data_value->kind == draft::ConstantKind::Type) {
    const draft::Type &raw_data_type = source.analysis.package.types.type(
        draft::TypeId{sliced_raw_data_value->type_index});
    EXPECT(state, raw_data_type.kind == draft::TypeKind::MultiPointer);
    EXPECT(state,
        raw_data_type.element ==
            source.analysis.package.types.builtins().u8_type);
  }
  if (length_value->kind == draft::ConstantKind::Type) {
    EXPECT(state,
        draft::TypeId{length_value->type_index} ==
            source.analysis.package.types.builtins().usize_type);
  }
  if (feature_type_value->kind == draft::ConstantKind::Type) {
    EXPECT(state,
        draft::TypeId{feature_type_value->type_index} ==
            source.analysis.package.types.builtins().bool_type);
  }
  EXPECT(state, target_macos_value->kind == draft::ConstantKind::Integer);
  EXPECT(state, target_macos_value->integer.to_decimal() == "0");
  EXPECT(state, target_linux_value->kind == draft::ConstantKind::Integer);
  EXPECT(state, target_linux_value->integer.to_decimal() == "1");
  EXPECT(state,
      target_linux_through_type_value_value->kind ==
          draft::ConstantKind::Integer);
  EXPECT(state,
      target_linux_through_type_value_value->integer.to_decimal() == "1");
  EXPECT(state,
      integer_through_type_value_value->kind == draft::ConstantKind::Integer);
  EXPECT(state,
      integer_through_type_value_value->integer.to_decimal() == "42");
  if (element_value->kind == draft::ConstantKind::Type) {
    EXPECT(state,
        source.analysis.package.types.type(
            draft::TypeId{element_value->type_index}).name == "u64");
  }
  if (meta_value->kind == draft::ConstantKind::Type) {
    EXPECT(state,
        draft::TypeId{meta_value->type_index} ==
            source.analysis.package.types.builtins().meta_type);
  }
  EXPECT(state, name_value->kind == draft::ConstantKind::String);
  EXPECT(state, name_value->text == "[4]u8");
  EXPECT(state, count_value->kind == draft::ConstantKind::Integer);
  EXPECT(state, count_value->integer.to_decimal() == "4");
  EXPECT(state, selected_value->kind == draft::ConstantKind::Integer);
  EXPECT(state, selected_value->integer.to_decimal() == "1");

  AnalyzedSource invalid(R"draft(
package conditions

Bad_Element :: type_element(bool)
Bad_Index :: type_member_type(Type_Kind, 100)
Bad_Raw_Data_Type :: type_of(raw_data(42))
Bad_Feature_Missing :: type_of(target.has_feature())
Bad_Feature_Extra :: type_of(target.has_feature("neon", "extra"))
Bad_Feature_Argument :: type_of(target.has_feature(42))
Bad_Target_OS_Value :: cast[Target_Operating_System](cast[u8](3))

runtime_text_with_value :: proc(value: int) -> string {
    return "runtime text"
}

Bad_Raw_Data_Call_Type :: type_of(raw_data(runtime_text_with_value()))
)draft");
  EXPECT(state, !invalid.analysis.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 8);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("type_element requires a pointer") !=
      std::string::npos);
  EXPECT(state, rendered.find("type_member_type index is out of bounds") !=
      std::string::npos);
  EXPECT(state, rendered.find("raw_data requires a string argument") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "procedure call is missing required argument 'value'") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "target.has_feature requires exactly one argument") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "target.has_feature requires a compile-time string") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "compile-time cast does not name an enum member") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "type_of requires an expression with a known static type") ==
      std::string::npos);
}

// Constant selection may learn type_of's result without evaluating its
// operand. Selected package/member conditions must still pass through the one
// ordinary expression checker before their declarations are trusted.
void test_selected_when_condition_type_validation(TestState &state) {
  AnalyzedSource valid(R"draft(
package conditions

Target_OS_Type :: type_of(target.os)
Target_OS_Name :: type_name(Target_OS_Type)
Target_OS_Member_Count :: type_member_count(Target_OS_Type)
Target_OS_First_Member :: type_member_name(Target_OS_Type, 0)
Target_OS_First_Value :: type_member_value(Target_OS_Type, 0)
Target_Arch_Type :: type_of(target.arch)
Target_ABI_Type :: type_of(target.abi)
Target_Byte_Order_Type :: type_of(target.byte_order)
Target_Object_Format_Type :: type_of(target.object_format)
Target_Pointer_Bits_Type :: type_of(target.pointer_bits)
Target_Page_Size_Type :: type_of(target.page_size)

when (target).os == .macos &&
     (target).has_feature("neon") &&
     target.os == (.macos) &&
     target.os == target.os &&
     type_kind(type_of(raw_data(target.identity))) == .multi_pointer {
    Target_Selected :: true
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.analysis.ok);
  EXPECT(state, !valid.diagnostics.has_errors());
  EXPECT(state,
      valid.analysis.package.symbols.lookup(
          valid.analysis.package.package_scope,
          "Target_Selected").has_value());
  const auto expect_type_constant =
      [&](std::string_view name, draft::TypeId expected) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(valid.analysis.package, name);
    EXPECT(state, symbol.has_value());
    if (!symbol.has_value()) return;
    const draft::ConstantValue *value =
        valid.analysis.constants.find(*symbol);
    EXPECT(state, value != nullptr);
    if (value == nullptr) return;
    EXPECT(state, value->kind == draft::ConstantKind::Type);
    EXPECT(state, value->type_index == expected.value);
  };
  const draft::BuiltinTypes &builtins = valid.analysis.package.types.builtins();
  expect_type_constant("Target_OS_Type", builtins.target_operating_system_type);
  expect_type_constant("Target_Arch_Type", builtins.target_architecture_type);
  expect_type_constant("Target_ABI_Type", builtins.target_abi_type);
  expect_type_constant("Target_Byte_Order_Type", builtins.target_byte_order_type);
  expect_type_constant(
      "Target_Object_Format_Type", builtins.target_object_format_type);
  expect_type_constant("Target_Pointer_Bits_Type", builtins.uint_type);
  expect_type_constant("Target_Page_Size_Type", builtins.usize_type);
  const std::optional<draft::SymbolId> target_os_name =
      find_symbol(valid.analysis.package, "Target_OS_Name");
  EXPECT(state, target_os_name.has_value());
  if (target_os_name.has_value()) {
    const draft::ConstantValue *value =
        valid.analysis.constants.find(*target_os_name);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::String);
      EXPECT(state, value->text == "Target_Operating_System");
    }
  }
  const auto expect_integer_constant =
      [&valid, &state](std::string_view name, std::string_view expected) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(valid.analysis.package, name);
    EXPECT(state, symbol.has_value());
    if (!symbol.has_value()) return;
    const draft::ConstantValue *value =
        valid.analysis.constants.find(*symbol);
    EXPECT(state, value != nullptr);
    if (value == nullptr) return;
    EXPECT(state, value->kind == draft::ConstantKind::Integer);
    EXPECT(state, value->integer.to_decimal() == expected);
  };
  expect_integer_constant("Target_OS_Member_Count", "3");
  expect_integer_constant("Target_OS_First_Value", "0");
  const std::optional<draft::SymbolId> target_os_first =
      find_symbol(valid.analysis.package, "Target_OS_First_Member");
  EXPECT(state, target_os_first.has_value());
  if (target_os_first.has_value()) {
    const draft::ConstantValue *value =
        valid.analysis.constants.find(*target_os_first);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::String);
      EXPECT(state, value->text == "macos");
    }
  }

  AnalyzedSource invalid(R"draft(
package conditions

runtime_text_with_value :: proc(value: int) -> string {
    return "runtime text"
}

when type_kind(type_of(raw_data(42))) == .multi_pointer {
    Package_Selected :: true
}

Broken_Member :: struct {
    when type_kind(type_of(raw_data(runtime_text_with_value()))) == .multi_pointer {
        selected: bool,
    }
}

when target.os != .macos &&
     type_kind(type_of(raw_data(false))) == .multi_pointer {
    Short_Circuited :: true
}

when false && target.has_feature("invented-feature") &&
     type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    Invalid_Feature :: true
}

when target.os == target.arch {
    Mismatched_Target_Types :: true
}

when target.os == .neon {
    Unknown_Target_Alternative :: true
}

when .macos == target.arch {
    Reverse_Mismatched_Target_Alternative :: true
}

when .neon == target.os {
    Reverse_Unknown_Target_Alternative :: true
}

when false && (target).has_feature() &&
     type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    Missing_Feature_Argument :: true
}

when false && target.has_feature("neon", "extra") &&
     type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    Extra_Feature_Argument :: true
}

when false && target.has_feature(42) &&
     type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    Invalid_Feature_Argument :: true
}
)draft");
  EXPECT(state, !invalid.analysis.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 11);
  if (invalid.diagnostics.error_count() != 11) {
    std::cerr << draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  }

  bool saw_wrong_type = false;
  bool saw_missing_argument = false;
  bool saw_short_circuited_wrong_type = false;
  bool saw_unknown_feature = false;
  bool saw_mismatched_target_types = false;
  bool saw_unknown_target_alternative = false;
  std::size_t unknown_target_alternative_errors = 0;
  std::size_t feature_arity_errors = 0;
  bool saw_invalid_feature_argument = false;
  for (const draft::Diagnostic &diagnostic :
       invalid.diagnostics.diagnostics()) {
    const std::string_view spelling = invalid.sources.text(diagnostic.range);
    saw_wrong_type = saw_wrong_type ||
        (diagnostic.message == "raw_data requires a string argument" &&
         spelling == "42");
    saw_missing_argument = saw_missing_argument ||
        (diagnostic.message ==
             "procedure call is missing required argument 'value'" &&
         spelling == "runtime_text_with_value()");
    saw_short_circuited_wrong_type = saw_short_circuited_wrong_type ||
        (diagnostic.message == "raw_data requires a string argument" &&
         spelling == "false");
    saw_unknown_feature = saw_unknown_feature ||
        (diagnostic.message ==
             "unrecognized target feature 'invented-feature'" &&
         spelling == "\"invented-feature\"");
    saw_mismatched_target_types = saw_mismatched_target_types ||
        diagnostic.message ==
            "compile-time operator is not defined for operand types";
    saw_unknown_target_alternative = saw_unknown_target_alternative ||
        diagnostic.message ==
            "compile-time enum initializer names no matching member";
    if (diagnostic.message ==
            "compile-time enum initializer names no matching member" &&
        (spelling == ".macos" || spelling == ".neon")) {
      ++unknown_target_alternative_errors;
    }
    if (diagnostic.message ==
        "target.has_feature requires exactly one argument") {
      ++feature_arity_errors;
    }
    saw_invalid_feature_argument = saw_invalid_feature_argument ||
        (diagnostic.message ==
             "target.has_feature requires a compile-time string" &&
         spelling == "42");
  }
  EXPECT(state, saw_wrong_type);
  EXPECT(state, saw_missing_argument);
  EXPECT(state, saw_short_circuited_wrong_type);
  EXPECT(state, saw_unknown_feature);
  EXPECT(state, saw_mismatched_target_types);
  EXPECT(state, saw_unknown_target_alternative);
  EXPECT(state, unknown_target_alternative_errors == 3);
  EXPECT(state, feature_arity_errors == 2);
  EXPECT(state, saw_invalid_feature_argument);
  if (!saw_mismatched_target_types || !saw_unknown_target_alternative ||
      unknown_target_alternative_errors != 3 ||
      feature_arity_errors != 2 || !saw_invalid_feature_argument) {
    std::cerr << draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  }
}

// A package constant may call a procedure before the later interface barrier
// has canonicalized its defaults. The evaluator therefore resolves an omitted
// source default lazily in declaration scope while preserving named binding.
void test_compile_time_named_and_default_arguments(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Offset :: 20

sum_three :: proc(first: i64, second: i64 = Offset, third: i64 = 30) -> i64 {
    return first + second + third
}

Forward_Result :: forward_default()
Later :: 7
forward_default :: proc(value: i64 = Later) -> i64 {
    return value
}

Named_Result :: sum_three(third = 3, first = 1)
Positional_Result :: sum_three(2)
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const auto expect_integer = [&](std::string_view name,
                                  std::string_view expected) {
    const std::optional<draft::SymbolId> symbol =
        find_symbol(source.analysis.package, name);
    EXPECT(state, symbol.has_value());
    if (!symbol.has_value()) return;
    const draft::ConstantValue *value = source.analysis.constants.find(*symbol);
    EXPECT(state, value != nullptr);
    if (value != nullptr) {
      EXPECT(state, value->kind == draft::ConstantKind::Integer);
      EXPECT(state, value->integer.to_decimal() == expected);
    }
  };
  expect_integer("Forward_Result", "7");
  expect_integer("Named_Result", "24");
  expect_integer("Positional_Result", "52");

  AnalyzedSource cycle(R"draft(
package conditions

left :: proc(value: i64 = right()) -> i64 {
    return value
}

right :: proc(value: i64 = left()) -> i64 {
    return value
}

Result :: left()
)draft");
  EXPECT(state, !cycle.analysis.ok);
  const std::string cycle_rendered =
      draft::render_diagnostics(cycle.sources, cycle.diagnostics);
  EXPECT(state, cycle_rendered.find(
      "procedure default arguments form a compile-time cycle") !=
      std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_append_only_constant_overlay(state);
  test_compile_time_bit_field_storage(state);
  test_single_constant_product_dependencies(state);
  test_single_conditional_product_dependencies(state);
  test_constants_and_conditional_rounds(state);
  test_invalid_required_constants(state);
  test_invalid_procedural_constants(state);
  test_compile_time_defer(state);
  test_compile_time_callee_expressions(state);
  test_procedural_layout_constants(state);
  test_compile_time_string_views(state);
  test_compile_time_structural_sequence_counts(state);
  test_operator_type_boundaries(state);
  test_contextual_rune_literals(state);
  test_global_initializers(state);
  test_global_type_value_storage_is_rejected(state);
  test_constant_dependency_chain_is_product_scheduled(state);
  test_type_values_and_structural_queries(state);
  test_selected_when_condition_type_validation(state);
  test_compile_time_named_and_default_arguments(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " constant evaluation expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all constant evaluation tests passed\n";
  return EXIT_SUCCESS;
}
