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
#include <vector>

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

// Declaration semantics are an immutable input generation. This regression
// deliberately creates body-local scopes, constants, and a concrete static-pack
// specialization, then runs the complete body phase twice from the same
// declaration result. Both outputs must be complete and equal in shape while
// the declaration tables remain byte-for-byte-sized at their original
// boundaries. Re-entering the first enriched result would either duplicate the
// specialization or make the second HIR refer into stale append-only tables.
void test_body_results_do_not_mutate_or_reenter_declarations(TestState &state) {
  CheckedSource source(R"draft(
package bodies

render :: proc(values: ..type) {
    for value, index in values {
        local :: index + 1
        when type_of(value) == string {
            _ = local
        } else when type_kind(type_of(value)) == .signed_integer {
            _ = local
        } else {
            static_assert(false, "unsupported value")
        }
    }
}

main :: proc() {
    render("draft", 42)
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  const std::size_t declaration_symbols =
      source.semantics.package.symbols.symbol_count();
  const std::size_t declaration_scopes =
      source.semantics.package.symbols.scope_count();
  const std::size_t declaration_types = source.semantics.package.types.size();
  const std::size_t declaration_constants =
      source.semantics.constants.bindings.size();
  EXPECT(state, source.semantics.package.parametric_instances.empty());
  EXPECT(state, source.bodies.package.parametric_instances.size() == 1);
  EXPECT(state,
         source.bodies.package.symbols.symbol_count() > declaration_symbols);
  EXPECT(state,
         source.bodies.package.symbols.scope_count() > declaration_scopes);
  EXPECT(state,
         source.bodies.constants.bindings.size() > declaration_constants);

  draft::DiagnosticSink repeated_diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  const draft::BodyCheckResult repeated = draft::check_package_bodies(
      source.sources, source.loaded, source.semantics.selections,
      source.semantics.package, source.semantics.constants, target.facts,
      repeated_diagnostics);
  if (repeated_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources,
                                           repeated_diagnostics);
  }
  EXPECT(state, repeated.ok);
  EXPECT(state, !repeated_diagnostics.has_errors());
  EXPECT(state, repeated.package.symbols.symbol_count() ==
                    source.bodies.package.symbols.symbol_count());
  EXPECT(state, repeated.package.symbols.scope_count() ==
                    source.bodies.package.symbols.scope_count());
  EXPECT(state,
         repeated.package.types.size() == source.bodies.package.types.size());
  EXPECT(state, repeated.constants.bindings.size() ==
                    source.bodies.constants.bindings.size());
  EXPECT(state, repeated.program.procedures().size() ==
                    source.bodies.program.procedures().size());
  EXPECT(state,
         repeated.checked_procedures == source.bodies.checked_procedures);

  EXPECT(state, source.semantics.package.symbols.symbol_count() ==
                    declaration_symbols);
  EXPECT(state,
         source.semantics.package.symbols.scope_count() == declaration_scopes);
  EXPECT(state, source.semantics.package.types.size() == declaration_types);
  EXPECT(state,
         source.semantics.constants.bindings.size() == declaration_constants);
  EXPECT(state, source.semantics.package.parametric_instances.empty());
}

// One root check must write only its returned task slot. The coordinator then
// adopts that successor explicitly. This is the ownership seam required before
// several body tasks can eventually share one immutable prefix and run in
// parallel; mutating PackageBodyWorkState from the worker would make result
// order observable even with task-local diagnostics.
void test_body_root_results_are_task_owned(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "bodies";
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source("package.draft", R"draft(
package bodies

Record :: struct {
    value: i64,
}

Mode :: enum {
    off,
    on,
}

first :: proc() -> i64 {
    local: i64 = 20
    return local
}

second :: proc() -> i64 {
    local: i64 = 22
    return local
}
)draft");
  file.syntax.emplace(
      draft::parse_source_file(sources, file.source, diagnostics));
  loaded.files.push_back(std::move(file));
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  const draft::SemanticAnalysisResult semantics =
      draft::analyze_package_semantics(
          sources, loaded, target.facts, diagnostics);
  draft::PackageBodyWorkState work = draft::begin_package_body_work(
      sources,
      loaded,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  EXPECT(state, work.ok);
  EXPECT(state, work.work.size() == 2);
  EXPECT(state, work.next_work == 0);
  const std::size_t initial_symbols = work.package.symbols.symbol_count();
  const std::size_t initial_scopes = work.package.symbols.scope_count();
  EXPECT(state, !work.package.aggregate_members.empty());
  EXPECT(state, !work.package.enum_member_values.empty());

  // Layout publication addresses the combined table by its global index, but
  // a task may mutate only a row it appended. Prove that the explicit mapper
  // reaches the local suffix without changing the retained prefix row.
  draft::SemanticPackage member_view = work.package.fork_body_task_view();
  const std::size_t retained_member_count = work.package.aggregate_members.size();
  const std::uint64_t retained_offset =
      work.package.aggregate_members.front().offset;
  member_view.aggregate_members.push_back(
      work.package.aggregate_members.front());
  member_view.aggregate_member_mut(retained_member_count).offset =
      retained_offset + 1;
  EXPECT(state,
         work.package.aggregate_members.front().offset == retained_offset);
  EXPECT(state,
         member_view.aggregate_members_for_read()[retained_member_count].offset ==
             retained_offset + 1);

  // Specialization records use the same combined global index domain. A task
  // reads retained instances, owns new records, and can promote only a record
  // in that local suffix.
  draft::SemanticPackage specialization_prefix;
  specialization_prefix.parametric_instances.push_back({});
  specialization_prefix.parametric_type_instances.push_back({});
  draft::SemanticPackage specialization_view =
      specialization_prefix.fork_body_task_view();
  EXPECT(state, specialization_view.parametric_instances.empty());
  EXPECT(state,
         specialization_view.parametric_instances_for_read().size() == 1);
  EXPECT(state, specialization_view.parametric_type_instances.empty());
  EXPECT(state,
         specialization_view.parametric_type_instances_for_read().size() == 1);
  specialization_view.parametric_instances.push_back({});
  specialization_view.parametric_instance_mut(1).externally_requested = true;
  EXPECT(state,
         !specialization_prefix.parametric_instances.front()
              .externally_requested);
  EXPECT(state,
         specialization_view.parametric_instances_for_read()[1]
             .externally_requested);

  draft::DiagnosticSink first_diagnostics;
  draft::ProcedureBodyTaskInput first_input =
      draft::take_next_procedure_body_work(work, first_diagnostics);
  EXPECT(state, first_input.valid);
  EXPECT(state, work.active_work == std::optional<std::size_t>{0});
  EXPECT(state, first_input.package.files.empty());
  EXPECT(state,
         first_input.package.files_for_read().size() ==
             work.package.files.size());
  EXPECT(state, first_input.package.owned_scopes.empty());
  EXPECT(state,
         first_input.package.owned_scopes_for_read().size() ==
             work.package.owned_scopes.size());
  EXPECT(state, first_input.package.aggregate_members.empty());
  EXPECT(state,
         first_input.package.aggregate_members_for_read().size() ==
             work.package.aggregate_members.size());
  EXPECT(state, first_input.package.enum_member_values.empty());
  EXPECT(state,
         first_input.package.enum_member_values_for_read().size() ==
             work.package.enum_member_values.size());
  EXPECT(state, first_input.package.parametric_parameters.empty());
  EXPECT(state,
         first_input.package.parametric_parameters_for_read().size() ==
             work.package.parametric_parameters.size());
  EXPECT(state, first_input.package.static_argument_packs.empty());
  EXPECT(state,
         first_input.package.static_argument_packs_for_read().size() ==
             work.package.static_argument_packs.size());
  EXPECT(state, first_input.package.symbols.symbol_count() == initial_symbols);
  EXPECT(state, first_input.package.symbols.scope_count() == initial_scopes);
  draft::ProcedureBodyTaskResult first =
      draft::check_procedure_body_work(
          sources,
          loaded,
          semantics.selections,
          target.facts,
          std::move(first_input),
          first_diagnostics);
  if (first_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, first_diagnostics);
  }
  EXPECT(state, first.ok);
  EXPECT(state, !first_diagnostics.has_errors());
  EXPECT(state, work.next_work == 0);
  EXPECT(state, work.procedures.empty());
  EXPECT(state, first.program.procedures().size() == 1);
  EXPECT(state, work.package.symbols.symbol_count() == initial_symbols);
  EXPECT(state, work.package.symbols.scope_count() == initial_scopes);
  EXPECT(state,
         first.semantic.prefix.symbol_count == initial_symbols);
  EXPECT(state,
         first.semantic.prefix.scope_count == initial_scopes);
  EXPECT(state, !first.semantic.symbols.symbols.empty());
  EXPECT(state, !first.semantic.symbols.scopes.empty());

  // A result may publish only against the exact canonical prefix it read.
  // Simulate an intervening coordinator publication on a copy and prove the
  // stale packet is rejected without disturbing the real state/result used by
  // the remainder of this test.
  draft::PackageBodyWorkState stale_work = work;
  stale_work.constants.bindings.push_back(
      {draft::SymbolId{}, draft::ConstantValue{}});
  draft::ProcedureBodyTaskResult stale_first = first;
  draft::DiagnosticSink stale_diagnostics;
  EXPECT(state, !draft::publish_procedure_body_work(
                    stale_work, std::move(stale_first), stale_diagnostics));
  EXPECT(state, stale_diagnostics.has_errors());

  EXPECT(state, draft::publish_procedure_body_work(
                    work, std::move(first), first_diagnostics));
  EXPECT(state, work.next_work == 1);
  EXPECT(state, !work.active_work.has_value());
  EXPECT(state, work.package.symbols.symbol_count() > initial_symbols);
  EXPECT(state, work.package.symbols.scope_count() > initial_scopes);
  EXPECT(state, work.procedures.size() == 1);
  if (work.procedures.size() == 1) {
    EXPECT(state, work.procedures.front().program.procedures().size() == 1);
  }
  const std::size_t published_symbols = work.package.symbols.symbol_count();

  draft::DiagnosticSink second_diagnostics;
  draft::ProcedureBodyTaskInput second_input =
      draft::take_next_procedure_body_work(work, second_diagnostics);
  EXPECT(state, second_input.valid);
  EXPECT(state, work.active_work == std::optional<std::size_t>{1});
  EXPECT(state,
         second_input.package.symbols.symbol_count() == published_symbols);
  draft::ProcedureBodyTaskResult second =
      draft::check_procedure_body_work(
          sources,
          loaded,
          semantics.selections,
          target.facts,
          std::move(second_input),
          second_diagnostics);
  EXPECT(state, second.ok);
  EXPECT(state, work.next_work == 1);
  EXPECT(state, work.procedures.size() == 1);
  EXPECT(state, second.program.procedures().size() == 1);
  EXPECT(state,
         work.package.symbols.symbol_count() == published_symbols);
  EXPECT(state, draft::publish_procedure_body_work(
                    work, std::move(second), second_diagnostics));

  const draft::BodyCheckResult bodies = draft::finish_package_body_work(
      loaded, target.facts, std::move(work), diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, bodies.ok);
  EXPECT(state, bodies.checked_procedures == 2);
  EXPECT(state, bodies.procedures.size() == 2);
  EXPECT(state, bodies.program.procedures().size() == 2);
  if (bodies.procedures.size() == 2 &&
      bodies.program.procedures().size() == 2) {
    const draft::HirProgram &first_local = bodies.procedures[0].program;
    const draft::HirProgram &second_local = bodies.procedures[1].program;
    EXPECT(state, first_local.procedures().size() == 1);
    EXPECT(state, second_local.procedures().size() == 1);
    if (first_local.procedures().size() != 1 ||
        second_local.procedures().size() != 1) {
      return;
    }
    const draft::HirProcedure &second_local_procedure =
        second_local.procedures().front();
    const draft::HirProcedure &second_aggregate_procedure =
        bodies.program.procedures()[1];
    EXPECT(state,
           second_aggregate_procedure.body.value ==
               second_local_procedure.body.value + first_local.block_count());

    // The second procedure's final return statement and its symbol expression
    // prove that the projection rewrites all three HIR-local ID domains rather
    // than only the procedure root block.
    const draft::HirBlock &second_local_body =
        second_local.block(second_local_procedure.body);
    const draft::HirBlock &second_aggregate_body =
        bodies.program.block(second_aggregate_procedure.body);
    EXPECT(state, second_local_body.statements.size() == 2);
    EXPECT(state, second_aggregate_body.statements.size() == 2);
    if (second_local_body.statements.size() == 2 &&
        second_aggregate_body.statements.size() == 2) {
      EXPECT(state,
             second_aggregate_body.statements.back().value ==
                 second_local_body.statements.back().value +
                     first_local.statement_count());
      const draft::HirStatement &second_local_return =
          second_local.statement(second_local_body.statements.back());
      const draft::HirStatement &second_aggregate_return =
          bodies.program.statement(second_aggregate_body.statements.back());
      EXPECT(state, second_local_return.expressions.size() == 1);
      EXPECT(state, second_aggregate_return.expressions.size() == 1);
      if (second_local_return.expressions.size() == 1 &&
          second_aggregate_return.expressions.size() == 1) {
        EXPECT(state,
               second_aggregate_return.expressions.front().value ==
                   second_local_return.expressions.front().value +
                       first_local.expression_count());
      }
    }
  }
}

void test_common_typed_bodies(TestState &state) {
  CheckedSource source(R"draft(
package bodies

OS_Type_Value :: type_of(target.os)

Box[T: type] :: struct {
    value: T,
}

selected_os: OS_Type_Value
selected_box: Box[OS_Type_Value]

Pair :: struct {
    left: u64,
    right: u64,
}

Mode :: enum {
    Off,
    On,
}

Choice :: union {
    none,
    some: i64,
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
}

unwrap :: proc(choice: Choice) -> i64 {
    switch choice {
    case .some(value):
        return value
    case .none:
        return 0
    }
}

views :: proc() -> usize {
    local: [3]u64 = [3]u64{1, 2, 3}
    part := local[1:]
    assert(len(part) > 0, "slice must not be empty")
    pointer: ^u64 = nil
    pair := Pair{left = local[0], right = 2}
    tuple: (u64, usize) = (pair.left, len(part))
    (first, _) := (40, 2)
    (_, second): (u64, usize) = tuple
    text := "draft"
    middle := text[1:4]
    assert(text[0] == cast[u8]('d'))
    multi_pointer := cast[[^]u64](&local[0])
    pointer_view := multi_pointer[:3]
    assert(pointer_view[2] == 3)
    return cast[usize](tuple.0) + cast[usize](first) + second +
        len(middle) + len(pointer_view)
}

rune_value :: proc() -> rune {
    assert('é' == '\u{e9}')
    return '🙂'
}

main :: proc() {
    Local_OS :: type_of(target.os)
    local_os: Local_OS = target.os
    current_os: OS_Type_Value = target.os
    current_box: Box[OS_Type_Value] =
        Box[OS_Type_Value]{value = target.os}
    assert(current_os == .macos)
    assert(local_os == .macos)
    assert(current_box.value == .macos)
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
  EXPECT(state, source.bodies.checked_procedures == 9);
  EXPECT(state, source.bodies.program.procedures().size() == 9);
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
  bool saw_tuple_destructuring = false;
  bool saw_union_payload_binding = false;
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
    saw_tuple_destructuring = saw_tuple_destructuring ||
        (statement.kind == draft::HirStatementKind::LocalDeclaration &&
         statement.local_destructures_tuple &&
         statement.bindings.size() == 1 &&
         statement.binding_member_indices.size() == 1);
    if (statement.kind == draft::HirStatementKind::Switch) {
      saw_switch_shape = saw_switch_shape || (statement.switch_cases.size() == 2 &&
          statement.switch_cases[0].label_count == 1 &&
          statement.switch_cases[1].label_count == 1 &&
          !statement.switch_cases[0].is_default &&
          !statement.switch_cases[1].is_default);
      for (const draft::HirSwitchCase &switch_case : statement.switch_cases) {
        saw_union_payload_binding = saw_union_payload_binding ||
            (switch_case.payload_alternative.is_valid() &&
             switch_case.payload_binding.is_valid());
      }
    }
  }
  EXPECT(state, saw_add);
  EXPECT(state, saw_greater);
  EXPECT(state, saw_compound_add);
  EXPECT(state, saw_switch_shape);
  EXPECT(state, saw_tuple_destructuring);
  EXPECT(state, saw_union_payload_binding);
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
    static_assert(size_of(T) > 0)
    return value
}

sum[T: number] :: proc(values: []T) -> T {
    result: T
    for value in values {
        result += value
    }
    return result
}

Box[T: type] :: struct {
    value: T,
}

store[T: type] :: proc(box: ^Box[T], value: T) {
    box^.value = value
}

last[N: usize] :: proc(values: [N]i64) -> i64 {
    static_assert(N > 0)
    return values[N - 1]
}

plus_one :: proc(value: usize) -> usize {
    return value + 1
}

procedural_last[N: usize] :: proc(values: [plus_one(N)]i64) -> i64 {
    return values[N]
}

pick_count[N: usize] :: proc() -> usize {
    return N
}

composed_count[N: usize] :: proc() -> usize {
    return pick_count[plus_one(N)]()
}

zero_values[N: usize] :: proc() -> [N]i64 {
    return [N]i64{}
}

composed_length[N: usize] :: proc() -> usize {
    values := zero_values[plus_one(N)]()
    return len(values)
}

layout_count[T: type] :: proc(extra: usize) -> usize {
    return size_of(T) + extra
}

composed_type_count[T: type, N: usize] :: proc() -> usize {
    return pick_count[layout_count[T](N)]()
}

main :: proc() -> i64 {
    value: u32 = 42
    explicit := identity[u32](&value)
    inferred := identity(&value)
    box: Box[u32]
    store(&box, 42)
    values := [3]i64{1, 2, 3}
    if explicit^ != inferred^ {
        return 0
    }
    explicit_last := last[3](values)
    inferred_last := last(values)
    procedural_last_value := procedural_last[2](values)
    return sum[i64](values[:]) + explicit_last + inferred_last +
        procedural_last_value + cast[i64](composed_count[2]()) +
        cast[i64](composed_length[2]()) +
        cast[i64](composed_type_count[u32, 2]())
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.bodies.checked_procedures == 24);
  EXPECT(state, source.bodies.program.procedures().size() == 24);
  std::size_t templates = 0;
  std::size_t concrete_instances = 0;
  for (const draft::HirProcedure &procedure :
       source.bodies.program.procedures()) {
    if (procedure.parametric_template) {
      ++templates;
    } else {
      const draft::Symbol &symbol =
          source.bodies.package.symbols.symbol(procedure.symbol);
      if (symbol.name.find("$instance") != std::string::npos) {
        ++concrete_instances;
        const draft::Type &signature =
            source.bodies.package.types.type(procedure.type);
        for (draft::TypeId member : signature.members) {
          EXPECT(
              state,
              source.bodies.package.types.type(member).kind !=
                  draft::TypeKind::TypeParameter);
        }
      }
    }
  }
  EXPECT(state, templates == 11);
  EXPECT(state, concrete_instances == 11);
}

void test_static_argument_pack_instances(TestState &state) {
  CheckedSource source(R"draft(
package bodies

inspect_all :: proc(values: ..type) {
    static_assert(len(values) >= 0)
    for value, index in values {
        ... "retain one synthesis site for the source pack body"
        judge "the pack element remains valid"
        when index == 0 {
            static_assert(index == 0)
        } else {
            static_assert(index > 0)
        }
        when type_of(value) == string {
            len(value)
        } else when type_of(value) == bool {
            !value
        } else when type_kind(type_of(value)) == .signed_integer {
            value + value
        } else when type_kind(type_of(value)) == .unsigned_integer {
            value + value
        } else when type_kind(type_of(value)) == .float {
            value + value
        } else {
            static_assert(false, "unsupported pack element")
        }
    }
}

inspect_after[T: type] :: proc(first: T, values: ..type) {
    inspect_all(first)
    for value in values {
        inspect_all(value)
    }
}

main :: proc() {
    inspect_nested :: proc(values: ..type) {
        for value, index in values {
            when index == 0 {
                static_assert(type_of(value) == string)
            } else when type_of(value) == bool {
                !value
            } else {
                static_assert(false, "unsupported nested pack element")
            }
        }
    }

    inspect_all()
    inspect_all(1, true, "draft")
    // The concrete values differ, but the ordered tail types are identical and
    // therefore reuse one specialization.
    inspect_all(2, false, "again")
    inspect_after(cast[u8](7), 1.5)
    inspect_after[u8](cast[u8](8), 2.5)
    inspect_nested("nested", true)
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  std::size_t pack_instances = 0;
  bool saw_empty = false;
  bool saw_mixed = false;
  bool saw_nested = false;
  for (const draft::ParametricInstanceRecord &instance :
       source.bodies.package.parametric_instances) {
    if (instance.pack_types.empty()) {
      saw_empty = true;
    }
    if (instance.pack_types.size() == 3) {
      saw_mixed = true;
      EXPECT(state,
          source.bodies.package.types.type(instance.pack_types[0]).name ==
              "int");
      EXPECT(state,
          source.bodies.package.types.type(instance.pack_types[1]).name ==
              "bool");
      EXPECT(state,
          source.bodies.package.types.type(instance.pack_types[2]).name ==
              "string");
    }
    if (!instance.pack_types.empty() ||
        source.bodies.package.symbols.symbol(instance.source).name ==
            "inspect_all") {
      ++pack_instances;
    }
    if (source.bodies.package.symbols.symbol(instance.source).name ==
        "inspect_nested") {
      saw_nested = true;
      EXPECT(state, instance.pack_types.size() == 2);
    }
    const draft::Type &signature = source.bodies.package.types.type(
        source.bodies.package.symbols.symbol(instance.instance).type);
    EXPECT(state, signature.members.size() >= instance.pack_types.size() + 1);
  }
  EXPECT(state, saw_empty);
  EXPECT(state, saw_mixed);
  EXPECT(state, saw_nested);
  EXPECT(state, pack_instances >= 3);
  std::size_t judgment_sites = 0;
  std::size_t synthesis_sites = 0;
  for (const draft::SemanticSite &site : source.bodies.package.sites) {
    if (site.kind == draft::SemanticSiteKind::Judgment) ++judgment_sites;
    if (site.kind == draft::SemanticSiteKind::SynthesisStatement) {
      ++synthesis_sites;
    }
  }
  EXPECT(state, judgment_sites == 1);
  EXPECT(state, synthesis_sites == 1);
}

void test_static_argument_pack_use_diagnostics(TestState &state) {
  CheckedSource source(R"draft(
package bodies

inspect_all :: proc(values: ..type) {
    values
}

outer :: proc(values: ..type) {
    inner :: proc() {
        values
    }
    inner()
}

control :: proc(values: ..type) {
    for value in values {
        break
        continue
    }
}

main :: proc() {
    callback := inspect_all
    inspect_all[u8](1)
}
)draft");

  EXPECT(state, !source.bodies.ok);
  for (const draft::Diagnostic &diagnostic :
       source.diagnostics.diagnostics()) {
    if (diagnostic.severity == draft::DiagnosticSeverity::Error) {
      EXPECT(state, diagnostic.range.is_valid());
    }
  }
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find(
      "static argument pack may be used only by len or static iteration") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "procedure with a static argument pack must be called directly") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "parametric procedure application has the wrong number of arguments") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "nested procedure cannot capture an enclosing static argument pack") !=
      std::string::npos);
  EXPECT(state, rendered.find("break is outside a loop or switch") !=
                    std::string::npos);
  EXPECT(state, rendered.find("continue is outside a loop") !=
                    std::string::npos);
}

