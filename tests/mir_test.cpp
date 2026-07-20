// Explicit CFG/MIR lowering, verifier, and unresolved-site boundary tests.

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
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "mir_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct LoweredSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticAnalysisResult semantics;
  draft::BodyCheckResult bodies;
  draft::MirLoweringResult mir;

  explicit LoweredSource(
      std::string text,
      draft::RuntimeAssertionMode runtime_assertions =
          draft::RuntimeAssertionMode::On) {
    loaded.short_name = "mir";
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
    mir = draft::lower_package_to_mir(
        semantics.package, bodies.program, runtime_assertions, diagnostics);
  }
};

void test_structured_lowering(TestState &state) {
  LoweredSource source(R"draft(
package mir

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

choose :: proc(mode: Mode) -> i64 {
    switch mode {
    case .Off:
        return 10
    case .On:
        return 20
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

wrap :: proc(value: i64) -> Choice {
    return .some(value)
}

compute :: proc(values: []i64, flag: bool) -> i64 {
    total: i64
    defer consume(total)
    if flag && len(values) > 0 {
        total = values[0]
    } else {
        total = 1
    }
    (forty, _): (i64, i64) = (40, 0)
    (_, two): (i64, i64) = (0, 2)
    total += forty + two
    selected := total if flag else 2
    fixed := [3]i64{10, 20, 30}
    selected += fixed[1 + 1]
    fixed_tail := fixed[1:3]
    selected += cast[i64](len(fixed_tail))
    for i: i64 = 0; i < 3; i += 1 {
        selected += i
    }
    for value, index in values {
        if index == 2 {
            continue
        }
        selected += value
        if selected > 100 {
            break
        }
    }
    unchecked {
        selected += values[0]
    }
    return selected
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, source.mir.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.mir.lowered_procedures == 5);
  EXPECT(state, source.mir.program.procedures().size() == 5);

  std::size_t conditional_branches = 0;
  std::size_t switches = 0;
  std::size_t calls = 0;
  std::size_t bounds_checks = 0;
  std::size_t slice_bounds_checks = 0;
  std::size_t stores = 0;
  std::size_t extracted_members = 0;
  for (const draft::MirProcedure &procedure : source.mir.program.procedures()) {
    EXPECT(state, procedure.valid);
    EXPECT(state, procedure.entry.is_valid());
    for (const draft::MirBlock &block : procedure.blocks) {
      EXPECT(state, block.terminator.kind != draft::MirTerminatorKind::Invalid);
      if (block.terminator.kind == draft::MirTerminatorKind::ConditionalBranch) {
        ++conditional_branches;
      }
      if (block.terminator.kind == draft::MirTerminatorKind::Switch) ++switches;
    }
    for (const draft::MirInstruction &instruction : procedure.instructions) {
      if (instruction.kind == draft::MirInstructionKind::Call) ++calls;
      if (instruction.kind == draft::MirInstructionKind::BoundsCheck) {
        ++bounds_checks;
      }
      if (instruction.kind == draft::MirInstructionKind::SliceBoundsCheck) {
        ++slice_bounds_checks;
      }
      if (instruction.kind == draft::MirInstructionKind::Store) ++stores;
      if (instruction.kind == draft::MirInstructionKind::ExtractMember) {
        ++extracted_members;
      }
    }
  }
  EXPECT(state, conditional_branches >= 6);
  EXPECT(state, switches == 2);
  EXPECT(state, calls == 1);
  EXPECT(state, bounds_checks == 1);
  EXPECT(state, slice_bounds_checks == 0);
  EXPECT(state, stores >= 12);
  EXPECT(state, extracted_members >= 4);
}

void test_unresolved_synthesis_stops_lowering(TestState &state) {
  LoweredSource source(R"draft(
package mir

main :: proc() -> i64 {
    value: i64 = ... "produce a value"
    return value
}
)draft");

  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.mir.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("unresolved synthesis expression") != std::string::npos);
}

void test_required_integer_traps_are_explicit(TestState &state) {
  LoweredSource source(R"draft(
package mir

divide :: proc(left, right: i64) -> i64 {
    return left / right
}

shift :: proc(value: u32, count: usize) -> u32 {
    return value << count
}

wide_shift :: proc(value: u128, count: i8) -> u128 {
    return value << count
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.mir.ok);
  std::size_t traps = 0;
  std::size_t unreachable = 0;
  std::size_t full_width_count_conversions = 0;
  const std::optional<draft::TypeId> u128_type =
      source.semantics.package.types.find_builtin("u128");
  EXPECT(state, u128_type.has_value());
  for (const draft::MirProcedure &procedure : source.mir.program.procedures()) {
    for (const draft::MirInstruction &instruction : procedure.instructions) {
      if (instruction.kind == draft::MirInstructionKind::Trap) ++traps;
      if (u128_type.has_value() &&
          instruction.kind == draft::MirInstructionKind::Convert &&
          instruction.type == *u128_type) {
        ++full_width_count_conversions;
      }
    }
    for (const draft::MirBlock &block : procedure.blocks) {
      if (block.terminator.kind == draft::MirTerminatorKind::Unreachable) {
        ++unreachable;
      }
    }
  }
  EXPECT(state, traps == 3);
  EXPECT(state, unreachable == 3);
  EXPECT(state, full_width_count_conversions == 2);
}

void test_disabled_assertions_do_not_evaluate_operands(TestState &state) {
  const std::string program = R"draft(
package mir

condition :: proc(value: ^i64) -> bool {
    value^ += 1
    return true
}

message :: proc(value: ^i64) -> string {
    value^ += 10
    return "assertion message"
}

main :: proc() -> i64 {
    value: i64 = 0
    assert(condition(&value), message(&value))
    return value
}
)draft";

  LoweredSource enabled(program, draft::RuntimeAssertionMode::On);
  LoweredSource disabled(program, draft::RuntimeAssertionMode::Off);
  if (enabled.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        enabled.sources, enabled.diagnostics);
  }
  if (disabled.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        disabled.sources, disabled.diagnostics);
  }
  EXPECT(state, enabled.mir.ok);
  EXPECT(state, disabled.mir.ok);

  auto main_counts = [](const LoweredSource &source) {
    std::size_t calls = 0;
    std::size_t assertions = 0;
    for (const draft::MirProcedure &procedure :
         source.mir.program.procedures()) {
      if (source.semantics.package.symbols.symbol(procedure.symbol).name !=
          "main") {
        continue;
      }
      for (const draft::MirInstruction &instruction :
           procedure.instructions) {
        if (instruction.kind == draft::MirInstructionKind::Call) ++calls;
        if (instruction.kind == draft::MirInstructionKind::Assert) {
          ++assertions;
        }
      }
    }
    return std::pair(calls, assertions);
  };

  const auto [enabled_calls, enabled_assertions] = main_counts(enabled);
  const auto [disabled_calls, disabled_assertions] = main_counts(disabled);
  EXPECT(state, enabled_calls == 2);
  EXPECT(state, enabled_assertions == 1);
  EXPECT(state, disabled_calls == 0);
  EXPECT(state, disabled_assertions == 0);
}

