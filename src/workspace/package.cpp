// Target-qualified file selection and folder-package construction.
//
// Selection is based only on each direct child's filename. A qualifier is the
// final `@tag` before a recognized extension; it participates only when tag
// exactly equals PackageLoadOptions.file_tag. After removing that qualifier,
// `_test.draft` and `_bench.draft` obey their command-specific switches.
// Assembly extensions use the target profile's eventual assembler contract and
// are loaded but not interpreted by the Draft parser.

#include "workspace/package.h"

#include "base/timing.h"
#include "base/work_graph.h"
#include "syntax/parser.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

struct SelectedFile {
  std::string name;
  PackageFileKind kind = PackageFileKind::DraftSource;
  const EmbeddedPackageFile *embedded = nullptr;
};

// PackageFileTask owns every mutable object touched while one complete file is
// read, lexed, and parsed. Its SourceManager begins empty and contains exactly
// one file after successful I/O. The worker never touches command-owned source
// or diagnostic storage, which makes complete files independently schedulable
// without locks. Coordinator publication later follows the vector's canonical
// filename order, not worker completion order.
struct PackageFileTask {
  SelectedFile selected;
  std::filesystem::path physical;
  const PackageSourceOverride *source_override = nullptr;
  const EmbeddedPackageFile *embedded = nullptr;
  SourceManager sources;
  DiagnosticSink diagnostics;
  std::optional<SyntaxTree> syntax;
  // False retains a canonical publication slot for a configuration diagnostic
  // but suppresses I/O, as for an illegal resolved assembly override.
  bool process = true;
  bool loaded = false;
  std::uint64_t source_nanoseconds = 0;
  std::uint64_t parse_nanoseconds = 0;
};

struct PackageFileTaskContext {
  std::vector<PackageFileTask> *tasks = nullptr;
  bool measure_detail = false;
};

using PackageFileClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_nanoseconds(
    PackageFileClock::time_point started) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      PackageFileClock::now() - started).count();
  assert(elapsed >= 0 && "source task duration must be nonnegative");
  return static_cast<std::uint64_t>(elapsed);
}

// Executes the full front-end chain for one file. In particular, this is not a
// token-range or parser-region task: one worker reads all bytes, lexes the whole
// file through parse_source_file, and constructs its complete SyntaxTree. I/O
// and syntax failures are ordinary task-local diagnostics and still count as a
// successfully scheduled operation; only a broken task-ID invariant fails the
// work graph itself.
[[nodiscard]] bool execute_package_file_task(
    void *opaque_context,
    WorkTaskId task_id,
    std::string &failure) {
  auto *context = static_cast<PackageFileTaskContext *>(opaque_context);
  if (context == nullptr || context->tasks == nullptr ||
      static_cast<std::size_t>(task_id) >= context->tasks->size()) {
    failure = "package file task ID is outside its result table";
    return false;
  }

  PackageFileTask &task = (*context->tasks)[task_id];
  if (!task.process) return true;
  const PackageFileClock::time_point source_started = context->measure_detail
      ? PackageFileClock::now()
      : PackageFileClock::time_point{};
  LoadFileResult load;
  if (task.source_override != nullptr) {
    load.ok = true;
    load.file = task.sources.add_source(
        task.physical.string() + " [resolved]",
        task.source_override->contents,
        task.source_override->expansion_maps);
  } else if (task.embedded != nullptr) {
    load.ok = true;
    load.file = task.sources.add_source(
        task.physical.string(), std::string(task.embedded->contents));
  } else {
    load = task.sources.load_file(task.physical.string());
  }
  if (context->measure_detail) {
    task.source_nanoseconds = elapsed_nanoseconds(source_started);
  }
  if (!load.ok) {
    task.diagnostics.error(SourceRange::invalid(), std::move(load.error));
    return true;
  }

  assert(load.file.value == 0);
  assert(task.sources.file_count() == 1);
  task.loaded = true;
  if (task.selected.kind == PackageFileKind::DraftSource) {
    const PackageFileClock::time_point parse_started = context->measure_detail
        ? PackageFileClock::now()
        : PackageFileClock::time_point{};
    task.syntax.emplace(
        parse_source_file(task.sources, load.file, task.diagnostics));
    if (context->measure_detail) {
      task.parse_nanoseconds = elapsed_nanoseconds(parse_started);
    }
  }
  return true;
}

// Rebases a task-local diagnostic range after its sole source file has been
// published into the command SourceManager. Invalid pre-source ranges remain
// invalid. Valid ranges must name the worker's file zero by construction.
[[nodiscard]] SourceRange rebase_task_range(
    SourceRange range,
    FileId published_file) {
  if (!range.is_valid()) return range;
  assert(published_file.is_valid());
  assert(range.begin.file.value == 0);
  assert(range.end.file.value == 0);
  range.begin.file = published_file;
  range.end.file = published_file;
  return range;
}

