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
#include "syntax/parser.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

// File detail is intentionally dormant outside --timings=all. The filename is
// package-relative and safe to display, but formatting it on every ordinary
// compile would make the diagnostic facility impose work while disabled.
[[nodiscard]] TimingScope time_file_operation(
    TimingRecorder *timings,
    std::string_view operation,
    std::string_view relative_name) {
  if (timings == nullptr || timings->output() != TimingOutput::All) return {};
  return timings->scope(
      std::string(operation) + std::string(relative_name),
      TimingVisibility::Detail);
}

struct SelectedFile {
  std::string name;
  PackageFileKind kind = PackageFileKind::DraftSource;
};

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
      kind == TokenKind::KeywordFlags || kind == TokenKind::KeywordMemory;
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

PackageLoadResult load_package(
    SourceManager &sources,
    const std::string &directory,
    const PackageLoadOptions &options,
    DiagnosticSink &diagnostics) {
  PackageLoadResult result;
  result.package.physical_directory = directory;
  const std::size_t initial_error_count = diagnostics.error_count();

  if (options.file_tag.empty()) {
    diagnostics.error(SourceRange::invalid(), "target file tag must not be empty");
    return result;
  }

  std::error_code error;
  const std::filesystem::path directory_path(directory);
  TimingScope discovery_timing = options.timings != nullptr
      ? options.timings->scope(
            "package file discovery", TimingVisibility::Detail)
      : TimingScope{};
  std::filesystem::directory_iterator iterator(directory_path, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot enumerate package directory '" + directory + "': " + error.message());
    return result;
  }

  std::vector<SelectedFile> selected;
  const std::filesystem::directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot enumerate package directory '" + directory + "': " + error.message());
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
        recognized_kind(iterator->path().extension().string());
    if (!kind.has_value()) {
      continue;
    }
    std::string unqualified_stem;
    if (file_participates(name, *kind, options, unqualified_stem)) {
      selected.push_back({name, *kind});
    }
  }

  // Bytewise filename order is the canonical source processing order. std::less
  // on std::string provides that ordering independently of filesystem locale.
  std::sort(selected.begin(), selected.end(), [](const SelectedFile &left, const SelectedFile &right) {
    return left.name < right.name;
  });
  discovery_timing.finish();

  bool saw_draft_source = false;
  std::vector<std::string> used_overrides;
  for (const SelectedFile &selected_file : selected) {
    const std::filesystem::path physical = directory_path / selected_file.name;
    const PackageSourceOverride *source_override = find_override(
        options, selected_file.name, diagnostics);
    if (source_override != nullptr &&
        selected_file.kind != PackageFileKind::DraftSource) {
      diagnostics.error(
          SourceRange::invalid(),
          "resolved source cannot override assembly file '" +
              selected_file.name + "'");
      continue;
    }
    LoadFileResult load;
    TimingScope source_timing = time_file_operation(
        options.timings,
        source_override != nullptr
            ? "resolved source install: "
            : "source file I/O: ",
        selected_file.name);
    if (source_override != nullptr) {
      load.ok = true;
      load.file = sources.add_source(
          physical.string() + " [resolved]",
          source_override->contents,
          source_override->expansion_maps);
      used_overrides.push_back(source_override->relative_name);
    } else {
      load = sources.load_file(physical.string());
    }
    source_timing.finish();
    if (!load.ok) {
      diagnostics.error(SourceRange::invalid(), std::move(load.error));
      continue;
    }

    LoadedPackageFile file;
    file.kind = selected_file.kind;
    file.relative_name = selected_file.name;
    file.source = load.file;
    if (selected_file.kind == PackageFileKind::DraftSource) {
      saw_draft_source = true;
      TimingScope parse_timing = time_file_operation(
          options.timings, "lex and parse: ", selected_file.name);
      file.syntax.emplace(parse_source_file(sources, load.file, diagnostics));
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
