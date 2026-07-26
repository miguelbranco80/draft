// Public C ABI for the bootstrap compiler service used by Draft development
// tools. This header contains only fixed-width scalars, byte pointers, and
// opaque handles. C and Draft callers own every input/output buffer and the
// explicit session lifetime; the implementation borrows buffers synchronously.
//
// The ABI is intentionally smaller than the C++ compiler library. Compiler
// representations, LLVM types, filesystem classes, and allocation ownership
// never cross this boundary.

#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(DRAFT_COMPILER_SERVICE_BUILD)
#define DRAFT_COMPILER_SERVICE_API __declspec(dllexport)
#else
#define DRAFT_COMPILER_SERVICE_API __declspec(dllimport)
#endif
#else
#define DRAFT_COMPILER_SERVICE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DraftCompilerServiceConfiguration {
  const void *workspace_data;
  size_t workspace_length;
  const void *root_data;
  size_t root_length;
  const void *source_data;
  size_t source_length;
  uint8_t target;
  // Zero makes target the host fallback behind manifest program targets. One
  // makes it an explicit embedding override for every selected root.
  uint8_t target_is_explicit;
} DraftCompilerServiceConfiguration;

// Result is always written by check/build when the pointer is non-null. Counts
// describe session-owned diagnostics and spans retrievable immediately after
// the operation; no pointer in this record crosses the ownership boundary.
typedef struct DraftCompilerServiceResult {
  uint8_t success;
  uint32_t diagnostic_count;
  size_t span_count;
} DraftCompilerServiceResult;

// SyntaxResult describes the independent foreground lexical pass. It contains
// no diagnostic count because colorize neither replaces nor validates semantic
// diagnostics. A valid byte range succeeds even when it contains invalid Draft
// tokens; those tokens are returned with the Invalid syntax style.
typedef struct DraftCompilerServiceSyntaxResult {
  uint8_t success;
  size_t span_count;
} DraftCompilerServiceSyntaxResult;

// Span is one half-open byte range in the exact source passed to the latest
// check/build/colorize operation. kind uses the fixed SyntaxStyle ordinal table
// documented by the Draft binding. Padding is part of this versioned native
// ABI and is asserted against the Draft c-struct layout by the implementation.
typedef struct DraftCompilerServiceSpan {
  size_t start;
  size_t end;
  uint8_t kind;
} DraftCompilerServiceSpan;

// Overlay is one ordinary editor buffer. path names the physical source file
// selected from the service source table; source contains its complete current
// bytes. Both ranges are borrowed only for check/build. The ABI converts the
// path to semantic package identity before compilation.
typedef struct DraftCompilerServiceOverlay {
  const void *path_data;
  size_t path_length;
  const void *source_data;
  size_t source_length;
} DraftCompilerServiceOverlay;

// PackageRow is the fixed record beside one copied package-tree label. kind is
// zero for a package and one for an authored import. package_index identifies
// the package row itself or the importing parent of an import row. depth is
// consequently zero or one in this first structured view. root and
// has_children are meaningful only for package rows. The record borrows no
// compiler memory and its padding is part of the asserted Draft C ABI.
typedef struct DraftCompilerServicePackageRow {
  size_t package_index;
  uint8_t kind;
  uint8_t depth;
  uint8_t root;
  uint8_t has_children;
} DraftCompilerServicePackageRow;

// NavigationLocation is one exact half-open source range in the latest
// successful checked graph. source is an opaque session-local file index used
// only with the navigation source-copy calls below. line and column are
// one-based display coordinates. The record contains no borrowed pointer.
typedef struct DraftCompilerServiceNavigationLocation {
  size_t source;
  size_t start;
  size_t end;
  size_t line;
  size_t column;
} DraftCompilerServiceNavigationLocation;

// create validates and canonicalizes the borrowed configuration ranges, owns a
// new compiler session on success, and returns NULL on failure. root may be
// empty to select the first deterministically discovered executable root;
// source may be empty to select that package's first target-qualified file in
// bytewise filename order. A nonempty error destination is always
// NUL-terminated and may contain a truncated message.
DRAFT_COMPILER_SERVICE_API void *draft_compiler_session_create(
    const DraftCompilerServiceConfiguration *configuration,
    uint8_t *error_destination, size_t error_capacity);

