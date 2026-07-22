// Deterministic target-profiled LLVM IR emission tests.

#include "backend/llvm_ir.h"
#include "backend/llvm_object_emitter.h"
#include "mir/lower.h"
#include "sema/body_checker.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "target/profile.h"
#include "native_pipeline.h"
#include "workspace/package.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct EmittedFixture {
  bool ok = false;
  std::string text;
  std::vector<draft::SourceCorrelationEntry> source_correlations;
  std::string diagnostics;
};

// Runs the complete agent-free semantic/MIR/LLVM path for one in-memory file.
// Keeping this helper local makes byte-for-byte comparisons independent from
// filesystem order and from the native toolchain adapter.
[[nodiscard]] EmittedFixture emit_fixture_for_target(
    std::string text, const draft::TargetProfile &target,
    std::vector<draft::SourceExpansionMap> expansion_maps = {}) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "agent_noop";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(
      "package.draft [resolved]", std::move(text), std::move(expansion_maps));
  file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
  loaded.files.push_back(std::move(file));
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded, target.facts, diagnostics);
  draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  draft::test_support::LoweredProcedureProducts mir =
      draft::test_support::lower_procedure_products(
          bodies.package, bodies.procedures, diagnostics);
  const draft::CAbiTable abi =
      draft::classify_c_types(bodies.package.types, target.facts);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "agent-noop"};
  draft::LlvmIrResult module =
      draft::test_support::emit_package_llvm_module(
      target,
      sources,
      options,
      bodies.package,
      abi,
      semantics.global_initializers,
      mir.procedures,
      diagnostics);

  EmittedFixture result;
  result.ok = semantics.ok && bodies.ok && mir.ok && module.ok &&
      !diagnostics.has_errors();
  result.text = std::move(module.text);
  result.source_correlations = std::move(module.source_correlations);
  result.diagnostics = draft::render_diagnostics(sources, diagnostics);
  return result;
}

// Most historical snapshots exercise Darwin AArch64. Target-specific ABI
// tests call the explicit helper above so adding another backend cannot quietly
// change the baseline target of unrelated expectations.
[[nodiscard]] EmittedFixture
emit_fixture(std::string text,
             std::vector<draft::SourceExpansionMap> expansion_maps = {}) {
  return emit_fixture_for_target(std::move(text),
                                 draft::make_aarch64_macos_profile(),
                                 std::move(expansion_maps));
}

void test_generated_debug_locations_are_hermetic(TestState &state) {
  std::string source = R"draft(package generated_debug

generated :: proc() -> i64 {
    return 7
}
)draft";
  const std::size_t begin = source.find("generated ::");
  const std::size_t end = source.find("}\n", begin) + 1;
  std::vector<draft::SourceExpansionMap> maps;
  maps.push_back({
      static_cast<std::uint32_t>(begin),
      static_cast<std::uint32_t>(end),
      "/private/checkout-that-must-not-leak/surface.draft",
      {41, 7},
      {41, 10},
      "workspace:generated-debug:surface.draft:declaration:0",
  });
  const EmittedFixture emitted = emit_fixture(std::move(source), std::move(maps));
  if (!emitted.ok) std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);
  EXPECT(state, emitted.text.find(
      "!DIFile(filename: \"surface.draft\", directory: "
      "\"draft/workspace/agent_2Dnoop\")") != std::string::npos);
  EXPECT(state, emitted.text.find(
      "!DILocation(line: 41, column: 7") != std::string::npos);
  EXPECT(state, emitted.text.find(", !dbg !") != std::string::npos);
  EXPECT(state, emitted.text.find("draft.debug.begin") == std::string::npos);
  EXPECT(state, emitted.text.find("draft.debug.end") == std::string::npos);
  EXPECT(state, emitted.text.find(
      "generated:workspace:generated-debug:surface.draft:declaration:0") !=
      std::string::npos);
  EXPECT(state, emitted.text.find("checkout-that-must-not-leak") ==
      std::string::npos);
  EXPECT(state, emitted.text.find("package.draft [resolved]") ==
      std::string::npos);

  EXPECT(state, !emitted.source_correlations.empty());
  if (!emitted.source_correlations.empty()) {
    const draft::SourceCorrelationEntry &entry =
        emitted.source_correlations.front();
    EXPECT(state, entry.package.root_identity == "workspace");
    EXPECT(state, entry.package.root_relative_path == "agent-noop");
    EXPECT(state, entry.procedure == "generated");
    EXPECT(state, !entry.operation.empty());
    EXPECT(state, entry.authored_file == "surface.draft");
    EXPECT(state, entry.authored.line == 41);
    EXPECT(state, entry.authored.column == 7);
    EXPECT(state, entry.generated_file == "package.draft");
    EXPECT(state, entry.generated.line == 3);
    EXPECT(
        state,
        entry.synthesis_site ==
            "workspace:generated-debug:surface.draft:declaration:0");
  }

  draft::SourceCorrelationMap map;
  map.target_identity = "draft-aarch64-macos-v5";
  map.compiler_identity = "compiler-v1";
  map.program_identity =
      "resolved-program-sha256:" + draft::sha256("resolved-program").hex();
  // Reverse the rows to prove that serialization owns canonical order rather
  // than inheriting whichever traversal order a backend happens to use.
  map.entries = emitted.source_correlations;
  std::reverse(map.entries.begin(), map.entries.end());
  const std::string correlation =
      draft::serialize_source_correlation_map(map);
  std::string correlation_error;
  EXPECT(state, draft::validate_source_correlation_map(
      map, correlation_error));
  EXPECT(state, correlation_error.empty());
  EXPECT(state, correlation.find("draft-source-correlation-v1") !=
      std::string::npos);
  EXPECT(state, correlation.find("\"authored\": {\"file\": \"surface.draft\"") !=
      std::string::npos);
  EXPECT(state, correlation.find("\"generated\": {\"file\": \"package.draft\"") !=
      std::string::npos);
  EXPECT(state, correlation.find("checkout-that-must-not-leak") ==
      std::string::npos);

  if (!map.entries.empty()) {
    map.entries.push_back(map.entries.front());
    EXPECT(state, !draft::validate_source_correlation_map(
        map, correlation_error));
    EXPECT(state, correlation_error.find("duplicated") != std::string::npos);
  }
}

