// C ABI validation and exact linker-symbol emission tests.

#include "backend/llvm_ir.h"
#include "interop/native.h"
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
      std::cerr << "native_interop_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct CheckedSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics;
  draft::BodyCheckResult bodies;
  draft::HirProgram hir;

  explicit CheckedSource(
      std::string text,
      draft::TargetProfile selected_target =
          draft::make_aarch64_macos_profile())
      : target(std::move(selected_target)) {
    loaded.short_name = "native";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    semantics = draft::analyze_package_semantics(
        sources, loaded, target.facts, diagnostics);
    bodies = draft::check_package_bodies(
        sources,
        loaded,
        semantics.selections,
        semantics.package,
        semantics.constants,
        target.facts,
        diagnostics);
    hir = draft::project_package_body_hir(bodies.procedures);
  }
};

void test_valid_import_and_export(TestState &state) {
  CheckedSource source(R"draft(
package native

foreign libc {
    puts :: c "puts" proc(text: cstring) -> i32
}

export increment :: c "draft_increment" proc(value: i32) -> i32 {
    return value + 1
}
)draft");

  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package,
      source.hir,
      source.target.facts,
      source.diagnostics);
  const draft::MirLoweringResult mir = draft::lower_package_to_mir(
      source.semantics.package, source.hir, source.diagnostics);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "native"};
  const draft::LlvmIrResult llvm = draft::emit_llvm_ir(
      source.target,
      source.sources,
      options,
      source.semantics.package,
      source.semantics.global_initializers,
      mir.program,
      source.diagnostics);
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, native.ok);
  EXPECT(state, native.providers.size() == 1);
  EXPECT(state, native.providers[0] == "libc");
  EXPECT(state, mir.ok);
  EXPECT(state, llvm.ok);
  EXPECT(state, llvm.text.find("declare i32 @\"puts\"(ptr)") !=
      std::string::npos);
  EXPECT(state, llvm.text.find("define i32 @\"draft_increment\"(i32 %arg0)") !=
      std::string::npos);
}

// GNU AAPCS64 deliberately omits Darwin's narrow-integer extension
// attributes. Clang 22 emits the same source-width LLVM parameters and results
// for signed and unsigned values; testing the complete declaration and wrapper
// keeps call sites and definitions synchronized.
void test_linux_narrow_integer_abi(TestState &state) {
  CheckedSource source(R"draft(
package native

foreign linux {
    narrow :: c "narrow" proc(signed: i8, unsigned: u16) -> i8
}

export wrap_narrow :: c "wrap_narrow" proc(
    signed: i8,
    unsigned: u16,
) -> i8 {
    return narrow(signed, unsigned)
}
)draft", draft::make_aarch64_linux_profile());

  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package,
      source.hir,
      source.target.facts,
      source.diagnostics);
  const draft::MirLoweringResult mir = draft::lower_package_to_mir(
      source.semantics.package, source.hir, source.diagnostics);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "native"};
  const draft::LlvmIrResult llvm = draft::emit_llvm_ir(
      source.target,
      source.sources,
      options,
      source.semantics.package,
      source.semantics.global_initializers,
      mir.program,
      source.diagnostics);
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        source.sources, source.diagnostics);
  }
  EXPECT(state, native.ok);
  EXPECT(state, mir.ok);
  EXPECT(state, llvm.ok);
  EXPECT(state, llvm.text.find(
      "declare i8 @\"narrow\"(i8, i16)") != std::string::npos);
  EXPECT(state, llvm.text.find(
      "define i8 @\"wrap_narrow\"(i8 %arg0, i16 %arg1)") !=
          std::string::npos);
  EXPECT(state, llvm.text.find("signext") == std::string::npos);
  EXPECT(state, llvm.text.find("zeroext") == std::string::npos);
}

void test_invalid_c_boundaries(TestState &state) {
  CheckedSource source(R"draft(
package native

export ordinary :: proc() {
}

foreign provider {
    bad_slice :: c proc(value: []u8)
}
)draft");
  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package,
      source.hir,
      source.target.facts,
      source.diagnostics);
  EXPECT(state, !native.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("C-ABI-legal 'c proc'") != std::string::npos);
}

