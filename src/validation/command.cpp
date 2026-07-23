// Native validation command orchestration and evidence policy.
//
// This module consumes one fully specified workspace/package/target selection,
// compiles its command-only test or benchmark graph, publishes a root/target-
// namespaced native harness, executes the fixed validation policy, and commits
// immutable evidence. It owns only command-local process and artifact state;
// source graphs belong to compile/, native mechanics belong to backend/, and
// evidence serialization belongs to validation/evidence_store.
//
// The selected package and workspace must remain valid for the synchronous
// call. Native output is derived and replaceable, while an evidence attempt is
// committed only after at least one harness process starts. Validation never
// mutates synthesis resolution state.

#include "validation/command.h"

#include "compile/compiler.h"
#include "base/timing.h"
#include "validation/evidence.h"
#include "validation/evidence_store.h"
#include "validation/runner.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace draft {
namespace {

[[nodiscard]] std::optional<std::string> toolchain_identity(
    const NativeBuildResult &built,
    DiagnosticSink &diagnostics) {
  if (built.toolchain_version.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "validation cannot identify the linked LLVM toolchain");
    return std::nullopt;
  }
  // Host kernel/architecture facts have their own environment identity below.
  // This field names the exact LLVM distribution used for object emission and
  // matching sanitizer/link tools; retaining the old `host-version` prefix
  // would misdescribe a compile-time constant as an ambient runtime probe.
  return "linked-llvm-version:" + built.toolchain_version;
}

[[nodiscard]] std::optional<std::string> environment_identity(
    const TargetProfile &target,
    DiagnosticSink &diagnostics) {
#if defined(_WIN32)
  diagnostics.error(
      SourceRange::invalid(),
      "validation environment identity is not implemented on this host");
  return std::nullopt;
#else
  struct utsname host {};
  if (::uname(&host) != 0) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot identify validation runner kernel and architecture");
    return std::nullopt;
  }
  std::string identity = "target-cpu=" + target.llvm_cpu +
      ";target-features=" + target.llvm_feature_string +
      ";host-machine=" + host.machine + ";host-release=" + host.release +
      ";process-environment=draft-validation-process-environment-v1";
#if defined(__APPLE__)
  std::size_t model_size = 0;
  if (::sysctlbyname("hw.model", nullptr, &model_size, nullptr, 0) != 0 ||
      model_size == 0) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot identify validation runner hardware model");
    return std::nullopt;
  }
  std::vector<char> model(model_size);
  if (::sysctlbyname(
          "hw.model", model.data(), &model_size, nullptr, 0) != 0 ||
      model_size == 0) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot read validation runner hardware model");
    return std::nullopt;
  }
  if (model[model_size - 1] == '\0') --model_size;
  identity += ";hw-model=" + std::string(model.data(), model_size);
#endif
  return identity;
#endif
}

[[nodiscard]] std::vector<ValidationObservation> empty_observations(
    const std::vector<ValidationEntry> &entries) {
  std::vector<ValidationObservation> result;
  result.reserve(entries.size());
  for (const ValidationEntry &entry : entries) {
    ValidationObservation observation;
    observation.package = entry.package;
    observation.procedure = entry.procedure;
    result.push_back(std::move(observation));
  }
  return result;
}

