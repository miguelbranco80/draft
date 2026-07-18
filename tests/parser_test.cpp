// Surface parser conformance for the complete Draft 1 concrete syntax tree.
//
// The primary valid fixture contains every non-error NodeKind. This is more
// deliberate than checking only a few representative constructs: adding a tree
// production requires adding source that reaches it. Focused malformed fixtures
// then exercise the parser's recovery boundaries without invoking private parser
// methods. Every case still passes through UTF-8 lexing and semicolon insertion,
// which catches integration mistakes that unit-testing individual parse methods
// would hide.

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "syntax/syntax_tree.h"

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
      std::cerr << "parser_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct ParsedSource {
  draft::SourceManager sources;
  draft::FileId file;
  draft::DiagnosticSink diagnostics;
  draft::SyntaxTree tree;

  explicit ParsedSource(std::string text)
      : file(sources.add_source("parser_test.draft", std::move(text))),
        tree(draft::parse_source_file(sources, file, diagnostics)) {}
};

void expect_clean(TestState &state, const ParsedSource &source) {
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
    std::cerr << draft::dump_syntax_tree(source.tree);
  }
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.tree.root().is_valid());
  EXPECT(state, source.tree.node(source.tree.root()).kind == draft::NodeKind::SourceFile);
}

void expect_every_valid_node_kind(TestState &state, const ParsedSource &source) {
  const int first = static_cast<int>(draft::NodeKind::SourceFile);
  const int last = static_cast<int>(draft::NodeKind::Last);

  // NodeKind is intentionally contiguous. Error is tested by the malformed
  // fixtures below; every other concrete production must occur in clean source.
  for (int value = first; value <= last; ++value) {
    const auto kind = static_cast<draft::NodeKind>(value);
    const std::string expectation =
        "valid grammar fixture contains " + std::string(draft::node_kind_name(kind));
    state.expect(source.tree.count(kind) != 0, expectation, __LINE__);
  }
}

void test_every_valid_node_production(TestState &state) {
  ParsedSource source(R"draft(
package grammar

docs "Top-level grammar fixture."
    file "GRAMMAR.md"
    folder "examples/"

import core/math as math
import core/memory

... "generate one declaration"
    file "GENERATED.md"

judge "The fixture intentionally reaches every concrete parser production."

Container[T: type, N: usize] :: @align(16) @fixture struct {
    docs "Member metadata."
    judge "Member judgment."
    ... "generate one member"

    first, second: T,
    when true {
        selected: [N]T,
    } else when false {
        fallback: []T,
    } else {
        final: [^]T,
    }
    deny asm, unchecked {
        guarded: ^T,
    }
}

Mode :: enum u16 {
    basic,
    extended = 7,
}

Choice :: union u8 {
    none,
    some: u32,
}

Bits :: @repr(C) raw union {
    integer: u64,
    pointer: rawptr,
}

Duration :: distinct i64
Lanes :: #simd[4]f32
Pair_Type :: (bool, u32)
Grouped_Type :: (u64)
Callback :: c proc(left, right: u32) -> u32
Applied :: Container[u32, 4]

when true {
    Selected :: u64
} else when false {
    Selected :: u32
} else {
    Selected :: u16
}

deny asm {
    Safe_Selected :: Selected
}

foreign libc {
    puts :: c "puts" proc(message: cstring) -> c.int
}

export draft_entry :: c "draft_entry" proc(argument: u32) -> c.int {
    values := [4]u32{1, 2, 3, 4}
    record := Container[u32, 4]{first = 1, second = 2}
    left, right: u32 = 1
    scratch: u32 = ---
    (_, initial): (bool, u32) = read_initial()
    pointer: ^u32

    grouped := (initial)
    tuple := (initial, 2)
    unary := -initial
    binary := initial + 1
    conditional := initial if true else 0
    called := helper(initial)
    indexed := values[0]
    sliced := values[1:3]
    member := tuple.1
    dereferenced := pointer^
    alternative := .some(initial)
    denied := deny math, asm { initial }
    generated := ... "generate an expression"

    helper(initial)
    left, right = right, left

    {
        defer finish(record)
        judge "Nested blocks are lexical statement regions."
    }

    if argument == 0 {
        left += 1
    } else if argument > 1 {
        right += 1
    } else {
        right -= 1
    }

    for {
        break
    }

    for value, index in values {
        left += value
        if index == 3 {
            continue
        }
    }

    for index: usize = 0; index < len(values); index += 1 {
        values[index] = left
    }

    switch alternative {
    case .some(value), .other(value):
        left = value
    case .none:
        left = 0
    case:
        right = 0
    }

    when target.pointer_bits == 64 {
        left += 1
    } else when false {
        left += 2
    } else {
        left += 3
    }

    deny math, asm {
        unchecked {
            right += values[0]
        }
    }

    ... "generate one statement"

    asm aarch64 {
        clobber memory
        ... "generate one assembly row"
        dmb ish
    }

    assembled := asm aarch64 -> u64 {
        in x0 = initial
        out x0
        add x0, x0, #1
    }

    return cast[c.int](assembled)
}
)draft");

  expect_clean(state, source);
  expect_every_valid_node_kind(state, source);
}

