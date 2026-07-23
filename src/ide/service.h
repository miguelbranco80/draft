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
} DraftCompilerServiceConfiguration;

// Result is always written by check/build when the pointer is non-null. Counts
// describe session-owned diagnostics and spans retrievable immediately after
// the operation; no pointer in this record crosses the ownership boundary.
typedef struct DraftCompilerServiceResult {
  uint8_t success;
  uint32_t diagnostic_count;
  size_t span_count;
} DraftCompilerServiceResult;

// Span is one half-open byte range in the exact source passed to the latest
// check/build. kind uses the fixed SyntaxStyle ordinal table documented by the
// Draft binding. Padding is part of this versioned native ABI and is asserted
// against the Draft c-struct layout by the implementation.
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

// create validates and canonicalizes the borrowed configuration ranges, owns a
// new compiler session on success, and returns NULL on failure. root may be
// empty to select the first deterministically discovered executable root;
// source may be empty to prefer package.draft. A nonempty error destination is
// always NUL-terminated and may contain a truncated message.
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

// check/build borrow a complete set of open project buffers synchronously.
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

// span and copy_diagnostics inspect the latest attempt. Out-of-range span
// access writes a zero record. Text-copy operations return the complete source
// byte count, copy at most capacity-1 bytes, and NUL-terminate when possible.
DRAFT_COMPILER_SERVICE_API void
draft_compiler_session_span(void *session, size_t index,
                            DraftCompilerServiceSpan *result);

DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_diagnostics(
    void *session, uint8_t *destination, size_t capacity);

// section uses ToolingSection's stable 0-4 C ordinals. An unknown value returns
// zero and writes an empty string.
DRAFT_COMPILER_SERVICE_API size_t draft_compiler_session_copy_tooling_section(
    void *session, uint8_t section, uint8_t *destination, size_t capacity);

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
