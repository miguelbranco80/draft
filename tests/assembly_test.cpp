// Parsed AArch64 assembly validation and MIR/LLVM handoff tests.

#include "assembly/aarch64.h"
#include "backend/llvm_ir.h"
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
      std::cerr << "assembly_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct CheckedAssembly {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::SemanticAnalysisResult semantics;
  draft::BodyCheckResult bodies;
  draft::HirProgram hir;
  draft::Aarch64CAbiTable abi;
  draft::AssemblyProgram assembly;

  explicit CheckedAssembly(std::string text) {
    loaded.short_name = "assembly";
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
    abi = draft::classify_aarch64_c_types(
        semantics.package.types, target.facts);
    if (bodies.ok) {
      assembly = draft::analyze_aarch64_assembly(
          sources,
          loaded,
          target,
          semantics.package,
          hir,
          diagnostics);
    }
  }
};

void test_valid_fixed_register_regions(TestState &state) {
  CheckedAssembly source(R"draft(
package assembly

increment :: proc(value: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = value
        out x0
        add x0, x0, #1
    }
}

load_value :: proc(pointer: ^u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = pointer
        out x1
        ldr x1, [x0]
    }
}

store_value :: proc(pointer: ^u32, value: u32) {
    asm aarch64 {
        in x0 = pointer
        in w1 = value
        str w1, [x0]
    }
}

add_float :: proc(left, right: f64) -> f64 {
    return asm aarch64 -> f64 {
        in d0 = left
        in d1 = right
        out d0
        fadd d0, d0, d1
    }
}

load_vector :: proc(pointer: ^#simd[2]u64) -> #simd[2]u64 {
    return asm aarch64 -> #simd[2]u64 {
        in x0 = pointer
        out q0
        ldr q0, [x0]
    }
}

barrier :: proc() {
    asm aarch64 {
        clobber memory
        dmb ish
    }
}

select_larger :: proc(left, right: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = left
        in x1 = right
        out x0
        clobber flags
        cmp x0, x1
        csel x0, x0, x1, ge
    }
}

load_second :: proc(pointer: ^u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = pointer
        out x1
        ldr x1, [x0, #8]
    }
}

load_pair_sum :: proc(pointer: ^u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = pointer
        out x1
        clobber x2
        ldp x1, x2, [x0, #0]
        add x1, x1, x2
    }
}

round_trip_integer :: proc(value: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = value
        out x0
        clobber d0
        scvtf d0, x0
        fcvtzu x0, d0
    }
}

double_vector :: proc(value: #simd[2]u64) -> #simd[2]u64 {
    return asm aarch64 -> #simd[2]u64 {
        in q0 = value
        out q0
        add v0.2d, v0.2d, v0.2d
    }
}

