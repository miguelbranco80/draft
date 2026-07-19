// Canonical native object task planning over completed compiler packages.
//
// See native_object_tasks.h for ownership and phase boundaries. Planning is a
// small deterministic traversal and performs no I/O or native tool invocation.

#include "backend/native_object_tasks.h"

#include "workspace/workspace.h"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace draft {
namespace {

// Resolves a selected filename back to the already validated target rule. A
// null result means the compiled input and selected profile disagree; allowing
// a later tool to infer behavior from the extension would make ambient host
// conventions part of Draft semantics.
[[nodiscard]] const AssemblyFileRule *assembly_rule(
    const TargetProfile &target,
    std::string_view relative_name) {
  const std::string extension =
      std::filesystem::path(relative_name).extension().string();
  for (const AssemblyFileRule &rule : target.assembly_files) {
    if (rule.extension == extension) return &rule;
  }
  return nullptr;
}

} // namespace

bool prepare_native_object_plan(
    const TargetProfile &target,
    const CompileWorkspaceResult &compiled,
    NativeObjectPlan &plan,
    std::string &reason) {
  plan = NativeObjectPlan{};
  reason.clear();

  // WorkTaskId must represent every eventual vector index. Check before
  // appending so no narrowing conversion can produce an aliased task ID. The
  // exact count includes one module plus every assembly input per package.
  const std::size_t maximum_task_count =
      static_cast<std::size_t>(std::numeric_limits<WorkTaskId>::max()) + 1U;
  std::size_t task_count = compiled.packages.size();
  if (task_count > maximum_task_count) {
    reason = "native object plan exceeds the WorkTaskId domain";
    return false;
  }
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    if (package->assembly_sources.size() > maximum_task_count - task_count) {
      reason = "native object plan exceeds the WorkTaskId domain";
      return false;
    }
    task_count += package->assembly_sources.size();
  }
  plan.tasks.reserve(task_count);
  plan.graph.tasks.reserve(task_count);

  for (std::size_t package_index = 0;
       package_index < compiled.packages.size();
       ++package_index) {
    const std::optional<CompiledPackage> &package =
        compiled.packages[package_index];
    if (!package.has_value() || !package->llvm.ok) {
      reason = "compiled package " + std::to_string(package_index) +
          " has no valid LLVM module";
      plan = NativeObjectPlan{};
      return false;
    }

    const std::string package_stem =
        "package-" + std::to_string(package_index);
    NativeObjectTask module;
    module.kind = NativeObjectTaskKind::LlvmModule;
    module.package_index = package_index;
    module.display_name = display_package_identity(package->identity);
    module.output_stem = package_stem;
    module.source_extension = ".ll";
    module.input_bytes = package->llvm.text;
    plan.tasks.push_back(std::move(module));
    plan.graph.tasks.emplace_back();

    for (std::size_t assembly_index = 0;
         assembly_index < package->assembly_sources.size();
         ++assembly_index) {
      const CompiledAssemblySource &input =
          package->assembly_sources[assembly_index];
      const AssemblyFileRule *rule = assembly_rule(target, input.relative_name);
      if (rule == nullptr ||
          rule->preprocessing != AssemblyPreprocessing::None) {
        reason = "package assembly input '" + input.relative_name +
            "' has no exact non-preprocessed target rule";
        plan = NativeObjectPlan{};
        return false;
      }
      NativeObjectTask assembly;
      assembly.kind = NativeObjectTaskKind::PackageAssembly;
      assembly.package_index = package_index;
      assembly.input_index = assembly_index;
      assembly.display_name = input.relative_name;
      assembly.output_stem = package_stem + "-assembly-" +
          std::to_string(assembly_index);
      assembly.source_extension = rule->extension;
      assembly.input_bytes = input.contents;
      plan.tasks.push_back(std::move(assembly));
      plan.graph.tasks.emplace_back();
    }
  }
  return true;
}

} // namespace draft