[[nodiscard]] bool merge_benchmark_observations(
    std::vector<ValidationObservation> &aggregate,
    const std::vector<ValidationObservation> &sample,
    bool first_sample,
    DiagnosticSink &diagnostics) {
  if (aggregate.size() != sample.size()) {
    diagnostics.error(
        SourceRange::invalid(),
        "benchmark report changed its selected procedure count");
    return false;
  }
  for (std::size_t index = 0; index < aggregate.size(); ++index) {
    ValidationObservation &destination = aggregate[index];
    const ValidationObservation &source = sample[index];
    if (destination.package != source.package ||
        destination.procedure != source.procedure ||
        source.durations_ns.size() != 1) {
      diagnostics.error(
          SourceRange::invalid(),
          "benchmark report changed its canonical entry identity or shape");
      return false;
    }
    if (first_sample) {
      destination.maximum_time_ns = source.maximum_time_ns;
    } else if (destination.maximum_time_ns != source.maximum_time_ns) {
      diagnostics.error(
          SourceRange::invalid(),
          "benchmark registered a different maximum-time budget across samples");
      return false;
    }
    if (destination.library_samples >
            std::numeric_limits<std::uint64_t>::max() - source.library_samples ||
        destination.failures >
            std::numeric_limits<std::uint64_t>::max() - source.failures) {
      diagnostics.error(
          SourceRange::invalid(), "benchmark observation counters overflowed");
      return false;
    }
    destination.library_samples += source.library_samples;
    destination.failures += source.failures;
    destination.durations_ns.push_back(source.durations_ns.front());
  }
  return true;
}

[[nodiscard]] std::uint64_t warmup_runs(ValidationKind kind) {
  return kind == ValidationKind::Benchmark ? 1 : 0;
}

[[nodiscard]] std::uint64_t sample_runs(ValidationKind kind) {
  return kind == ValidationKind::Benchmark ? 10 : 1;
}

[[nodiscard]] std::string policy_identity(
    ValidationKind kind,
    NativeOptimizationLevel optimization) {
  const std::string optimization_field =
      ":native-optimization=" +
      std::string(native_optimization_level_name(optimization));
  return kind == ValidationKind::Benchmark
      ? "draft-benchmark-policy-v1:warmup=1:samples=10:process-isolated" +
          optimization_field
      : "draft-test-policy-v1:single-process" + optimization_field;
}

void initialize_claim(
    ValidationEvidence &evidence,
    const CompileWorkspaceResult &compiled,
    const TargetProfile &target,
    ValidationKind kind,
    NativeOptimizationLevel optimization,
    std::span<const ValidationInstrumentationKind> instrumentation,
    std::string selected_toolchain,
    std::string selected_environment) {
  evidence.resolved_program = *compiled.resolved_program_digest;
  evidence.kind = kind;
  evidence.target_identity = target.facts.identity;
  evidence.compiler_identity = compiled.compiler_content_identity;
  evidence.toolchain_identity = std::move(selected_toolchain);
  evidence.environment_identity = std::move(selected_environment);
  evidence.runner_identity = "draft-native-validation-runner-v2-fd3";
  evidence.policy_identity = policy_identity(kind, optimization);
  evidence.instrumentation_identity =
      validation_instrumentation_identity(instrumentation);
  evidence.artifact_identity =
      "aarch64-macos-executable-v1:" + target.llvm_triple;
  evidence.warmup_runs = warmup_runs(kind);
  evidence.sample_runs = sample_runs(kind);
  evidence.entries = compiled.validation_entries;
}

[[nodiscard]] CompileWorkspaceResult compile_validation(
    SourceManager &sources,
    const std::filesystem::path &package_directory,
    const TargetProfile &target,
    WorkspaceLoadOptions workspace,
    ValidationKind kind,
    NativeOptimizationLevel optimization,
    std::span<const ValidationInstrumentationKind> instrumentation,
    std::vector<ForeignProviderAudit> audits,
    TimingRecorder *timings,
    const std::shared_ptr<WorkExecutor> &work_executor,
    DiagnosticSink &diagnostics) {
  CompileWorkspaceOptions options;
  options.target = target;
  options.workspace = std::move(workspace);
  options.validation_kind = kind;
  options.foreign_provider_audits = std::move(audits);
  options.lower_mir = true;
  options.emit_native_output = true;
  options.native_output.output_kind = LlvmNativeOutputKind::Object;
  options.native_output.optimization = optimization;
  if (!instrumentation.empty()) {
    options.native_output.instrumentation =
        LlvmNativeInstrumentation::AddressSanitizer;
  }
  options.emit_program_entry = true;
  options.timings = timings;
  options.work_executor = work_executor;
  return compile_workspace_with_resolution(
      sources, package_directory.string(), std::move(options), diagnostics);
}