void test_types_declarations_and_interop(TestState &state) {
  ParsedSource source(R"draft(
package model

docs "Package design context."
    file "DESIGN.md"
    folder "notes/"

import core/c as c
import core/result

Pair[T: type, U: type] :: struct {
    first: T,
    second: U,
}

Buffer[T: type, N: usize] :: @align(16) struct {
    data: [N]T,
}

Mode :: enum u16 {
    Basic,
    Extended = 7,
}

Choice :: union u16 {
    none,
    some: u32,
}

C_Value :: @repr(C) raw union {
    bits: u64,
    pointer: rawptr,
}

Duration :: distinct i64
Callback :: c proc(value: u8) -> c.int

when target.pointer_bits == 64 {
    Word :: u64
} else {
    Word :: u32
}

deny asm {
    Safe_Word :: Word
}

foreign libc {
    puts :: c "puts" proc(message: cstring) -> c.int
}

export draft_entry :: c "draft_entry" proc() -> c.int {
    return 0
}
)draft");

  expect_clean(state, source);
  EXPECT(state, source.tree.count(draft::NodeKind::StructType) == 2);
  EXPECT(state, source.tree.count(draft::NodeKind::EnumType) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::TaggedUnionType) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::RawUnionType) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::DistinctType) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::ForeignBlock) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::ExportDeclaration) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::Documentation) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::Attachment) == 2);
}

void test_procedure_control_flow_and_expressions(TestState &state) {
  ParsedSource source(R"draft(
package program

import core/heap as heap

main :: proc() -> int {
    values := [4]u32{1, 2, 3, 4}
    sum: u32
    scratch: u32 = ---
    (_, initial): (bool, u32) = read_initial()
    sum = initial
    sum += (sum, initial).1

    for value, index in values {
        sum += value
        if index == 3 {
            break
        } else if index > 4 {
            continue
        }
    }

    for i: usize = 0; i < len(values); i += 1 {
        values[i] = sum if sum > values[i] else values[i]
    }

    switch .some(sum) {
    case .some(value):
        sum = value
    case .none:
        sum = 0
    case:
        return -1
    }

    when target.pointer_bits == 64 {
        sum += 1
    } else {
        sum += 2
    }

    deny heap, asm, context.allocator {
        unchecked {
            sum += values[0]
        }
    }

    {
        defer finish(sum)
        judge "The final sum follows the selected branches."
    }

    return cast[int](sum)
}
)draft");

  expect_clean(state, source);
  EXPECT(state, source.tree.count(draft::NodeKind::ForStatement) == 2);
  EXPECT(state, source.tree.count(draft::NodeKind::IfStatement) == 2);
  EXPECT(state, source.tree.count(draft::NodeKind::SwitchStatement) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::WhenStatement) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::DenyStatement) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::UncheckedStatement) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::ConditionalExpression) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::TuplePattern) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::UninitializedExpression) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::Judgment) == 1);
}

void test_binary_xor_and_postfix_dereference(TestState &state) {
  ParsedSource source(R"draft(
package operators

combine :: proc(left, right: u32, pointer: ^u32) -> u32 {
    bits := (left ^ right)
    return bits + pointer^
}
)draft");

  expect_clean(state, source);
  EXPECT(state, source.tree.count(draft::NodeKind::BinaryExpression) == 2);
  EXPECT(state, source.tree.count(draft::NodeKind::DereferenceExpression) == 1);
}