// Proves the erasure boundary for static argument packs. Body checking must
// replace the open source tail with ordinary parameters and must splice one
// loop body per argument before MIR sees the program. The calls to `record`
// make expansion order observable in the MIR instruction stream without
// relying on a native backend or optimizer.
void test_static_argument_packs_erase_before_mir(TestState &state) {
  LoweredSource source(R"draft(
package mir

record :: proc(index: usize) {
}

expand :: proc(values: ..type) {
    static_assert(len(values) >= 0)
    for value, index in values {
        when index == 0 {
            static_assert(type_of(value) == int)
        } else when index == 1 {
            static_assert(type_of(value) == bool)
        } else when index == 2 {
            static_assert(type_of(value) == string)
        } else {
            static_assert(type_of(value) == string)
        }
        record(index)
    }
}

main :: proc() {
    expand()
    expand(7, true, "draft")
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, source.mir.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  draft::SymbolId populated_instance;
  draft::SymbolId empty_instance;
  for (const draft::ParametricInstanceRecord &instance :
       source.semantics.package.parametric_instances) {
    const draft::Symbol &source_symbol =
        source.semantics.package.symbols.symbol(instance.source);
    if (source_symbol.name != "expand") continue;
    if (instance.pack_types.empty()) {
      empty_instance = instance.instance;
    } else if (instance.pack_types.size() == 3) {
      populated_instance = instance.instance;
    }
  }
  EXPECT(state, empty_instance.is_valid());
  EXPECT(state, populated_instance.is_valid());

  bool saw_empty = false;
  bool saw_populated = false;
  for (const draft::MirProcedure &procedure : source.mir.program.procedures()) {
    if (procedure.symbol != empty_instance &&
        procedure.symbol != populated_instance) {
      continue;
    }

    const draft::Type &signature =
        source.semantics.package.types.type(procedure.type);
    std::size_t parameter_count = 0;
    for (const draft::MirLocal &local : procedure.locals) {
      if (local.kind == draft::MirLocalKind::Parameter) ++parameter_count;
      EXPECT(
          state,
          source.semantics.package.types.type(local.type).kind !=
              draft::TypeKind::TypeParameter);
    }
    for (const draft::MirInstruction &instruction : procedure.instructions) {
      EXPECT(state, instruction.kind != draft::MirInstructionKind::Length);
      if (instruction.type.is_valid()) {
        EXPECT(
            state,
            source.semantics.package.types.type(instruction.type).kind !=
                draft::TypeKind::TypeParameter);
      }
    }

    if (procedure.symbol == empty_instance) {
      saw_empty = true;
      EXPECT(state, signature.members.size() == 1);
      EXPECT(state, parameter_count == 0);
      for (const draft::MirInstruction &instruction : procedure.instructions) {
        EXPECT(state, instruction.kind != draft::MirInstructionKind::Call);
      }
      continue;
    }

    saw_populated = true;
    EXPECT(state, signature.members.size() == 4);
    EXPECT(state, parameter_count == 3);
    std::vector<std::string> recorded_indices;
    for (const draft::MirInstruction &instruction : procedure.instructions) {
      if (instruction.kind != draft::MirInstructionKind::Call ||
          instruction.operands.empty()) {
        continue;
      }
      const draft::MirValue &argument =
          procedure.value(instruction.operands.back());
      const draft::MirInstruction &definition =
          procedure.instruction(argument.definition);
      if (definition.kind == draft::MirInstructionKind::Constant &&
          definition.constant.kind == draft::ConstantKind::Integer) {
        recorded_indices.push_back(
            definition.constant.integer.to_decimal());
      }
    }
    EXPECT(state, recorded_indices == std::vector<std::string>({"0", "1", "2"}));
  }
  EXPECT(state, saw_empty);
  EXPECT(state, saw_populated);
}

void test_compile_time_type_procedures_erase_before_mir(TestState &state) {
  LoweredSource source(R"draft(
package mir

choose_type :: proc(wide: bool) -> type {
    return u64 if wide else u8
}

type_rank :: proc(candidate: type) -> usize {
    return 8 if candidate == u64 else 1
}

Chosen :: choose_type(true)
Rank :: type_rank(Chosen)

main :: proc() -> int {
    static_assert(Chosen == u64)
    static_assert(Rank == 8)
    return 0
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, source.mir.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.bodies.program.procedures().size() == 3);
  EXPECT(state, source.mir.program.procedures().size() == 1);
  if (source.mir.program.procedures().size() == 1) {
    const draft::Symbol &symbol = source.semantics.package.symbols.symbol(
        source.mir.program.procedures().front().symbol);
    EXPECT(state, symbol.name == "main");
  }
}

// String data extraction remains an explicit MIR operation. This prevents the
// backend from inferring language meaning from the current two-word string
// representation and proves both literals and derived string views reach the
// same target-independent operation.
void test_raw_string_data_lowering(TestState &state) {
  LoweredSource source(R"draft(
package mir

literal :: proc() -> [^]u8 {
    return raw_data("literal")
}

slice :: proc(text: string) -> [^]u8 {
    return raw_data(text[2:])
}
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(
        source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, source.mir.ok);
  EXPECT(state, !source.diagnostics.has_errors());

  std::size_t extractions = 0;
  for (const draft::MirProcedure &procedure :
       source.mir.program.procedures()) {
    for (const draft::MirInstruction &instruction : procedure.instructions) {
      if (instruction.kind == draft::MirInstructionKind::RawData) {
        ++extractions;
        EXPECT(state, instruction.operands.size() == 1);
        const draft::Type &result =
            source.semantics.package.types.type(instruction.type);
        EXPECT(state, result.kind == draft::TypeKind::MultiPointer);
        EXPECT(state, result.element ==
            source.semantics.package.types.builtins().u8_type);
      }
    }
  }
  EXPECT(state, extractions == 2);
}

} // namespace

int main() {
  TestState state;
  test_structured_lowering(state);
  test_unresolved_synthesis_stops_lowering(state);
  test_required_integer_traps_are_explicit(state);
  test_disabled_assertions_do_not_evaluate_operands(state);
  test_static_argument_packs_erase_before_mir(state);
  test_compile_time_type_procedures_erase_before_mir(state);
  test_raw_string_data_lowering(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " MIR expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all MIR tests passed\n";
  return EXIT_SUCCESS;
}
