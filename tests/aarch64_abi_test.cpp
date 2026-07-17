// AArch64 Darwin C ABI classification tests.

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

C1 :: @repr(C) struct { bytes: [1]u8, }
C3 :: @repr(C) struct { bytes: [3]u8, }
C5 :: @repr(C) struct { bytes: [5]u8, }
C9 :: @repr(C) struct { bytes: [9]u8, }
C16 :: @repr(C) struct { words: [2]i64, }
C16_Aligned :: @repr(C) @align(16) struct { word: i64, }
C24 :: @repr(C) struct { words: [3]i64, }
HF2 :: @repr(C) struct { first: f32, second: f32, }
HD4 :: @repr(C) struct { values: [4]f64, }
HD5 :: @repr(C) struct { values: [5]f64, }
HF2_Aligned :: @repr(C) @align(16) struct { first: f32, second: f32, }
Float_Union :: @repr(C) raw union { first: f32, second: f32, }
Mixed_Union :: @repr(C) raw union { first: f32, second: f64, }
Default_Record :: struct { value: i64, }
Bad_Member :: @repr(C) struct { value: []u8, }
C_Enum :: @repr(C) enum { off, on, }
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const auto classify = [&source](std::string_view name) {
    return draft::classify_aarch64_darwin_c_type(
        source.semantics.package.types, source.type(name));
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
  const draft::Aarch64CAbiType hd4 = classify("HD4");
  EXPECT(state, hd4.homogeneous_element_bits == 64);
  EXPECT(state, hd4.homogeneous_element_count == 4);
  EXPECT(state, classify("HD5").classification ==
                    draft::Aarch64CAbiClass::Indirect);
  EXPECT(state, classify("HF2_Aligned").classification ==
                    draft::Aarch64CAbiClass::SmallAggregate);
  EXPECT(state, classify("Float_Union").classification ==
                    draft::Aarch64CAbiClass::HomogeneousFloatAggregate);
  EXPECT(state, classify("Mixed_Union").classification ==
                    draft::Aarch64CAbiClass::SmallAggregate);

  EXPECT(state, classify("Default_Record").classification ==
                    draft::Aarch64CAbiClass::Illegal);
  EXPECT(state, classify("Bad_Member").classification ==
                    draft::Aarch64CAbiClass::Illegal);
  EXPECT(state, classify("C_Enum").classification ==
                    draft::Aarch64CAbiClass::Direct);
}

} // namespace

int main() {
  TestState state;
  test_darwin_arm64_classes(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " ABI classifier expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all AArch64 ABI classifier tests passed\n";
  return EXIT_SUCCESS;
}
