// Canonical native object task planning over completed compiler packages.
//
// See native_object_tasks.h for ownership and phase boundaries. Planning is a
// small deterministic traversal and performs no I/O or native tool invocation.

#include "backend/native_object_tasks.h"

#include "backend/hosted_runtime_bundle.h"
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
  // artifact layouts already contain the exact unit/assembly count.
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

  bool saw_root_package = false;
  bool saw_root_runtime = false;

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
    const bool is_root = compiled.graph.root_package.is_valid() &&
        package_index == compiled.graph.root_package.value;
    if (is_root) saw_root_package = true;
    bool saw_runtime = false;
    bool saw_assembly = false;
    std::size_t next_unit = 0;
    std::size_t next_assembly = 0;
    for (const PackageArtifactInput &layout :
         package->artifact_layout.inputs) {
      if (!layout.producer.is_valid()) {
        reason = "package artifact layout has an invalid producer";
        plan = NativeObjectPlan{};
        return false;
      }
      if (layout.kind == PackageArtifactInputKind::PackageLlvmUnit) {
        if (saw_runtime || saw_assembly || layout.index != next_unit) {
          reason = "package LLVM unit layout is malformed";
          plan = NativeObjectPlan{};
          return false;
        }
        NativeObjectTask task;
        task.kind = NativeObjectTaskKind::PackageLlvmUnit;
        task.package_unit_input = options.package_unit_input;
        task.package_index = package_index;
        task.input_index = layout.index;
        task.producer = layout.producer;
        task.display_name = display_package_identity(package->identity) +
            " LLVM unit " + std::to_string(layout.index);
        task.output_stem = package_stem + "-unit-" +
            std::to_string(layout.index);
        if (options.package_unit_input ==
            NativePackageUnitInputKind::EmittedNativeBytes) {
          if (layout.index >= package->native_outputs.size()) {
            reason = "compiled package " + std::to_string(package_index) +
                " has no native output for LLVM unit " +
                std::to_string(layout.index);
            plan = NativeObjectPlan{};
            return false;
          }
          const PackageNativeOutput &native =
              package->native_outputs[layout.index];
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
          // Retained LLVM text is deliberately package-wide. Splitting it
          // would make the qualification oracle compare a different
          // optimization unit from production O2 and complicate inspection.
          if (layout.index != 0 || next_unit != 0 ||
              !package->llvm_module.ok || package->llvm_module.text.empty()) {
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
        ++next_unit;
        continue;
      }
      if (layout.kind == PackageArtifactInputKind::HostedRuntime) {
        if (next_unit == 0 || saw_runtime || saw_assembly ||
            layout.index != 0) {
          reason = "hosted runtime layout is malformed";
          plan = NativeObjectPlan{};
          return false;
        }
        if (!compiled.graph.root_package.is_valid() ||
            package_index != compiled.graph.root_package.value) {
          reason = "hosted runtime is not owned by the root package layout";
          plan = NativeObjectPlan{};
          return false;
        }
        saw_runtime = true;
        saw_root_runtime = true;
        if (!options.include_hosted_runtime)
          continue;
        const EmbeddedHostedRuntimeObject *runtime =
            embedded_hosted_runtime_object(target.facts.identity);
        if (runtime == nullptr) {
          reason = "compiler has no hosted runtime for target '" +
              target.facts.identity + "'";
          plan = NativeObjectPlan{};
          return false;
        }
        NativeObjectTask task;
        task.kind = NativeObjectTaskKind::HostedRuntime;
        task.package_unit_input =
            NativePackageUnitInputKind::EmittedNativeBytes;
        task.package_index = package_index;
        task.producer = layout.producer;
        task.display_name = target.facts.identity + " hosted runtime";
        task.output_stem = "hosted-runtime";
        if (options.expected_native_output.output_kind ==
            LlvmNativeOutputKind::Assembly) {
          task.source_extension = ".s";
          task.input_bytes = runtime->assembly_bytes;
        } else {
          task.input_bytes = runtime->object_bytes;
        }
        if (task.input_bytes.empty()) {
          reason = "compiler hosted runtime input is empty";
          plan = NativeObjectPlan{};
          return false;
        }
        plan.tasks.push_back(std::move(task));
        plan.graph.tasks.emplace_back();
        continue;
      }
      if (layout.kind != PackageArtifactInputKind::PackageAssembly) {
        reason = "package artifact layout has an unknown input kind";
        plan = NativeObjectPlan{};
        return false;
      }
      saw_assembly = true;
      if (next_unit == 0 || layout.index != next_assembly ||
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
    const bool unit_count_matches = options.package_unit_input ==
            NativePackageUnitInputKind::EmittedNativeBytes
        ? next_unit == package->native_outputs.size()
        : next_unit == 1;
    if (next_unit == 0 || !unit_count_matches ||
        next_assembly != package->assembly_sources.size()) {
      reason = "package artifact layout does not cover every native input";
      plan = NativeObjectPlan{};
      return false;
    }
  }
  // Every complete compiler layout records the target runtime even when the
  // caller is producing a relocatable object and intentionally omits that row
  // from the resulting task plan. Requiring the row here prevents a malformed
  // final executable/library plan from silently linking without the Context,
  // process-state, and runtime-check definitions its package units reference.
  if (compiled.graph.root_package.is_valid() &&
      (!saw_root_package || !saw_root_runtime)) {
    reason = "root package artifact layout has no hosted runtime";
    plan = NativeObjectPlan{};
    return false;
  }
  return true;
}

} // namespace draft
