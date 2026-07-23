# Turbo Draft and the embedded compiler service

Status: first synchronous terminal IDE implemented on every current hosted
target; native execution is qualified on matching hosts.

Turbo Draft is a Draft program, not a C++ editor wrapped around Draft widgets.
`tools/draftide` contains the hosted Draft `main`; `lib/turbo_editor_app` owns
buffers, terminal resources, immediate-mode UI state, project interaction,
Build/Run policy, and the event loop. The same application package runs without
a compiler in `examples/turbo-editor`, which keeps the editor and Turbo UI
independently reusable.

The bootstrap compiler remains C++ and LLVM-backed, so one narrow shared
library bridges the bootstrapping boundary:

```text
tools/draftide                 hosted Draft executable and main
  -> lib/turbo_editor_app      editor, project UI, Build/Run policy
       -> Host_Api             provider-free Draft procedure table
            -> lib/draft_compiler
                 -> C ABI      opaque handle and fixed-layout records
                      -> CompilerSession -> draft_compiler -> LLVM
```

The C ABI owns no editor concept and never calls back into Draft. Draft creates
and destroys the opaque compiler-session handle and supplies caller-owned byte
buffers for every operation. The service borrows those buffers only during a
synchronous call. C++ retains `SourceManager`, `CompileWorkspaceResult`,
diagnostic text, syntax spans, tooling projections, and temporary native build
artifacts. No STL, filesystem, compiler, or LLVM type crosses the boundary.
This seam can therefore be replaced by the self-hosted compiler without
rewriting the Draft application.

## Files and project selection

The project model is “open a workspace directory,” with a small durable
`draft.project` file selecting the initial root package and source file. The
target defaults to the native host and remains an explicit command-line/session
choice. The compiler service discovers other executable roots using the same
workspace selection operation as `draftc`; the initially opened root remains
selectable even when it is a library. Root and target changes discard the
previous checked graph and select an ordinary target-applicable source file for
the new root.

The first manifest is intentionally closed and small:

```text
draft-project-v1
root = examples/turbo-editor
source = package.draft
```

`root` is required and workspace-relative. `source` is optional and defaults to
`package.draft`; it names one direct file inside the root. If `--root` is
present, the command-line root is an explicit override and no manifest is
required. `--source` overrides the manifest source. Turbo Draft reads exactly
`<workspace>/draft.project`: it does not search parent directories, enumerate
source files from the manifest, or change package/import discovery.

These selections already contain the information needed for Build and Run. The
workspace is the filesystem and import-identity boundary. The active root is
the package whose closed import graph is checked, and a package-level `main`
makes that root executable. A workspace may contain several such roots, so the
IDE exposes root selection instead of pretending there is one implicit
executable. A library root remains useful for focused editing and checking, but
cannot produce an executable until it supplies a valid `main`.

No directory name other than `core` has package-resolution meaning. For a
workspace `/project`, `import lib/text` reads `/project/lib/text`; `lib` is only
an ordinary first path component. `import core/console` instead selects the
compiler distribution's pinned core root, regardless of the shell's current
directory. Consequently the workspace argument, not the directory from which
`draftide` happens to be launched, must enclose every ordinary package the
program imports. From this repository's `examples/` directory, for example,
open `..` with root `examples/turbo-editor`; opening `.` with root
`turbo-editor` would intentionally exclude the repository's top-level `lib/`.

Source remains normal `.draft` files. `turbo_editor.Buffer` owns each open file
and its unsaved bytes. Switching roots reuses an already-open buffer by path or
opens another ordinary buffer; every buffer records the compiler root that owns
it. Reactivating an older buffer first reselects that root, so Check or F5 cannot
submit one root's source under another root's graph. Dirty buffers are neither
overwritten nor merged. Saving writes the normal file, and polling reports an
explicit conflict when disk and dirty memory diverge. There is no IDE source
database, candidate workspace, revision history, or IDE state under `.draft/`.

The manifest is operator configuration, not a language or dependency manifest.
Later versions may add named build/run configurations, foreign provider
artifacts, arguments, environment, or working directories when the IDE can
actually consume them. They must not list source files, redefine package
discovery, or become a dependency manager.