main :: proc() -> int {
    return cast[int](increment(40))
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, source.assembly.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.assembly.regions.size() == 11);
  if (source.assembly.regions.size() == 11) {
    EXPECT(state, source.assembly.regions[0].llvm_constraints == "={x0},0");
    EXPECT(state, source.assembly.regions[0].instruction_text ==
        "add x0, x0, #1");
    EXPECT(state, source.assembly.regions[1].llvm_constraints ==
        "={x1},{x0},~{memory}");
    EXPECT(state, source.assembly.regions[2].llvm_constraints ==
        "={d0},0,{d1}");
    EXPECT(state, source.assembly.regions[3].llvm_constraints ==
        "={q0},{x0},~{memory}");
    EXPECT(state, source.assembly.regions[4].instruction_text ==
        "cmp x0, x1\n\tcsel x0, x0, x1, ge");
    EXPECT(state, source.assembly.regions[5].instruction_text ==
        "ldr x1, [x0, #8]");
    EXPECT(state, source.assembly.regions[6].instruction_text ==
        "ldp x1, x2, [x0, #0]\n\tadd x1, x1, x2");
    EXPECT(state, source.assembly.regions[8].instruction_text ==
        "add v0.2d, v0.2d, v0.2d");
    EXPECT(state, source.assembly.regions[9].llvm_constraints ==
        "{x0},{w1},~{memory}");
    EXPECT(state, source.assembly.regions[10].llvm_constraints == "~{memory}");
  }

  const draft::MirLoweringResult mir = draft::lower_package_to_mir(
      source.semantics.package,
      source.hir,
      source.assembly,
      source.diagnostics);
  draft::LlvmIrOptions options;
  options.package = {"workspace", "assembly"};
  options.emit_program_entry = true;
  const draft::LlvmIrResult llvm = draft::emit_llvm_ir(
      source.target,
      source.sources,
      options,
      source.semantics.package,
      source.abi,
      source.semantics.global_initializers,
      mir.program,
      source.diagnostics);
  EXPECT(state, mir.ok);
  EXPECT(state, llvm.ok);
  EXPECT(state, llvm.text.find(
      "asm sideeffect \"add x0, x0, #1\", \"={x0},0\"") !=
      std::string::npos);
  EXPECT(state, llvm.text.find(
      "asm sideeffect \"dmb ish\", \"~{memory}\"") !=
      std::string::npos);
  EXPECT(state, llvm.text.find(
      "asm sideeffect \"ldr x1, [x0]\", \"={x1},{x0},~{memory}\"") !=
      std::string::npos);
  EXPECT(state, llvm.text.find(
      "asm sideeffect \"fadd d0, d0, d1\", \"={d0},0,{d1}\"") !=
      std::string::npos);
  EXPECT(state, llvm.text.find(
      "asm sideeffect \"ldr q0, [x0]\", \"={q0},{x0},~{memory}\"") !=
      std::string::npos);
  EXPECT(state, llvm.text.find("csel x0, x0, x1, ge") != std::string::npos);
  EXPECT(state, llvm.text.find("ldr x1, [x0, #8]") != std::string::npos);
  EXPECT(state, llvm.text.find("add v0.2d, v0.2d, v0.2d") !=
      std::string::npos);
}

void test_invalid_effects_and_architecture(TestState &state) {
  CheckedAssembly source(R"draft(
package assembly

bad_arch :: proc(value: u64) -> u64 {
    return asm x86_64 -> u64 {
        in x0 = value
        out x0
        add x0, x0, #1
    }
}

bad_effects :: proc(value: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = value
        out x0
        add x1, x0, #1
        cmp x0, #0
        b somewhere
    }
}

bad_untyped_memory :: proc(pointer: rawptr) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = pointer
        out x1
        ldr x1, [x0]
    }
}

bad_memory_width :: proc(pointer: ^u32) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = pointer
        out x1
        ldr x1, [x0]
    }
}

bad_offset :: proc(pointer: ^u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = pointer
        out x1
        ldr x1, [x0, #7]
    }
}

bad_flags :: proc(left, right: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = left
        in x1 = right
        out x0
        csel x0, x0, x1, ge
    }
}

bad_vector_shape :: proc(value: #simd[2]u64) -> #simd[2]u64 {
    return asm aarch64 -> #simd[2]u64 {
        in q0 = value
        out q0
        mul v0.2d, v0.2d, v0.2d
    }
}
)draft");

  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.assembly.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("does not match target") != std::string::npos);
  EXPECT(state, rendered.find("must be declared as an output or clobber") !=
      std::string::npos);
  EXPECT(state, rendered.find("without 'clobber flags'") != std::string::npos);
  EXPECT(state, rendered.find("unsupported instruction 'b'") != std::string::npos);
  EXPECT(state, rendered.find("output register is never written") !=
                    std::string::npos);
  EXPECT(state, rendered.find("memory access is not typed by a matching pointer") !=
                    std::string::npos);
  EXPECT(state, rendered.find("invalid operands for AArch64 instruction 'ldr'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("reads condition flags before") !=
                    std::string::npos);
  EXPECT(state, rendered.find("invalid operands for AArch64 instruction 'mul'") !=
                    std::string::npos);
}

void test_unresolved_assembly_synthesis(TestState &state) {
  CheckedAssembly source(R"draft(
package assembly

unresolved :: proc() {
    asm aarch64 {
        clobber memory
        ... "produce a target barrier"
    }
}
)draft");

  EXPECT(state, source.semantics.ok);
  EXPECT(state, source.bodies.ok);
  EXPECT(state, !source.assembly.ok);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find(
      "unresolved assembly synthesis prevents native lowering") !=
      std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_valid_fixed_register_regions(state);
  test_invalid_effects_and_architecture(state);
  test_unresolved_assembly_synthesis(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " assembly expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all assembly tests passed\n";
  return EXIT_SUCCESS;
}
