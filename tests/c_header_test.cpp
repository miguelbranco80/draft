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

Pair :: @repr(C) struct {
    left: i32,
    right: i32,
}

Aligned :: @repr(C) @align(16) struct {
    bytes: [3]u8,
}

Choice :: @repr(C) enum {
    neither = -1,
    first = 2,
}

Number :: @repr(C) raw union {
    integer: i64,
    decimal: f64,
}

Wide :: @repr(C) struct {
    value: i128,
}

Hidden :: struct {
    value: i64,
}

export map_pair :: c "draft_map_pair" proc(
    value: Pair,
    callback: c proc(value: i32) -> i32,
) -> Pair {
    return value
}

export inspect :: c "draft.inspect" proc(
    pair: ^Pair,
    aligned: Aligned,
    choice: Choice,
    number: Number,
) -> i32 {
    return 0
}

export widen :: c "draft_widen" proc(
    value: i128,
    output: ^^u8,
    record: ^Wide,
) -> u128 {
    return cast[u128](value)
}

export return_grid :: c "draft_return_grid" proc(
    value: ^[4]u8,
) -> ^[4]u8 {
    return value
}

export opaque_chain :: c "draft_opaque_chain" proc(
    value: ^^Hidden,
) -> ^^Hidden {
    return value
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
  const draft::NativeInteropResult native = draft::validate_native_interop(
      semantics.package, bodies.program, diagnostics);
  const draft::CHeaderResult header = draft::emit_c_header(
      semantics.package, {}, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }

  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, native.ok);
  EXPECT(state, header.ok);
  EXPECT(state, header.export_count == 5);
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
      "DRAFT_C_LIBRARY_CHOICE_NEITHER ((draft_c_library_Choice)-1)") !=
      std::string::npos);
  EXPECT(state, header.text.find(
      "typedef int32_t (*draft_c_library_proc_") != std::string::npos);
  EXPECT(state, header.text.find(
      "extern draft_c_library_Pair draft_map_pair(") != std::string::npos);
  EXPECT(state, header.text.find(
      "__asm__(\"_draft.inspect\")") != std::string::npos);
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
