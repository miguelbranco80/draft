// Typed procedure-body HIR and common diagnostic tests.

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
      std::cerr << "body_checker_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct CheckedSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticAnalysisResult semantics;
  draft::BodyCheckResult bodies;

  explicit CheckedSource(std::string text) {
    loaded.short_name = "bodies";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    const draft::TargetProfile target = draft::make_aarch64_macos_profile();
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
  }
};

void test_common_typed_bodies(TestState &state) {
  CheckedSource source(R"draft(
package bodies

Pair :: struct {
    left: u64,
    right: u64,
}

Mode :: enum {
    Off,
    On,
}

consume :: proc(value: i64) {
}

add :: proc(a, b: i64) -> i64 {
    sum := a + b
    sum += 1
    defer consume(sum)
    if sum > 0 {
        return sum
    } else {
        return 0
    }
}

read_left :: proc(pair: ^Pair) -> u64 {
    return pair^.left
}

loop_sum :: proc(values: []i64) -> i64 {
    total: i64
    for value, index in values {
        total += value
        if index > 10 {
            break
        }
    }
    for total < 100 {
        total += 1
    }
    for i: i64 = 0; i < 3; i += 1 {
        total += i
    }
    return total
}

choose :: proc(mode: Mode) -> i64 {
    switch mode {
    case .Off:
        return 0
    case .On:
        return 1
    }
    return -1
}

views :: proc() -> usize {
    local: [3]u64 = [3]u64{1, 2, 3}
    part := local[1:]
    assert(len(part) > 0, "slice must not be empty")
    pointer: ^u64 = nil
    pair := Pair{left = local[0], right = 2}
    tuple: (u64, usize) = (pair.left, len(part))
    return cast[usize](tuple.0)
}

main :: proc() {
    pair: Pair
    pair.left = 1
    value := add(20, 21)
    when target.pointer_bits == 64 {
        selected: u64 = pair.left
    } else {
        selected: Does_Not_Exist
    }
    pair.right = selected
    if value > 0 {
        pair.right = pair.left
    }
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.bodies.checked_procedures == 7);
  EXPECT(state, source.bodies.program.procedures().size() == 7);
  EXPECT(state, source.bodies.program.expression_count() >= 55);
  EXPECT(state, source.bodies.program.statement_count() >= 28);
  EXPECT(state, source.bodies.program.block_count() >= 12);
  for (const draft::HirProcedure &procedure : source.bodies.program.procedures()) {
    EXPECT(state, procedure.valid);
    EXPECT(state, procedure.body.is_valid());
  }


  bool saw_add = false;
  bool saw_greater = false;
  bool saw_compound_add = false;
  bool saw_switch_shape = false;
  for (std::size_t index = 0; index < source.bodies.program.expression_count(); ++index) {
    const draft::HirExpression &expression =
        source.bodies.program.expression(draft::HirExpressionId{
            static_cast<std::uint32_t>(index)});
    saw_add = saw_add ||
        (expression.kind == draft::HirExpressionKind::Binary &&
         expression.operation == draft::HirOperation::Add);
    saw_greater = saw_greater ||
        (expression.kind == draft::HirExpressionKind::Binary &&
         expression.operation == draft::HirOperation::Greater);
  }
  for (std::size_t index = 0; index < source.bodies.program.statement_count(); ++index) {
    const draft::HirStatement &statement =
        source.bodies.program.statement(draft::HirStatementId{
            static_cast<std::uint32_t>(index)});
    saw_compound_add = saw_compound_add ||
        (statement.kind == draft::HirStatementKind::Assignment &&
         statement.operation == draft::HirOperation::Add);
    if (statement.kind == draft::HirStatementKind::Switch) {
      saw_switch_shape = statement.switch_cases.size() == 2 &&
          statement.switch_cases[0].label_count == 1 &&
          statement.switch_cases[1].label_count == 1 &&
          !statement.switch_cases[0].is_default &&
          !statement.switch_cases[1].is_default;
    }
  }
  EXPECT(state, saw_add);
  EXPECT(state, saw_greater);
  EXPECT(state, saw_compound_add);
  EXPECT(state, saw_switch_shape);
}

void test_body_diagnostics(TestState &state) {
  CheckedSource source(R"draft(
package bodies

bad :: proc(flag: int) -> int {
    too_large: u8 = 256
    if flag {
        missing := unknown_name
    }
    1 = 2
    return
}

falls_through :: proc(flag: bool) -> int {
    if flag {
        return 1
    }
}
)draft");

  EXPECT(state, !source.bodies.ok);
  EXPECT(state, source.diagnostics.error_count() >= 5);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("expected type 'bool'") != std::string::npos);
  EXPECT(state, rendered.find("unknown name 'unknown_name'") != std::string::npos);
  EXPECT(state, rendered.find("not addressable") != std::string::npos);
  EXPECT(state, rendered.find("return requires a value") != std::string::npos);
  EXPECT(state, rendered.find("not every path returns") != std::string::npos);
  EXPECT(state, rendered.find("not representable") != std::string::npos);
}