## Synchronous checking transaction

Every changed editor buffer is checked synchronously in the first version. The
service always asks the production lexer for tooling tokens, including comments
and invalid ranges, so classic Turbo-style syntax colors describe the exact
current bytes even when semantic checking fails. Declaration coloring is a
small token-context classification over that same stream, not a second lexer.

After one successful compile, the common semantic path copies the retained
command-local graph, applies the complete source-file override as a conservative
interface change, and resumes semantic work. Success atomically replaces the
retained graph. Failure publishes the attempt's diagnostics while leaving the
last successful graph available for inspection. If imports or packages change
topology, the stable-PackageId transition rejects the update and the service
performs a complete fresh workspace compile with the same in-memory override.
Neither path writes the unsaved bytes to disk.

This internal transaction is not a user-visible “candidate” mechanism. The
editor has one current buffer and one latest diagnostic set. Background work,
generation counters, and cancellation are intentionally absent until measured
latency justifies them.

## Compiler-derived views and native actions

On each successful check, the service derives deterministic text projections
from existing compiler products:

- package/import rows from `WorkspaceGraph`;
- declarations and visibility from package symbol tables;
- references and direct calls from HIR and effect-call summaries;
- closed procedure effects from effect summaries;
- denial regions from semantic denial records; and
- diagnostics from the latest check attempt.

These are views, not another parser, indexer, or semantic engine. The Draft UI
copies them into its own bounded storage and presents independent movable,
resizable windows. `lib/turbo_ui` distinguishes ordinary zoomable windows,
non-zooming tool windows, fixed-size dialogs, and topmost popup scopes through
plain capability records and balanced immediate-mode calls. Close is a request
returned to Draft application policy; modal dialogs exclusively route mouse,
keyboard, and focus; drop-down menus and combo lists share one popup capture.
No retained widget tree, callback table, or editor pointer crosses that layer.

F6 opens Project: its top list selects a runnable root by
mouse or Enter and its lower section displays the checked package/dependency
graph. F7 through F11 toggle declaration, reference/call, effect, denial, and
diagnostic views. F12 remains a quick root cycle and Shift-F12 cycles targets.
The Window menu tiles or cascades ordinary movable/resizable windows without
disturbing fixed dialogs.
The semantic sections are currently read-only text projections, not trees with
source-jump navigation.

Build first checks the exact current buffer. Only a successful current check
continues that retained graph through MIR, per-package LLVM modules, object
emission, and native executable linking. The service returns the resulting path
to Draft. Run then suspends mouse reporting, the alternate screen, and raw input
in restoration order; Draft launches and waits through `core/process`; and the
application resumes the terminal and invalidates the differential renderer.
Consequently an invalid edit can never cause Run to execute the older retained
program.

## Deliberate first-version limits

Checking is synchronous and conservatively treats every edit as an interface
change. The compiler service overlays one active source at a time, while the
editor may retain several open and dirty buffers. `core/process` currently runs
one exact executable path with inherited environment/current directory and no
arguments, pipes, shell, or background lifetime. Native IDE builds use `-O2`;
the compiler optimization selected for the open project remains independent.

Identifier completion, background checking, broad Codex worktree editing,
structured semantic navigation, richer build/run configurations, and body-only
invalidation remain later measurements or features. Ordinary Codex can
continue editing the normal files, and Draft `...` retains its existing
compiler-synthesis meaning.

## Verification boundary

`draft_project` package tests cover the versioned manifest grammar and path
constraints. `draft_compiler_service_tests` cover C ABI creation, canonical paths, production
syntax spans, semantic views, invalid-overlay diagnostics, retained last-good
state, topology changes, root/target switching, traversal rejection, and native
Build. Draft package tests cover the application-side Host table and Draft-owned
Run plus the buffer/root invariant. `draft_draftide_smoke` launches the real
Draft-built executable without `--root`, reads the repository manifest, builds
its selected program through the real shared compiler service, retrieves
syntax/semantic products, and paints a frame without entering an interactive
terminal.