// The semantic type of Record.value is still u32, but its occurrence begins at
// byte offset one. LLVM must receive align 1 on both directions of access;
// advertising u32's natural alignment here would make ordinary source code UB
// in the backend despite the front end accepting the packed layout.
void test_packed_field_access_uses_occurrence_alignment(TestState &state) {
  const EmittedFixture emitted = emit_fixture(R"draft(
package packed_access

Record :: struct {
    head: u8,
    packed value: u32,
}

increment :: proc(record: ^Record) -> u32 {
    record^.value += 1
    return record^.value
}
)draft");
  if (!emitted.ok) std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);

  // Check complete instruction lines rather than merely observing an unrelated
  // byte load elsewhere in the module's runtime support.
  const std::size_t load = emitted.text.find("load i32, ptr %v");
  EXPECT(state, load != std::string::npos);
  if (load != std::string::npos) {
    const std::size_t line_end = emitted.text.find('\n', load);
    EXPECT(state, emitted.text.substr(load, line_end - load).find("align 1") !=
                      std::string::npos);
  }
  const std::size_t store = emitted.text.find("store i32 %v");
  EXPECT(state, store != std::string::npos);
  if (store != std::string::npos) {
    const std::size_t line_end = emitted.text.find('\n', store);
    EXPECT(state, emitted.text.substr(store, line_end - store).find("align 1") !=
                      std::string::npos);
  }
}

// Bit fields use opaque nominal byte storage plus explicit bytewise extraction
// and read-modify-write. This covers a field crossing a byte boundary, signed
// extension, and preservation of neighboring bits in the same storage unit.
void test_bit_field_access_uses_explicit_bit_operations(TestState &state) {
  const EmittedFixture emitted = emit_fixture(R"draft(
package bit_field_access

Record :: struct {
    bits(3) small: u8,
    bits(6) signed_value: i16,
    bits(1) flag: bool,
}

update :: proc(record: ^Record) -> i16 {
    record^.small += 1
    record^.signed_value = -3
    record^.flag = !record^.flag
    return cast[i16](record^.small) + record^.signed_value
}
)draft");
  if (!emitted.ok) std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);
  EXPECT(state, emitted.text.find(" = type [2 x i8]") != std::string::npos);
  EXPECT(state, emitted.text.find("sext i6") != std::string::npos);
  EXPECT(state, emitted.text.find("load i8, ptr") != std::string::npos);
  EXPECT(state, emitted.text.find("freeze i8") != std::string::npos);
  EXPECT(state, emitted.text.find("store i8") != std::string::npos);
}

