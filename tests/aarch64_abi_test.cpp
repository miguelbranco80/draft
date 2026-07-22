// Darwin and GNU AArch64 C ABI classification tests.

#include "interop/aarch64_abi.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "target/profile.h"
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
      std::cerr << "aarch64_abi_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct SemanticSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticAnalysisResult semantics;

  explicit SemanticSource(std::string text) {
    loaded.short_name = "abi";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    const draft::TargetProfile target = draft::make_aarch64_macos_profile();
    semantics = draft::analyze_package_semantics(
        sources, loaded, target.facts, diagnostics);
  }

  [[nodiscard]] draft::TypeId type(std::string_view name) const {
    const std::optional<draft::SymbolId> symbol =
        semantics.package.symbols.lookup_direct(
            semantics.package.package_scope, name);
    return symbol.has_value()
        ? semantics.package.symbols.symbol(*symbol).type
        : draft::TypeId{};
  }
};

void test_darwin_arm64_classes(TestState &state) {
  SemanticSource source(R"draft(
package abi

C1 :: c struct { bytes: [1]u8, }
C3 :: c struct { bytes: [3]u8, }
C5 :: c struct { bytes: [5]u8, }
C9 :: c struct { bytes: [9]u8, }
C16 :: c struct { words: [2]i64, }
C16_Aligned :: c align(16) struct { word: i64, }
C24 :: c struct { words: [3]i64, }
HF2 :: c struct { first: f32, second: f32, }
HH3 :: c struct { values: [3]f16, }
HD4 :: c struct { values: [4]f64, }
HD5 :: c struct { values: [5]f64, }
HF2_Aligned :: c align(16) struct { first: f32, second: f32, }
Float_Union :: c raw union { first: f32, second: f32, }
Unequal_Float_Union :: c raw union {
    scalar: f32,
    pair: [2]f32,
}
Nested_Union_HF4 :: c struct {
    union_value: Unequal_Float_Union,
    pair: [2]f32,
}
Mixed_Union :: c raw union { first: f32, second: f64, }
Default_Record :: struct { value: i64, }
Bad_Member :: c struct { value: []u8, }
C_Enum :: c enum { off, on, }
Bad_Callback :: c proc(value: []u8)
Recursive_Callback_Record :: c struct {
    callback: c proc(value: Recursive_Callback_Record),
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const auto classify = [&source](std::string_view name) {
    return draft::classify_aarch64_c_type(
        source.semantics.package.types,
        source.type(name),
        draft::make_aarch64_macos_profile().facts);
  };
  const draft::Aarch64CAbiType c1 = classify("C1");
  EXPECT(state, c1.classification == draft::Aarch64CAbiClass::SmallAggregate);
  EXPECT(state, c1.argument_integer_bits == 64);
  EXPECT(state, c1.result_integer_bits == 8);
  EXPECT(state, classify("C3").result_integer_bits == 24);
  EXPECT(state, classify("C5").result_integer_bits == 40);

  const draft::Aarch64CAbiType c9 = classify("C9");
  EXPECT(state, c9.argument_integer_bits == 64);
  EXPECT(state, c9.argument_integer_count == 2);
  EXPECT(state, c9.result_integer_count == 2);
  EXPECT(state, classify("C16").argument_integer_count == 2);
  const draft::Aarch64CAbiType aligned = classify("C16_Aligned");
  EXPECT(state, aligned.argument_integer_bits == 128);
  EXPECT(state, aligned.argument_integer_count == 1);
  EXPECT(state, classify("C24").classification ==
                    draft::Aarch64CAbiClass::Indirect);

  const draft::Aarch64CAbiType hf2 = classify("HF2");
  EXPECT(state, hf2.classification ==
                    draft::Aarch64CAbiClass::HomogeneousFloatAggregate);
  EXPECT(state, hf2.homogeneous_element_bits == 32);
  EXPECT(state, hf2.homogeneous_element_count == 2);
  const draft::Aarch64CAbiType hh3 = classify("HH3");
  EXPECT(state, hh3.classification ==
                    draft::Aarch64CAbiClass::HomogeneousFloatAggregate);
  EXPECT(state, hh3.homogeneous_element_bits == 16);
  EXPECT(state, hh3.homogeneous_element_count == 3);
  const draft::Aarch64CAbiType hd4 = classify("HD4");
  EXPECT(state, hd4.homogeneous_element_bits == 64);
  EXPECT(state, hd4.homogeneous_element_count == 4);
  EXPECT(state, classify("HD5").classification ==
                    draft::Aarch64CAbiClass::Indirect);
  EXPECT(state, classify("HF2_Aligned").classification ==
                    draft::Aarch64CAbiClass::SmallAggregate);
  EXPECT(state, classify("Float_Union").classification ==
                    draft::Aarch64CAbiClass::HomogeneousFloatAggregate);
  const draft::Aarch64CAbiType unequal_union =
      classify("Unequal_Float_Union");
  EXPECT(state, unequal_union.classification ==
                    draft::Aarch64CAbiClass::HomogeneousFloatAggregate);
  EXPECT(state, unequal_union.homogeneous_element_bits == 32);
  EXPECT(state, unequal_union.homogeneous_element_count == 2);
  const draft::Aarch64CAbiType nested_union = classify("Nested_Union_HF4");
  EXPECT(state, nested_union.classification ==
                    draft::Aarch64CAbiClass::HomogeneousFloatAggregate);
  EXPECT(state, nested_union.homogeneous_element_count == 4);
  EXPECT(state, classify("Mixed_Union").classification ==
                    draft::Aarch64CAbiClass::SmallAggregate);

  EXPECT(state, classify("Default_Record").classification ==
                    draft::Aarch64CAbiClass::Illegal);
  EXPECT(state, classify("Bad_Member").classification ==
                    draft::Aarch64CAbiClass::Illegal);
  EXPECT(state, classify("C_Enum").classification ==
                    draft::Aarch64CAbiClass::Direct);
  EXPECT(state, classify("Bad_Callback").classification ==
                    draft::Aarch64CAbiClass::Illegal);
  EXPECT(state, classify("Recursive_Callback_Record").classification ==
                    draft::Aarch64CAbiClass::SmallAggregate);
}

void test_linux_aapcs64_classes(TestState &state) {
  SemanticSource source(R"draft(
package abi

C3 :: c struct { bytes: [3]u8, }
C24 :: c struct { words: [3]i64, }
HF2 :: c struct { first: f32, second: f32, }
)draft");
  const draft::TargetFacts target =
      draft::make_aarch64_linux_profile().facts;
  const auto classify = [&source, &target](std::string_view name) {
    return draft::classify_aarch64_c_type(
        source.semantics.package.types, source.type(name), target);
  };
  EXPECT(state, classify("C3").classification ==
      draft::Aarch64CAbiClass::SmallAggregate);
  EXPECT(state, classify("C3").result_integer_bits == 24);
  EXPECT(state, classify("C24").classification ==
      draft::Aarch64CAbiClass::Indirect);
  EXPECT(state, classify("HF2").classification ==
      draft::Aarch64CAbiClass::HomogeneousFloatAggregate);

  draft::TargetFacts unknown = target;
  unknown.abi = "unknown";
  EXPECT(state, draft::classify_aarch64_c_type(
      source.semantics.package.types, source.type("C3"), unknown)
          .classification == draft::Aarch64CAbiClass::Illegal);
}

} // namespace

int main() {
  TestState state;
  test_darwin_arm64_classes(state);
  test_linux_aapcs64_classes(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " ABI classifier expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all AArch64 ABI classifier tests passed\n";
  return EXIT_SUCCESS;
}
