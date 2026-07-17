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

  explicit LoweredSource(std::string text) {
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
        semantics.package, bodies.program, diagnostics);
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
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.mir.ok);
  std::size_t traps = 0;
  std::size_t unreachable = 0;
  for (const draft::MirProcedure &procedure : source.mir.program.procedures()) {
    for (const draft::MirInstruction &instruction : procedure.instructions) {
      if (instruction.kind == draft::MirInstructionKind::Trap) ++traps;
    }
    for (const draft::MirBlock &block : procedure.blocks) {
      if (block.terminator.kind == draft::MirTerminatorKind::Unreachable) {
        ++unreachable;
      }
    }
  }
  EXPECT(state, traps == 2);
  EXPECT(state, unreachable == 2);
}

} // namespace

int main() {
  TestState state;
  test_structured_lowering(state);
  test_unresolved_synthesis_stops_lowering(state);
  test_required_integer_traps_are_explicit(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " MIR expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all MIR tests passed\n";
  return EXIT_SUCCESS;
}