void test_parametric_procedure_instances(TestState &state) {
  CheckedSource source(R"draft(
package bodies

identity[T: type] :: proc(value: ^T) -> ^T {
    return value
}

sum[T: number] :: proc(values: []T) -> T {
    result: T
    for value in values {
        result += value
    }
    return result
}

main :: proc() -> i64 {
    value: u32 = 42
    explicit := identity[u32](&value)
    inferred := identity(&value)
    values := [3]i64{1, 2, 3}
    if explicit^ != inferred^ {
        return 0
    }
    return sum[i64](values[:])
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.bodies.checked_procedures == 5);
  EXPECT(state, source.bodies.program.procedures().size() == 5);
  std::size_t templates = 0;
  std::size_t concrete_instances = 0;
  for (const draft::HirProcedure &procedure :
       source.bodies.program.procedures()) {
    if (procedure.parametric_template) {
      ++templates;
    } else {
      const draft::Symbol &symbol =
          source.semantics.package.symbols.symbol(procedure.symbol);
      if (symbol.name.find("$instance") != std::string::npos) {
        ++concrete_instances;
        const draft::Type &signature =
            source.semantics.package.types.type(procedure.type);
        for (draft::TypeId member : signature.members) {
          EXPECT(
              state,
              source.semantics.package.types.type(member).kind !=
                  draft::TypeKind::TypeParameter);
        }
      }
    }
  }
  EXPECT(state, templates == 2);
  EXPECT(state, concrete_instances == 2);
}

void test_definite_initialization(TestState &state) {
  CheckedSource safe(R"draft(
package bodies

fill :: proc(value: ^int) {
}

assigned :: proc() -> int {
    value: int = ---
    value = 42
    return value
}

assigned_on_both_paths :: proc(flag: bool) -> int {
    value: int = ---
    if flag {
        value = 1
    } else {
        value = 2
    }
    return value
}

address_may_initialize :: proc() -> int {
    value: int = ---
    fill(&value)
    return value
}
)draft");
  if (safe.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(safe.sources, safe.diagnostics);
  }
  EXPECT(state, safe.bodies.ok);
  EXPECT(state, !safe.diagnostics.has_errors());

  CheckedSource unsafe(R"draft(
package bodies

direct :: proc() -> int {
    value: int = ---
    return value
}

loop_may_not_run :: proc(flag: bool) -> int {
    value: int = ---
    for flag {
        value = 1
    }
    return value
}

compound_reads_first :: proc() -> int {
    value: int = ---
    value += 1
    return value
}
)draft");
  EXPECT(state, !unsafe.bodies.ok);
  EXPECT(state, unsafe.diagnostics.error_count() == 3);
  const std::string rendered =
      draft::render_diagnostics(unsafe.sources, unsafe.diagnostics);
  EXPECT(state, rendered.find("read of uninitialized local 'value'") !=
                    std::string::npos);
}

void test_layout_intrinsics_and_static_assert(TestState &state) {
  CheckedSource safe(R"draft(
package bodies

Header :: struct {
    tag: u8,
    value: u64,
}

main :: proc() -> usize {
    static_assert(size_of(Header) == 16)
    static_assert(align_of(Header) == 8, "Header alignment changed")
    return size_of(Header) + align_of(Header)
}
)draft");
  if (safe.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(safe.sources, safe.diagnostics);
  }
  EXPECT(state, safe.bodies.ok);
  EXPECT(state, !safe.diagnostics.has_errors());

  CheckedSource failing(R"draft(
package bodies

main :: proc() {
    static_assert(size_of(u64) == 4, "u64 layout mismatch")
}
)draft");
  EXPECT(state, !failing.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(failing.sources, failing.diagnostics);
  EXPECT(state, rendered.find("static assertion failed: u64 layout mismatch") !=
                    std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_common_typed_bodies(state);
  test_body_diagnostics(state);
  test_parametric_procedure_instances(state);
  test_definite_initialization(state);
  test_layout_intrinsics_and_static_assert(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " body checker expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all body checker tests passed\n";
  return EXIT_SUCCESS;
}