DRAFT_COMPILER_SERVICE_API void draft_compiler_session_destroy(void *session);

// open_workspace constructs a complete replacement compiler session first and
// swaps it behind the stable opaque handle only on success. An empty root uses
// deterministic executable-root discovery. The borrowed path may be relative
// to the process current directory. Failures preserve the old workspace and
// copy a possibly truncated NUL-terminated explanation.
DRAFT_COMPILER_SERVICE_API uint8_t draft_compiler_session_open_workspace(
    void *session, const void *workspace_data, size_t workspace_length,
    uint8_t *error_destination, size_t error_capacity);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_workspace_path(
    void *session, uint8_t *destination, size_t capacity);

// check/build borrow a complete set of open workspace buffers synchronously.
// active_overlay selects the row whose syntax spans are published. A nil row,
// invalid range, duplicate path, unknown path, or out-of-range active index
// writes a failed result. Build always checks the exact set first and publishes
// an artifact path only for those current bytes.
DRAFT_COMPILER_SERVICE_API void
draft_compiler_session_check(void *session,
                             const DraftCompilerServiceOverlay *overlays,
                             size_t overlay_count, size_t active_overlay,
                             DraftCompilerServiceResult *result);

DRAFT_COMPILER_SERVICE_API void
draft_compiler_session_build(void *session,
                             const DraftCompilerServiceOverlay *overlays,
                             size_t overlay_count, size_t active_overlay,
                             DraftCompilerServiceResult *result);

// colorize lexes exactly one complete source buffer and publishes only its
// syntax spans. It performs no filesystem discovery, package checking, tooling
// rebuild, or native work. The caller must still check changed bytes before
// relying on semantic navigation or diagnostics.
DRAFT_COMPILER_SERVICE_API void
draft_compiler_session_colorize(void *session,
                                const DraftCompilerServiceOverlay *source,
                                DraftCompilerServiceSyntaxResult *result);

// span inspects the latest check/build/colorize operation, while diagnostics
// inspect the latest semantic check/build attempt. Out-of-range span access
// writes a zero record. Text-copy operations return the complete source byte
// count, copy at most capacity-1 bytes, and NUL-terminate when possible.
DRAFT_COMPILER_SERVICE_API void
draft_compiler_session_span(void *session, size_t index,
                            DraftCompilerServiceSpan *result);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_diagnostics(
    void *session, uint8_t *destination, size_t capacity);

// section uses ToolingSection's stable 0-4 C ordinals. An unknown value returns
// zero and writes an empty string.
DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_tooling_section(
    void *session, uint8_t section, uint8_t *destination, size_t capacity);

// Package rows enumerate the retained successful WorkspaceGraph in
// deterministic package/import order. A failed check leaves this table paired
// with the same last-good semantic program as the textual tooling sections.
// Out-of-range record access writes a zero record. copy_package_row_text uses
// the complete-size/truncated-copy contract described above.
DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_package_row_count(void *session);

DRAFT_COMPILER_SERVICE_API void
draft_compiler_session_package_row(void *session, size_t index,
                                   DraftCompilerServicePackageRow *result);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_package_row_text(
    void *session, size_t index, uint8_t *destination, size_t capacity);

// prepare_navigation resolves the semantic symbol beneath byte_offset in path
// and replaces the session's definition/usage rows. It returns the stable
// NavigationStatus ordinal: unavailable=0, current-check-failed=1,
// source-not-found=2, no-symbol=3, no-definition=4, ready=5. Navigation never
// consults a retained last-good graph after the latest source check failed.
//
// definition writes a zero record when no definition is available. Usage rows
// are deterministic by source and byte offset; out-of-range access also writes
// a zero record. Source path/text calls use the ordinary complete-size copy
// contract. editable returns one only for an ordinary workspace-owned file;
// compiler-distributed core and dependency sources are intentionally read-only.
DRAFT_COMPILER_SERVICE_API uint8_t draft_compiler_session_prepare_navigation(
    void *session, const void *path_data, size_t path_length,
    size_t byte_offset);