void test_aggregate_c_abi_lowering(TestState &state) {
  CheckedSource source(R"draft(
package native

C8 :: @repr(C) struct {
    left: i32,
    right: i32,
}

C3 :: @repr(C) struct {
    bytes: [3]u8,
}

C16_Aligned :: @repr(C) @align(16) struct {
    word: i64,
}

HF2 :: @repr(C) struct {
    left: f32,
    right: f32,
}

HH3 :: @repr(C) struct {
    values: [3]f16,
}

Float_Overlay :: @repr(C) raw union {
    scalar: f32,
    pair: [2]f32,
}

Unsigned_Code :: @repr(C) enum u8 {
    zero,
    one,
}

Signed_Code :: @repr(C) enum i8 {
    negative = -1,
    zero = 0,
    positive = 1,
}

C24 :: @repr(C) struct {
    words: [3]i64,
}

foreign provider {
    small :: c "small" proc(value: C8) -> C8
    odd :: c "odd" proc(value: C3) -> C3
    aligned :: c "aligned" proc(value: C16_Aligned) -> C16_Aligned
    floats :: c "floats" proc(value: HF2) -> HF2
    halves :: c "halves" proc(value: HH3) -> HH3
    float_overlay :: c "float_overlay" proc(value: Float_Overlay) -> Float_Overlay
    unsigned_code :: c "unsigned_code" proc(value: Unsigned_Code) -> Unsigned_Code
    signed_code :: c "signed_code" proc(value: Signed_Code) -> Signed_Code
    large :: c "large" proc(value: C24) -> C24
    narrow :: c "narrow" proc(signed: i8, unsigned: u16) -> i8
}

export wrap_small :: c "wrap_small" proc(value: C8) -> C8 {
    return small(value)
}

export wrap_odd :: c "wrap_odd" proc(value: C3) -> C3 {
    return odd(value)
}

export wrap_aligned :: c "wrap_aligned" proc(value: C16_Aligned) -> C16_Aligned {
    return aligned(value)
}

export wrap_floats :: c "wrap_floats" proc(value: HF2) -> HF2 {
    return floats(value)
}

export wrap_halves :: c "wrap_halves" proc(value: HH3) -> HH3 {
    return halves(value)
}

export wrap_float_overlay :: c "wrap_float_overlay" proc(
    value: Float_Overlay,
) -> Float_Overlay {
    return float_overlay(value)
}

export wrap_unsigned_code :: c "wrap_unsigned_code" proc(
    value: Unsigned_Code,
) -> Unsigned_Code {
    return unsigned_code(value)
}

export wrap_signed_code :: c "wrap_signed_code" proc(
    value: Signed_Code,
) -> Signed_Code {
    return signed_code(value)
}

export wrap_large :: c "wrap_large" proc(value: C24) -> C24 {
    return large(value)
}
)draft");

  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package,
      source.hir,
      source.target.facts,
      source.diagnostics);
  const draft::MirLoweringResult mir = draft::lower_package_to_mir(
      source.semantics.package, source.hir, source.diagnostics);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "native"};
  const draft::LlvmIrResult llvm = draft::emit_llvm_ir(
      source.target,
      source.sources,
      options,
      source.semantics.package,
      source.semantics.global_initializers,
      mir.program,
      source.diagnostics);
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, native.ok);
  EXPECT(state, mir.ok);
  EXPECT(state, llvm.ok);
  EXPECT(state, llvm.text.find("declare i64 @\"small\"(i64)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("define i64 @\"wrap_small\"(i64 %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("declare i24 @\"odd\"(i64)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("define i24 @\"wrap_odd\"(i64 %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("declare i128 @\"aligned\"(i128)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("define i128 @\"wrap_aligned\"(i128 %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("= type <{ i64, [8 x i8] }>") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("@\"floats\"([2 x float]") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("@\"wrap_floats\"([2 x float] %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("@\"halves\"([3 x half]") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("@\"wrap_halves\"([3 x half] %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("@\"float_overlay\"([2 x float]") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find(
                    "@\"wrap_float_overlay\"([2 x float] %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find(
                    "declare zeroext i8 @\"unsigned_code\"(i8 zeroext)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find(
                    "define zeroext i8 @\"wrap_unsigned_code\"(i8 zeroext %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find(
                    "declare signext i8 @\"signed_code\"(i8 signext)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find(
                    "define signext i8 @\"wrap_signed_code\"(i8 signext %arg0)") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("declare void @\"large\"(ptr sret(") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find("define void @\"wrap_large\"(ptr sret(") !=
                    std::string::npos);
  EXPECT(state, llvm.text.find(
                    "declare signext i8 @\"narrow\"(i8 signext, i16 zeroext)") !=
                    std::string::npos);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_invalid_c_aggregate_member(TestState &state) {
  CheckedSource source(R"draft(
package native

Bad :: @repr(C) struct {
    view: []u8,
}

foreign provider {
    consume :: c proc(value: Bad)
}
)draft");
  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package,
      source.hir,
      source.target.facts,
      source.diagnostics);
  EXPECT(state, !native.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("C-ABI-legal 'c proc'") != std::string::npos);
}

void test_invalid_local_c_procedure_type(TestState &state) {
  CheckedSource source(R"draft(
package native

Bad_Callback :: c proc(value: []u8)

bad_callback :: c proc(value: []u8) {
}

callback_slot: Bad_Callback
)draft");
  const draft::NativeInteropResult native = draft::validate_native_interop(
      source.semantics.package,
      source.hir,
      source.target.facts,
      source.diagnostics);
  EXPECT(state, !native.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find(
                    "c proc parameter and result types must be Draft 1 C-ABI-legal") !=
                    std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_valid_import_and_export(state);
  test_linux_narrow_integer_abi(state);
  test_invalid_c_boundaries(state);
  test_aggregate_c_abi_lowering(state);
  test_invalid_c_aggregate_member(state);
  test_invalid_local_c_procedure_type(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " native interop expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all native interop tests passed\n";
  return EXIT_SUCCESS;
}
