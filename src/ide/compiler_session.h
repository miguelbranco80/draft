// CompilerSession is the long-lived compiler boundary behind the native C ABI.
// It binds one canonical workspace, selected root package, target, and set of
// complete in-memory source overlays to the last successful
// CompileWorkspaceResult. Each check builds a private candidate through the
// existing complete-file override API; success atomically replaces the stored
// source/graph pair, while failure publishes diagnostics and leaves the
// previous checked program intact.
//
// The class owns no terminal, editor buffer, or Draft allocation. C ABI bridge
// functions in service.cpp borrow source bytes synchronously and copy only
// plain result records back to the Draft owner. The module depends on compiler
// and workspace products, the production lexer, and native publication, never
// on driver CLI internals or the Turbo UI.

#pragma once

#include "backend/build_policy.h"
#include "backend/foreign_summaries.h"
#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/manifest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft::ide {

// SyntaxStyle is the stable byte vocabulary copied through service.h. Values
// describe display hints only and never affect parsing or language meaning.
// Keep explicit ordinals synchronized with turbo_editor.Syntax_Kind.
enum class SyntaxStyle : std::uint8_t {
  Plain = 0,
  Keyword = 1,
  Comment = 2,
  String = 3,
  Number = 4,
  Declaration = 5,
  Invalid = 6,
};

// SyntaxSpan covers one half-open range in the exact current source buffer.
// Spans are source-ordered, non-owning style facts rebuilt on every check,
// including failed checks. Plain text is represented by the absence of a row.
struct SyntaxSpan {
  std::size_t start = 0;
  std::size_t end = 0;
  SyntaxStyle style = SyntaxStyle::Plain;
};

// CheckResult reports the semantic outcome and the saturated number of errors
// published for that attempt. Syntax spans and diagnostics are session side
// products; a successful result also replaces the last-good compiler graph.
struct CheckResult {
  bool ok = false;
  std::uint32_t diagnostic_count = 0;
};

// SourceOverlay is one synchronously borrowed editor buffer. physical_path is
// an I/O-boundary identity only: CompilerSession resolves it against the
// deterministic source table and converts it to PackageIdentity plus relative
// filename before entering workspace compilation. contents is never retained.
struct SourceOverlay {
  std::filesystem::path physical_path;
  std::string_view contents;
};

// ToolingSection selects one read-only projection of the last successful
// compiler graph. Text is rebuilt only when that graph is replaced and remains
// valid until the next successful check or session destruction. Diagnostics
// are separate because they describe the latest attempt, including failures.
enum class ToolingSection : std::uint8_t {
  Packages = 0,
  Declarations = 1,
  References = 2,
  Effects = 3,
  Denials = 4,
  Count = 5,
};

// PackageTreeRowKind distinguishes a semantic package from one of its direct
// authored import edges. The IDE presents packages as top-level expandable
// rows and their imports as depth-one children; it never parses the textual
// tooling projection or reconstructs the compiler graph.
enum class PackageTreeRowKind : std::uint8_t {
  Package = 0,
  Import = 1,
};

// PackageTreeRow is one deterministic display row derived from WorkspaceGraph.
// package_index is the graph index of a package row or the importing parent of
// an import row. label is UI text, not semantic identity. root and has_children
// are meaningful only for package rows. Values are rebuilt with the retained
// successful graph and never serialized or included in compiler hashes.
struct PackageTreeRow {
  std::string label;
  std::size_t package_index = 0;
  PackageTreeRowKind kind = PackageTreeRowKind::Package;
  bool root = false;
  bool has_children = false;
};

// NavigationStatus distinguishes an unavailable host/current graph from a
// valid graph where the cursor simply does not name a navigable symbol. The C
// service exposes these fixed ordinals directly; keep them synchronized with
// draft_compiler_api.Navigation_Status. A failed latest check is deliberately
// distinct from NoSymbol because tools must not navigate through retained
// last-good semantics while showing different edited bytes.
enum class NavigationStatus : std::uint8_t {
  Unavailable = 0,
  CurrentCheckFailed = 1,
  SourceNotFound = 2,
  NoSymbol = 3,
  NoDefinition = 4,
  Ready = 5,
};

