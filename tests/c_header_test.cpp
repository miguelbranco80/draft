// Deterministic C header generation for explicit Draft exports.

#include "interop/c_header.h"
#include "interop/native.h"
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
      std::cerr << "c_header_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_header_covers_exported_abi(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "c_library";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source("package.draft", R"draft(
package c_library

Pair :: c struct {
    left: i32,
    right: i32,
}

Aligned :: c align(16) struct {
    bytes: [3]u8,
}

Choice :: c enum {
    neither = -1,
    none = 0,
    first = 2,
}

Unsigned_Choice :: c enum {
    none,
    largest = 4294967295,
}

Wide_Choice :: c enum {
    none,
    large = 4294967296,
}

Maximum_Choice :: c enum {
    none,
    maximum = 18446744073709551615,
}

Minimum_Choice :: c enum {
    minimum = -9223372036854775808,
    none = 0,
}

Huge_Choice :: c enum u128 {
    none,
    huge = 340282366920938463463374607431768211455,
}

Number :: c union {
    integer: i64,
    decimal: f64,
}

Wide :: c struct {
    value: i128,
}

Hidden :: struct {
    value: i64,
}

Opaque_View :: c struct {
    view: []u8,
}

Opaque_View_Box :: c struct {
    value: ^Opaque_View,
}

Variadic_Callback :: c proc(format: cstring, ..) -> i32

export map_pair as "draft_map_pair" :: c proc(
    value: Pair,
    callback: c proc(value: i32) -> i32,
) -> Pair {
    return value
}

export inspect as "draft.inspect" :: c proc(
    pair: ^Pair,
    aligned: Aligned,
    choice: Choice,
    unsigned_choice: Unsigned_Choice,
    wide_choice: Wide_Choice,
    maximum_choice: Maximum_Choice,
    minimum_choice: Minimum_Choice,
    huge_choice: Huge_Choice,
    number: Number,
) -> i32 {
    return 0
}

export widen as "draft_widen" :: c proc(
    value: i128,
    output: ^^u8,
    record: ^Wide,
) -> u128 {
    return cast[u128](value)
}

export return_grid as "draft_return_grid" :: c proc(
    value: ^[4]u8,
) -> ^[4]u8 {
    return value
}

export opaque_chain as "draft_opaque_chain" :: c proc(
    value: ^^Hidden,
) -> ^^Hidden {
    return value
}

export opaque_draft_callback as "draft_opaque_draft_callback" :: c proc(
    value: ^proc(value: i32) -> i32,
    slot: ^^proc(value: i32) -> i32,
) -> ^proc(value: i32) -> i32 {
    return value
}

export opaque_view as "draft_opaque_view" :: c proc(
    value: ^Opaque_View,
) -> ^Opaque_View {
    return value
}

export opaque_view_box as "draft_opaque_view_box" :: c proc(
    value: Opaque_View_Box,
) -> Opaque_View_Box {
    return value
}

export echo_rune as "draft_echo_rune" :: c proc(value: rune) -> rune {
    return value
}

export accept_variadic_callback as "draft_accept_variadic_callback" :: c proc(
    callback: Variadic_Callback,
) -> i32 {
    return 0
}
)draft");
  file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
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
  const draft::CAbiTable abi =
      draft::classify_c_types(semantics.package.types, target.facts);
  const draft::TargetProfile linux_target =
      draft::make_aarch64_linux_profile();
  const draft::CAbiTable linux_abi =
      draft::classify_c_types(
          semantics.package.types, linux_target.facts);
  const draft::NativeInteropResult native = draft::validate_native_interop(
      semantics.package, bodies.procedures, abi, target.facts, diagnostics);
  const draft::CHeaderResult header = draft::emit_c_header(
      semantics.package, abi, target, {}, diagnostics);
  const draft::CHeaderResult linux_header = draft::emit_c_header(
      semantics.package,
      linux_abi,
      linux_target,
      {},
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }

  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, native.ok);
  EXPECT(state, header.ok);
  EXPECT(state, linux_header.ok);
  EXPECT(state, header.export_count == 10);
  EXPECT(state, header.text.find(
      "typedef struct draft_c_library_Pair draft_c_library_Pair;") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "int32_t left;") != std::string::npos);
  EXPECT(state, header.text.find(
      "uint8_t bytes[3];") != std::string::npos);
  EXPECT(state, header.text.find(
      "struct __attribute__((aligned(16))) draft_c_library_Aligned {") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "DRAFT_STATIC_ASSERT(offsetof(draft_c_library_Pair, right) == 4") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "typedef int32_t draft_c_library_Choice;") != std::string::npos);
  EXPECT(state, header.text.find(
      "typedef uint32_t draft_c_library_Unsigned_Choice;") !=
                    std::string::npos);
  EXPECT(state, header.text.find(
      "typedef uint64_t draft_c_library_Wide_Choice;") !=
                    std::string::npos);
  EXPECT(state, header.text.find(
      "DRAFT_C_LIBRARY_CHOICE_NEITHER ((draft_c_library_Choice)"
      "(-INT32_C(1)))") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "DRAFT_C_LIBRARY_MAXIMUM_CHOICE_MAXIMUM "
      "((draft_c_library_Maximum_Choice)UINT64_C(18446744073709551615))") !=
                    std::string::npos);
  EXPECT(state, header.text.find(
      "DRAFT_C_LIBRARY_MINIMUM_CHOICE_MINIMUM "
      "((draft_c_library_Minimum_Choice)"
      "(-INT64_C(9223372036854775807) - INT64_C(1)))") !=
                    std::string::npos);
  EXPECT(state, header.text.find(
      "typedef unsigned __int128 draft_c_library_Huge_Choice;") !=
                    std::string::npos);
  EXPECT(state, header.text.find(
      "(unsigned __int128)UINT64_C(18446744073709551615) << 64") !=
                    std::string::npos);
  EXPECT(state, header.text.find(
      "typedef int32_t (*draft_c_library_proc_") != std::string::npos);
  EXPECT(state, header.text.find("char * arg0, ...);") != std::string::npos);
  EXPECT(state, header.text.find(
      "extern draft_c_library_Pair draft_map_pair(") != std::string::npos);
  EXPECT(state, header.text.find(
      "__asm__(\"_draft.inspect\")") != std::string::npos);
  EXPECT(state, linux_header.text.find(
      "__asm__(\"draft.inspect\")") != std::string::npos);
  EXPECT(state, header.text.find(
      "__int128 value;") != std::string::npos);
  EXPECT(state, header.text.find(
      "extern unsigned __int128 draft_widen(__int128 arg0, uint8_t **arg1, "
      "draft_c_library_Wide *arg2);") != std::string::npos);
  EXPECT(state, header.text.find(
      "extern uint8_t (*draft_return_grid(uint8_t (*arg0)[4]))[4];") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "extern void **draft_opaque_chain(void **arg0);") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "extern void *draft_opaque_draft_callback(void *arg0, void **arg1);") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "extern void *draft_opaque_view(void *arg0);") !=
      std::string::npos);
  EXPECT(state, header.text.find("draft_c_library_Opaque_View {") ==
                    std::string::npos);
  EXPECT(state, header.text.find(
      "struct draft_c_library_Opaque_View_Box {") !=
      std::string::npos);
  EXPECT(state, header.text.find("void *value;") != std::string::npos);
  EXPECT(state, header.text.find(
      "extern draft_c_library_Opaque_View_Box draft_opaque_view_box(") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "extern int32_t draft_echo_rune(int32_t arg0);") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "extern int32_t draft_accept_variadic_callback(") !=
      std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_header_covers_exported_abi(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " C header expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all C header tests passed\n";
  return EXIT_SUCCESS;
}