// A semantic package lowers to one independently valid LLVM module containing
// its package storage and every concrete procedure definition. Compiling that
// complete unit through LLVM catches missing or conflicting definitions which
// textual substring checks alone would miss.
void test_package_module_is_independently_compilable(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "split_native";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source("package.draft", R"draft(
package split_native

answer: i64 = 41

add_answer :: proc(value: i64) -> i64 {
    return value + answer
}

main :: proc() -> int {
    return cast[int](add_answer(1))
}
)draft");
  file.syntax.emplace(draft::parse_source_file(
      sources, file.source, diagnostics));
  loaded.files.push_back(std::move(file));

  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded, target.facts, diagnostics);
  draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  draft::test_support::LoweredProcedureProducts mir =
      draft::test_support::lower_procedure_products(
          bodies.package, bodies.procedures, diagnostics);
  const draft::CAbiTable abi =
      draft::classify_c_types(bodies.package.types, target.facts);
  std::vector<const draft::MirProcedure *> procedures;
  for (const draft::MirProcedure &procedure : mir.procedures) {
    procedures.push_back(&procedure);
  }

  draft::LlvmIrOptions options;
  options.package = {"workspace", "split-native"};
  options.emit_runtime_support = true;
  options.emit_program_entry = true;
  const draft::LlvmIrResult package_module =
      draft::emit_llvm_package_module(
          target,
          sources,
          options,
          bodies.package,
          abi,
          semantics.global_initializers,
          procedures,
          diagnostics);
  EXPECT(state, package_module.ok);
  EXPECT(state, package_module.text.find(
      "@\"draft.workspace.split_2Dnative.answer\" = hidden global i64 41") !=
      std::string::npos);
  EXPECT(state, package_module.text.find(
      "define hidden i64 @\"draft.workspace.split_2Dnative.add_5Fanswer\"") !=
      std::string::npos);
  EXPECT(state, package_module.text.find(
      "define hidden i64 @\"draft.workspace.split_2Dnative.main\"") !=
      std::string::npos);
  EXPECT(state, package_module.text.find("define i32 @main") !=
      std::string::npos);
  const draft::LlvmObjectEmissionResult package_object =
      draft::emit_llvm_object_in_process(
          target, "semantic package module", package_module.text, {});
  if (!package_object.ok) std::cerr << package_object.failure << '\n';
  EXPECT(state, package_object.ok);

  for (const draft::SourceCorrelationEntry &entry :
       package_module.source_correlations) {
    EXPECT(state, entry.procedure_ordinal < mir.procedures.size());
  }

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, mir.ok);
  EXPECT(state, !diagnostics.has_errors());
}

// The native backend extracts the pointer field from a checked string value;
// it must not allocate storage, copy bytes, or call a runtime helper merely to
// expose data that is already present in the string view.
void test_raw_string_data_is_direct_pointer_extraction(TestState &state) {
  const EmittedFixture fixture = emit_fixture(R"draft(
package raw_data

expose :: proc(text: string) -> [^]u8 {
    return raw_data(text)
}
)draft");
  if (!fixture.ok) std::cerr << fixture.diagnostics << fixture.text;
  EXPECT(state, fixture.ok);
  EXPECT(state, fixture.text.find(
      "extractvalue { ptr, i64 }") != std::string::npos);
  EXPECT(state, fixture.text.find("@raw_data") == std::string::npos);
}

