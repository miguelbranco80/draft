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
    const NativeObjectPlanOptions &options,
    NativeObjectPlan &plan,
    std::string &reason) {
  plan = NativeObjectPlan{};
  reason.clear();

  // WorkTaskId must represent every eventual vector index. Check before
  // appending so no narrowing conversion can produce an aliased task ID. The
  // artifact layouts already contain the exact module/assembly count.
  const std::size_t maximum_task_count =
      static_cast<std::size_t>(std::numeric_limits<WorkTaskId>::max()) + 1U;
  std::size_t task_count = 0;
  if (task_count > maximum_task_count) {
    reason = "native object plan exceeds the WorkTaskId domain";
    return false;
  }
  for (const std::optional<CompiledPackage> &package : compiled.packages) {
    if (!package.has_value()) continue;
    if (package->artifact_layout.inputs.size() >
        maximum_task_count - task_count) {
      reason = "native object plan exceeds the WorkTaskId domain";
      return false;
    }
    task_count += package->artifact_layout.inputs.size();
  }
  plan.tasks.reserve(task_count);
  plan.graph.tasks.reserve(task_count);

  for (std::size_t package_index = 0;
       package_index < compiled.packages.size();
       ++package_index) {
    const std::optional<CompiledPackage> &package =
        compiled.packages[package_index];
    if (!package.has_value() || !package->artifact_layout.ok) {
      reason = "compiled package " + std::to_string(package_index) +
          " has no valid native artifact layout";
      plan = NativeObjectPlan{};
      return false;
    }

    const std::string package_stem =
        "package-" + std::to_string(package_index);
    bool saw_module = false;
    bool saw_assembly = false;
    std::size_t next_assembly = 0;
    for (const PackageArtifactInput &layout :
         package->artifact_layout.inputs) {
      if (!layout.producer.is_valid()) {
        reason = "package artifact layout has an invalid producer";
        plan = NativeObjectPlan{};
        return false;
      }
      if (layout.kind == PackageArtifactInputKind::PackageLlvmModule) {
        if (saw_module || saw_assembly || layout.index != 0) {
          reason = "package LLVM module layout is malformed";
          plan = NativeObjectPlan{};
          return false;
        }
        NativeObjectTask task;
        task.kind = NativeObjectTaskKind::PackageLlvmModule;
        task.package_module_input = options.package_module_input;
        task.package_index = package_index;
        task.producer = layout.producer;
        task.display_name =
            display_package_identity(package->identity) + " LLVM module";
        task.output_stem = package_stem + "-module";
        if (options.package_module_input ==
            NativePackageModuleInputKind::EmittedNativeBytes) {
          const PackageNativeOutput &native = package->native_output;
          if (!native.ok || native.bytes.empty() ||
              native.output_kind !=
                  options.expected_native_output.output_kind ||
              native.optimization !=
                  options.expected_native_output.optimization ||
              native.instrumentation !=
                  options.expected_native_output.instrumentation) {
            reason = "compiled package " + std::to_string(package_index) +
                " has no matching native package output";
            plan = NativeObjectPlan{};
            return false;
          }
          task.source_extension = native.output_kind ==
                  LlvmNativeOutputKind::Assembly
              ? ".s"
              : std::string{};
          task.input_bytes = native.bytes;
        } else {
          if (!package->llvm_module.ok || package->llvm_module.text.empty()) {
            reason = "compiled package " + std::to_string(package_index) +
                " has no retained LLVM text for the external oracle";
            plan = NativeObjectPlan{};
            return false;
          }
          task.source_extension = ".ll";
          task.input_bytes = package->llvm_module.text;
        }
        plan.tasks.push_back(std::move(task));
        plan.graph.tasks.emplace_back();
        saw_module = true;
        continue;
      }
      saw_assembly = true;
      if (!saw_module || layout.index != next_assembly ||
          layout.index >= package->assembly_sources.size()) {
        reason = "package assembly layout is malformed";
        plan = NativeObjectPlan{};
        return false;
      }
      const std::size_t assembly_index = layout.index;
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
      assembly.producer = layout.producer;
      assembly.display_name = input.relative_name;
      assembly.output_stem = package_stem + "-assembly-" +
          std::to_string(assembly_index);
      assembly.source_extension = rule->extension;
      assembly.input_bytes = input.contents;
      plan.tasks.push_back(std::move(assembly));
      plan.graph.tasks.emplace_back();
      ++next_assembly;
    }
    if (!saw_module ||
        next_assembly != package->assembly_sources.size()) {
      reason = "package artifact layout does not cover every native input";
      plan = NativeObjectPlan{};
      return false;
    }
  }
  return true;
}

} // namespace draft