void publish_task_diagnostics(
    DiagnosticSink &destination,
    const DiagnosticSink &source,
    FileId published_file) {
  for (const Diagnostic &diagnostic : source.diagnostics()) {
    destination.report(
        diagnostic.severity,
        rebase_task_range(diagnostic.range, published_file),
        diagnostic.message);
  }
}

// Selects at most one exact-name in-memory replacement. A duplicate is a
// caller/configuration error rather than a last-wins rule because choosing one
// would change the resolved program according to vector order.
[[nodiscard]] const PackageSourceOverride *find_override(
    const PackageLoadOptions &options,
    std::string_view relative_name,
    DiagnosticSink &diagnostics) {
  const PackageSourceOverride *result = nullptr;
  for (const PackageSourceOverride &candidate : options.source_overrides) {
    if (candidate.relative_name != relative_name) continue;
    if (result != nullptr) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolved source contains duplicate overrides for '" +
              std::string(relative_name) + "'");
      return nullptr;
    }
    result = &candidate;
  }
  return result;
}

[[nodiscard]] bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] std::optional<PackageFileKind> recognized_kind(std::string_view extension) {
  if (extension == ".draft") {
    return PackageFileKind::DraftSource;
  }
  if (extension == ".s" || extension == ".S" || extension == ".asm") {
    return PackageFileKind::AssemblySource;
  }
  return std::nullopt;
}

// Package clauses use the parser's contextual-name vocabulary. In particular,
// `package memory` is required by the compiler-distributed `core/memory`
// package even though `memory` also has meaning after assembly `clobber`.
[[nodiscard]] bool token_is_package_name(TokenKind kind) {
  return kind == TokenKind::Identifier || kind == TokenKind::KeywordC ||
      kind == TokenKind::KeywordType || kind == TokenKind::KeywordInteger ||
      kind == TokenKind::KeywordFloat || kind == TokenKind::KeywordNumber ||
      kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory ||
      kind == TokenKind::KeywordBits;
}

// Determines participation and returns the filename with a matching target
// qualifier removed for test/benchmark classification. The physical selected
// name remains unchanged in SelectedFile and diagnostics.
[[nodiscard]] bool file_participates(
    std::string_view filename,
    PackageFileKind kind,
    const PackageLoadOptions &options,
    std::string &unqualified_stem) {
  const std::size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos) {
    return false;
  }
  std::string_view stem = filename.substr(0, dot);
  const std::size_t qualifier = stem.rfind('@');
  if (qualifier != std::string_view::npos) {
    const std::string_view tag = stem.substr(qualifier + 1);
    if (tag.empty() || tag != options.file_tag) {
      return false;
    }
    stem = stem.substr(0, qualifier);
  }
  unqualified_stem.assign(stem);

  if (kind != PackageFileKind::DraftSource) {
    return true;
  }
  if (ends_with(stem, "_test")) {
    return options.include_tests;
  }
  if (ends_with(stem, "_bench")) {
    return options.include_benchmarks;
  }
  return true;
}

[[nodiscard]] std::optional<std::string> package_name_from_tree(
    const SourceManager &sources, const SyntaxTree &tree) {
  if (!tree.root().is_valid()) {
    return std::nullopt;
  }
  const SyntaxNode &root = tree.node(tree.root());
  if (root.children.empty()) {
    return std::nullopt;
  }
  const SyntaxNode &package = tree.node(root.children.front());
  if (package.kind != NodeKind::PackageClause || package.token_end <= package.token_begin + 1) {
    return std::nullopt;
  }
  const Token &name = tree.token(package.token_begin + 1);
  if (!token_is_package_name(name.kind)) {
    return std::nullopt;
  }
  return std::string(sources.text(name.range));
}

[[nodiscard]] SourceRange package_name_range(const SyntaxTree &tree) {
  const SyntaxNode &root = tree.node(tree.root());
  if (root.children.empty()) {
    return SourceRange::invalid();
  }
  const SyntaxNode &package = tree.node(root.children.front());
  if (package.token_end <= package.token_begin + 1) {
    return package.range;
  }
  return tree.token(package.token_begin + 1).range;
}

} // namespace