// NavigationLocation is one exact source token range in the current successful
// compiler graph. source is a SourceManager FileId widened to size_t for the C
// ABI; start/end are half-open byte offsets; line/column are one-based display
// coordinates computed by SourceManager. The source index remains valid only
// until the next mutating session operation. Callers copy its path/content
// synchronously and never retain this process-local identity.
struct NavigationLocation {
  std::size_t source = 0;
  std::size_t start = 0;
  std::size_t end = 0;
  std::size_t line = 0;
  std::size_t column = 0;
};

// CompilerConfiguration contains only durable selection facts. Physical paths
// are canonicalized by the C service before construction. source_relative_name
// names a selected Draft file inside root_package_directory and is also the
// exact PackageSourceOverride relative name.
struct CompilerConfiguration {
  std::filesystem::path workspace_directory;
  std::filesystem::path root_package_directory;
  std::string root_relative_path;
  // Empty selects the first target-qualified package file in deterministic
  // filename order; a nonempty name is an explicit UI/CLI preference.
  std::string source_relative_name;
  TargetProfile target;
  // The create-time target is the host fallback unless the user supplied
  // --target. An explicit target replaces manifest target settings for every
  // selected root; a fallback yields to each root's effective build layer.
  bool target_is_explicit = false;
};

// EffectiveProgramConfiguration is the fully parsed operator policy for one
// root. It is rebuilt from draft.workspace whenever roots are rediscovered and
// then remains immutable while that selection is active. Physical input paths
// are absolute process-facing facts; arguments and environment rows preserve
// manifest source order. No value enters semantic identity except target and
// runtime_assertions through CompileWorkspaceOptions.
struct EffectiveProgramConfiguration {
  ResolvedBuildPolicy build;
  std::vector<ForeignProviderAudit> foreign_provider_audits;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
  std::optional<std::filesystem::path> working_directory;
};

// SourceOption is one editable, target-selected workspace Draft file reachable
// from the current checked root. physical_path is exposed only for file I/O;
// identity and relative_name are the semantic override key. display_name is a
// stable workspace-relative label used by editor and semantic navigation.
struct SourceOption {
  std::string display_name;
  std::filesystem::path physical_path;
  PackageIdentity identity;
  std::string relative_name;
};

// RootOption is one valid workspace executable root available for the selected
// target, plus its current editable source set. Before that root has checked,
// sources contains its direct package files; afterward it retains the complete
// reachable workspace set so switching away and back cannot omit an unsaved
// imported-package buffer. The explicitly opened root is retained even when it
// is a library without main.
struct RootOption {
  std::string root_relative_path;
  std::filesystem::path physical_directory;
  std::string source_relative_name;
  std::vector<SourceOption> sources;
  EffectiveProgramConfiguration program;
};

class CompilerSession {
public:
  // Construction records already-canonical selection facts and allocates no
  // compiler graph. initialize later creates one collision-checked temporary
  // build directory, which destruction removes best-effort.
  explicit CompilerSession(CompilerConfiguration configuration);
  ~CompilerSession();

  CompilerSession(const CompilerSession &) = delete;
  CompilerSession &operator=(const CompilerSession &) = delete;

  // Checks a complete set of in-memory source replacements. Every range is
  // borrowed only for this call. active_overlay selects the buffer whose
  // lexical spans are published. Success replaces retained semantic products;
  // failure replaces diagnostics/spans but preserves the last-good graph and
  // all tooling projections derived from it.
  [[nodiscard]] CheckResult check(std::span<const SourceOverlay> overlays,
                                  std::size_t active_overlay);

