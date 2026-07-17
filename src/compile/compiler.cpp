// Provider-free workspace compiler orchestration.

#include "compile/compiler.h"

#include "sema/denial.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace draft {

CompileWorkspaceResult compile_workspace(
    SourceManager &sources,
    const std::string &root_package_directory,
    CompileWorkspaceOptions options,
    DiagnosticSink &diagnostics) {
  CompileWorkspaceResult result;
  const std::size_t initial_errors = diagnostics.error_count();
  std::string profile_error;
  if (!validate_target_profile(options.target, profile_error)) {
    diagnostics.error(
        SourceRange::invalid(), "invalid target profile: " + profile_error);
    return result;
  }
  options.workspace.package_options.file_tag = options.target.facts.file_tag;
  WorkspaceLoadResult loaded = load_workspace(
      sources,
      root_package_directory,
      options.workspace,
      diagnostics);
  result.graph = std::move(loaded.graph);
  result.packages.resize(result.graph.packages.size());
  if (!loaded.ok) return result;

  // Workspace discovery is importer-first. Reverse traversal guarantees every
  // dependency interface exists before a consumer asks to bind its imports.
  for (std::size_t remaining = result.graph.packages.size(); remaining > 0; --remaining) {
    const std::size_t package_index = remaining - 1;
    WorkspacePackage &workspace_package = result.graph.packages[package_index];
    AvailablePackageImports available;
    bool dependencies_ready = true;
    for (const PackageImport &import : result.graph.imports) {
      if (static_cast<std::size_t>(import.importing_package.value) != package_index) {
        continue;
      }
      const std::size_t dependency_index =
          static_cast<std::size_t>(import.imported_package.value);
      if (dependency_index >= result.packages.size() ||
          !result.packages[dependency_index].has_value()) {
        diagnostics.error(
            SourceRange::invalid(),
            "dependency did not produce a package interface before its consumer");
        dependencies_ready = false;
        continue;
      }
      available.entries.push_back({
          {import.file, import.syntax},
          &result.packages[dependency_index]->interface,
      });
    }
    if (!dependencies_ready) continue;

    CompiledPackage package;
    package.identity = workspace_package.identity;
    package.semantics = analyze_package_semantics(
        sources,
        workspace_package.loaded,
        options.target.facts,
        available,
        diagnostics);
    if (!package.semantics.ok) continue;
    package.bodies = check_package_bodies(
        sources,
        workspace_package.loaded,
        package.semantics.selections,
        package.semantics.package,
        package.semantics.constants,
        diagnostics);
    if (!package.bodies.ok) continue;
    package.metadata = collect_agent_metadata(
        sources,
        workspace_package.loaded,
        package.semantics.package,
        options.attachments,
        diagnostics);
    package.effects = summarize_package_effects(
        package.semantics.package, package.bodies.program);
    package.native_interop = validate_native_interop(
        package.semantics.package, package.bodies.program, diagnostics);
    const bool denials_ok = check_package_denials(
        sources,
        workspace_package.loaded,
        package.semantics.package,
        package.bodies.program,
        package.effects,
        diagnostics);
    package.interface = build_package_interface(
        workspace_package.identity,
        package.semantics.package,
        package.semantics.constants,
        package.metadata,
        package.effects,
        diagnostics);
    if (!package.metadata.ok || !denials_ok || !package.native_interop.ok) continue;

    if (options.lower_mir || options.emit_llvm) {
      package.assembly = analyze_aarch64_assembly(
          sources,
          workspace_package.loaded,
          options.target,
          package.semantics.package,
          package.bodies.program,
          diagnostics);
      if (!package.assembly.ok) continue;
      package.mir = lower_package_to_mir(
          package.semantics.package,
          package.bodies.program,
          package.assembly,
          diagnostics);
      if (!package.mir.ok) continue;
    }
    if (options.emit_llvm) {
      LlvmIrOptions llvm_options;
      llvm_options.package = workspace_package.identity;
      llvm_options.emit_program_entry =
          package_index ==
          static_cast<std::size_t>(result.graph.root_package.value);
      package.llvm = emit_llvm_ir(
          options.target,
          sources,
          llvm_options,
          package.semantics.package,
          package.semantics.constants,
          package.mir.program,
          diagnostics);
      if (!package.llvm.ok) continue;
    }
    result.packages[package_index] = std::move(package);
  }

  bool every_package_ready = true;
  for (const std::optional<CompiledPackage> &package : result.packages) {
    every_package_ready = every_package_ready && package.has_value();
  }
  result.ok = every_package_ready &&
      diagnostics.error_count() == initial_errors;
  return result;
}

} // namespace draft