void test_value_parametric_nominal_composition(TestState &state) {
  CheckedSource source(R"draft(
package bodies

Buffer[N: usize] :: struct {
    values: [N + 1]i64,
}

Envelope[N: usize] :: struct {
    buffer: Buffer[N + 1],
}

last[N: usize] :: proc(value: ^Buffer[N]) -> i64 {
    return value^.values[N]
}

last_enveloped[N: usize] :: proc(value: ^Envelope[N]) -> i64 {
    return last[N + 1](&value^.buffer)
}

last_byte[N: u8] :: proc(value: ^Buffer[cast[usize](N)]) -> i64 {
    return last[cast[usize](N)](value)
}

main :: proc() -> i64 {
    value: Envelope[2]
    value.buffer.values[3] = 42
    byte_value: Buffer[3]
    byte_value.values[3] = 7
    return last_enveloped(&value) + last_byte[3](&byte_value)
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  bool saw_concrete_envelope = false;
  for (const draft::ParametricTypeInstanceRecord &instance :
       source.bodies.package.parametric_type_instances) {
    const draft::Symbol &template_symbol =
        source.bodies.package.symbols.symbol(instance.source);
    if (template_symbol.name != "Envelope" || instance.arguments.size() != 1 ||
        instance.arguments.front().value_expression.is_valid()) {
      continue;
    }
    const draft::TypeId type =
        source.bodies.package.symbols.symbol(instance.instance).type;
    saw_concrete_envelope = true;
    EXPECT(state, source.bodies.package.types.type(type).layout ==
                      draft::TypeLayout({true, 32, 8}));
  }
  EXPECT(state, saw_concrete_envelope);
}

void test_procedural_structural_alias_composition(TestState &state) {
  CheckedSource source(R"draft(
package bodies

increment :: proc(value: usize) -> usize {
    return value + 1
}

Bytes[N: usize] :: [N]u8

relay[N: usize] :: proc(
    value: Bytes[increment(N)],
) -> Bytes[increment(N)] {
    return value
}

outer[N: usize] :: proc(
    value: Bytes[increment(increment(N))],
) -> usize {
    result := relay[increment(N)](value)
    return len(result)
}

main :: proc() -> usize {
    value: [3]u8
    return outer[1](value)
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_dependent_value_inference(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

Buffer[N: usize] :: struct {
    values: [N]i64,
}

array_offset[N: usize] :: proc(values: [N + 1]i64) -> usize {
    return N
}

buffer_offset[N: usize] :: proc(value: ^Buffer[N + 1]) -> usize {
    return N
}

reverse_offset[N: usize] :: proc(values: [10 - N]i64) -> usize {
    return N
}

narrow_offset[N: u8] :: proc(values: [cast[usize](N) + 1]i64) -> u8 {
    return N
}

consistent[N: usize] :: proc(first: [N]i64, second: [N + 1]i64) -> usize {
    return N
}

main :: proc() -> usize {
    values: [3]i64
    buffer: Buffer[3]
    return array_offset(values) + buffer_offset(&buffer) +
        reverse_offset(values) + cast[usize](narrow_offset(values)) +
        consistent([2]i64{}, values)
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.semantics.ok);
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource non_unique(R"draft(
package bodies

scaled[N: usize] :: proc(values: [N * 2]u8) {
}

main :: proc() {
    values: [4]u8
    scaled(values)
}
)draft");
  EXPECT(state, !non_unique.bodies.ok);
  const std::string rendered = draft::render_diagnostics(
      non_unique.sources, non_unique.diagnostics);
  EXPECT(state, rendered.find(
                    "procedure type arguments cannot be inferred uniquely") !=
                    std::string::npos);
}

void test_nested_procedures(TestState &state) {
  const std::string text = R"draft(
package bodies

factorial :: proc(value: u64) -> u64 {
    One :: 1
    recurse :: proc(current: u64) -> u64 {
        if current <= One {
            return One
        }
        return current * recurse(current - 1)
    }
    return recurse(value)
}

last_of[T: type, N: usize] :: proc(values: [N]T) -> T {
    // Both T and N are compile-time bindings. The nested procedure may use
    // them without acquiring a runtime closure environment.
    last :: proc(input: [N]T) -> T {
        return input[N - 1]
    }
    identity[U: type] :: proc(input: U) -> U {
        return input
    }
    return identity[T](last(values))
}

make_increment :: proc() -> proc(value: i64) -> i64 {
    increment :: proc(value: i64) -> i64 {
        return value + 1
    }
    Increment :: increment
    // A nested procedure has an ordinary code-pointer representation and can
    // therefore outlive this invocation without an environment object.
    return Increment
}

shared: i64 = 7

read_shared_and_context :: proc() -> i64 {
    read :: proc() -> i64 {
        // Package storage and the hidden runtime context are not captures.
        return shared + cast[i64](context.user_index)
    }
    context.user_index = 8
    return read()
}

left :: proc() -> i64 {
    Base :: 19
    Answer :: Base + 1
    helper :: proc() -> i64 {
        return Answer
    }
    return helper()
}

right :: proc() -> i64 {
    // Reusing the same short name in a different lexical scope is legal. The
    // two symbols must receive different native linkage identities.
    helper :: proc() -> i64 {
        return 22
    }
    return helper()
}

main :: proc() -> i64 {
    values := [3]i64{4, 5, 6}
    other := [2]u32{8, 9}
    callback := make_increment()
    return cast[i64](factorial(5)) + last_of(values) + callback(1) +
        left() + right() + read_shared_and_context() +
        cast[i64](last_of(other))
}
)draft";
  CheckedSource source(text);

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.bodies.checked_procedures ==
                    source.bodies.program.procedures().size());

  std::size_t nested_procedures = 0;
  std::vector<std::string> linkage_names;
  std::vector<std::string> helper_linkage_names;
  for (const draft::HirProcedure &procedure :
       source.bodies.program.procedures()) {
    const draft::Symbol &symbol =
        source.bodies.package.symbols.symbol(procedure.symbol);
    if (!symbol.linkage_name.empty()) {
      ++nested_procedures;
      linkage_names.push_back(symbol.linkage_name);
      EXPECT(state, symbol.linkage_name.find("$nested$") != std::string::npos);
      if (symbol.name == "helper") {
        helper_linkage_names.push_back(symbol.linkage_name);
      }
    }
  }
  EXPECT(state, nested_procedures >= 8);
  EXPECT(state, helper_linkage_names.size() == 2);
  if (helper_linkage_names.size() == 2) {
    EXPECT(state, helper_linkage_names[0] != helper_linkage_names[1]);
  }

  // The inner identity specialization is discovered while checking a
  // concrete last_of body. Its public argument identity contains only U, but
  // checking its body also needs the enclosing last_of bindings T and N. The
  // retained execution environment must therefore be strictly richer than the
  // specialization key; otherwise a fresh per-root checker would lose N or
  // leave T symbolic when it reconstructs this nested instance.
  bool saw_nested_instance_environment = false;
  for (const draft::ParametricInstanceRecord &instance :
       source.bodies.package.parametric_instances) {
    const draft::Symbol &source_symbol =
        source.bodies.package.symbols.symbol(instance.source);
    if (source_symbol.name != "identity" ||
        source_symbol.linkage_name.empty()) {
      continue;
    }
    saw_nested_instance_environment = true;
    EXPECT(state, instance.arguments.size() == 1);
    EXPECT(state, instance.type_substitutions.size() == 2);
    EXPECT(state, instance.value_substitutions.size() == 1);
  }
  EXPECT(state, saw_nested_instance_environment);

  // Compile through a fresh semantic graph, rather than merely emitting the
  // first graph twice. The exact linkage sequence must not depend on transient
  // SymbolId/ScopeId allocation from another compilation.
  CheckedSource repeated(text);
  std::vector<std::string> repeated_linkage_names;
  for (const draft::HirProcedure &procedure :
       repeated.bodies.program.procedures()) {
    const draft::Symbol &symbol =
        repeated.bodies.package.symbols.symbol(procedure.symbol);
    if (!symbol.linkage_name.empty()) {
      repeated_linkage_names.push_back(symbol.linkage_name);
    }
  }
  EXPECT(state, repeated.bodies.ok);
  EXPECT(state, linkage_names == repeated_linkage_names);
}

void test_nested_procedure_capture_diagnostics(TestState &state) {
  CheckedSource source(R"draft(
package bodies

bad :: proc(parameter: i64) -> i64 {
    local := parameter
    items := [1]i64{3}
    for item in items {
        capture :: proc() -> i64 {
            from_parameter := parameter
            from_local := local
            return item + from_parameter + from_local
        }
        return capture()
    }
    return 0
}
)draft");

  EXPECT(state, !source.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("cannot capture enclosing runtime binding 'parameter'") !=
      std::string::npos);
  EXPECT(state, rendered.find("cannot capture enclosing runtime binding 'local'") !=
      std::string::npos);
  EXPECT(state, rendered.find("cannot capture enclosing runtime binding 'item'") !=
      std::string::npos);
  EXPECT(state, rendered.find("pass it as an explicit parameter") !=
      std::string::npos);
  bool parameter_range = false;
  bool local_range = false;
  bool item_range = false;
  for (const draft::Diagnostic &diagnostic : source.diagnostics.diagnostics()) {
    if (diagnostic.message.find("cannot capture enclosing runtime binding") ==
        std::string::npos) {
      continue;
    }
    const std::string_view spelling = source.sources.text(diagnostic.range);
    parameter_range = parameter_range || spelling == "parameter";
    local_range = local_range || spelling == "local";
    item_range = item_range || spelling == "item";
  }
  EXPECT(state, parameter_range);
  EXPECT(state, local_range);
  EXPECT(state, item_range);
}

void test_assignment_discards_and_tuple_patterns(TestState &state) {
  CheckedSource source(R"draft(
package bodies

pair :: proc() -> (i64, i64) {
    return (20, 22)
}

main :: proc() -> i64 {
    left: i64
    right: i64
    left, _ = 10, 99
    (_, right) = pair()
    (left, right) = (right, left)
    _ = 123
    return left + right
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  std::size_t discard_rows = 0;
  std::size_t tuple_assignments = 0;
  for (std::size_t index = 0;
       index < source.bodies.program.statement_count();
       ++index) {
    const draft::HirStatement &statement = source.bodies.program.statement(
        draft::HirStatementId{static_cast<std::uint32_t>(index)});
    if (statement.kind != draft::HirStatementKind::Assignment) continue;
    if (statement.assignment_destructures_tuple) {
      ++tuple_assignments;
      EXPECT(
          state,
          statement.assignment_member_indices.size() ==
              statement.assignment_target_count);
    }
    for (std::size_t expression = 0;
         expression < statement.assignment_target_count;
         ++expression) {
      if (source.bodies.program.expression(
              statement.expressions[expression]).kind ==
          draft::HirExpressionKind::Discard) {
        ++discard_rows;
      }
    }
  }
  EXPECT(state, tuple_assignments == 2);
  EXPECT(state, discard_rows == 2);

  CheckedSource invalid(R"draft(
package bodies

main :: proc() {
    _ += 1
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(
      state,
      rendered.find("discard cannot be used with compound assignment") !=
          std::string::npos);
}

void test_composite_literal_shapes(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

Record :: struct {
    left: i64,
    right: i64,
}

Bits :: raw union {
    signed: i64,
    unsigned: u64,
}

read :: proc(value: i64) -> i64 {
    record := Record{right = value}
    values := [3]i64{value}
    bits := Bits{signed = value}
    return record.left + record.right + values[1] + bits.signed
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

Record :: struct {
    left: i64,
    right: i64,
}

Bits :: raw union {
    signed: i64,
    unsigned: u64,
}

Tuple_Alias :: (i64, i64)

duplicate :: proc(value: i64) {
    record := Record{left = value, left = 2}
}

positional_struct :: proc(value: i64) {
    record := Record{value, 2}
}

keyed_array :: proc(value: i64) {
    values := [2]i64{first = value}
}

empty_union :: proc() {
    bits := Bits{}
}

multiple_union :: proc(value: i64) {
    bits := Bits{signed = value, unsigned = 2}
}

positional_union :: proc(value: i64) {
    bits := Bits{value}
}

tuple_composite :: proc(value: i64) {
    tuple := Tuple_Alias{value, 2}
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(
      state,
      rendered.find("composite member is initialized more than once") !=
          std::string::npos);
  EXPECT(
      state,
      rendered.find("struct composite elements must name a field") !=
          std::string::npos);
  EXPECT(
      state,
      rendered.find("array composite elements must be positional") !=
          std::string::npos);
  EXPECT(
      state,
      rendered.find("raw union composite literal must initialize exactly one field") !=
          std::string::npos);
  EXPECT(
      state,
      rendered.find("raw union composite element must name a field") !=
          std::string::npos);
  EXPECT(
      state,
      rendered.find("type does not support a composite literal") !=
          std::string::npos);
}

void test_switch_case_semantics(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

Base :: 40

select_integer :: proc(value: i64) -> i64 {
    switch value {
    case Base + 2:
        return 1
    case:
        return 0
    }
}

select_float :: proc(value: f64) -> i64 {
    switch value {
    case 1.5:
        return 1
    case:
        return 0
    }
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

runtime_label :: proc(subject, label: i64) {
    switch subject {
    case label:
        return
    case:
        return
    }
}

duplicate_value :: proc(subject: i64) {
    switch subject {
    case 1, 1:
        return
    case:
        return
    }
}

duplicate_default :: proc(subject: i64) {
    switch subject {
    case:
        return
    case:
        return
    }
}

unsupported_subject :: proc(subject: []i64) {
    switch subject {
    case:
        return
    }
}

duplicate_float_zero :: proc(subject: f64) {
    switch subject {
    case 0.0, -0.0:
        return
    case:
        return
    }
}

make_nan :: proc() -> f64 {
    return cast[f64](0) / cast[f64](0)
}

unreachable_nan :: proc(subject: f64) {
    switch subject {
    case make_nan():
        return
    case:
        return
    }
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("compile-time") != std::string::npos);
  const std::size_t first_duplicate =
      rendered.find("duplicate switch case value");
  EXPECT(state, first_duplicate != std::string::npos);
  EXPECT(state, first_duplicate != std::string::npos &&
                    rendered.find(
                        "duplicate switch case value", first_duplicate + 1) !=
                        std::string::npos);
  EXPECT(state, rendered.find("duplicate default switch case") !=
                    std::string::npos);
  EXPECT(state, rendered.find("does not have built-in scalar equality") !=
                    std::string::npos);
  EXPECT(state, rendered.find("NaN switch case value can never match") !=
                    std::string::npos);
}

void test_local_type_declarations(TestState &state) {
  CheckedSource source(R"draft(
package bodies

local_types[T: type, N: usize] :: proc(value: T, values: [N]T) -> T {
    // This value is symbolic in the template and exact in each concrete body.
    Extra :: N + 1
    Alias :: T
    Buffer :: [N]Alias
    Pair :: struct {
        head: Alias,
        tail: Buffer,
    }
    Mode :: enum {
        Off,
        On,
    }
    Choice :: union {
        none,
        some: Alias,
    }
    Bits :: raw union {
        value: Alias,
    }
    Wrapped :: distinct Alias
    Local_Box[U: type] :: struct {
        outer: Alias,
        inner: U,
    }
    Tuple_Alias :: (Alias, u32)
    Box_Alias :: Local_Box[u32]
    read :: proc(pair: ^Pair, box: ^Local_Box[u32]) -> Alias {
        if box^.inner > 0 {
            return pair^.head
        }
        return box^.outer
    }

    pair := Pair{head = value, tail = values}
    box: Box_Alias
    box.outer = value
    box.inner = 1
    static_assert(Extra > N)
    return read(&pair, &box)
}

main :: proc() -> u64 {
    values := [2]u64{40, 41}
    return local_types(values[0], values)
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  std::size_t local_types = 0;
  std::size_t local_type_instances = 0;
  std::size_t type_statements = 0;
  for (std::size_t index = 0;
       index < source.bodies.package.symbols.symbol_count();
       ++index) {
    const draft::Symbol &symbol = source.bodies.package.symbols.symbol(
        draft::SymbolId{static_cast<std::uint32_t>(index)});
    if (symbol.kind == draft::SymbolKind::Type &&
        source.bodies.package.symbols.scope(symbol.scope).kind ==
            draft::ScopeKind::Block) {
      if (symbol.name.find("$instance") == std::string::npos) {
        ++local_types;
      } else {
        ++local_type_instances;
      }
      EXPECT(state, symbol.type.is_valid());
    }
  }
  for (std::size_t index = 0;
       index < source.bodies.program.statement_count();
       ++index) {
    const draft::HirStatement &statement = source.bodies.program.statement(
        draft::HirStatementId{static_cast<std::uint32_t>(index)});
    if (statement.kind == draft::HirStatementKind::TypeDeclaration) {
      ++type_statements;
      EXPECT(state, statement.bindings.size() == 1);
    }
  }
  // The template and its concrete u64/2 specialization each own a separate
  // lexical declaration set. TypeIds and member scopes must never be shared
  // across those two semantic bodies.
  EXPECT(state, local_types == 20);
  EXPECT(state, local_type_instances == 2);
  EXPECT(state, type_statements == 20);
}

void test_string_index_is_immutable(TestState &state) {
  CheckedSource source(R"draft(
package bodies

main :: proc() {
    text := "draft"
    text[0] = cast[u8]('D')
}
)draft");

  EXPECT(state, !source.bodies.ok);
  EXPECT(state, source.diagnostics.error_count() == 1);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("assignment target is not addressable") !=
                    std::string::npos);
}

void test_parameter_immutability(TestState &state) {
  // Parameters carry values into a procedure; they are not hidden mutable
  // locals. Explicit indirection is different: a pointer, multi-pointer, or
  // slice parameter is an immutable view value whose referenced storage may be
  // mutated. Keeping those two rules separate makes mutation visible in the
  // procedure signature without making ordinary value parameters surprising.
  CheckedSource explicit_views(R"draft(
package bodies

mutate_views :: proc(slice: []i64, pointer: ^i64, multi: [^]i64) {
    slice[0] = 1
    pointer^ = 2
    multi[0] = 3
}
)draft");

  if (explicit_views.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        explicit_views.sources, explicit_views.diagnostics);
  }
  EXPECT(state, explicit_views.bodies.ok);
  EXPECT(state, !explicit_views.diagnostics.has_errors());

  CheckedSource copied_values(R"draft(
package bodies

Pair :: struct {
    left: i64,
    right: i64,
}

mutate_values :: proc(value: i64, pair: Pair, fixed: [2]i64) {
    value = 1
    pair.left = 2
    fixed[0] = 3
    pointer := &value
    view := fixed[:]
}
)draft");

  EXPECT(state, !copied_values.bodies.ok);
  EXPECT(state, copied_values.diagnostics.error_count() == 5);
  const std::string rendered = draft::render_diagnostics(
      copied_values.sources, copied_values.diagnostics);
  EXPECT(state, rendered.find("assignment target is not addressable") !=
                    std::string::npos);
  EXPECT(state, rendered.find("address-of requires addressable storage") !=
                    std::string::npos);
  EXPECT(state, rendered.find("slicing an array requires addressable storage") !=
                    std::string::npos);
}

void test_multi_pointer_slice_shape(TestState &state) {
  CheckedSource source(R"draft(
package bodies

main :: proc() {
    values := [3]u8{1, 2, 3}
    pointer := cast[[^]u8](&values[0])
    missing_length := pointer[:]
    offset_range := pointer[1:3]
}
)draft");

  EXPECT(state, !source.bodies.ok);
  EXPECT(state, source.diagnostics.error_count() == 2);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("requires the form 'pointer[:length]'") !=
                    std::string::npos);
}

void test_constant_bounds_diagnostics(TestState &state) {
  CheckedSource source(R"draft(
package bodies

main :: proc() {
    values := [3]u8{1, 2, 3}
    bad_index := values[1 + 2]
    bad_high := values[1:4]
    reversed := values[2:1]
    signed_index: i64 = 0
    bad_index_type := values[signed_index]
    bad_bound_type := values[signed_index:]
}
)draft");

  EXPECT(state, !source.bodies.ok);
  // Each typed i64 operand produces one contextual-type diagnostic. The
  // checker must not follow it with a redundant "must be an integer" error:
  // i64 is an integer, but the language boundary specifically requires usize.
  EXPECT(state, source.diagnostics.error_count() == 5);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("constant index 3 is out of bounds for length 3") !=
                    std::string::npos);
  EXPECT(state, rendered.find("constant slice bounds [1:4]") !=
                    std::string::npos);
  EXPECT(state, rendered.find("constant slice bounds [2:1]") !=
                    std::string::npos);
  EXPECT(state,
         rendered.find("does not match expected type 'unsigned integer'") !=
             std::string::npos);
}

void test_parametric_procedure_value_diagnostics(TestState &state) {
  CheckedSource wrong_procedural_type(R"draft(
package bodies

pick[N: usize] :: proc() -> usize {
    return N
}
wrong_type :: proc(value: usize) -> u64 {
    return cast[u64](value)
}
outer[N: usize] :: proc() -> usize {
    return pick[wrong_type(N)]()
}
main :: proc() -> usize {
    return outer[1]()
}
)draft");
  EXPECT(state, !wrong_procedural_type.bodies.ok);
  const std::string wrong_procedural_type_rendered =
      draft::render_diagnostics(
          wrong_procedural_type.sources,
          wrong_procedural_type.diagnostics);
  EXPECT(state, wrong_procedural_type_rendered.find(
                    "does not match expected type") != std::string::npos);

  CheckedSource too_wide(R"draft(
package bodies

pick[N: u8] :: proc() -> u8 {
    return N
}
main :: proc() -> u8 {
    return pick[256]()
}
)draft");
  EXPECT(state, !too_wide.bodies.ok);
  const std::string wide_rendered =
      draft::render_diagnostics(too_wide.sources, too_wide.diagnostics);
  EXPECT(state, wide_rendered.find("not representable in its parameter type") !=
                    std::string::npos);

  CheckedSource runtime_value(R"draft(
package bodies

pick[N: usize] :: proc() -> usize {
    return N
}
main :: proc(value: usize) -> usize {
    return pick[value]()
}
)draft");
  EXPECT(state, !runtime_value.bodies.ok);
  const std::string runtime_rendered =
      draft::render_diagnostics(runtime_value.sources, runtime_value.diagnostics);
  EXPECT(state, runtime_rendered.find("not compile-time evaluable") !=
                    std::string::npos);

  CheckedSource ambiguous(R"draft(
package bodies

pick[N: usize] :: proc(value: i64) -> i64 {
    return value
}
main :: proc(value: i64) -> i64 {
    return pick(value)
}
)draft");
  EXPECT(state, !ambiguous.bodies.ok);
  const std::string ambiguous_rendered =
      draft::render_diagnostics(ambiguous.sources, ambiguous.diagnostics);
  EXPECT(state, ambiguous_rendered.find(
                    "procedure value arguments cannot be inferred uniquely") !=
                    std::string::npos);

  CheckedSource failed_assertion(R"draft(
package bodies

positive[N: usize] :: proc() -> usize {
    static_assert(N > 0, "N must be positive")
    return N
}
main :: proc() -> usize {
    return positive[0]()
}
)draft");
  EXPECT(state, !failed_assertion.bodies.ok);
  const std::string assertion_rendered = draft::render_diagnostics(
      failed_assertion.sources, failed_assertion.diagnostics);
  EXPECT(state, assertion_rendered.find(
                    "static assertion failed: N must be positive") !=
                    std::string::npos);

  CheckedSource failed_type_assertion(R"draft(
package bodies

four_bytes[T: type] :: proc() {
    static_assert(size_of(T) == 4, "T must occupy four bytes")
}
main :: proc() {
    four_bytes[u64]()
}
)draft");
  EXPECT(state, !failed_type_assertion.bodies.ok);
  const std::string type_assertion_rendered = draft::render_diagnostics(
      failed_type_assertion.sources, failed_type_assertion.diagnostics);
  EXPECT(state, type_assertion_rendered.find(
                    "static assertion failed: T must occupy four bytes") !=
                    std::string::npos);

  CheckedSource distinct_constraint(R"draft(
package bodies

Counter :: distinct u32

accept[T: number] :: proc(value: T) -> T {
    return value
}

main :: proc() {
    counter := cast[Counter](cast[u32](1))
    explicit := accept[Counter](counter)
    inferred := accept(counter)
}
)draft");
  if (distinct_constraint.diagnostics.error_count() != 2) {
    std::cerr << draft::render_diagnostics(
        distinct_constraint.sources, distinct_constraint.diagnostics);
  }
  EXPECT(state, !distinct_constraint.bodies.ok);
  EXPECT(state, distinct_constraint.diagnostics.error_count() == 2);
  const std::string distinct_rendered = draft::render_diagnostics(
      distinct_constraint.sources, distinct_constraint.diagnostics);
  EXPECT(state, distinct_rendered.find(
                    "procedure type argument does not satisfy its constraint") !=
                    std::string::npos);
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

measure[T: type] :: proc() -> usize {
    return size_of(T) + align_of(T)
}

main :: proc() -> usize {
    static_assert(size_of(Header) == 16)
    static_assert(align_of(Header) == 8, "Header alignment changed")
    return size_of(Header) + align_of(Header) + measure[u32]()
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

void test_checked_numeric_casts(TestState &state) {
  CheckedSource safe(R"draft(
package bodies

Code :: enum i16 {
    Zero,
    Seven = 7,
}

Target_OS_Type_Value :: type_of(target.os)

small :: proc() -> i8 {
    return cast[i8](127.9)
}

wrapped :: proc() -> u8 {
    return cast[u8](256)
}

dynamic :: proc(value: f64) -> i32 {
    return cast[i32](value)
}

scalar :: proc(value: i64) -> rune {
    return cast[rune](value)
}

code :: proc(value: i64) -> Code {
    return cast[Code](cast[i16](value))
}

target_os :: proc(value: u8) -> Target_Operating_System {
    static_assert(cast[Target_Operating_System](cast[u8](0)) == .macos)
    static_assert(cast[Target_Operating_System](cast[u8](1)) == .linux)
    static_assert(cast[Target_OS_Type_Value](cast[u8](1)) == .linux)
    return cast[Target_Operating_System](value)
}
)draft");
  if (safe.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(safe.sources, safe.diagnostics);
  }
  EXPECT(state, safe.bodies.ok);
  EXPECT(state, !safe.diagnostics.has_errors());

  CheckedSource failing(R"draft(
package bodies

Code :: enum i16 {
    Zero,
    Seven = 7,
}

too_large :: proc() -> i8 {
    return cast[i8](128.0)
}

surrogate :: proc() -> rune {
    return cast[rune](0xd800)
}

invalid_code :: proc() -> Code {
    return cast[Code](cast[i16](1))
}

invalid_target_os :: proc() -> Target_Operating_System {
    return cast[Target_Operating_System](cast[u8](2))
}
)draft");
  EXPECT(state, !failing.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(failing.sources, failing.diagnostics);
  EXPECT(state, rendered.find("float-to-integer cast is out of range") !=
                    std::string::npos);
  EXPECT(state, rendered.find("does not produce a Unicode scalar") !=
                    std::string::npos);
  EXPECT(state, rendered.find("does not name an enum member") !=
                    std::string::npos);
}

void test_storage_pointer_and_distinct_semantics(TestState &state) {
  CheckedSource safe(R"draft(
package bodies

Counter :: distinct u32
Truth :: distinct bool
Handle :: distinct ^u64
Multi_Handle :: distinct [^]u64
Callback_Handle :: distinct proc(value: u64) -> u64

Stored_Pair :: struct {
    first: u64,
    second: u64,
}

Stored_Choice :: union {
    none,
    some: u64,
}

Stored_Mode :: enum u8 {
    Zero,
    One,
}

Pair_Handle :: distinct Stored_Pair
Tuple_Handle :: distinct (u64, u64)
Array_Handle :: distinct [3]u64
Slice_Handle :: distinct []u64
Text_Handle :: distinct string
Choice_Handle :: distinct Stored_Choice
Mode_Handle :: distinct Stored_Mode

storage_truth :: proc(flag: bool, bits: b32) -> bool {
    encoded := cast[b32](flag)
    return encoded == cast[b32](flag) && cast[bool](bits)
}

integer_endian :: proc(value: u32) -> u32 {
    big := cast[u32be](value)
    little := cast[u32le](value)
    assert(big == cast[u32be](value))
    return cast[u32](big) + cast[u32](little)
}

float_endian :: proc(value: f64) -> f64 {
    stored := cast[f64be](value)
    return cast[f64](stored)
}

same_pointer :: proc(left, right: ^u64) -> bool {
    address := cast[uintptr](left)
    return left == right || cast[^u64](address) == left
}

pointer_distance :: proc(base: [^]u64, count: isize) -> isize {
    advanced := ptr_offset(base, count)
    return ptr_sub(advanced, base)
}

increment :: proc(value: Counter) -> Counter {
    return value + 1
}

logical_truth :: proc(left, right: Truth) -> Truth {
    return !left || (left && right)
}

nil_handle :: proc(value: Handle) -> bool {
    return value == nil
}

zero_handle :: proc() -> Handle {
    value: Handle = nil
    return value
}

nil_callback_handle :: proc(value: Callback_Handle) -> bool {
    return value == nil
}

use_handle :: proc(
    value: Handle,
    values: Multi_Handle,
    callback: Callback_Handle,
) -> u64 {
    same := ptr_offset(value, 0)
    return same^ + values[1] + callback(1)
}

use_aggregate_handles :: proc(
    pair: Pair_Handle,
    tuple: Tuple_Handle,
    array: Array_Handle,
    slice: Slice_Handle,
    text: Text_Handle,
) -> u64 {
    slice_tail: Slice_Handle = slice[1:]
    text_tail: Text_Handle = text[1:]
    total := pair.first + tuple.1 + array[2] + slice_tail[0]
    for value, index in array {
        total += value + cast[u64](index)
    }
    return total + cast[u64](len(text_tail))
}

use_choice_handle :: proc(value: Choice_Handle) -> u64 {
    switch value {
    case .some(payload):
        return payload
    case .none:
        return 0
    }
}

make_choice_handle :: proc(value: u64) -> Choice_Handle {
    return .some(value)
}

use_mode_handle :: proc(value: Mode_Handle) -> u64 {
    switch value {
    case .One:
        return 1
    case .Zero:
        return 0
    }
}

make_mode_handle :: proc() -> Mode_Handle {
    return .One
}

constant_storage :: proc() -> bool {
    return cast[bool](cast[b64](true))
}
)draft");
  if (safe.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(safe.sources, safe.diagnostics);
  }
  EXPECT(state, safe.bodies.ok);
  EXPECT(state, !safe.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

Counter :: distinct u32
Truth :: distinct bool

Code :: enum i16 {
    Zero,
}

bad_storage_order :: proc(left, right: b32) -> bool {
    return left < right
}

bad_endian_order :: proc(left, right: u32be) -> bool {
    return left < right
}

bad_endian_pair :: proc(value: u32) -> u64be {
    return cast[u64be](value)
}

bad_enum_pair :: proc(value: i64) -> Code {
    return cast[Code](value)
}

bad_distinct_pair :: proc(value: Counter) -> i64 {
    return cast[i64](value)
}

bad_distinct_logical_pair :: proc(left: Truth, right: bool) -> Truth {
    return left && right
}

bad_pointer_kind :: proc(bits: rawptr) -> rawptr {
    return ptr_offset(bits, 1)
}

bad_pointer_count :: proc(value: ^u32, count: usize) -> ^u32 {
    return ptr_offset(value, count)
}

bad_pointer_pair :: proc(left: ^u32, right: ^u64) -> isize {
    return ptr_sub(left, right)
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  EXPECT(state, invalid.diagnostics.error_count() >= 8);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("comparison is not defined") != std::string::npos);
  EXPECT(state, rendered.find("cast source and target types are incompatible") !=
                    std::string::npos);
  EXPECT(state, rendered.find("logical operators require matching bool operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find("ptr_offset requires a ^T or [^]T pointer") !=
                    std::string::npos);
  EXPECT(state, rendered.find("ptr_sub requires two matching") !=
                    std::string::npos);
}

// raw_data is the one explicit escape from normal string immutability. These
// checks pin its deliberately narrow type contract and ensure malformed uses
// point at the offending call or value rather than failing later in MIR.
void test_raw_string_data_intrinsic(TestState &state) {
  CheckedSource safe(R"draft(
package bodies

literal_data :: proc() -> [^]u8 {
    return raw_data("Draft")
}

sliced_data :: proc(text: string) -> [^]u8 {
    return raw_data(text[1:])
}

main :: proc() {
    empty: string = ""
    empty_pointer: [^]u8 = raw_data(empty)
    pointer := raw_data("bytes")
    assert(pointer[0] == cast[u8]('b'))
}
)draft");
  if (safe.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(safe.sources, safe.diagnostics);
  }
  EXPECT(state, safe.bodies.ok);
  EXPECT(state, !safe.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

missing :: proc() {
    raw_data()
}

extra :: proc() {
    raw_data("one", "two")
}

wrong :: proc() {
    raw_data(42)
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 3);

  bool saw_missing = false;
  bool saw_extra = false;
  bool saw_wrong = false;
  for (const draft::Diagnostic &diagnostic :
       invalid.diagnostics.diagnostics()) {
    const std::string_view spelling = invalid.sources.text(diagnostic.range);
    if (diagnostic.message ==
        "raw_data requires exactly one string argument") {
      saw_missing = saw_missing || spelling == "raw_data()";
      saw_extra = saw_extra || spelling == "raw_data(\"one\", \"two\")";
    }
    if (diagnostic.message == "raw_data requires a string argument") {
      saw_wrong = saw_wrong || spelling == "42";
    }
  }
  EXPECT(state, saw_missing);
  EXPECT(state, saw_extra);
  EXPECT(state, saw_wrong);
}

// Procedure-body `when` may reuse a package-wide constant selection, but that
// value is not a substitute for checking non-evaluated type_of operands at the
// condition's lexical program point.
void test_selected_when_condition_type_validation(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

main :: proc() {
    result := 0
    when (target).os == .macos &&
         (target).has_feature("neon") &&
         target.os == (.macos) &&
         (.macos) == target.os &&
         .aarch64 == target.arch &&
         target.os == target.os &&
         type_kind(type_of(raw_data(target.identity))) == .multi_pointer {
        result = 42
    }
    assert(result == 42)
}
)draft");
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());
  bool saw_selected_assignment = false;
  for (std::size_t index = 0;
       index < valid.bodies.program.statement_count();
       ++index) {
    const draft::HirStatement &statement = valid.bodies.program.statement(
        draft::HirStatementId{static_cast<std::uint32_t>(index)});
    saw_selected_assignment = saw_selected_assignment ||
        (statement.kind == draft::HirStatementKind::Assignment &&
         valid.sources.text(statement.range) == "result = 42");
  }
  EXPECT(state, saw_selected_assignment);

  CheckedSource invalid(R"draft(
package bodies

runtime_text_with_value :: proc(value: int) -> string {
    return "runtime text"
}

main :: proc() {
    when type_kind(type_of(raw_data(42))) == .multi_pointer {
    }
    when type_kind(type_of(raw_data(runtime_text_with_value()))) == .multi_pointer {
    }
    when target.os != .macos &&
         type_kind(type_of(raw_data(false))) == .multi_pointer {
    }
    when false && target.has_feature("invented-feature") &&
         type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    }
    when .macos == target.arch {
    }
    when .neon == target.os {
    }
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 6);

  bool saw_wrong_type = false;
  bool saw_missing_argument = false;
  bool saw_short_circuited_wrong_type = false;
  bool saw_unknown_feature = false;
  bool saw_reverse_os = false;
  bool saw_reverse_feature = false;
  for (const draft::Diagnostic &diagnostic :
       invalid.diagnostics.diagnostics()) {
    const std::string_view spelling = invalid.sources.text(diagnostic.range);
    saw_wrong_type = saw_wrong_type ||
        (diagnostic.message == "raw_data requires a string argument" &&
         spelling == "42");
    saw_missing_argument = saw_missing_argument ||
        (diagnostic.message ==
             "procedure call has the wrong number of arguments" &&
         spelling == "runtime_text_with_value()");
    saw_short_circuited_wrong_type = saw_short_circuited_wrong_type ||
        (diagnostic.message == "raw_data requires a string argument" &&
         spelling == "false");
    saw_unknown_feature = saw_unknown_feature ||
        (diagnostic.message ==
             "unrecognized target feature 'invented-feature'" &&
         spelling == "\"invented-feature\"");
    saw_reverse_os = saw_reverse_os || spelling == ".macos";
    saw_reverse_feature = saw_reverse_feature || spelling == ".neon";
  }
  EXPECT(state, saw_wrong_type);
  EXPECT(state, saw_missing_argument);
  EXPECT(state, saw_short_circuited_wrong_type);
  EXPECT(state, saw_unknown_feature);
  EXPECT(state, saw_reverse_os);
  EXPECT(state, saw_reverse_feature);
}

// A folded target query is a leaf only after each source argument has passed
// ordinary type checking. Constant evaluation may select one conditional
// argument branch, but package, member, and body preflight must still reject a
// malformed branch which execution did not visit.
void test_target_query_argument_preflight(TestState &state) {
  CheckedSource invalid(R"draft(
package bodies

when target.has_feature("neon" if true else 42) &&
     type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    Package_Selected :: true
}

Broken :: struct {
    when target.has_feature("neon" if true else false) &&
         type_kind(type_of(raw_data("ok"))) == .multi_pointer {
        selected: bool,
    }
}

main :: proc() {
    when target.has_feature("neon" if true else 1.5) &&
         type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    }
}

probe :: proc(feature: string) {
    when false && target.has_feature(feature) &&
         type_kind(type_of(raw_data("ok"))) == .multi_pointer {
    }
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 4);

  bool saw_package_branch = false;
  bool saw_member_branch = false;
  bool saw_body_branch = false;
  bool saw_runtime_feature = false;
  for (const draft::Diagnostic &diagnostic :
       invalid.diagnostics.diagnostics()) {
    const std::string_view spelling = invalid.sources.text(diagnostic.range);
    const bool expected_mismatch = diagnostic.message.find(
        "does not match expected type") != std::string::npos;
    saw_package_branch = saw_package_branch ||
        (expected_mismatch && spelling == "42");
    saw_member_branch = saw_member_branch ||
        (expected_mismatch && spelling == "false");
    saw_body_branch = saw_body_branch ||
        (expected_mismatch && spelling == "1.5");
    saw_runtime_feature = saw_runtime_feature ||
        (diagnostic.message ==
             "target.has_feature requires a compile-time string" &&
         spelling == "feature");
  }
  EXPECT(state, saw_package_branch);
  EXPECT(state, saw_member_branch);
  EXPECT(state, saw_body_branch);
  EXPECT(state, saw_runtime_feature);
  if (invalid.diagnostics.error_count() != 4 || !saw_package_branch ||
      !saw_member_branch || !saw_body_branch || !saw_runtime_feature) {
    std::cerr << draft::render_diagnostics(
        invalid.sources, invalid.diagnostics);
  }
}

// Statement `when` selection occurs after preceding locals are declared. This
// is essential for ordinary lexical shadowing: the package semantic graph must
// not select a branch against the predeclared target object before it can see a
// body-local binding with the same name.
void test_statement_when_uses_lexical_bindings(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

Target_Snapshot :: struct {
    os: Target_Operating_System,
}

compile_time_shadow :: proc() -> int {
    target :: 42
    when target == 42 {
        static_assert(type_of(target) == int)
        return 1
    } else {
        return 0
    }
}

runtime_shadow :: proc() -> int {
    target := 42
    when type_of(target) == int {
        return target
    } else {
        return 0
    }
}

member_type_shadow :: proc() -> int {
    target := Target_Snapshot{os = .macos}
    when type_of(target.os) == Target_Operating_System {
        return 1
    } else {
        return 0
    }
}

materialized_target_facts :: proc() -> bool {
    os := target.os
    neon := target.has_feature("neon")
    return os == target.os && neon == target.has_feature("neon")
}

main :: proc() {
    assert(compile_time_shadow() == 1)
    assert(runtime_shadow() == 42)
    assert(member_type_shadow() == 1)
    assert(materialized_target_facts())
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.semantics.ok);
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());
}

void test_integer_shift_count_types(TestState &state) {
  CheckedSource source(R"draft(
package bodies

mask :: proc(bits: uint) -> uint {
    result: uint
    for i: uint = 0; i < bits; i += 1 {
        result |= 1 << i
    }
    return result
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.diagnostics.has_errors());
}

void test_conditional_context_from_either_branch(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

Mode :: enum {
    Off,
    On,
}

Choice :: union {
    none,
    some: i64,
}

choose_pointer :: proc(condition: bool, fallback: ^i64) -> ^i64 {
    selected := (nil if condition else nil) if condition else fallback
    return selected
}

grouped_nil_comparison :: proc(value: ^i64) -> bool {
    return (nil) != value && value != (nil)
}

choose_mode :: proc(condition: bool, fallback: Mode) -> Mode {
    selected := (.On) if condition else fallback
    return selected
}

choose_choice :: proc(condition: bool, fallback: Choice) -> Choice {
    selected := .some(42) if condition else fallback
    return selected
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

untyped_nil_pair :: proc(condition: bool) {
    selected := nil if condition else nil
}

untyped_alternative_pair :: proc(condition: bool) {
    selected := .left if condition else .right
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  const std::string rendered = draft::render_diagnostics(
      invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("nil requires an expected pointer type") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "contextual alternative requires an expected enum or union type") !=
                    std::string::npos);
}

void test_compound_assignment_operators(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

apply :: proc(start: u32, count: usize) -> u32 {
    value := start
    value += 1
    value %= 7
    value &= 3
    value <<= count
    return value
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

bad :: proc() {
    flag := true
    flag += true

    floating: f64 = 4.0
    floating %= 2.0

    character := 'a'
    character += 'b'

    stored := cast[u32be](cast[u32](1))
    stored += cast[u32be](cast[u32](1))

    bits: u32 = 1
    bits <<= 1.5
}
)draft");
  if (invalid.diagnostics.error_count() != 5) {
    std::cerr << draft::render_diagnostics(
        invalid.sources, invalid.diagnostics);
  }
  EXPECT(state, !invalid.bodies.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 5);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find(
                    "compound assignment operator requires numeric operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "compound assignment operator requires integer operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
                    "compound shift assignment requires integer operands") !=
                    std::string::npos);
}

void test_invalid_operator_type_matrix(TestState &state) {
  CheckedSource invalid(R"draft(
package bodies

Record :: struct {
    value: u32,
}

Choice :: union {
    none,
    some: u32,
}

Mode :: enum {
    Zero,
    One,
}

Other_Mode :: enum {
    Zero,
    One,
}

bad_bool_arithmetic :: proc(left, right: bool) -> bool {
    return left + right
}

bad_float_remainder :: proc(left, right: f32) -> f32 {
    return left % right
}

bad_float_bitwise :: proc(left, right: f32) -> f32 {
    return left & right
}

bad_float_shift :: proc(left, right: f32) -> f32 {
    return left << right
}

bad_storage_logical :: proc(left, right: b32) -> b32 {
    return left && right
}

bad_storage_order :: proc(left, right: b32) -> bool {
    return left < right
}

bad_endian_arithmetic :: proc(left, right: u32be) -> u32be {
    return left + right
}

bad_endian_order :: proc(left, right: u32be) -> bool {
    return left < right
}

bad_enum_arithmetic :: proc(left, right: Mode) -> Mode {
    return left + right
}

bad_enum_pair :: proc(left: Mode, right: Other_Mode) -> bool {
    return left == right
}

bad_rune_arithmetic :: proc(left, right: rune) -> rune {
    return left + right
}

bad_record_equality :: proc(left, right: Record) -> bool {
    return left == right
}

bad_union_equality :: proc(left, right: Choice) -> bool {
    return left == right
}

bad_tuple_equality :: proc(left, right: (u32, u32)) -> bool {
    return left == right
}

bad_array_equality :: proc(left, right: [2]u32) -> bool {
    return left == right
}

bad_slice_equality :: proc(left, right: []u32) -> bool {
    return left == right
}

bad_string_equality :: proc(left, right: string) -> bool {
    return left == right
}

bad_pointer_order :: proc(left, right: ^u32) -> bool {
    return left < right
}

bad_pointer_pair :: proc(left: ^u32, right: ^u64) -> bool {
    return left == right
}

bad_procedure_order :: proc(
    left, right: proc(value: u32) -> u32,
) -> bool {
    return left < right
}

bad_procedure_pair :: proc(
    left: proc(value: u32) -> u32,
    right: proc(value: u64) -> u64,
) -> bool {
    return left == right
}

bad_raw_dereference :: proc(value: rawptr) -> u8 {
    return value^
}

bad_temporary_address :: proc(left, right: u32) -> ^u32 {
    return &(left + right)
}

bad_logical_not :: proc(value: u32) -> u32 {
    return !value
}

bad_bitwise_not :: proc(value: f32) -> f32 {
    return ~value
}

bad_numeric_negation :: proc(value: bool) -> bool {
    return -value
}

bad_integer_logical :: proc(left, right: u32) -> u32 {
    return left && right
}

bad_mixed_integer :: proc(left: u32, right: u64) -> u32 {
    return left + right
}

bad_mixed_numeric :: proc(left: f32, right: i32) -> f32 {
    return left + right
}
)draft");

  if (invalid.diagnostics.error_count() != 29) {
    std::cerr << draft::render_diagnostics(
        invalid.sources, invalid.diagnostics);
  }
  EXPECT(state, !invalid.bodies.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 29);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("numeric operands require one common type") !=
                    std::string::npos);
  EXPECT(state, rendered.find("operator requires integer operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find("shift requires integer operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find("logical operators require matching bool operands") !=
                    std::string::npos);
  EXPECT(state, rendered.find("comparison is not defined for operand types") !=
                    std::string::npos);
  EXPECT(state, rendered.find("dereference requires a typed data pointer") !=
                    std::string::npos);
  EXPECT(state, rendered.find("address-of requires addressable storage") !=
                    std::string::npos);
  EXPECT(state, rendered.find("logical not requires bool") !=
                    std::string::npos);
  EXPECT(state, rendered.find("bitwise not requires an integer") !=
                    std::string::npos);
  EXPECT(state, rendered.find("unary numeric operator requires a number") !=
                    std::string::npos);
}

void test_numeric_context_boundaries(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

floating :: proc(value: f32) -> f32 {
    return value + 1
}

mixed_untyped :: proc(condition: bool) -> f64 {
    arithmetic := 1 + 1.5
    selected := 1 if condition else 2.5
    return arithmetic + selected
}

exact_untyped_comparisons :: proc() -> bool {
    return 9007199254740993 != 9007199254740992.0 &&
        340282366920938463463374607431768211456 > 0
}

One :: 1
Untyped_Tuple :: (1, 2.5)

contextual_float_constants :: proc() -> f64 {
    direct: f64 = 1
    return direct + One + Untyped_Tuple.0 + Untyped_Tuple.1
}

contextual_tuple_constant :: proc() -> (f64, f64) {
    return Untyped_Tuple
}

destructure_tuple_constant :: proc() -> f64 {
    (left, right): (f64, f64) = Untyped_Tuple
    assigned_left, assigned_right: f64
    (assigned_left, assigned_right) = Untyped_Tuple
    return left + right + assigned_left + assigned_right
}

inferred_tuple_conditional :: proc(condition: bool) -> f64 {
    pair := Untyped_Tuple if condition else (3, 4.5)
    return cast[f64](pair.0) + pair.1
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

integer_and_decimal :: proc(value: i32) -> i32 {
    return value + 1.5
}

mixed_concrete :: proc(left: f32, right: f64) -> f64 {
    return left + right
}

generic_decimal[T: number] :: proc(value: T) -> T {
    return value + 1.5
}

concrete_tuple_conversion :: proc(value: (f32, f32)) -> (f64, f64) {
    return value
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  EXPECT(state, invalid.diagnostics.error_count() == 4);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("numeric operands require one common type") !=
                    std::string::npos);
}

void test_builtin_context_value(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

read_user_index :: proc() -> int {
    return context.user_index
}

set_user_index :: proc() -> int {
    context.user_index = 42
    return read_user_index()
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, valid.bodies.package.runtime_context_type.is_valid());
  if (valid.bodies.package.runtime_context_type.is_valid()) {
    const draft::Type &context = valid.bodies.package.types.type(
        valid.bodies.package.runtime_context_type);
    EXPECT(state, context.layout.size == 96);
    EXPECT(state, context.member_offsets.size() == 8);
  }

  CheckedSource invalid(R"draft(
package bodies

ordinary :: proc() {
}

bad :: c proc() -> int {
    return context.user_index
}

bad_calls :: c proc() {
    ordinary()
    assert(true)
    static_assert(true)
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("context value is unavailable in a c proc") !=
      std::string::npos);
  EXPECT(state, rendered.find("c proc cannot call an ordinary Draft procedure") !=
      std::string::npos);
  EXPECT(state, rendered.find("runtime assert is unavailable in a c proc") !=
      std::string::npos);
}

void test_compile_time_type_inspection(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

Record :: @repr(C) @align(16) struct {
    tag: u8,
    value: u64,
}

Mode :: enum u8 {
    Off,
    On,
}

Choice :: union {
    none,
    some: u32,
}

Meters :: distinct i64

Overlay :: @repr(C) @align(8) raw union {
    byte: u8,
    word: u64,
}

Callback :: proc(value: i32, flag: bool) -> u64
C_Callback :: c proc(value: i32) -> u64
Pair :: (u8, bool)
Vector :: #simd[4]u32

runtime_value :: proc() -> int {
    return context.user_index
}

no_result :: proc() {
}

inspect_types :: proc() {
    // type_of checks the call's static result type but does not invoke it.
    static_assert(type_of(runtime_value()) == int)
    static_assert(type_of(1) == int)

    // Runtime locals still have compile-time-visible static types. An inline
    // constructor is the exact type value, so this comparison neither reads
    // the string nor requires a named alias for [^]u8.
    payload := "draft"
    data := raw_data(payload)
    static_assert(type_of(data) == [^]u8)
    static_assert([^]u8 == type_of(data))
    static_assert(type_of(payload) == string)
    static_assert([]u8 != [^]u8)
    static_assert(Callback == proc(value: i32, flag: bool) -> u64)
    static_assert(Vector == #simd[4]u32)

    // Exercise every stable Type_Kind alternative. This is intentionally
    // exhaustive: the compiler-defined enum is source API, and reserved-word
    // alternatives such as `.struct` and `.distinct` must remain usable.
    static_assert(type_kind(type_of(no_result())) == .void)
    static_assert(type_kind(bool) == .bool)
    static_assert(type_kind(b8) == .boolean_storage)
    static_assert(type_kind(i64) == .signed_integer)
    static_assert(type_kind(u64) == .unsigned_integer)
    static_assert(type_kind(f64) == .float)
    static_assert(type_kind(rune) == .rune)
    static_assert(type_kind(u32be) == .endian_scalar)
    static_assert(type_kind(rawptr) == .raw_pointer)
    static_assert(type_kind(cstring) == .c_string)
    static_assert(type_kind(string) == .string)
    static_assert(type_kind(^u8) == .pointer)
    static_assert(type_kind([^]u8) == .multi_pointer)
    static_assert(type_kind([]u8) == .slice)
    static_assert(type_kind([4]u8) == .array)
    static_assert(type_kind(Pair) == .tuple)
    static_assert(type_kind(Callback) == .procedure)
    static_assert(type_kind(Vector) == .simd)
    static_assert(type_kind(Record) == .struct)
    static_assert(type_kind(Mode) == .enumeration)
    static_assert(type_kind(Choice) == .tagged_union)
    static_assert(type_kind(Overlay) == .raw_union)
    static_assert(type_kind(Meters) == .distinct)
    static_assert(type_kind(type) == .type)

    // Cover every applicability family, including each aggregate and element
    // category, so a query cannot remain implemented only for the one shape
    // used by its first test.
    static_assert(len(type_name(^u32)) == 4)
    static_assert(type_bit_width(u128) == 128)
    static_assert(type_byte_order(u32) == .native)
    static_assert(type_byte_order(u32be) == .big)
    static_assert(type_element(^u32) == u32)
    static_assert(type_element([^]u16) == u16)
    static_assert(type_element([]u32) == u32)
    static_assert(type_element([4]u64) == u64)
    static_assert(type_element(Vector) == u32)
    static_assert(type_element_count([4]u8) == 4)
    static_assert(type_element_count(Vector) == 4)
    static_assert(type_member_count(Pair) == 2)
    static_assert(type_member_count(Record) == 2)
    static_assert(type_member_count(Mode) == 2)
    static_assert(type_member_count(Choice) == 2)
    static_assert(type_member_count(Overlay) == 2)
    static_assert(len(type_member_name(Pair, 1)) == 1)
    static_assert(len(type_member_name(Record, 1)) == 5)
    static_assert(len(type_member_name(Mode, 0)) == 3)
    static_assert(len(type_member_name(Choice, 1)) == 4)
    static_assert(len(type_member_name(Overlay, 0)) == 4)
    static_assert(type_member_type(Pair, 1) == bool)
    static_assert(type_member_type(Record, 1) == u64)
    static_assert(type_member_type(Mode, 1) == u8)
    static_assert(type_member_type(Choice, 1) == u32)
    static_assert(type_member_type(Overlay, 1) == u64)
    static_assert(type_member_offset(Pair, 1) == 1)
    static_assert(type_member_offset(Record, 1) == 8)
    static_assert(type_member_offset(Choice, 1) > 0)
    static_assert(type_member_offset(Overlay, 1) == 0)
    static_assert(type_member_value(Mode, 1) == 1)
    static_assert(type_underlying(Meters) == i64)
    static_assert(type_underlying(Mode) == u8)
    static_assert(type_underlying(u32be) == u32)
    static_assert(type_kind(type_discriminator(Choice)) == .unsigned_integer)
    static_assert(type_parameter_count(Callback) == 2)
    static_assert(type_parameter_type(Callback, 1) == bool)
    static_assert(type_result(Callback) == u64)
    static_assert(type_calling_convention(Callback) == .draft)
    static_assert(type_calling_convention(C_Callback) == .c)
    static_assert(type_is_c_repr(Record))
    static_assert(type_is_c_repr(Mode) == false)
    static_assert(type_is_c_repr(Overlay))
    static_assert(type_requested_alignment(Record) == 16)
    static_assert(type_requested_alignment(Overlay) == 8)
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.semantics.ok);
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource invalid(R"draft(
package bodies

bad_queries :: proc() {
    static_assert(type_element(u32) == u32)
    type_member_name(Type_Kind, 100)
    static_assert(type_byte_order(^u32) == .native)
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("type_element requires a pointer") !=
      std::string::npos);
  EXPECT(state, rendered.find("type_member_name index is out of bounds") !=
      std::string::npos);
  EXPECT(state, rendered.find("type_byte_order requires a scalar storage type") !=
      std::string::npos);
}

void test_compile_time_type_runtime_boundary(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

choose_type :: proc(use_wide: bool) -> type {
    if use_wide {
        return u64
    }
    return u8
}

type_rank :: proc(candidate: type) -> usize {
    return 8 if candidate == u64 else 1
}

Chosen :: choose_type(true)
Chosen_Rank :: type_rank(Chosen)

main :: proc() {
    static_assert(Chosen == u64)
    static_assert(Chosen_Rank == 8)
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.semantics.ok);
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  std::size_t compile_time_only = 0;
  for (const draft::HirProcedure &procedure :
       valid.bodies.program.procedures()) {
    const draft::Symbol &symbol =
        valid.bodies.package.symbols.symbol(procedure.symbol);
    if (symbol.name == "choose_type" || symbol.name == "type_rank") {
      EXPECT(state, procedure.compile_time_only);
      ++compile_time_only;
    } else if (symbol.name == "main") {
      EXPECT(state, !procedure.compile_time_only);
    }
  }
  EXPECT(state, compile_time_only == 2);

  CheckedSource invalid(R"draft(
package bodies

type_rank :: proc(candidate: type) -> usize {
    return 1 if candidate == u8 else 2
}

main :: proc() {
    inferred := u32
    explicit: type
    _ = u64
    _ = type_rank(u8)
    callback := type_rank
    aggregate := (u16, true)
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  for (const draft::Diagnostic &diagnostic :
       invalid.diagnostics.diagnostics()) {
    if (diagnostic.severity == draft::DiagnosticSeverity::Error) {
      EXPECT(state, diagnostic.range.is_valid());
    }
  }
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find(
      "runtime storage cannot contain compile-time 'type' values") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "value containing compile-time 'type' cannot reach runtime") !=
      std::string::npos);
}

void test_dependent_when_type_refinement(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

inspect[T: type] :: proc(value: T) {
    when type_of(value) == string {
        len(value)
    } else when type_of(value) == bool {
        !value
    } else when type_kind(type_of(value)) == .signed_integer {
        cast[i128](value + value)
    } else when type_kind(type_of(value)) == .unsigned_integer {
        cast[u128](value + value)
    } else {
        // The assertion belongs to an unsupported concrete instance. It must
        // not reject the symbolic template or either selected valid branch.
        static_assert(false, "unsupported inspect type")
    }
}

main :: proc() {
    inspect("draft")
    inspect(true)
    inspect(cast[i32](21))
    inspect(cast[u32](21))
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.semantics.ok);
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource synthesized(R"draft(
package bodies

fill[T: type] :: proc(value: T) {
    when type_of(value) == string {
        ... "emit a string-specific statement"
    } else {
        ... "emit a non-string statement"
    }
}

main :: proc() {
    fill("draft")
    fill(cast[i32](1))
}
)draft");
  if (synthesized.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        synthesized.sources, synthesized.diagnostics);
  }
  EXPECT(state, synthesized.bodies.ok);
  std::size_t synthesis_sites = 0;
  for (const draft::SemanticSite &site : synthesized.bodies.package.sites) {
    if (site.kind != draft::SemanticSiteKind::SynthesisStatement) continue;
    ++synthesis_sites;
    EXPECT(state, site.branch_refinements.size() == 1);
  }
  // Sites belong to symbolic source branches. Rechecking two concrete
  // instances must not manufacture four more provider obligations.
  EXPECT(state, synthesis_sites == 2);

  CheckedSource invalid(R"draft(
package bodies

inspect[T: type] :: proc(value: T) {
    when type_of(value) == string {
        len(value)
    } else when type_of(value) == bool {
        !value
    } else when type_kind(type_of(value)) == .signed_integer {
        cast[i128](value + value)
    } else when type_kind(type_of(value)) == .unsigned_integer {
        cast[u128](value + value)
    } else {
        static_assert(false, "unsupported inspect type")
    }
}

main :: proc() {
    inspect(cast[f64](1))
}
)draft");
  EXPECT(state, !invalid.bodies.ok);
  const std::string invalid_rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, invalid_rendered.find(
      "static assertion failed: unsupported inspect type") !=
      std::string::npos);
}

void test_dependent_when_assertion_selection(TestState &state) {
  CheckedSource valid(R"draft(
package bodies

exactly_one :: proc(values: ..type) {
    for value, index in values {
        when index == 0 {
            _ = value
        } else {
            // The literal assertion does not itself name index. It still
            // belongs to the dependent branch and must wait for expansion.
            static_assert(false, "exactly_one received another value")
        }
    }
}

at_most_two :: proc(values: ..type) {
    for value, index in values {
        when index == 0 {
            _ = value
        } else when index == 1 {
            _ = value
        } else {
            static_assert(false, "at_most_two received another value")
        }
    }
}

accept_zero[N: usize] :: proc() {
    when N == 0 {
        static_assert(true)
    } else {
        static_assert(false, "N must be zero")
    }
}

main :: proc() {
    exactly_one(42)
    at_most_two("draft", true)
    accept_zero[0]()
}
)draft");
  if (valid.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(valid.sources, valid.diagnostics);
  }
  EXPECT(state, valid.semantics.ok);
  EXPECT(state, valid.bodies.ok);
  EXPECT(state, !valid.diagnostics.has_errors());

  CheckedSource selected_invalid(R"draft(
package bodies

exactly_one :: proc(values: ..type) {
    for value, index in values {
        when index == 0 {
            _ = value
        } else {
            static_assert(false, "exactly_one received another value")
        }
    }
}

accept_zero[N: usize] :: proc() {
    when N == 0 {
    } else {
        static_assert(false, "N must be zero")
    }
}

main :: proc() {
    exactly_one(1, 2)
    accept_zero[1]()
}
)draft");
  EXPECT(state, !selected_invalid.bodies.ok);
  const std::string selected_rendered = draft::render_diagnostics(
      selected_invalid.sources, selected_invalid.diagnostics);
  EXPECT(state, selected_rendered.find(
      "static assertion failed: exactly_one received another value") !=
      std::string::npos);
  EXPECT(state, selected_rendered.find(
      "static assertion failed: N must be zero") != std::string::npos);

  CheckedSource runtime_if(R"draft(
package bodies

ordinary :: proc(condition: bool) {
    if condition {
        static_assert(false, "runtime if cannot delay static_assert")
    }
}
)draft");
  EXPECT(state, !runtime_if.bodies.ok);
  const std::string runtime_rendered =
      draft::render_diagnostics(runtime_if.sources, runtime_if.diagnostics);
  EXPECT(state, runtime_rendered.find(
      "static assertion failed: runtime if cannot delay static_assert") !=
      std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_body_results_do_not_mutate_or_reenter_declarations(state);
  test_body_root_results_are_task_owned(state);
  test_common_typed_bodies(state);
  test_body_diagnostics(state);
  test_parametric_procedure_instances(state);
  test_static_argument_pack_instances(state);
  test_static_argument_pack_use_diagnostics(state);
  test_value_parametric_nominal_composition(state);
  test_procedural_structural_alias_composition(state);
  test_dependent_value_inference(state);
  test_nested_procedures(state);
  test_nested_procedure_capture_diagnostics(state);
  test_assignment_discards_and_tuple_patterns(state);
  test_composite_literal_shapes(state);
  test_switch_case_semantics(state);
  test_local_type_declarations(state);
  test_string_index_is_immutable(state);
  test_parameter_immutability(state);
  test_multi_pointer_slice_shape(state);
  test_constant_bounds_diagnostics(state);
  test_parametric_procedure_value_diagnostics(state);
  test_definite_initialization(state);
  test_layout_intrinsics_and_static_assert(state);
  test_checked_numeric_casts(state);
  test_storage_pointer_and_distinct_semantics(state);
  test_raw_string_data_intrinsic(state);
  test_selected_when_condition_type_validation(state);
  test_target_query_argument_preflight(state);
  test_statement_when_uses_lexical_bindings(state);
  test_integer_shift_count_types(state);
  test_conditional_context_from_either_branch(state);
  test_compound_assignment_operators(state);
  test_invalid_operator_type_matrix(state);
  test_numeric_context_boundaries(state);
  test_builtin_context_value(state);
  test_compile_time_type_inspection(state);
  test_compile_time_type_runtime_boundary(state);
  test_dependent_when_type_refinement(state);
  test_dependent_when_assertion_selection(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " body checker expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all body checker tests passed\n";
  return EXIT_SUCCESS;
}