void test_multistep_call_lowering_keeps_debug_locations(TestState &state) {
  const EmittedFixture emitted = emit_fixture(R"draft(package call_debug

Pair :: c struct {
    left: i32,
    right: i32,
}

foreign debug_provider {
    pair_identity as "draft_debug_pair_identity" :: c proc(value: Pair) -> Pair
}

sink :: proc(value: i32) {
}

ordinary_draft :: proc(value: i32) {
    sink(value)
}

ordinary :: proc(value: Pair) -> Pair {
    return pair_identity(value)
}
)draft");
  if (!emitted.ok) std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);

  // The aggregate C call expands into scratch storage around the physical
  // call. The call is not necessarily the first LLVM instruction for its MIR
  // row, but LLVM still requires a location because a function with debug
  // metadata cannot contain an inlinable location-free call.
  std::size_t call = emitted.text.find("call ");
  while (call != std::string::npos) {
    const std::size_t candidate_end = emitted.text.find('\n', call);
    const std::string_view candidate(
        emitted.text.data() + call,
        (candidate_end == std::string::npos
             ? emitted.text.size()
             : candidate_end) - call);
    if (candidate.find("draft_debug_pair_identity") !=
        std::string_view::npos) {
      break;
    }
    call = emitted.text.find("call ", call + 1);
  }
  EXPECT(state, call != std::string::npos);
  if (call != std::string::npos) {
    const std::size_t line_begin = emitted.text.rfind('\n', call);
    const std::size_t line_end = emitted.text.find('\n', call);
    const std::size_t begin =
        line_begin == std::string::npos ? 0 : line_begin + 1;
    const std::string_view call_line(
        emitted.text.data() + begin,
        (line_end == std::string::npos ? emitted.text.size() : line_end) - begin);
    EXPECT(state, call_line.find(", !dbg !") != std::string_view::npos);
  }

  // A void Draft call has no SSA result prefix. It must still retain ordinary
  // LLVM instruction indentation so the same linear debug pass sees and tags
  // it; otherwise LLVM strips debug information from the caller during parse.
  std::size_t draft_call = emitted.text.find("call void ");
  while (draft_call != std::string::npos) {
    const std::size_t candidate_end = emitted.text.find('\n', draft_call);
    const std::string_view candidate(
        emitted.text.data() + draft_call,
        (candidate_end == std::string::npos
             ? emitted.text.size()
             : candidate_end) - draft_call);
    if (candidate.find("draft.workspace.agent_2Dnoop.sink") !=
        std::string_view::npos) {
      break;
    }
    draft_call = emitted.text.find("call void ", draft_call + 1);
  }
  EXPECT(state, draft_call != std::string::npos);
  if (draft_call != std::string::npos) {
    const std::size_t line_begin = emitted.text.rfind('\n', draft_call);
    const std::size_t line_end = emitted.text.find('\n', draft_call);
    const std::size_t begin =
        line_begin == std::string::npos ? 0 : line_begin + 1;
    const std::string_view call_line(
        emitted.text.data() + begin,
        (line_end == std::string::npos ? emitted.text.size() : line_end) - begin);
    EXPECT(state, call_line.starts_with("  call void "));
    EXPECT(state, call_line.find(", !dbg !") != std::string_view::npos);
  }
}

void test_agent_constructs_have_no_runtime_footprint(TestState &state) {
  // The comments occupy exactly the lines used by docs/judge in the second
  // fixture. Source coordinates therefore remain stable; exact module equality
  // proves the constructs add neither an instruction nor a hidden data object.
  const EmittedFixture baseline = emit_fixture(R"draft(package agent_noop

// Public increment operation.
pub increment :: proc(value: i64) -> i64 {
    result := value + 1
    // The result is one greater than the input.
    return result
}
)draft");
  const EmittedFixture with_agents = emit_fixture(R"draft(package agent_noop

docs "Public increment operation."
pub increment :: proc(value: i64) -> i64 {
    result := value + 1
    judge "The result is one greater than the input."
    return result
}
)draft");

  if (!baseline.ok) std::cerr << baseline.diagnostics;
  if (!with_agents.ok) std::cerr << with_agents.diagnostics;
  EXPECT(state, baseline.ok);
  EXPECT(state, with_agents.ok);
  EXPECT(state, baseline.text == with_agents.text);
}

// The frontend erases an open static pack into a conventional monomorphized
// procedure before LLVM emission. Checking the physical parameter and call
// spellings here guards against accidentally introducing a runtime pack
// descriptor, an erased `any` value, or a target variadic ABI in the backend.
void test_static_argument_pack_emits_fixed_signature(TestState &state) {
  const EmittedFixture emitted = emit_fixture(R"draft(package pack_llvm

combine :: proc(values: ..type) -> i64 {
    total: i64
    for value in values {
        when type_kind(type_of(value)) == .signed_integer {
            total += cast[i64](value)
        } else when type_kind(type_of(value)) == .unsigned_integer {
            total += cast[i64](value)
        } else {
            static_assert(false, "unsupported pack value")
        }
    }
    return total + cast[i64](len(values))
}

main :: proc() -> i64 {
    return combine(cast[i8](3), cast[u16](4))
}
)draft");

  if (!emitted.ok) std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);
  EXPECT(state, emitted.text.find("i8 %arg0, i16 %arg1") != std::string::npos);
  EXPECT(state, emitted.text.find("i8 3, i16 4") != std::string::npos);
  EXPECT(state, emitted.text.find("TypeParameter") == std::string::npos);
  EXPECT(state, emitted.text.find("static_pack") == std::string::npos);
}