std::optional<PackageFileKind> selected_package_file_kind(
    std::string_view filename,
    const PackageLoadOptions &options) {
  const std::filesystem::path path{std::string(filename)};
  const std::optional<PackageFileKind> kind =
      recognized_kind(path.extension().string());
  if (!kind.has_value()) return std::nullopt;
  std::string unqualified_stem;
  if (!file_participates(filename, *kind, options, unqualified_stem)) {
    return std::nullopt;
  }
  return kind;
}

PackageLoadResult load_package(
    SourceManager &sources,
    const std::string &directory,
    const PackageLoadOptions &options,
    DiagnosticSink &diagnostics) {
  PackageLoadResult result;
  result.package.physical_directory = directory;
  result.package.embedded = !options.embedded_files.empty();
  const std::size_t initial_error_count = diagnostics.error_count();

  if (options.file_tag.empty()) {
    diagnostics.error(SourceRange::invalid(), "target file tag must not be empty");
    return result;
  }

  const std::filesystem::path directory_path(directory);
  TimingScope discovery_timing = options.timings != nullptr
      ? options.timings->scope(
            "package file discovery", TimingVisibility::Detail)
      : TimingScope{};
  std::vector<SelectedFile> selected;
  if (!options.embedded_files.empty()) {
    if (options.embedded_package_path.empty()) {
      diagnostics.error(
          SourceRange::invalid(),
          "embedded package path must not be empty");
      return result;
    }
    for (const EmbeddedPackageFile &file : options.embedded_files) {
      const std::size_t slash = file.relative_path.rfind('/');
      const std::string_view parent = slash == std::string_view::npos
          ? std::string_view(".")
          : file.relative_path.substr(0, slash);
      if (parent != options.embedded_package_path) continue;
      const std::string_view filename = slash == std::string_view::npos
          ? file.relative_path
          : file.relative_path.substr(slash + 1);
      const std::optional<PackageFileKind> kind =
          selected_package_file_kind(filename, options);
      if (kind.has_value()) {
        selected.push_back({std::string(filename), *kind, &file});
      }
    }
  } else {
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory_path, error);
    if (error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot enumerate package directory '" + directory + "': " + error.message());
      return result;
    }
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
      if (error) {
        diagnostics.error(
            SourceRange::invalid(),
            "cannot enumerate package directory '" + directory + "': " +
                error.message());
        return result;
      }

      std::error_code type_error;
      if (!iterator->is_regular_file(type_error)) {
        if (type_error) {
          diagnostics.error(
              SourceRange::invalid(),
              "cannot inspect package entry '" + iterator->path().string() + "': " +
                  type_error.message());
          return result;
        }
        continue;
      }

      const std::string name = iterator->path().filename().string();
      const std::optional<PackageFileKind> kind =
          selected_package_file_kind(name, options);
      if (kind.has_value()) selected.push_back({name, *kind, nullptr});
    }
  }

  // Bytewise filename order is the canonical source processing order. std::less
  // on std::string provides that ordering independently of filesystem locale.
  std::sort(selected.begin(), selected.end(), [](const SelectedFile &left, const SelectedFile &right) {
    return left.name < right.name;
  });
  discovery_timing.finish();

  // Resolve overrides and reject impossible task rows before starting workers.
  // This coordinator walk follows the same sorted filename order later used for
  // publication, so configuration diagnostics remain deterministic too.
  std::vector<PackageFileTask> file_tasks;
  file_tasks.reserve(selected.size());
  bool saw_draft_source = false;
  std::vector<std::string> used_overrides;
  for (const SelectedFile &selected_file : selected) {
    const std::filesystem::path physical = directory_path / selected_file.name;
    PackageFileTask task;
    task.selected = selected_file;
    task.physical = physical;
    task.embedded = selected_file.embedded;
    const PackageSourceOverride *source_override = find_override(
        options, selected_file.name, task.diagnostics);
    if (source_override != nullptr &&
        selected_file.kind != PackageFileKind::DraftSource) {
      task.diagnostics.error(
          SourceRange::invalid(),
          "resolved source cannot override assembly file '" +
              selected_file.name + "'");
      task.process = false;
    }
    if (task.process && source_override != nullptr) {
      used_overrides.push_back(source_override->relative_name);
    }
    task.source_override = source_override;
    file_tasks.push_back(std::move(task));
  }

  if (file_tasks.size() >
      static_cast<std::size_t>(std::numeric_limits<WorkTaskId>::max())) {
    diagnostics.error(
        SourceRange::invalid(),
        "package directory '" + directory + "' contains too many source files");
    return result;
  }

  // Every graph row owns one whole file and has no dependency on another file.
  // A one-file package stays on the calling thread; larger packages use the
  // command executor's bounded pool. The synchronous join makes every task
  // slot immutable before the coordinator publishes any source or diagnostic.
  WorkGraph file_graph;
  file_graph.tasks.resize(file_tasks.size());
  PackageFileTaskContext task_context{
      &file_tasks,
      options.timings != nullptr &&
          options.timings->output() == TimingOutput::All};
  // Compiler commands always provide their command-owned executor. A direct
  // package-loader client still receives parallel complete-file work, but its
  // fallback executor is allocated only when it is actually needed; merely
  // borrowing the command pool must not construct a second dormant executor.
  WorkGraphRunResult scheduled;
  if (!file_graph.tasks.empty()) {
    std::unique_ptr<WorkExecutor> local_executor;
    WorkExecutor *executor = options.work_executor;
    if (executor == nullptr) {
      local_executor = std::make_unique<WorkExecutor>();
      executor = local_executor.get();
    }
    scheduled = executor->run(
        file_graph,
        WorkGraphRunOptions{options.file_worker_count},
        execute_package_file_task,
        &task_context);
    if (!scheduled.ok) {
      std::string reason = "unknown scheduler failure";
      for (const WorkTaskResult &task : scheduled.tasks) {
        if (task.state == WorkTaskState::Failed && !task.failure.empty()) {
          reason = task.failure;
          break;
        }
      }
      diagnostics.error(
          SourceRange::invalid(),
          "cannot process package source files: " + reason);
      return result;
    }
  }
  if (options.timings != nullptr) {
    options.timings->add_counter(
        "source file tasks",
        static_cast<std::uint64_t>(file_tasks.size()));
    if (!file_tasks.empty()) {
      options.timings->add_counter("source file task waves", 1);
      options.timings->add_counter(
          "source file worker slots",
          static_cast<std::uint64_t>(scheduled.workers_used));
    }
  }

  // Publish in canonical filename order. FileId assignment, diagnostic order,
  // package-name selection, and the LoadedPackage file vector are therefore
  // byte-for-byte independent of worker scheduling.
  for (PackageFileTask &task : file_tasks) {
    if (task_context.measure_detail && task.process) {
      options.timings->record_completed_event(
          std::string(task.source_override != nullptr
                          ? "resolved source install: "
                          : task.embedded != nullptr
                          ? "embedded source load: "
                          : "source file I/O: ") + task.selected.name,
          task.source_nanoseconds,
          TimingVisibility::Detail);
      if (task.loaded &&
          task.selected.kind == PackageFileKind::DraftSource) {
        options.timings->record_completed_event(
            "lex and parse: " + task.selected.name,
            task.parse_nanoseconds,
            TimingVisibility::Detail);
      }
    }

    if (!task.loaded) {
      publish_task_diagnostics(diagnostics, task.diagnostics, FileId{});
      continue;
    }

    const FileId published_file =
        sources.append_sources(std::move(task.sources));
    if (task.syntax.has_value()) {
      task.syntax->rebase_file(published_file);
    }
    publish_task_diagnostics(
        diagnostics, task.diagnostics, published_file);

    LoadedPackageFile file;
    file.kind = task.selected.kind;
    file.relative_name = task.selected.name;
    file.source = published_file;
    if (task.selected.kind == PackageFileKind::DraftSource) {
      saw_draft_source = true;
      assert(task.syntax.has_value());
      file.syntax.emplace(std::move(*task.syntax));
      const std::optional<std::string> name = package_name_from_tree(sources, *file.syntax);
      if (name.has_value()) {
        if (result.package.short_name.empty()) {
          result.package.short_name = *name;
        } else if (result.package.short_name != *name) {
          diagnostics.error(
              package_name_range(*file.syntax),
              "package name '" + *name + "' does not match package name '" +
                  result.package.short_name + "' from another file in this directory");
        }
      }
    }
    result.package.files.push_back(std::move(file));
  }

  // An override which does not name a selected physical Draft file indicates a
  // stale manifest/file association or a target-selection mismatch. Silently
  // ignoring it could build a different resolved program than the caller
  // requested.
  for (const PackageSourceOverride &source_override : options.source_overrides) {
    if (std::find(
            used_overrides.begin(),
            used_overrides.end(),
            source_override.relative_name) == used_overrides.end()) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolved source override does not match a selected Draft file '" +
              source_override.relative_name + "'");
    }
  }

  if (!saw_draft_source) {
    diagnostics.error(
        SourceRange::invalid(),
        "package directory '" + directory + "' contains no selected .draft source files");
  }
  if (result.package.short_name.empty()) {
    diagnostics.error(
        SourceRange::invalid(),
        "package directory '" + directory + "' has no valid package declaration");
  }

  result.ok = diagnostics.error_count() == initial_error_count;
  return result;
}

} // namespace draft