[[nodiscard]] ValidationCommandResult execute_compiled_validation(
    const CompileWorkspaceResult &compiled,
    ValidationCommandOptions options,
    DiagnosticSink &diagnostics) {
  TimingScope validation_timing = options.timings != nullptr
      ? options.timings->scope("validation execution and evidence")
      : TimingScope{};
  ValidationCommandResult result;
  result.selected_procedures = compiled.validation_entries.size();
  if (!compiled.ok || !compiled.resolved_program_digest.has_value()) {
    diagnostics.error(
        SourceRange::invalid(),
        "prepared validation compilation has no resolved-program identity");
    return result;
  }
  for (const ValidationEntry &entry : compiled.validation_entries) {
    if (entry.kind != options.kind) {
      diagnostics.error(
          SourceRange::invalid(),
          "prepared validation entry kind does not match its command");
      return result;
    }
  }

  const std::string command(validation_kind_name(options.kind));
  const WorkspacePackage &selected_root =
      compiled.graph.package(compiled.graph.root_package);
  std::filesystem::path artifact_directory =
      std::filesystem::path(options.workspace.workspace_directory) /
      ".draft" / "build" / options.target.facts.file_tag;
  if (selected_root.identity.root_relative_path == ".") {
    artifact_directory /= "workspace";
  } else {
    artifact_directory /= "packages";
    artifact_directory /= selected_root.identity.root_relative_path;
  }
  artifact_directory /= "validation";

  // Test and benchmark executables are derived native artifacts just like an
  // ordinary build. Keep both their private scratch and published executable
  // under the same root/target namespace so alternating validation roots or
  // profiles cannot replace one another's files. Evidence shares the
  // workspace-owned store used by judgments; its typed key contains the
  // selected resolved program and target, so sibling roots cannot alias.
  NativeBuildOptions native;
  native.build_directory =
      (artifact_directory / ("." + command + "-native")).string();
  native.output_path =
      (artifact_directory /
       (selected_root.loaded.short_name + "-" + command)).string();
  native.artifact_kind = NativeArtifactKind::Executable;
  native.optimization = options.optimization;
  if (!options.instrumentation.empty()) {
    native.instrumentation = NativeInstrumentationProfile::AddressSanitizer;
  }
  native.foreign_providers = options.foreign_providers;
  native.runtime_assets = options.runtime_assets;
  native.timings = options.timings;
  native.work_executor = options.work_executor;
  const NativeBuildResult built = build_native_executable(
      options.target, compiled, native, diagnostics);
  if (!built.ok) return result;

  const std::optional<std::string> selected_environment =
      environment_identity(options.target, diagnostics);
  const std::optional<std::string> selected_toolchain =
      toolchain_identity(built, diagnostics);
  if (!selected_environment.has_value() || !selected_toolchain.has_value()) {
    return result;
  }

  const bool benchmark = options.kind == ValidationKind::Benchmark;
  const std::uint64_t warmups = warmup_runs(options.kind);
  const std::uint64_t samples = sample_runs(options.kind);
  std::vector<ValidationObservation> aggregate =
      empty_observations(compiled.validation_entries);
  bool observations_complete = true;
  int aggregate_exit_code = 0;
  int aggregate_signal = 0;
  bool started_any = false;
  const std::uint64_t total_runs = warmups + samples;
  TimingScope runs_timing = options.timings != nullptr
      ? options.timings->scope("validation artifact execution")
      : TimingScope{};
  for (std::uint64_t run_index = 0; run_index < total_runs; ++run_index) {
    ValidationRunOptions run_options;
    run_options.executable = built.output_path;
    run_options.working_directory = options.package_directory.string();
    run_options.environment = {
        "LANG=C",
        "LC_ALL=C",
        "PATH=",
        "HOME=/",
        "TMPDIR=/tmp",
    };
    if (!options.instrumentation.empty()) {
      // Detection does not require an external symbolizer. Keeping the same
      // explicit process environment as ordinary validation means evidence is
      // independent of whichever developer tools happen to be on PATH.
      run_options.environment.push_back(
          "ASAN_OPTIONS=abort_on_error=1:symbolize=0");
    }
    const std::string run_timing_name = options.timings != nullptr &&
            options.timings->output() == TimingOutput::All
        ? "validation process " + std::to_string(run_index + 1)
        : std::string{};
    TimingScope run_timing = options.timings != nullptr
        ? options.timings->scope(
              run_timing_name, TimingVisibility::Detail)
        : TimingScope{};
    const ValidationRunResult run = run_validation_executable(
        run_options, diagnostics);
    if (options.timings != nullptr && run.started) {
      options.timings->record_child_process(
          run.user_nanoseconds, run.system_nanoseconds);
    }
    if (!run.started) {
      observations_complete = false;
      break;
    }
    started_any = true;
    if (!run.exited) {
      aggregate_signal = run.signal;
      observations_complete = false;
      break;
    }
    std::vector<ValidationObservation> one_run;
    if (!decode_validation_report(
            run.report, compiled.validation_entries, one_run, diagnostics)) {
      observations_complete = false;
      break;
    }
    if (run_index < warmups) continue;
    if (run.exit_code != 0) aggregate_exit_code = 1;
    if (benchmark) {
      if (!merge_benchmark_observations(
              aggregate,
              one_run,
              run_index == warmups,
              diagnostics)) {
        observations_complete = false;
        break;
      }
    } else {
      aggregate = std::move(one_run);
    }
  }
  runs_timing.finish();
  // exec failure is infrastructure, not a semantic attempt. Once any process
  // started, a signal or malformed report must still revoke this exact key.
  if (!started_any) return result;
  for (const ValidationObservation &observation : aggregate) {
    if (observation.failures != 0) aggregate_exit_code = 1;
  }
  const bool passed = observations_complete &&
      aggregate_signal == 0 && aggregate_exit_code == 0;

  result.passed = passed;
  result.exit_code = aggregate_exit_code;
  result.signal = aggregate_signal;

  ValidationEvidence evidence;
  initialize_claim(
      evidence,
      compiled,
      options.target,
      options.kind,
      options.optimization,
      options.instrumentation,
      *selected_toolchain,
      *selected_environment);
  evidence.observations = std::move(aggregate);
  evidence.observations_complete = observations_complete;
  evidence.passed = passed;
  evidence.exit_code = aggregate_exit_code;
  evidence.signal = aggregate_signal;
  TimingScope evidence_timing = options.timings != nullptr
      ? options.timings->scope("validation evidence commit")
      : TimingScope{};
  const ValidationEvidenceCommitResult committed = commit_validation_evidence(
      options.workspace.workspace_directory, std::move(evidence), diagnostics);
  if (!committed.ok) return result;

  result.completed = true;
  result.evidence_key = committed.key;
  result.evidence_digest = committed.evidence_digest;
  result.attempt = committed.attempt;
  return result;
}

} // namespace

ValidationCommandResult execute_validation_command(
    SourceManager &sources,
    ValidationCommandOptions options,
    DiagnosticSink &diagnostics) {
  TimingScope command_timing = options.timings != nullptr
      ? options.timings->scope("validation command")
      : TimingScope{};
  if (options.kind == ValidationKind::None ||
      options.package_directory.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "validation command requires a package and test or benchmark kind");
    return {};
  }
  if (!validate_validation_instrumentation(
          options.target,
          options.instrumentation,
          diagnostics)) {
    return {};
  }
  TimingScope compilation_timing = options.timings != nullptr
      ? options.timings->scope("validation compilation")
      : TimingScope{};
  CompileWorkspaceResult compiled = compile_validation(
      sources,
      options.package_directory,
      options.target,
      options.workspace,
      options.kind,
      options.optimization,
      options.instrumentation,
      options.foreign_provider_audits,
      options.timings,
      options.work_executor,
      diagnostics);
  compilation_timing.finish();
  if (!compiled.ok) return {};
  return execute_compiled_validation(
      compiled, std::move(options), diagnostics);
}

} // namespace draft