  // Rechecks the exact current bytes, then continues only that successful graph
  // through MIR, LLVM, and native linking. A failure clears the published
  // artifact path, so callers cannot accidentally run an older program.
  [[nodiscard]] CheckResult build(std::span<const SourceOverlay> overlays,
                                  std::size_t active_overlay);

  // Lexes one complete editor buffer with the production lexer and replaces
  // only syntax_spans_. This operation deliberately does not refresh package
  // configuration, type-check source, publish diagnostics, or mutate the
  // retained semantic graph. It is therefore suitable for the foreground
  // typing path; callers still perform check before requesting semantic
  // navigation for changed bytes.
  void colorize(const SourceOverlay &source);

  // Discovers target-selected executable roots and establishes the stable root
  // list used by the Draft workspace UI. Selection replaces compiler products;
  // it never attempts to reinterpret a checked graph under another root/target.
  [[nodiscard]] bool initialize(DiagnosticSink &diagnostics);
  [[nodiscard]] std::size_t root_count() const;
  [[nodiscard]] std::size_t selected_root() const;
  [[nodiscard]] std::string_view root_name(std::size_t index) const;
  [[nodiscard]] bool select_root(std::size_t index,
                                 DiagnosticSink &diagnostics);
  [[nodiscard]] bool select_target(TargetProfile target,
                                   DiagnosticSink &diagnostics);
  [[nodiscard]] const TargetProfile &target() const;
  [[nodiscard]] bool target_is_explicit() const;
  [[nodiscard]] const TargetProfile &fallback_target() const;

  // Source rows are refreshed from the successful checked graph and contain
  // only ordinary workspace-owned Draft files. Before the first successful
  // check, the selected root's direct target-applicable sources are available.
  [[nodiscard]] std::size_t source_count() const;
  [[nodiscard]] std::string_view source_name(std::size_t index) const;
  [[nodiscard]] const std::filesystem::path &
  source_path(std::size_t index) const;

  // Returned views borrow session-owned storage and remain valid only until the
  // next mutating session operation or destruction. C callers copy through the
  // fixed-buffer facade instead of observing these C++ representations.
  [[nodiscard]] const std::vector<SyntaxSpan> &syntax_spans() const;
  [[nodiscard]] std::string_view diagnostics_text() const;
  [[nodiscard]] std::string_view tooling_text(ToolingSection section) const;
  [[nodiscard]] const std::vector<PackageTreeRow> &package_tree_rows() const;
  [[nodiscard]] const std::filesystem::path &workspace_directory() const;
  [[nodiscard]] const std::filesystem::path &source_path() const;
  [[nodiscard]] const std::filesystem::path &built_output_path() const;
  [[nodiscard]] NativeArtifactKind built_artifact_kind() const;
  [[nodiscard]] std::span<const std::string> run_arguments() const;
  [[nodiscard]] std::span<const std::string> run_environment() const;
  [[nodiscard]] const std::optional<std::filesystem::path> &
  run_working_directory() const;

  // Returns a deterministic human-readable projection of the selected root's
  // effective compiler policy. Run arguments, environment, and working
  // directory have typed service operations of their own and are intentionally
  // absent: DraftIDE must not present runtime policy as a compiler option.
  [[nodiscard]] std::string build_configuration_text() const;

  // Returns the compact, deterministic identity shown on DraftIDE's status
  // line. It contains only current session facts and is never parsed back into
  // compiler state.
  [[nodiscard]] std::string session_summary_text() const;

