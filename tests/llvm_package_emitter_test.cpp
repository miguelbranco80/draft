// Direct package-module construction tests from checked Draft source.
//
// These tests start above MIR so they exercise the real semantic TypeId,
// SymbolId, local, and control-flow domains consumed by the direct builder.
// They request native bytes from the same constructed module and assert that
// the textual-input preparation/parser phases remain absent. Text is retained
// only as an inspection aid after construction, never fed back into LLVM.

#include "backend/llvm_package_emitter.h"
#include "mir/lower.h"
#include "native_pipeline.h"
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
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (condition)
      return;
    ++failures;
    std::cerr << "llvm_package_emitter_test.cpp:" << line
              << ": expectation failed: " << expression << '\n';
  }
};

#define EXPECT(state, expression)                                              \
  (state).expect((expression), #expression, __LINE__)

struct DirectFixture {
  draft::LlvmPackageEmissionResult emitted;
  std::string diagnostics;
};

// Builds one small package twice through the complete checked-source-to-object
// path. The fixture deliberately has a local parameter, load, integer constant,
// binary operation, and return so the test cannot pass with an empty hand-made
// LLVM module.
[[nodiscard]] DirectFixture
emit_increment_package(const draft::TargetProfile &target) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "direct";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source("package.draft",
                                   R"draft(package direct

Pair :: struct {
    left: i64,
    right: i64,
}

Outcome :: variant {
    empty,
    value: i64,
}

Text_Outcome :: variant {
    empty,
    value: string,
}

Callback_Outcome :: variant {
    empty,
    value: proc(value: i64) -> i64,
}

Relocation_Box :: struct {
    marker: u8,
    text: Text_Outcome,
    callback: Callback_Outcome,
}

Overlay :: union {
    byte: u8,
    word: u64,
}

Bit_Header :: struct {
    bits(3) kind: u8,
    bits(6) delta: i16,
    bits(1) active: bool,
}

C_Float_Pair :: c struct {
    left: f32,
    right: f32,
}

C_Large :: c struct {
    values: [3]u64,
}

foreign host {
    host_pair as "draft_test_host_pair" :: c proc(
        value: C_Float_Pair,
    ) -> C_Float_Pair
    host_large as "draft_test_host_large" :: c proc(value: C_Large) -> C_Large
    host_wide as "draft_test_host_wide" :: c proc(value: i128) -> i128
    host_sum as "draft_test_host_sum" :: c proc(first: i32, ..) -> i32
}

base: i64 = 40
label: string = "draft"
table: [3]i64 = [3]i64{1, 4, 9}
origin: Pair = Pair{left = 3, right = 7}
global_outcome: Outcome = .value(42)
global_overlay: Overlay = Overlay{word = 0x1020304050607080}
global_bits: Bit_Header = Bit_Header{
    kind = 5,
    delta = -7,
    active = true,
}
global_text_outcome: Text_Outcome = .value("global")

increment :: proc(value: i64) -> i64 {
    return value + 1
}

Compile_Time_Box :: Relocation_Box{
    marker = 9,
    text = .value("local"),
    callback = .value(increment),
}

global_relocation_box: Relocation_Box = Relocation_Box{
    marker = 7,
    text = .value("nested"),
    callback = .value(increment),
}

answer :: proc() -> i64 {
    return base + 2
}

label_length :: proc() -> usize {
    return len(label)
}

pair_sum :: proc(left, right: i64) -> i64 {
    pair := Pair{left = left, right = right}
    return pair.left + pair.right
}

narrow :: proc(value: i64) -> u8 {
    return cast[u8](value)
}

make_outcome :: proc(value: i64) -> Outcome {
    return .value(value)
}

read_outcome :: proc(outcome: Outcome) -> i64 {
    switch outcome {
    case .value(payload):
        return payload
    case .empty:
        return 0
    }
}

overlay_word :: proc(value: u64) -> u64 {
    overlay := Overlay{word = value}
    return overlay.word
}

bit_total :: proc(kind: u8, delta: i16, active: bool) -> i16 {
    header := Bit_Header{kind = kind, delta = delta, active = active}
    return cast[i16](header.kind) + header.delta
}

compile_time_box :: proc() -> Relocation_Box {
    return Compile_Time_Box
}

require_nonzero :: proc(value: i64) {
    assert(value != 0, "expected a nonzero value")
}

first_value :: proc(values: []i64) -> i64 {
    return values[0]
}

subview :: proc(values: []i64, low, high: usize) -> []i64 {
    return values[low:high]
}

export add_bytes as "draft_test_add_bytes" :: c proc(left, right: u8) -> u8 {
    return left + right
}

export echo_pair as "draft_test_echo_pair" :: c proc(
    value: C_Float_Pair,
) -> C_Float_Pair {
    return value
}

export echo_large as "draft_test_echo_large" :: c proc(
    value: C_Large,
) -> C_Large {
    return value
}

export echo_wide as "draft_test_echo_wide" :: c proc(value: i128) -> i128 {
    return value
}

call_host_pair :: proc(value: C_Float_Pair) -> C_Float_Pair {
    return host_pair(value)
}

call_host_large :: proc(value: C_Large) -> C_Large {
    return host_large(value)
}

call_host_wide :: proc(value: i128) -> i128 {
    return host_wide(value)
}

call_host_variadic :: proc(value: i8) -> i32 {
    return host_sum(1, value, 3.5)
}
)draft");
  file.syntax.emplace(
      draft::parse_source_file(sources, file.source, diagnostics));
  loaded.files.push_back(std::move(file));

  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded, target.facts, diagnostics);
  draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources, loaded, semantics.selections, semantics.package,
      semantics.constants, target.facts, diagnostics);
  draft::test_support::LoweredProcedureProducts mir =
      draft::test_support::lower_procedure_products(
          bodies.package, bodies.procedures, diagnostics);
  const draft::CAbiTable abi =
      draft::classify_c_types(bodies.package.types, target.facts);

  std::vector<const draft::MirProcedure *> procedures;
  procedures.reserve(mir.procedures.size());
  for (const draft::MirProcedure &procedure : mir.procedures) {
    procedures.push_back(&procedure);
  }
  std::vector<draft::SymbolId> globals;
  for (draft::SymbolId symbol :
       bodies.package.symbols.symbols_in_scope(bodies.package.package_scope)) {
    const draft::Symbol &candidate = bodies.package.symbols.symbol(symbol);
    if (candidate.kind == draft::SymbolKind::Variable &&
        !candidate.flags.foreign) {
      globals.push_back(symbol);
    }
  }
  draft::LlvmPackageEmissionOptions options;
  options.module.package = {"workspace", "direct"};
  options.retain_llvm_text = true;
  options.collect_phase_timings = true;
  options.native_options.emplace();
  options.native_options->collect_phase_timings = true;

  DirectFixture result;
  if (semantics.ok && bodies.ok && mir.ok) {
    result.emitted = draft::emit_llvm_package_direct(
        target, sources, options, bodies.package, abi,
        semantics.global_initializers, globals, procedures, diagnostics);
  }
  result.diagnostics = draft::render_diagnostics(sources, diagnostics);
  return result;
}

