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
    return sum[i64](values[:]) + explicit_last + inferred_last
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
  EXPECT(state, templates == 4);
  EXPECT(state, concrete_instances == 4);
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

main :: proc() -> i64 {
    value: Envelope[2]
    value.buffer.values[3] = 42
    return last_enveloped(&value)
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
       source.semantics.package.parametric_type_instances) {
    const draft::Symbol &template_symbol =
        source.semantics.package.symbols.symbol(instance.source);
    if (template_symbol.name != "Envelope" || instance.arguments.size() != 1 ||
        instance.arguments.front().value_expression.is_valid()) {
      continue;
    }
    const draft::TypeId type =
        source.semantics.package.symbols.symbol(instance.instance).type;
    saw_concrete_envelope = true;
    EXPECT(state, source.semantics.package.types.type(type).layout ==
                      draft::TypeLayout({true, 32, 8}));
  }
  EXPECT(state, saw_concrete_envelope);
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

  std::size_t nested_procedures = 0;
  std::vector<std::string> linkage_names;
  std::vector<std::string> helper_linkage_names;
  for (const draft::HirProcedure &procedure :
       source.bodies.program.procedures()) {
    const draft::Symbol &symbol =
        source.semantics.package.symbols.symbol(procedure.symbol);
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

  // Compile through a fresh semantic graph, rather than merely emitting the
  // first graph twice. The exact linkage sequence must not depend on transient
  // SymbolId/ScopeId allocation from another compilation.
  CheckedSource repeated(text);
  std::vector<std::string> repeated_linkage_names;
  for (const draft::HirProcedure &procedure :
       repeated.bodies.program.procedures()) {
    const draft::Symbol &symbol =
        repeated.semantics.package.symbols.symbol(procedure.symbol);
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
       index < source.semantics.package.symbols.symbol_count();
       ++index) {
    const draft::Symbol &symbol = source.semantics.package.symbols.symbol(
        draft::SymbolId{static_cast<std::uint32_t>(index)});
    if (symbol.kind == draft::SymbolKind::Type &&
        source.semantics.package.symbols.scope(symbol.scope).kind ==
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
  EXPECT(state, valid.semantics.package.runtime_context_type.is_valid());
  if (valid.semantics.package.runtime_context_type.is_valid()) {
    const draft::Type &context = valid.semantics.package.types.type(
        valid.semantics.package.runtime_context_type);
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

} // namespace

int main() {
  TestState state;
  test_common_typed_bodies(state);
  test_body_diagnostics(state);
  test_parametric_procedure_instances(state);
  test_value_parametric_nominal_composition(state);
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
  test_integer_shift_count_types(state);
  test_compound_assignment_operators(state);
  test_numeric_context_boundaries(state);
  test_builtin_context_value(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " body checker expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all body checker tests passed\n";
  return EXIT_SUCCESS;
}