void test_synthesis_and_assembly_surface(TestState &state) {
  ParsedSource source(R"draft(
package generated

... "generate package declarations"
    file "API.md"

Packet :: struct {
    ... "generate wire fields"
        file "PROTOCOL.md"
}

compute :: proc(input: []u8) -> u64 {
    value: u64 = ... "compute a starting value"
        file "ALGORITHM.md"

    ... "perform the remaining statements"

    asm aarch64 {
        clobber memory
        ... "emit the required barrier"
        dmb ish
    }

    return asm aarch64 -> u64 {
        in x0 = value
        out x0
        add x0, x0, #1
    }
}
)draft");

  expect_clean(state, source);
  EXPECT(state, source.tree.count(draft::NodeKind::SynthesisDeclaration) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::SynthesisMember) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::SynthesisExpression) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::SynthesisStatement) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::SynthesisAssembly) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::AsmStatement) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::AsmExpression) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::AsmInput) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::AsmOutput) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::AsmClobber) == 1);
  EXPECT(state, source.tree.count(draft::NodeKind::AsmInstruction) == 2);
  EXPECT(state, source.tree.count(draft::NodeKind::Attachment) == 3);
}

struct InvalidGrammarCase {
  std::string_view name;
  std::string_view source;
  std::string_view expected_diagnostic;
};