// A C variadic call remains genuinely variadic in LLVM. The semantic checker
// has already changed every unnamed operand to its C promoted type, leaving
// LLVM's target lowering to choose the Darwin/GNU AArch64 or SysV AMD64
// physical register/stack locations from an ordinary vararg function type.
void test_c_variadic_call_emits_promoted_tail(TestState &state) {
  const EmittedFixture emitted = emit_fixture(R"draft(package c_vararg_llvm

foreign libc {
    sink as "draft_variadic_sink" :: c proc(tag: i32, ..) -> i32
}

invoke :: proc(
    narrow: u8,
    single: f32,
    condition: bool,
    pointer: rawptr,
) -> i32 {
    return sink(1, narrow, single, condition, pointer)
}
)draft");

  if (!emitted.ok) std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);
  EXPECT(state, emitted.text.find(
      "declare i32 @\"draft_variadic_sink\"(i32, ...)") !=
      std::string::npos);
  EXPECT(state, emitted.text.find("zext i8") != std::string::npos);
  EXPECT(state, emitted.text.find("fpext float") != std::string::npos);
  EXPECT(state, emitted.text.find("zext i1") != std::string::npos);
  EXPECT(state, emitted.text.find(
      "i32 (i32, ...) @\"draft_variadic_sink\"(i32 1, i32 ") !=
      std::string::npos);
  EXPECT(state, emitted.text.find(", double ") != std::string::npos);
  EXPECT(state, emitted.text.find(", ptr ") != std::string::npos);
}

// SysV AMD64 decomposes a small C record into one or two INTEGER/SSE
// parameters, except when the complete record no longer fits the registers
// remaining at its source position. The latter case must use LLVM byval so the
// backend performs the ABI-mandated stack copy; treating it as an ordinary
// pointer would pass an address in a register and disagree with C.
void test_sysv_aggregate_signatures_and_calls(TestState &state) {
  const draft::TargetProfile target = draft::make_x86_64_linux_profile();
  const EmittedFixture emitted = emit_fixture_for_target(R"draft(
package sysv_aggregate

Mixed :: c struct {
    floating: f64,
    integer: i32,
}

One_Integer :: c struct {
    value: i64,
}

Integer_Pair :: c struct {
    first: i64,
    second: i64,
}

Large :: c struct {
    values: [3]i64,
}

export mixed_identity as "draft_mixed_identity" :: c proc(
    value: Mixed,
) -> Mixed {
    return value
}

export large_identity as "draft_large_identity" :: c proc(
    value: Large,
) -> Large {
    return value
}

foreign libc {
    after_six as "draft_after_six" :: c proc(
        a, b, c, d, e, f: i64,
        value: Mixed,
    ) -> Mixed
    pair_after_five as "draft_pair_after_five" :: c proc(
        a, b, c, d, e: i64,
        value: Integer_Pair,
    ) -> Integer_Pair
    one_after_five as "draft_one_after_five" :: c proc(
        a, b, c, d, e: i64,
        value: One_Integer,
    ) -> One_Integer
}

call_after_six :: proc(value: Mixed) -> Mixed {
    return after_six(1, 2, 3, 4, 5, 6, value)
}

call_pair_after_five :: proc(value: Integer_Pair) -> Integer_Pair {
    return pair_after_five(1, 2, 3, 4, 5, value)
}

call_one_after_five :: proc(value: One_Integer) -> One_Integer {
    return one_after_five(1, 2, 3, 4, 5, value)
}
)draft",
                                                         target);

  if (!emitted.ok)
    std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);
  EXPECT(state, emitted.text.find(
                    "define { double, i32 } @\"draft_mixed_identity\""
                    "(double %arg0.0, i32 %arg0.1)") != std::string::npos);
  EXPECT(state,
         emitted.text.find("define void @\"draft_large_identity\"(ptr sret(") !=
             std::string::npos);
  EXPECT(state,
         emitted.text.find("@\"draft_after_six\"(i64, i64, i64, i64, i64, i64, "
                           "ptr byval(") != std::string::npos);
  EXPECT(state, emitted.text.find(
                    "@\"draft_pair_after_five\"(i64, i64, i64, i64, i64, "
                    "ptr byval(") != std::string::npos);
  EXPECT(state,
         emitted.text.find(
             "@\"draft_one_after_five\"(i64, i64, i64, i64, i64, i64)") !=
             std::string::npos);

  // LLVM parsing and x86 object emission are part of this test. A textual ABI
  // spelling that violates LLVM's attribute or aggregate rules must fail here
  // before native Linux CI reaches the independent C-client oracle.
  const draft::LlvmObjectEmissionResult object =
      draft::emit_llvm_object_in_process(target, "sysv-aggregate", emitted.text,
                                         {});
  if (!object.ok)
    std::cerr << object.failure << '\n';
  EXPECT(state, object.ok);
  EXPECT(state, !object.bytes.empty());
}

