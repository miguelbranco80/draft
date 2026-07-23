// Deterministic loading of one Draft folder package.
//
// A directory is Draft's namespace, visibility boundary, compilation unit, and
// synthesis-context boundary. This module selects participating direct child
// files for one target profile, loads their exact bytes into SourceManager,
// parses Draft files, and verifies that every Draft file declares one identical
// short package name. It does not resolve imports or assign canonical workspace
// identities; those operations require the workspace/dependency root graph.
//
// Files are sorted by their package-relative filename before loading. Filesystem
// enumeration order must never influence diagnostics, syntax processing, or
// later hashes. Physical directory paths remain I/O facts, not semantic IDs.
//
// Relevant specification: docs/specification/01-core-language.md, section 3, "Folder packages".

#pragma once

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/syntax_tree.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

class TimingRecorder;
class WorkExecutor;

enum class PackageFileKind {
  DraftSource,
  AssemblySource,
};

// PackageSourceOverride supplies one complete in-memory source file under the
// same package-relative name as a selected physical .draft file. The workspace
// layer uses this only for a checked resolution overlay: package enumeration,
// target qualification, imports, and package identity still come from the
// selected physical workspace. Exact replacement bytes re-enter through the
// ordinary parser. Assembly files are never overridden through this seam.
struct PackageSourceOverride {
  std::string relative_name;
  std::string contents;
  // Coordinates refer to contents, not the physical surface file. Persistent
  // surface identity has already been converted to display coordinates so the
  // override can safely cross SourceManager instances in tests and embeddings.
  std::vector<SourceExpansionMap> expansion_maps;
};

// PackageLoadOptions contains selection facts defined by the target and
// command plus one optional non-semantic observer. file_tag is the exact
// target.file_tag; tests and benchmarks are independently selected because
// validation commands may include either set. source_overrides is normally
// empty and is populated by the resolved-program orchestrator only after it has
// verified pins and generated-source hashes. timings changes none of those
// choices and is never inspected by identity construction. work_executor and
// file_worker_count are likewise scheduling policy: each selected file remains
// one indivisible read/lex/parse task, and the coordinator publishes completed
// files afterward in canonical filename order.
struct PackageLoadOptions {
  std::string file_tag;
  bool include_tests = false;
  bool include_benchmarks = false;
  std::vector<PackageSourceOverride> source_overrides;
  // Optional observer for file discovery, I/O, and parsing. It is not a
  // package-selection fact and never participates in source identity.
  TimingRecorder *timings = nullptr;
  // Optional command-owned executor borrowed only for the synchronous call to
  // load_package. A null pointer uses a local executor, which keeps standalone
  // package-loader clients correct without creating persistent state.
  WorkExecutor *work_executor = nullptr;
  // Zero selects the executor's host bound. The executor caps the count to the
  // number of complete-file tasks. This value never changes source selection,
  // FileId assignment, syntax, or diagnostic order.
  std::size_t file_worker_count = 0;
};

// LoadedPackageFile owns no source bytes; FileId points into the caller-owned
// SourceManager. Draft sources have a parsed tree, while assembly source remains
// exact bytes for the selected target assembler and linker contract.
struct LoadedPackageFile {
  PackageFileKind kind = PackageFileKind::DraftSource;
  std::string relative_name;
  FileId source;
  std::optional<SyntaxTree> syntax;
};

// LoadedPackage is valid only when PackageLoadResult.ok is true. short_name is
// copied from source rather than retained as a string_view so adding later files
// to SourceManager cannot invalidate it. files are in canonical filename order.
struct LoadedPackage {
  std::string physical_directory;
  std::string short_name;
  std::vector<LoadedPackageFile> files;
};

struct PackageLoadResult {
  bool ok = false;
  LoadedPackage package;
};

// Classifies one direct-child filename under the exact same target and
// validation selection rules used by load_package. The operation inspects no
// filesystem state and returns no physical path: workspace discovery uses it
// to decide whether a directory contains ordinary Draft source without
// duplicating target-qualifier or `_test`/`_bench` policy. A missing result
// means the filename is not selected for this command.
[[nodiscard]] std::optional<PackageFileKind> selected_package_file_kind(
    std::string_view filename,
    const PackageLoadOptions &options);

// Loads and parses one directory without throwing. All filesystem and source
// failures become diagnostics. Syntax errors remain in the returned trees for
// tooling, but ok is false whenever any new error was reported.
[[nodiscard]] PackageLoadResult load_package(
    SourceManager &sources,
    const std::string &directory,
    const PackageLoadOptions &options,
    DiagnosticSink &diagnostics);

} // namespace draft
