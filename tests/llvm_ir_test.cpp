// Deterministic target-profiled LLVM IR emission tests.

#include "backend/llvm_ir.h"
#include "mir/lower.h"
#include "sema/body_checker.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "target/profile.h"
#include "workspace/package.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "llvm_ir_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_scalar_executable_module(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "native";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source("package.draft", R"draft(
package native

add :: proc(left, right: i64) -> i64 {
    return left + right
}

divide :: proc(left, right: i64) -> i64 {
    return left / right
}

shift :: proc(value: u32, count: usize) -> u32 {
    return value << count
}

scale :: proc(value: f64) -> f64 {
    return value * 0.5
}

tenth64 :: proc() -> f64 {
    return 0.1
}

tenth32 :: proc() -> f32 {
    return 0.1
}

identity[T: number] :: proc(value: T) -> T {
    return value
}

Code :: enum i16 {
    Zero,
    Seven = 7,
}

Outcome :: union {
    empty,
    value: i64,
    failure: u32,
}

Pair[T: type, U: type] :: struct {
    first: T,
    second: U,
}

Maybe[T: type] :: union {
    none,
    some: T,
}

pair_total :: proc(pair: Pair[i64, i64]) -> i64 {
    return pair.first + pair.second
}

unwrap_maybe :: proc(value: Maybe[i64]) -> i64 {
    switch value {
    case .some(payload):
        return payload
    case .none:
        return 0
    }
}

make_outcome :: proc(value: i64) -> Outcome {
    return .value(value)
}

read_outcome :: proc(outcome: Outcome) -> i64 {
    switch outcome {
    case .value(payload):
        return payload
    case .failure(_):
        return -1
    case .empty:
        return 0
    }
}

truncate_checked :: proc(value: f64) -> i32 {
    return cast[i32](value)
}

to_rune_checked :: proc(value: i64) -> rune {
    return cast[rune](value)
}

to_code_checked :: proc(value: i64) -> Code {
    return cast[Code](value)
}

main :: proc() -> i32 {
    value := add(20, 22)
    (left, _): (i64, i64) = (20, 0)
    (_, right): (i64, i64) = (0, 22)
    copy := identity[i64](value)
    inferred := identity(value)
    small := 1 + 2
    assert(left + right == 42)
    assert(read_outcome(make_outcome(42)) == 42)
    assert(read_outcome(.failure(7)) == -1)
    assert(read_outcome(.empty) == 0)
    zero_outcome: Outcome
    assert(read_outcome(zero_outcome) == 0)
    pair := Pair[i64, i64]{first = 20, second = 22}
    assert(pair_total(pair) == 42)
    assert(unwrap_maybe(.some(42)) == 42)
    assert(unwrap_maybe(.none) == 0)
    assert(copy == 42)
    assert(inferred == 42)
    assert(small == 3)
    return cast[i32](value)
}
)draft");
  file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
  loaded.files.push_back(std::move(file));
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded, target.facts, diagnostics);
  draft::BodyCheckResult bodies = draft::check_package_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  draft::MirLoweringResult mir = draft::lower_package_to_mir(
      semantics.package, bodies.program, diagnostics);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "native"};
  options.emit_program_entry = true;
  const draft::LlvmIrResult module = draft::emit_llvm_ir(
      target,
      sources,
      options,
      semantics.package,
      semantics.constants,
      mir.program,
      diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    std::cerr << module.text;
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, mir.ok);
  EXPECT(state, module.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, module.text.find(
      "target triple = \"arm64-apple-macosx14.0.0\"") != std::string::npos);
  EXPECT(state, module.text.find(
      "define i64 @\"draft.workspace.native.add\"(ptr %context, i64 %arg0, i64 %arg1)") !=
      std::string::npos);
  EXPECT(state, module.text.find("identity_24instance") != std::string::npos);
  EXPECT(state, module.text.find("<type-parameter>") == std::string::npos);
  EXPECT(state, module.text.find(
      "call void @__draft.assert(ptr %context, i1") != std::string::npos);
  EXPECT(state, module.text.find("copy == 42") != std::string::npos);
  EXPECT(state, module.text.find("package.draft") != std::string::npos);
  EXPECT(state, module.text.find("define i32 @main(i32 %argc, ptr %argv)") !=
      std::string::npos);
  EXPECT(state, module.text.find("ptr @__draft.root_context") != std::string::npos);
  EXPECT(state, module.text.find("(ptr null)") == std::string::npos);
  EXPECT(state, module.text.find("trunc i64") != std::string::npos);
  EXPECT(state, module.text.find("sdiv i64") != std::string::npos);
  EXPECT(state, module.text.find("shl i32") != std::string::npos);
  EXPECT(state, module.text.find("call void @llvm.trap()") != std::string::npos);
  EXPECT(state, module.text.find("fmul double") != std::string::npos);
  EXPECT(state, module.text.find("fcmp ogt double") != std::string::npos);
  EXPECT(state, module.text.find("fcmp olt double") != std::string::npos);
  EXPECT(state, module.text.find("fptosi double") != std::string::npos);
  EXPECT(state, module.text.find("icmp eq i16") != std::string::npos);
  EXPECT(state, module.text.find(", 7") != std::string::npos);
  EXPECT(state, module.text.find("extractvalue { i64, i64 }") != std::string::npos);
  EXPECT(state, module.text.find("switch i8") != std::string::npos);
  EXPECT(state, module.text.find("getelementptr i8") != std::string::npos);
  EXPECT(state, module.text.find("bitcast (i64 4602678819172646912 to double)") !=
      std::string::npos);
  EXPECT(state, module.text.find("bitcast (i64 4591870180066957722 to double)") !=
      std::string::npos);
  EXPECT(state, module.text.find("bitcast (i32 1036831949 to float)") !=
      std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_scalar_executable_module(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " LLVM IR expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all LLVM IR tests passed\n";
  return EXIT_SUCCESS;
}
