// Microsoft x64 C ABI record classification and LLP64 enum tests.
//
// These cases isolate the Win64 rule from the SysV AMD64 rule implemented for
// the same instruction architecture. Exact 1, 2, 4, and 8-byte C records use
// one integer carrier; every other nonempty record crosses the boundary by
// reference. The expectations also cover the asymmetric __int128 extension:
// it is passed by address and returned as <2 x i64>. They mirror Clang's
// x86_64-pc-windows-msvc IR and are supplemented by the backend's independent
// Clang ABI oracle.

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
      std::cerr << "win64_abi_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

// SemanticSource owns every source/semantic object used by one classifier
// fixture. The selected profile is deliberately Windows even when this test
// executable runs on macOS or Linux: ABI classification is pure target data
// and must never inherit the host process ABI.
struct SemanticSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::TargetProfile target = draft::make_x86_64_windows_profile();
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

void test_record_carriers(TestState &state) {
  SemanticSource source(R"draft(
package abi

One :: c struct { bytes: [1]u8, }
Two :: c struct { bytes: [2]u8, }
Three :: c struct { bytes: [3]u8, }
Four :: c struct { bytes: [4]u8, }
Six :: c struct { bytes: [6]u8, }
Eight :: c struct { bytes: [8]u8, }
Float_Pair :: c struct { first: f32, second: f32, }
Sixteen :: c struct { first: u64, second: u64, }
Bad_Member :: c struct { value: []u8, }

foreign windows {
    take_three :: c proc(value: Three)
    return_sixteen :: c proc() -> Sixteen
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const auto expect_small = [&](std::string_view name, std::uint32_t bits) {
    const draft::CAbiType abi = source.classify(name);
    EXPECT(state, abi.classification == draft::CAbiClass::SmallAggregate);
    EXPECT(state, abi.argument_integer_bits == bits);
    EXPECT(state, abi.argument_integer_count == 1);
    EXPECT(state, abi.result_integer_bits == bits);
    EXPECT(state, abi.result_integer_count == 1);
  };
  expect_small("One", 8);
  expect_small("Two", 16);
  expect_small("Four", 32);
  expect_small("Eight", 64);
  // Record contents do not create Win64 SSE aggregate classes.
  expect_small("Float_Pair", 64);

  EXPECT(state, source.classify("Three").classification ==
      draft::CAbiClass::Indirect);
  EXPECT(state, source.classify("Six").classification ==
      draft::CAbiClass::Indirect);
  EXPECT(state, source.classify("Sixteen").classification ==
      draft::CAbiClass::Indirect);
  EXPECT(state, source.classify("Bad_Member").classification ==
      draft::CAbiClass::Illegal);

  const draft::CAbiFunctionPlan parameter = source.plan("take_three");
  EXPECT(state, parameter.ok);
  EXPECT(state, parameter.parameters.size() == 1);
  EXPECT(state, parameter.parameters[0].mode ==
      draft::CAbiParameterMode::Indirect);
  const draft::CAbiFunctionPlan result = source.plan("return_sixteen");
  EXPECT(state, result.ok);
  EXPECT(state, result.result.classification == draft::CAbiClass::Indirect);
}

void test_llp64_enum_boundary(TestState &state) {
  SemanticSource accepted(R"draft(
package abi

Unsigned_Max :: c enum { zero, value = 4294967295, }
Signed_Min :: c enum { zero, value = -2147483648, }
)draft");
  if (accepted.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        accepted.sources, accepted.diagnostics);
  }
  EXPECT(state, accepted.semantics.ok);
  const std::optional<draft::TypeId> u32 =
      accepted.semantics.package.types.find_builtin("u32");
  const std::optional<draft::TypeId> i32 =
      accepted.semantics.package.types.find_builtin("i32");
  EXPECT(state, u32.has_value());
  EXPECT(state, i32.has_value());
  EXPECT(state, accepted.semantics.package.types.type(
      accepted.type("Unsigned_Max")).element ==
      u32.value_or(draft::TypeId{}));
  EXPECT(state, accepted.semantics.package.types.type(
      accepted.type("Signed_Min")).element ==
      i32.value_or(draft::TypeId{}));

  SemanticSource rejected(R"draft(
package abi

Too_Large :: c enum { zero, value = 4294967296, }
)draft");
  EXPECT(state, !rejected.semantics.ok);
  EXPECT(state, rejected.diagnostics.has_errors());
}

void test_wide_integer_boundary(TestState &state) {
  SemanticSource source(R"draft(
package abi

Huge :: c enum u128 { zero, maximum = 340282366920938463463374607431768211455, }

foreign windows {
    signed_identity :: c proc(value: i128) -> i128
    endian_identity :: c proc(value: u128le) -> u128le
    enum_identity :: c proc(value: Huge) -> Huge
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const std::optional<draft::TypeId> i128 =
      source.semantics.package.types.find_builtin("i128");
  EXPECT(state, i128.has_value());
  if (i128.has_value()) {
    EXPECT(state, draft::classify_c_type(
        source.semantics.package.types, *i128, source.target.facts)
        .classification == draft::CAbiClass::Win64WideInteger);
  }
  const std::optional<draft::TypeId> u128le =
      source.semantics.package.types.find_builtin("u128le");
  EXPECT(state, u128le.has_value());
  if (u128le.has_value()) {
    EXPECT(state, draft::classify_c_type(
        source.semantics.package.types, *u128le, source.target.facts)
        .classification == draft::CAbiClass::Win64WideInteger);
  }
  EXPECT(state, source.classify("Huge").classification ==
      draft::CAbiClass::Win64WideInteger);

  for (std::string_view procedure :
       {"signed_identity", "endian_identity", "enum_identity"}) {
    const draft::CAbiFunctionPlan plan = source.plan(procedure);
    EXPECT(state, plan.ok);
    EXPECT(state, plan.parameters.size() == 1);
    EXPECT(state, plan.parameters[0].mode ==
        draft::CAbiParameterMode::Indirect);
    EXPECT(state, plan.result.classification ==
        draft::CAbiClass::Win64WideInteger);
  }
}

} // namespace

int main() {
  TestState state;
  test_record_carriers(state);
  test_llp64_enum_boundary(state);
  test_wide_integer_boundary(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " Win64 ABI expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all Win64 ABI classifier tests passed\n";
  return EXIT_SUCCESS;
}