void test_invalid_production_recovery(TestState &state) {
  // Each fixture isolates one local recovery boundary. Keeping the surrounding
  // source valid makes the expected diagnostic meaningful and ensures a parser
  // change cannot pass merely by failing earlier for an unrelated reason.
  static constexpr InvalidGrammarCase cases[] = {
      {"missing package", "value := 1\n", "source file must begin with a package declaration"},
      {"missing package name", "package\n", "expected package name after 'package'"},
      {"missing import path", "package bad\nimport /name\n", "expected package path after 'import'"},
      {"missing import component", "package bad\nimport core/\n", "expected package path component after '/'"},
      {"missing import alias", "package bad\nimport core as\n", "expected local package alias after 'as'"},
      {"empty docs", "package bad\ndocs\n", "docs requires an inline string"},
      {"unquoted attachment", "package bad\ndocs file README\n", "attachment path must be a quoted string"},
      {"missing judgment claim", "package bad\njudge file \"proof.txt\"\n", "judge requires a claim string"},
      {"missing declaration operator", "package bad\nvalue u32\n", "expected ':', ':=', or '::'"},
      {"missing tuple binding", "package bad\n(, value): (u32, u32)\n", "expected binding name in tuple pattern"},
      {"missing parametric colon", "package bad\nBox[T type] :: struct {}\n", "expected ':' after parametric parameter name"},
      {"missing parametric close", "package bad\nBox[T: type :: struct {}\n", "expected ']' after parametric parameters"},
      {"missing parameter colon", "package bad\nrun :: proc(value u32) {}\n", "expected ':' after parameter name"},
      {"missing procedure keyword", "package bad\nrun :: c \"run\" u32\n", "expected 'proc'"},
      {"missing attribute name", "package bad\nRecord :: @() struct {}\n", "expected attribute name after '@'"},
      {"missing attribute close", "package bad\nRecord :: @align(16 struct {}\n", "expected ')' after attribute arguments"},
      {"missing multi-pointer close", "package bad\nPointer :: [^u32\n", "expected ']' in multi-pointer type"},
      {"missing array close", "package bad\nArray :: [4 u32\n", "expected ']' after array length"},
      {"missing simd open", "package bad\nVector :: #simd 4]f32\n", "expected '[' before SIMD lane count"},
      {"raw without union", "package bad\nBits :: raw struct {}\n", "expected 'union' after 'raw'"},
      {"missing qualified type name", "package bad\nvalue: core.\n", "expected type name after '.'"},
      {"missing type argument close", "package bad\nvalue: Box[u32\n", "expected ']' after type arguments"},
      {"missing member region", "package bad\nRecord :: struct\n", "expected '{' to begin type members"},
      {"missing field colon", "package bad\nRecord :: struct { field u32 }\n", "expected ':' after field name"},
      {"missing member terminator", "package bad\nRecord :: struct { first: u32 second: u32 }\n", "expected ',' or semicolon after field"},
      {"missing conditional else", "package bad\nvalue := 1 if true\n", "conditional expression requires 'else'"},
      {"comparison chain", "package bad\nvalue := 1 < 2 < 3\n", "comparison operators do not associate"},
      {"missing call close", "package bad\nvalue := function(1\n", "expected ')' after call arguments"},
      {"missing bracket close", "package bad\nvalue := values[1\n", "expected ']' after bracket expression"},
      {"missing member selector", "package bad\nvalue := values.\n", "expected member name or tuple index after '.'"},
      {"missing alternative name", "package bad\nvalue := .\n", "expected alternative name after '.'"},
      {"missing group close", "package bad\nvalue := (1\n", "expected ')' after expression"},
      {"missing composite close", "package bad\nvalue := [1]u8{0\n", "expected '}' after composite literal"},
      {"missing denied expression open", "package bad\nvalue := deny asm 1\n", "expected '{' before denied expression"},
      {"missing block open", "package bad\nrun :: proc() { if true return }\n", "expected '{' to begin block"},
      {"missing for separator", "package bad\nrun :: proc() { for i := 0; i < 4 {} }\n", "expected second ';' in for clause"},
      {"missing switch open", "package bad\nrun :: proc(value: u32) { switch value case: return }\n", "expected '{' after switch subject"},
      {"missing switch case keyword", "package bad\nrun :: proc(value: u32) { switch value { value: return } }\n", "expected 'case' in switch"},
      {"missing switch case colon", "package bad\nrun :: proc(value: u32) { switch value { case 1 return } }\n", "expected ':' after switch case labels"},
      {"missing unchecked block", "package bad\nrun :: proc() { unchecked return }\n", "expected '{' to begin block"},
      {"missing assembly architecture", "package bad\nrun :: proc() { asm {} }\n", "expected assembly architecture after 'asm'"},
      {"missing assembly input equal", "package bad\nrun :: proc() { asm aarch64 { in x0 value } }\n", "expected '=' before assembly input value"},
      {"value assembly statement", "package bad\nrun :: proc() { asm aarch64 -> u64 {} }\n", "value-producing asm expression cannot be used as an asm statement"},
      {"missing assembly close", "package bad\nrun :: proc() { asm aarch64 { nop\n", "expected '}' after assembly instructions"},
  };

  for (const InvalidGrammarCase &test : cases) {
    ParsedSource source(std::string(test.source));
    const std::string rendered =
        draft::render_diagnostics(source.sources, source.diagnostics);
    const std::string error_expectation =
        std::string(test.name) + " reports a parser error";
    const std::string diagnostic_expectation =
        std::string(test.name) + " reports '" + std::string(test.expected_diagnostic) + "'";
    const std::string root_expectation =
        std::string(test.name) + " retains a syntax-tree root";

    if (!source.diagnostics.has_errors() ||
        rendered.find(test.expected_diagnostic) == std::string::npos) {
      std::cerr << "invalid grammar fixture '" << test.name << "':\n"
                << rendered << draft::dump_syntax_tree(source.tree);
    }
    state.expect(source.diagnostics.has_errors(), error_expectation, __LINE__);
    state.expect(
        rendered.find(test.expected_diagnostic) != std::string::npos,
        diagnostic_expectation,
        __LINE__);
    state.expect(source.tree.root().is_valid(), root_expectation, __LINE__);
  }
}

void test_recovery_builds_a_tree(TestState &state) {
  ParsedSource source(R"draft(
not_package broken

value :: struct {
    field u32,
    other: []
}

run :: proc(input u32) {
    if {
        return
    }
}
)draft");

  EXPECT(state, source.diagnostics.error_count() >= 4);
  EXPECT(state, source.tree.root().is_valid());
  EXPECT(state, source.tree.count(draft::NodeKind::Error) >= 1);
  const std::string rendered = draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("package") != std::string::npos);
  EXPECT(state, rendered.find("expected") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_every_valid_node_production(state);
  test_types_declarations_and_interop(state);
  test_procedure_control_flow_and_expressions(state);
  test_binary_xor_and_postfix_dereference(state);
  test_synthesis_and_assembly_surface(state);
  test_invalid_production_recovery(state);
  test_recovery_builds_a_tree(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " parser test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all parser tests passed\n";
  return EXIT_SUCCESS;
}
