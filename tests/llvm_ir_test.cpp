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

global_answer: u64 = 40 + 2
inferred_global := 21
thread_local tls_value: i32 = -7
global_text: string = "draft"
global_pointer: ^i64 = nil

add :: proc(left, right: i64) -> i64 {
    return left + right
}

increment_value :: proc(value: u32) -> u32 {
    return value + 1
}

Increment_Callback :: increment_value
global_callback: proc(value: u32) -> u32 = Increment_Callback

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
    static_assert(size_of(T) > 0)
    return value
}

compile_time_infinity :: proc() -> f32 {
    zero: f32
    return 1.0 / zero
}

compile_time_nan :: proc() -> f32 {
    zero: f32
    return zero / zero
}

compile_time_negative_zero :: proc() -> f32 {
    zero: f32
    return -zero
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

Text_Outcome :: union {
    empty,
    value: string,
}

Pair[T: type, U: type] :: struct {
    first: T,
    second: U,
}

Header :: struct {
    tag: u8,
    value: u64,
}

Text_Count :: struct {
    text: string,
    count: u64,
}

Overlay :: raw union {
    byte: u8,
    word: u64,
}

Table :: ([4]u32{1, 4, 9, 16})
global_table: [4]u32 = Table
global_tuple: (i32, u64) = (7, 9)
global_header: Header = Header{tag = 1, value = 42}
global_text_count: Text_Count = Text_Count{text = "draft", count = 5}
global_outcome: Outcome = .value(9)
global_text_outcome: Text_Outcome = .value("draft")
global_overlay: Overlay = Overlay{word = 0x1020304050607080}
global_infinity: f32 = compile_time_infinity()
global_nan: f32 = compile_time_nan()
global_negative_zero: f32 = compile_time_negative_zero()

Maybe[T: type] :: union {
    none,
    some: T,
}

pair_total :: proc(pair: Pair[i64, i64]) -> i64 {
    return pair.first + pair.second
}

constant_table_value :: proc() -> u32 {
    return Table[3]
}

unwrap_maybe :: proc(value: Maybe[i64]) -> i64 {
    switch value {
    case .some(payload):
        return payload
    case .none:
        return 0
    }
}

last[N: usize] :: proc(values: [N]i64) -> i64 {
    static_assert(N > 0)
    return values[N - 1]
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

read_text_outcome :: proc(outcome: Text_Outcome) -> usize {
    switch outcome {
    case .value(payload):
        return len(payload)
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
    return cast[Code](cast[i16](value))
}

storage_roundtrip :: proc(flag: bool, bits: b32) -> bool {
    encoded := cast[b32](flag)
    return cast[bool](encoded) || cast[bool](bits)
}

endian_roundtrip :: proc(value: u32) -> u32 {
    big := cast[u32be](value)
    little := cast[u32le](value)
    assert(big == cast[u32be](value))
    return cast[u32](big) + cast[u32](little)
}

endian_float_roundtrip :: proc(value: f64) -> f64 {
    big := cast[f64be](value)
    return cast[f64](big)
}

pointer_roundtrip :: proc(value: ^i64) -> bool {
    bits := cast[uintptr](value)
    return cast[^i64](bits) == value
}

pointer_distance :: proc(value: [^]i64, count: isize) -> isize {
    return ptr_sub(ptr_offset(value, count), value)
}

main :: proc() -> int {
    assert(global_callback(41) == 42)
    assert(global_answer == 42)
    global_answer = global_answer + 1
    assert(global_answer == 43)
    assert(inferred_global == 21)
    assert(tls_value == -7)
    assert(len(global_text) == 5)
    assert(global_pointer == nil)
    assert(global_table[3] == 16)
    assert(global_tuple.0 == 7 && global_tuple.1 == 9)
    assert(global_header.value == 42)
    assert(len(global_text_count.text) == 5)
    assert(global_text_count.count == 5)
    assert(read_outcome(global_outcome) == 9)
    assert(read_text_outcome(global_text_outcome) == 5)
    assert(global_overlay.word == 0x1020304050607080)
    assert(constant_table_value() == 16)
    assert(global_infinity > 1e30)
    assert(global_nan != global_nan)
    assert(global_negative_zero == 0.0)
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
    values := [3]i64{10, 20, 42}
    assert(pair_total(pair) == 42)
    assert(unwrap_maybe(.some(42)) == 42)
    assert(unwrap_maybe(.none) == 0)
    assert(copy == 42)
    assert(inferred == 42)
    assert(last[3](values) == 42)
    assert(last(values) == 42)
    assert(storage_roundtrip(true, cast[b32](false)))
    assert(endian_roundtrip(21) == 42)
    assert(endian_float_roundtrip(0.5) == 0.5)
    assert(pointer_roundtrip(&values[0]))
    multi := cast[[^]i64](&values[0])
    assert(ptr_offset(multi, 2)^ == 42)
    assert(pointer_distance(multi, 2) == 2)
    assert('é' == '\u{e9}')
    assert(cast[u32]('🙂') == 0x1f642)
    text := "draft"
    middle := text[1:4]
    assert(text[0] == cast[u8]('d'))
    assert(len(middle) == 3)
    assert(middle[2] == cast[u8]('f'))
    pointer := cast[[^]i64](&values[0])
    pointer_view := pointer[:3]
    assert(len(pointer_view) == 3)
    assert(pointer_view[2] == 42)
    assert(small == 3)
    return cast[int](value)
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
      semantics.global_initializers,
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
  EXPECT(state, module.text.find(
      "@\"draft.workspace.native.global_5Fanswer\" = global i64 42") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "load i64, ptr @\"draft.workspace.native.global_5Fanswer\"") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "store i64 %v") != std::string::npos);
  EXPECT(state, module.text.find(
      "@\"draft.workspace.native.inferred_5Fglobal\" = global i64 21") !=
      std::string::npos);
  EXPECT(state, module.text.find("thread_local global i32 -7") !=
      std::string::npos);
  EXPECT(state, module.text.find("global { ptr, i64 } { ptr @.draft.string.") !=
      std::string::npos);
  EXPECT(state, module.text.find("global ptr null") != std::string::npos);
  EXPECT(state, module.text.find(
      "global ptr @\"draft.workspace.native.increment_5Fvalue\"") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "global [4 x i32] [i32 1, i32 4, i32 9, i32 16]") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "global { i32, i64 } { i32 7, i64 9 }") != std::string::npos);
  EXPECT(state, module.text.find(
      "<{ i8 1, [7 x i8] zeroinitializer, i64 42 }>") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "<{ { ptr, i64 } { ptr @.draft.string.") != std::string::npos);
  EXPECT(state, module.text.find(
      "[i8 1, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0, i8 9") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "global <{ [8 x i8], { ptr, i64 }, [0 x i8] }> <{ [8 x i8] "
      "[i8 1, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0], "
      "{ ptr, i64 } { ptr @.draft.string.") != std::string::npos);
  EXPECT(state, module.text.find(
      "[i8 128, i8 112, i8 96, i8 80, i8 64, i8 48, i8 32, i8 16]") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "bitcast (i32 2139095040 to float)") != std::string::npos);
  EXPECT(state, module.text.find(
      "bitcast (i32 2143289344 to float)") != std::string::npos);
  EXPECT(state, module.text.find(
      "bitcast (i32 2147483648 to float)") != std::string::npos);
  EXPECT(state, module.text.find("identity_24instance") != std::string::npos);
  EXPECT(state, module.text.find("last_24instance_24v3") != std::string::npos);
  EXPECT(state, module.text.find("<type-parameter>") == std::string::npos);
  EXPECT(state, module.text.find(
      "call void @__draft.assert(ptr %context, i1") != std::string::npos);
  EXPECT(state, module.text.find("copy == 42") != std::string::npos);
  EXPECT(state, module.text.find("package.draft") != std::string::npos);
  EXPECT(state, module.text.find("define i32 @main(i32 %argc, ptr %argv)") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "%draft.runtime.Context = type { %draft.runtime.Allocator, "
      "%draft.runtime.Allocator, ptr, %draft.runtime.Logger, "
      "%draft.runtime.RandomGenerator, ptr, i64, ptr }") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "call void %handler(ptr %context, { ptr, i64 } %condition_text") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "store i32 %argc, ptr @__draft.process_argc") != std::string::npos);
  EXPECT(state, module.text.find(
      "store ptr %argv, ptr @__draft.process_argv") != std::string::npos);
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
  EXPECT(state, module.text.find("icmp ne i32") != std::string::npos);
  EXPECT(state, module.text.find("call i32 @llvm.bswap.i32") != std::string::npos);
  EXPECT(state, module.text.find("call i64 @llvm.bswap.i64") != std::string::npos);
  EXPECT(state, module.text.find("bitcast double") != std::string::npos);
  EXPECT(state, module.text.find("ptrtoint ptr") != std::string::npos);
  EXPECT(state, module.text.find("inttoptr i64") != std::string::npos);
  EXPECT(state, module.text.find("sdiv exact i64") != std::string::npos);
  EXPECT(state, module.text.find("sub nsw i64") != std::string::npos);
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
