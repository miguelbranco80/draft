// Native object planning tests independent of LLVM and the host filesystem.
//
// The fixture constructs completed package products directly so these tests
// isolate stable task identity, canonical module/assembly order, borrowed input
// bytes, and target-rule rejection. Toolchain tests separately prove that the
// planned work reaches object emitters, publication, and linking.

#include "backend/native_object_tasks.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

struct TestState {
  int failures = 0;
};

#define EXPECT(state, condition)                                                \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << __FILE__ << ':' << __LINE__                                  \
                << ": expectation failed: " #condition << '\n';                \
      ++(state).failures;                                                       \
    }                                                                           \
  } while (false)

// Produces two package rows in compiler PackageId order. The dependency package
// owns two assembly inputs so ordering within one package is independently
// observable from package ordering.
draft::CompileWorkspaceResult make_compiled_fixture() {
  draft::CompileWorkspaceResult compiled;
  compiled.ok = true;
  compiled.packages.resize(2);

  draft::CompiledPackage root;
  root.identity = {"workspace", "app"};
  root.llvm.ok = true;
  root.llvm.static_data.ok = true;
  root.llvm.static_data.text = "; root static data\n";
  root.artifact_layout.ok = true;
  root.artifact_layout.inputs.push_back({
      draft::PackageArtifactInputKind::PackageStaticData, 0, {0}});
  compiled.packages[0] = std::move(root);

  draft::CompiledPackage dependency;
  dependency.identity = {"draft-core-test", "os"};
  dependency.llvm.ok = true;
  dependency.llvm.static_data.ok = true;
  dependency.llvm.static_data.text = "; dependency static data\n";
  dependency.llvm.machine_functions.push_back(
      {true, "; dependency function\n", {}});
  dependency.assembly_sources.push_back({"first.s", "first:\n  ret\n"});
  dependency.assembly_sources.push_back({"second.S", "second:\n  ret\n"});
  dependency.artifact_layout.ok = true;
  dependency.artifact_layout.inputs = {
      {draft::PackageArtifactInputKind::PackageStaticData, 0, {1}},
      {draft::PackageArtifactInputKind::MachineFunction, 0, {2}},
      {draft::PackageArtifactInputKind::PackageAssembly, 0, {3}},
      {draft::PackageArtifactInputKind::PackageAssembly, 1, {3}},
  };
  compiled.packages[1] = std::move(dependency);
  return compiled;
}

// Planning must preserve one simple, inspectable order regardless of future
// completion order: package static data, its machine functions, and then its
// selected assembly sources before advancing to the next package.
void test_canonical_task_order(TestState &state) {
  const draft::CompileWorkspaceResult compiled = make_compiled_fixture();
  draft::NativeObjectPlan plan;
  std::string reason;
  EXPECT(state, draft::prepare_native_object_plan(
      draft::make_aarch64_macos_profile(), compiled, plan, reason));
  EXPECT(state, reason.empty());
  EXPECT(state, plan.tasks.size() == 5);
  EXPECT(state, plan.graph.tasks.size() == plan.tasks.size());
  for (const draft::WorkTask &task : plan.graph.tasks) {
    EXPECT(state, task.dependencies.empty());
  }

  EXPECT(state,
      plan.tasks[0].kind == draft::NativeObjectTaskKind::PackageStaticData);
  EXPECT(state, plan.tasks[0].package_index == 0);
  EXPECT(state, plan.tasks[0].producer == draft::SemanticProductId{0});
  EXPECT(state, plan.tasks[0].display_name == "workspace:app static data");
  EXPECT(state, plan.tasks[0].output_stem == "package-0-static");
  EXPECT(state, plan.tasks[0].input_bytes == "; root static data\n");

  EXPECT(state,
      plan.tasks[1].kind == draft::NativeObjectTaskKind::PackageStaticData);
  EXPECT(state, plan.tasks[1].package_index == 1);
  EXPECT(state, plan.tasks[1].output_stem == "package-1-static");
  EXPECT(state,
      plan.tasks[2].kind == draft::NativeObjectTaskKind::MachineFunction);
  EXPECT(state, plan.tasks[2].package_index == 1);
  EXPECT(state, plan.tasks[2].input_index == 0);
  EXPECT(state, plan.tasks[2].producer == draft::SemanticProductId{2});
  EXPECT(state, plan.tasks[2].output_stem == "package-1-function-0");
  EXPECT(state, plan.tasks[2].source_extension == ".ll");
  EXPECT(state,
      plan.tasks[3].kind == draft::NativeObjectTaskKind::PackageAssembly);
  EXPECT(state, plan.tasks[3].input_index == 0);
  EXPECT(state, plan.tasks[3].output_stem == "package-1-assembly-0");
  EXPECT(state, plan.tasks[3].source_extension == ".s");
  EXPECT(state, plan.tasks[4].input_index == 1);
  EXPECT(state, plan.tasks[4].output_stem == "package-1-assembly-1");
  EXPECT(state, plan.tasks[4].source_extension == ".S");
}

// A missing lowered package and an extension outside the selected target
// contract must fail during planning, before any task or filesystem output is
// published.
void test_plan_rejects_incomplete_or_unknown_input(TestState &state) {
  draft::CompileWorkspaceResult incomplete = make_compiled_fixture();
  incomplete.packages[0]->artifact_layout.ok = false;
  draft::NativeObjectPlan plan;
  std::string reason;
  EXPECT(state, !draft::prepare_native_object_plan(
      draft::make_aarch64_macos_profile(), incomplete, plan, reason));
  EXPECT(state, reason ==
      "compiled package 0 has no valid native artifact layout");
  EXPECT(state, plan.tasks.empty());

  draft::CompileWorkspaceResult incomplete_layout = make_compiled_fixture();
  incomplete_layout.packages[1]->artifact_layout.inputs.erase(
      incomplete_layout.packages[1]->artifact_layout.inputs.begin() + 1);
  EXPECT(state, !draft::prepare_native_object_plan(
      draft::make_aarch64_macos_profile(),
      incomplete_layout,
      plan,
      reason));
  EXPECT(state, reason ==
      "package artifact layout does not cover every native input");
  EXPECT(state, plan.tasks.empty());

  draft::CompileWorkspaceResult unknown = make_compiled_fixture();
  unknown.packages[1]->assembly_sources[0].relative_name = "first.asm-unknown";
  EXPECT(state, !draft::prepare_native_object_plan(
      draft::make_aarch64_macos_profile(), unknown, plan, reason));
  EXPECT(state, reason ==
      "package assembly input 'first.asm-unknown' has no exact "
      "non-preprocessed target rule");
  EXPECT(state, plan.tasks.empty());
}

} // namespace

int main() {
  TestState state;
  test_canonical_task_order(state);
  test_plan_rejects_incomplete_or_unknown_input(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " native object task expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all native object task tests passed\n";
  return EXIT_SUCCESS;
}
