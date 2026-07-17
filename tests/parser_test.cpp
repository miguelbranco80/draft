// Surface parser coverage for every major Draft 1 grammar category.
//
// These tests intentionally use representative complete source rather than
// invoking private parser methods. A successful parse therefore exercises UTF-8
// lexing, semicolon insertion, attachment continuation, recursive declarations,
// expression precedence, and braced-category selection together. Node counts
// ensure important constructs are represented rather than merely skipped.

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
  test_types_declarations_and_interop(state);
  test_procedure_control_flow_and_expressions(state);
  test_synthesis_and_assembly_surface(state);
  test_recovery_builds_a_tree(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " parser test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all parser tests passed\n";
  return EXIT_SUCCESS;
}