DRAFT_COMPILER_SERVICE_API void draft_compiler_session_navigation_definition(
    void *session, DraftCompilerServiceNavigationLocation *result);

DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_navigation_usage_count(void *session);

DRAFT_COMPILER_SERVICE_API void draft_compiler_session_navigation_usage(
    void *session, size_t index,
    DraftCompilerServiceNavigationLocation *result);

DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_copy_navigation_source_path(void *session, size_t source,
                                                   uint8_t *destination,
                                                   size_t capacity);

DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_copy_navigation_source_text(void *session, size_t source,
                                                   uint8_t *destination,
                                                   size_t capacity);

DRAFT_COMPILER_SERVICE_API uint8_t
draft_compiler_session_navigation_source_editable(void *session, size_t source);

// source_path identifies the active ordinary file. artifact_path is empty until
// one successful build and is cleared by any later failed build or selection
// change. Both use the same complete-size/truncated-copy contract as
// diagnostics.
DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_source_path(
    void *session, uint8_t *destination, size_t capacity);

// Source rows enumerate target-selected, workspace-owned Draft files in the
// active checked package graph. Before the first check they contain the direct
// files of the selected root. name is a workspace-relative UI label; path is
// the canonical file-I/O path accepted by Overlay.
DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_source_count(void *session);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_source_name(
    void *session, size_t index, uint8_t *destination, size_t capacity);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_source_path_at(
    void *session, size_t index, uint8_t *destination, size_t capacity);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_artifact_path(
    void *session, uint8_t *destination, size_t capacity);

// The artifact kind and run settings belong to the same effective selected
// program configuration used by build. Kind uses executable=0, object=1,
// static-library=2, dynamic-library=3, assembly=4. Arguments exclude argv[0];
// environment rows are NAME=value overrides. Text follows the complete-size
// copy contract, and an absent working directory returns zero.
DRAFT_COMPILER_SERVICE_API uint8_t
draft_compiler_session_artifact_kind(void *session);

DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_run_argument_count(void *session);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_run_argument(
    void *session, size_t index, uint8_t *destination, size_t capacity);

DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_run_environment_count(void *session);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_run_environment(
    void *session, size_t index, uint8_t *destination, size_t capacity);

DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_copy_run_working_directory(void *session,
                                                  uint8_t *destination,
                                                  size_t capacity);

// Copies the selected root's effective compiler policy as deterministic
// labeled lines for direct IDE presentation. Run arguments, environment, and
// working directory use the typed operations above and are not repeated here.
DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_copy_build_configuration(void *session,
                                                uint8_t *destination,
                                                size_t capacity);

// Copies a compact Workspace/Root/Target/Optimization identity for a status
// line. The returned text is presentation-only and follows the standard
// complete-size copy contract.
DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_session_summary(
    void *session, uint8_t *destination, size_t capacity);

// Root rows are deterministic workspace-relative package names for the current
// target. Selection is synchronous, returns one on success, and invalidates the
// retained compiler graph rather than reinterpreting it under a different root.
DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_root_count(void *session);

DRAFT_COMPILER_SERVICE_API size_t
draft_compiler_session_selected_root(void *session);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_root_name(
    void *session, size_t index, uint8_t *destination, size_t capacity);

DRAFT_COMPILER_SERVICE_API uint8_t
draft_compiler_session_select_root(void *session, size_t index);

// Target values use the fixed Draft Target ordinals 0-3. A successful change
// rediscovers roots and invalidates retained products. Unknown values fail
// without changing the session.
DRAFT_COMPILER_SERVICE_API uint8_t draft_compiler_session_target(void *session);

DRAFT_COMPILER_SERVICE_API uint8_t
draft_compiler_session_select_target(void *session, uint8_t target);

#ifdef __cplusplus
}
#endif

#undef DRAFT_COMPILER_SERVICE_API