void test_direct_scalar_package_emits_native_object(TestState &state) {
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  const DirectFixture first = emit_increment_package(target);
  if (!first.diagnostics.empty())
    std::cerr << first.diagnostics;
  EXPECT(state, first.emitted.ok);
  EXPECT(state, first.diagnostics.empty());
  EXPECT(state, first.emitted.llvm_text.find("increment") != std::string::npos);
  EXPECT(state, first.emitted.llvm_text.find("add i64") != std::string::npos);
  EXPECT(state, first.emitted.llvm_text.find("answer") != std::string::npos);
  EXPECT(state,
         first.emitted.llvm_text.find("pair_5Fsum") != std::string::npos);
  EXPECT(state, first.emitted.llvm_text.find("narrow") != std::string::npos);
  EXPECT(state,
         first.emitted.llvm_text.find(".draft.string.0") != std::string::npos);
  EXPECT(state,
         first.emitted.llvm_text.find("__draft.assert") != std::string::npos);
  EXPECT(state,
         first.emitted.llvm_text.find("__draft.bounds") != std::string::npos);
  EXPECT(state, first.emitted.llvm_text.find("__draft.slice_bounds") !=
                    std::string::npos);
  EXPECT(state, first.emitted.native.ok);
  EXPECT(state, !first.emitted.native.bytes.empty());
  EXPECT(state,
         first.emitted.phase_timings.module_construction_nanoseconds != 0);
  EXPECT(state,
         first.emitted.phase_timings.llvm_text_printing_nanoseconds != 0);
  EXPECT(state,
         first.emitted.native.phase_timings.input_preparation_nanoseconds == 0);
  EXPECT(state, first.emitted.native.phase_timings.ir_parsing_nanoseconds == 0);
  EXPECT(state,
         first.emitted.native.phase_timings.ir_verification_nanoseconds != 0);
  EXPECT(state,
         first.emitted.native.phase_timings.machine_code_emission_nanoseconds !=
             0);

  const DirectFixture second = emit_increment_package(target);
  if (!second.diagnostics.empty())
    std::cerr << second.diagnostics;
  EXPECT(state, second.emitted.ok);
  EXPECT(state, second.emitted.llvm_text == first.emitted.llvm_text);
  EXPECT(state, second.emitted.native.bytes == first.emitted.native.bytes);
}

void test_direct_c_abi_modules_cover_every_target(TestState &state) {
  const std::vector<draft::TargetProfile> targets{
      draft::make_aarch64_linux_profile(),
      draft::make_x86_64_linux_profile(),
      draft::make_x86_64_windows_profile(),
  };
  for (const draft::TargetProfile &target : targets) {
    const DirectFixture fixture = emit_increment_package(target);
    if (!fixture.diagnostics.empty())
      std::cerr << target.facts.identity << ":\n" << fixture.diagnostics;
    EXPECT(state, fixture.emitted.ok);
    EXPECT(state, fixture.diagnostics.empty());
    EXPECT(state, fixture.emitted.native.ok);
    EXPECT(state, !fixture.emitted.native.bytes.empty());
    EXPECT(state, fixture.emitted.llvm_text.find("draft_test_host_pair") !=
                      std::string::npos);
    EXPECT(state, fixture.emitted.llvm_text.find("draft_test_host_large") !=
                      std::string::npos);
    EXPECT(state, fixture.emitted.llvm_text.find("draft_test_host_sum") !=
                      std::string::npos);
    if (target.facts.abi == "win64") {
      EXPECT(state,
             fixture.emitted.llvm_text.find("<2 x i64>") != std::string::npos);
    }
  }
}

} // namespace

int main() {
  TestState state;
  test_direct_scalar_package_emits_native_object(state);
  test_direct_c_abi_modules_cover_every_target(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " direct LLVM package expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all direct LLVM package tests passed\n";
  return EXIT_SUCCESS;
}