  // Resolves the semantic symbol at one exact current editor byte offset and
  // publishes its definition plus deterministic reference rows. Navigation is
  // permitted only after the latest check succeeded; retained last-good data
  // is intentionally invisible following a rejected edit. path may name an
  // editable workspace file or a read-only compiler/dependency source returned
  // by navigation_source_path.
  [[nodiscard]] NavigationStatus prepare_navigation(std::string_view path,
                                                    std::size_t byte_offset);
  [[nodiscard]] const std::optional<NavigationLocation> &
  navigation_definition() const;
  [[nodiscard]] std::span<const NavigationLocation> navigation_usages() const;
  [[nodiscard]] std::string navigation_source_path(std::size_t source) const;
  [[nodiscard]] std::string_view
  navigation_source_text(std::size_t source) const;
  [[nodiscard]] bool navigation_source_editable(std::size_t source) const;

private:
  [[nodiscard]] CompileWorkspaceOptions compile_options() const;
  [[nodiscard]] std::optional<std::vector<WorkspaceSourceOverride>>
  source_overrides(std::span<const SourceOverlay> overlays,
                   DiagnosticSink &diagnostics) const;
  void collect_syntax_spans(const SourceOverlay &active);
  void rebuild_source_options();
  void select_root_sources(const RootOption &root);
  void rebuild_tooling_index();
  void publish_diagnostics(const SourceManager &sources,
                           const DiagnosticSink &diagnostics);
  [[nodiscard]] bool
  fresh_check(const std::vector<WorkspaceSourceOverride> &overrides,
              SourceManager &candidate_sources,
              CompileWorkspaceResult &candidate, DiagnosticSink &diagnostics);
  [[nodiscard]] CheckResult build_checked();
  [[nodiscard]] bool create_build_directory(DiagnosticSink &diagnostics);
  // Reads and resolves operator policy at foreground-operation boundaries.
  // Root refresh publishes a complete replacement only after discovery and all
  // configurations succeed. Resolution shares draftc's backend input parsers,
  // so the IDE never gains a parallel provider, asset, or artifact-kind
  // grammar.
  [[nodiscard]] bool read_workspace_manifest(WorkspaceManifest &manifest,
                                             DiagnosticSink &diagnostics) const;
  [[nodiscard]] bool refresh_root_options(const WorkspaceManifest &manifest,
                                          DiagnosticSink &diagnostics);
  [[nodiscard]] bool refresh_configuration(DiagnosticSink &diagnostics);
  [[nodiscard]] bool resolve_program_configuration(
      const WorkspaceManifest &manifest, std::string_view root,
      EffectiveProgramConfiguration &result, DiagnosticSink &diagnostics) const;
  [[nodiscard]] const EffectiveProgramConfiguration &active_program() const;
  [[nodiscard]] std::filesystem::path default_output_path() const;
  void reset_checked_program();

  CompilerConfiguration configuration_;
  TargetProfile fallback_target_;
  std::filesystem::path source_path_;
  std::filesystem::path build_directory_;
  std::filesystem::path built_output_path_;
  NativeArtifactKind built_artifact_kind_ = NativeArtifactKind::Executable;
  WorkspaceManifest manifest_;
  std::vector<RootOption> root_options_;
  std::vector<SourceOption> source_options_;
  std::size_t selected_root_ = 0;

  // The session invokes many compiler commands, but only one synchronously at
  // a time. Keeping the execution resource here avoids recreating operating-
  // system workers after each edit while retaining no checked-program product
  // in the executor itself.
  std::shared_ptr<WorkExecutor> work_executor_ =
      std::make_shared<WorkExecutor>();

  SourceManager last_good_sources_;
  std::optional<CompileWorkspaceResult> last_good_;
  std::vector<SyntaxSpan> syntax_spans_;
  std::array<std::string, static_cast<std::size_t>(ToolingSection::Count)>
      tooling_text_;
  std::vector<PackageTreeRow> package_tree_rows_;
  // These rows are derived only from last_good_ after the latest successful
  // check and are cleared before every subsequent check/selection. They never
  // become semantic input or survive a graph generation.
  bool latest_check_succeeded_ = false;
  std::optional<NavigationLocation> navigation_definition_;
  std::vector<NavigationLocation> navigation_usages_;
  std::string diagnostics_text_;
  std::uint32_t diagnostic_count_ = 0;
};

} // namespace draft::ide