// Win64 uses integer carriers only for records whose complete size is exactly
// 1, 2, 4, or 8 bytes. Other records use plain pointers for parameters and a
// hidden sret pointer for results. The plain-pointer assertion is important:
// SysV's `byval` spelling would request a caller copy and select a different
// Windows ABI contract.
void test_win64_aggregate_signatures_and_calls(TestState &state) {
  const draft::TargetProfile target = draft::make_x86_64_windows_profile();
  const EmittedFixture emitted = emit_fixture_for_target(R"draft(
package win64_aggregate

Eight :: c struct { bytes: [8]u8, }
Float_Pair :: c struct { first: f32, second: f32, }
Six :: c struct { bytes: [6]u8, }
Sixteen :: c struct { first: u64, second: u64, }

export float_pair_identity as "draft_float_pair_identity" :: c proc(
    value: Float_Pair,
) -> Float_Pair {
    return value
}

export six_identity as "draft_six_identity" :: c proc(value: Six) -> Six {
    return value
}

foreign windows {
    take_eight as "draft_take_eight" :: c proc(value: Eight) -> Eight
    take_sixteen as "draft_take_sixteen" :: c proc(
        value: Sixteen,
    ) -> Sixteen
}

call_eight :: proc(value: Eight) -> Eight {
    return take_eight(value)
}

call_sixteen :: proc(value: Sixteen) -> Sixteen {
    return take_sixteen(value)
}
)draft",
                                                         target);
  if (!emitted.ok)
    std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);
  EXPECT(state, emitted.text.find(
      "define dllexport i64 @\"draft_float_pair_identity\"(i64 %arg0)") !=
      std::string::npos);
  EXPECT(state, emitted.text.find(
      "define dllexport void @\"draft_six_identity\"(ptr sret(") !=
      std::string::npos);
  EXPECT(state, emitted.text.find("ptr byval(") == std::string::npos);
  EXPECT(state, emitted.text.find("@\"draft_take_eight\"(i64)") !=
      std::string::npos);
  EXPECT(state, emitted.text.find("@\"draft_take_sixteen\"(ptr sret(") !=
      std::string::npos);
  EXPECT(state, emitted.text.find("!\"CodeView\", i32 1") !=
      std::string::npos);
  EXPECT(state, emitted.text.find("!\"Dwarf Version\"") ==
      std::string::npos);

  const draft::LlvmObjectEmissionResult object =
      draft::emit_llvm_object_in_process(target, "win64-aggregate", emitted.text,
                                         {});
  if (!object.ok)
    std::cerr << object.failure << '\n';
  EXPECT(state, object.ok);
  EXPECT(state, object.bytes.size() >= 2);
  if (object.bytes.size() >= 2) {
    EXPECT(state, static_cast<unsigned char>(object.bytes[0]) == 0x64U);
    EXPECT(state, static_cast<unsigned char>(object.bytes[1]) == 0x86U);
  }
}

// Verifies that tuple extraction uses the tuple's logical member numbers even
// when semantic layout places alignment padding before a member. LLVM owns the
// implicit padding of tuple types; only named packed structs contain explicit
// padding fields in our emitted representation. A leading discard makes this
// regression especially visible because the remaining locals select members
// one and two rather than member zero.
void test_padded_tuple_extraction_uses_logical_indices(TestState &state) {
  const EmittedFixture emitted = emit_fixture(R"draft(package tuple_extract

make_result :: proc() -> (rune, usize, u8) {
    return ('A', 7, 2)
}

main :: proc() -> int {
    (_, width, error) := make_result()
    if width == 7 && error == 2 {
        return 0
    }
    return 1
}
)draft");
  if (!emitted.ok) std::cerr << emitted.diagnostics;
  EXPECT(state, emitted.ok);

  // Inspect the two extraction lines themselves. Debug metadata separates an
  // extract from its later store, so adjacency would make this test depend on
  // an unrelated source-correlation detail.
  const std::string tuple_extract = "extractvalue { i32, i64, i8 }";
  const std::size_t width_position = emitted.text.find(tuple_extract);
  const std::size_t width_end = emitted.text.find('\n', width_position);
  const std::size_t error_position = emitted.text.find(
      tuple_extract,
      width_end == std::string::npos ? width_end : width_end + 1);
  const std::size_t error_end = emitted.text.find('\n', error_position);
  EXPECT(state, width_position != std::string::npos);
  EXPECT(state, width_end != std::string::npos);
  EXPECT(state, error_position != std::string::npos);
  EXPECT(state, error_end != std::string::npos);
  if (width_position != std::string::npos && width_end != std::string::npos) {
    EXPECT(state, emitted.text.substr(
        width_position, width_end - width_position).find(", 1,") !=
        std::string::npos);
  }
  if (error_position != std::string::npos && error_end != std::string::npos) {
    EXPECT(state, emitted.text.substr(
        error_position, error_end - error_position).find(", 2,") !=
        std::string::npos);
  }
}

