// SysV AMD64 C ABI classification and ordered register-budget tests.
//
// The type rows below isolate recursive eightbyte merging. Procedure rows then
// prove the ABI's all-or-memory rule: a register aggregate is expanded only
// when every INTEGER and SSE component fits the registers remaining at that
// exact parameter position. These expectations are mirrored by independent
// Clang IR/native-client integration tests at the LLVM and artifact layers.

#include "interop/c_abi.h"
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
      std::cerr << "x86_64_abi_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct SemanticSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::TargetProfile target = draft::make_x86_64_linux_profile();
  draft::SemanticAnalysisResult semantics;
  draft::CAbiTable abi;

  explicit SemanticSource(std::string text) {
    loaded.short_name = "abi";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(
        draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    semantics = draft::analyze_package_semantics(
        sources, loaded, target.facts, diagnostics);
    abi = draft::classify_c_types(semantics.package.types, target.facts);
  }

  [[nodiscard]] draft::TypeId type(std::string_view name) const {
    const std::optional<draft::SymbolId> symbol =
        semantics.package.symbols.lookup_direct(
            semantics.package.package_scope, name);
    return symbol.has_value()
        ? semantics.package.symbols.symbol(*symbol).type
        : draft::TypeId{};
  }

  [[nodiscard]] draft::CAbiType classify(std::string_view name) const {
    return draft::classify_c_type(
        semantics.package.types, type(name), target.facts);
  }

  [[nodiscard]] draft::CAbiFunctionPlan plan(std::string_view name) const {
    return draft::plan_c_abi_function(
        semantics.package.types, type(name), abi, target.facts);
  }
};

void test_eightbyte_classes(TestState &state) {
  SemanticSource source(R"draft(
package abi

One_Byte :: c struct { value: u8, }
Three_Bytes :: c struct { value: [3]u8, }
Integer_Pair :: c struct { first: u64, second: u64, }
One_Float :: c struct { value: f32, }
Float_Pair :: c struct { first: f32, second: f32, }
Float_Int :: c struct { first: f32, second: i32, }
Double_Int :: c struct { first: f64, second: i32, }
Three_Doubles :: c struct { values: [3]f64, }
Aligned_Float :: c align(16) struct { value: f32, }
Mixed_Union :: c union { floating: f64, integer: i64, }
Bad_Member :: c struct { value: []u8, }
C_Enum :: c enum { off, on, }
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const draft::CAbiType one = source.classify("One_Byte");
  EXPECT(state, one.classification == draft::CAbiClass::EightbyteAggregate);
  EXPECT(state, one.eightbyte_count == 1);
  EXPECT(state, one.eightbytes[0].classification ==
      draft::CAbiEightbyteClass::Integer);
  EXPECT(state, one.eightbytes[0].bits == 8);
  EXPECT(state, source.classify("Three_Bytes").eightbytes[0].bits == 24);

  const draft::CAbiType integers = source.classify("Integer_Pair");
  EXPECT(state, integers.eightbyte_count == 2);
  EXPECT(state, integers.eightbytes[0].classification ==
      draft::CAbiEightbyteClass::Integer);
  EXPECT(state, integers.eightbytes[1].classification ==
      draft::CAbiEightbyteClass::Integer);

  const draft::CAbiType one_float = source.classify("One_Float");
  EXPECT(state, one_float.eightbytes[0].classification ==
      draft::CAbiEightbyteClass::Sse);
  EXPECT(state, one_float.eightbytes[0].bits == 32);
  EXPECT(state, source.classify("Float_Pair").eightbytes[0].bits == 64);
  EXPECT(state, source.classify("Float_Int").eightbytes[0].classification ==
      draft::CAbiEightbyteClass::Integer);

  const draft::CAbiType mixed = source.classify("Double_Int");
  EXPECT(state, mixed.eightbyte_count == 2);
  EXPECT(state, mixed.eightbytes[0].classification ==
      draft::CAbiEightbyteClass::Sse);
  EXPECT(state, mixed.eightbytes[1].classification ==
      draft::CAbiEightbyteClass::Integer);
  EXPECT(state, source.classify("Three_Doubles").classification ==
      draft::CAbiClass::Indirect);
  EXPECT(state, source.classify("Aligned_Float").eightbyte_count == 1);
  EXPECT(state, source.classify("Mixed_Union").eightbytes[0].classification ==
      draft::CAbiEightbyteClass::Integer);
  EXPECT(state, source.classify("Bad_Member").classification ==
      draft::CAbiClass::Illegal);
  EXPECT(state, source.classify("C_Enum").classification ==
      draft::CAbiClass::Direct);
}

void test_ordered_register_budget(TestState &state) {
  SemanticSource source(R"draft(
package abi

One_Integer :: c struct { value: i64, }
Integer_Pair :: c struct { first: i64, second: i64, }
Mixed :: c struct { floating: f64, integer: i32, }
Large :: c struct { values: [3]i64, }

foreign libc {
    after_six_integers :: c proc(
        a, b, c, d, e, f: i64,
        value: Mixed,
    )
    pair_after_five :: c proc(a, b, c, d, e: i64, value: Integer_Pair)
    one_after_five :: c proc(a, b, c, d, e: i64, value: One_Integer)
    mixed_after_seven_sse :: c proc(
        a, b, c, d, e, f, g: f64,
        value: Mixed,
    )
    mixed_after_eight_sse :: c proc(
        a, b, c, d, e, f, g, h: f64,
        value: Mixed,
    )
    after_hidden_result :: c proc(
        a, b, c, d, e: i64,
        value: One_Integer,
    ) -> Large
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);

  const draft::CAbiFunctionPlan six = source.plan("after_six_integers");
  EXPECT(state, six.ok);
  EXPECT(state, six.parameters[6].mode == draft::CAbiParameterMode::Indirect);

  const draft::CAbiFunctionPlan pair = source.plan("pair_after_five");
  EXPECT(state, pair.ok);
  EXPECT(state, pair.parameters[5].mode == draft::CAbiParameterMode::Indirect);
  const draft::CAbiFunctionPlan one = source.plan("one_after_five");
  EXPECT(state, one.ok);
  EXPECT(state, one.parameters[5].mode == draft::CAbiParameterMode::Expanded);

  const draft::CAbiFunctionPlan seven = source.plan("mixed_after_seven_sse");
  EXPECT(state, seven.ok);
  EXPECT(state, seven.parameters[7].mode == draft::CAbiParameterMode::Expanded);
  const draft::CAbiFunctionPlan eight = source.plan("mixed_after_eight_sse");
  EXPECT(state, eight.ok);
  EXPECT(state, eight.parameters[8].mode == draft::CAbiParameterMode::Indirect);

  const draft::CAbiFunctionPlan hidden = source.plan("after_hidden_result");
  EXPECT(state, hidden.ok);
  EXPECT(state, hidden.result.classification == draft::CAbiClass::Indirect);
  EXPECT(state, hidden.parameters[5].mode == draft::CAbiParameterMode::Indirect);
}

} // namespace

int main() {
  TestState state;
  test_eightbyte_classes(state);
  test_ordered_register_budget(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " SysV AMD64 ABI expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all SysV AMD64 ABI classifier tests passed\n";
  return EXIT_SUCCESS;
}