void test_scalar_executable_module(
    TestState &state,
    const draft::TargetProfile &target) {
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

Outcome :: variant {
    empty,
    value: i64,
    failure: u32,
}

Text_Outcome :: variant {
    empty,
    value: string,
}

Callback_Outcome :: variant {
    empty,
    value: proc(value: u32) -> u32,
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

Overlay :: union {
    byte: u8,
    word: u64,
}

Text_Overlay :: union {
    text: string,
    words: [2]u64,
}

Relocation_Box :: struct {
    marker: u8,
    text: Text_Outcome,
    callback: Callback_Outcome,
    overlay: Text_Overlay,
}

Table :: ([4]u32{1, 4, 9, 16})
global_table: [4]u32 = Table
global_tuple: (i32, u64) = (7, 9)
global_header: Header = Header{tag = 1, value = 42}
global_text_count: Text_Count = Text_Count{text = "draft", count = 5}
global_outcome: Outcome = .value(9)
global_text_outcome: Text_Outcome = .value("draft")
global_overlay: Overlay = Overlay{word = 0x1020304050607080}
global_relocation_box: Relocation_Box = Relocation_Box{
    marker = 7,
    text = .value("nested"),
    callback = .value(Increment_Callback),
    overlay = Text_Overlay{text = "overlay"},
}
global_text_outcomes: [2]Text_Outcome = [2]Text_Outcome{
    .value("first"),
    .value("second"),
}
Compile_Time_Relocation_Box :: Relocation_Box{
    marker = 8,
    text = .value("constant"),
    callback = .value(Increment_Callback),
    overlay = Text_Overlay{text = "local"},
}
global_infinity: f32 = compile_time_infinity()
global_nan: f32 = compile_time_nan()
global_negative_zero: f32 = compile_time_negative_zero()

Maybe[T: type] :: variant {
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

call_callback_outcome :: proc(value: Callback_Outcome, argument: u32) -> u32 {
    switch value {
    case .value(callback):
        return callback(argument)
    case .empty:
        return argument
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
    assert(global_relocation_box.marker == 7)
    assert(read_text_outcome(global_relocation_box.text) == 6)
    assert(call_callback_outcome(global_relocation_box.callback, 41) == 42)
    assert(len(global_relocation_box.overlay.text) == 7)
    assert(read_text_outcome(global_text_outcomes[0]) == 5)
    assert(read_text_outcome(global_text_outcomes[1]) == 6)
    local_relocation_box := Compile_Time_Relocation_Box
    assert(local_relocation_box.marker == 8)
    assert(read_text_outcome(local_relocation_box.text) == 8)
    assert(call_callback_outcome(local_relocation_box.callback, 41) == 42)
    assert(len(local_relocation_box.overlay.text) == 5)
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
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded, target.facts, diagnostics);
  draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  draft::test_support::LoweredProcedureProducts mir =
      draft::test_support::lower_procedure_products(
          bodies.package, bodies.procedures, diagnostics);
  const draft::CAbiTable abi =
      draft::classify_c_types(bodies.package.types, target.facts);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "native"};
  options.emit_program_entry = true;
  const draft::LlvmIrResult module =
      draft::test_support::emit_package_llvm_module(
      target,
      sources,
      options,
      bodies.package,
      abi,
      semantics.global_initializers,
      mir.procedures,
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
      "target triple = \"" + target.llvm_triple + "\"") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "define hidden i64 @\"draft.workspace.native.add\"(ptr %context, i64 %arg0, i64 %arg1)") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "@\"draft.workspace.native.global_5Fanswer\" = hidden global i64 42") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "load i64, ptr @\"draft.workspace.native.global_5Fanswer\"") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "store i64 %v") != std::string::npos);
  EXPECT(state, module.text.find(
      "@\"draft.workspace.native.inferred_5Fglobal\" = hidden global i64 21") !=
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
      "global <{ [8 x i8], { ptr, i64 } }> <{ [8 x i8] "
      "[i8 1, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0], "
      "{ ptr, i64 } { ptr @.draft.string.") != std::string::npos);
  EXPECT(state, module.text.find(
      "@\"draft.workspace.native.global_5Frelocation_5Fbox\" = hidden "
      "global <{") != std::string::npos);
  EXPECT(state, module.text.find(
      "@\"draft.workspace.native.global_5Ftext_5Foutcomes\" = hidden "
      "global <{") != std::string::npos);
  EXPECT(state, module.text.find("@.draft.constant.") != std::string::npos);
  EXPECT(state, module.text.find(" = private constant <{") !=
      std::string::npos);
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
  EXPECT(state, module.text.find(
      "define i32 @main(i32 %argc, ptr %argv, ptr %envp)") !=
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
  EXPECT(state, module.text.find(
      "store ptr %envp, ptr @__draft.process_envp") != std::string::npos);
  EXPECT(state, module.text.find(
      "call void @__draft.initialize_process_views") != std::string::npos);
  EXPECT(state, module.text.find(
      "define hidden ptr @\"__draft.os.args_data\"") != std::string::npos);
  EXPECT(state, module.text.find("ptr @__draft.root_context") != std::string::npos);
  EXPECT(state, module.text.find(
      "define internal void @__draft.default_logger") != std::string::npos);
  EXPECT(state, module.text.find(
      "define internal i1 @__draft.default_random") != std::string::npos);
  EXPECT(state, module.text.find(
      "%resize.grows = icmp ugt i64 %new_size, %old_size") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "%resize.tail = getelementptr i8, ptr %resized, i64 %old_size") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "ptr %resize.tail, i8 0, i64 %resize.growth, i1 false") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "define internal ptr @__draft.ensure_thread_context") !=
      std::string::npos);
  if (target.facts.os == "linux") {
    EXPECT(state, module.text.find(
        "%draft.runtime.PthreadOnce = type { i32 }") != std::string::npos);
    EXPECT(state, module.text.find(
        "@__draft.temp_key_once = internal global "
        "%draft.runtime.PthreadOnce zeroinitializer, align 4") !=
        std::string::npos);
    EXPECT(state, module.text.find(
        "declare ptr @pthread_getspecific(i32)") != std::string::npos);
    EXPECT(state, module.text.find(
        "load i32, ptr @__draft.temp_key, align 4") != std::string::npos);
    EXPECT(state, module.text.find("i64 816954554") == std::string::npos);
  } else {
    EXPECT(state, module.text.find(
        "@__draft.temp_key_once = internal global "
        "%draft.runtime.PthreadOnce { i64 816954554") !=
        std::string::npos);
    EXPECT(state, module.text.find(
        "declare ptr @pthread_getspecific(i64)") != std::string::npos);
  }
  EXPECT(state, module.text.find(
      "define internal ptr @__draft.temp_allocator") != std::string::npos);
  EXPECT(state, module.text.find(
      "define hidden void "
      "@\"__draft.runtime.reset_temporary_allocator\"") !=
      std::string::npos);
  EXPECT(state, module.text.find(
      "call void @__draft.destroy_current_temp_state()") !=
      std::string::npos);
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
  test_agent_constructs_have_no_runtime_footprint(state);
  test_generated_debug_locations_are_hermetic(state);
  test_packed_field_access_uses_occurrence_alignment(state);
  test_bit_field_access_uses_explicit_bit_operations(state);
  test_package_module_is_independently_compilable(state);
  test_raw_string_data_is_direct_pointer_extraction(state);
  test_multistep_call_lowering_keeps_debug_locations(state);
  test_static_argument_pack_emits_fixed_signature(state);
  test_c_variadic_call_emits_promoted_tail(state);
  test_sysv_aggregate_signatures_and_calls(state);
  test_win64_aggregate_signatures_and_calls(state);
  test_padded_tuple_extraction_uses_logical_indices(state);
  test_scalar_executable_module(
      state, draft::make_aarch64_macos_profile());
  test_scalar_executable_module(
      state, draft::make_aarch64_linux_profile());
  test_scalar_executable_module(state, draft::make_x86_64_linux_profile());
  test_scalar_executable_module(state, draft::make_x86_64_windows_profile());
  if (state.failures != 0) {
    std::cerr << state.failures << " LLVM IR expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all LLVM IR tests passed\n";
  return EXIT_SUCCESS;
}
