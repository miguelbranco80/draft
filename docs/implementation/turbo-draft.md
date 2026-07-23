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

The project model is “open a workspace directory.” The compiler service
discovers executable roots using the same workspace selection operation as
`draftc`; with one root it selects it directly, and with several it selects the
first deterministic row and opens Project so the user can choose. The target
defaults to the native host and remains an explicit command-line/session
choice. An explicitly opened library root remains selectable even without
`main`. Root and target changes discard the previous checked graph and select
an ordinary target-applicable source file for the new root.

The first manifest is intentionally closed and small:

```text
draft-project-v1
root = examples/turbo-editor
source = package.draft
```

The complete `draft.project` file is optional. When present, its `root` is
required and workspace-relative; `source` is optional, defaults to
`package.draft`, and names one direct file inside the root. `--root` and
`--source` are explicit overrides. Turbo Draft reads only
`<workspace>/draft.project`: it does not search parents, enumerate source files
from the manifest, or change package/import discovery. With neither manifest
nor `--root`, executable-root discovery provides the in-memory choice.

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

Source remains normal `.draft` files. After checking, the service publishes the
target-selected workspace files in the reachable package graph as deterministic
workspace-relative names plus canonical I/O paths. Files browses that table;
Buffers separately lists the ordinary documents currently open in memory.
`turbo_editor.Buffer` owns each open file and its unsaved bytes. Switching roots
reuses an already-open buffer by path or opens another ordinary buffer; every
buffer records the compiler root that owns it. Reactivating an older buffer
first reselects that root, so Check or F5 cannot submit one root's bytes under
another root's graph. Dirty buffers are neither overwritten nor merged. Saving
writes the normal file, and polling reports an explicit conflict when disk and
dirty memory diverge. There is no IDE source database, candidate workspace,
revision history, or IDE state under `.draft/`.

File > Open Workspace accepts another directory without restarting. The native
service constructs and validates a complete replacement compiler session before
swapping it behind the stable host handle. Draft then replaces its source table
and buffers. Dirty buffers require an explicit Save all, Discard, or Cancel
choice; save never silently overwrites a disk conflict. Open File initially
means selecting a compiler-known row in Files rather than introducing a second
filesystem browser.

The manifest is operator configuration, not a language or dependency manifest.
Later versions may add named build/run configurations, foreign provider
artifacts, arguments, environment, or working directories when the IDE can
actually consume them. They must not list source files, redefine package
discovery, or become a dependency manager.

## Synchronous checking transaction

The active buffer plus every other dirty buffer belonging to the checked graph
enters one synchronous source transaction. Clean inactive files remain ordinary
disk inputs, avoiding conservative invalidation of unchanged packages. The
active overlay separately selects which exact bytes the production lexer
colors, including comments and invalid ranges, so classic
Turbo-style syntax colors remain buffer-local even when semantic checking
fails. Declaration coloring is a small token-context classification over that
same stream, not a second lexer.

After one successful compile, the common semantic path copies the retained
command-local graph, applies all complete source-file overrides as conservative
interface changes, and resumes semantic work. Physical paths are resolved
against the service source table and converted to package identities before
this compiler API is entered. Success atomically replaces the
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
resizable windows. The document editor uses that same ordinary window model:
its content is the reusable `turbo_editor` view, while the application owns its
close, zoom, z-order, and arrangement policy. Files and Buffers are auxiliary
non-tileable tool panes, so Tile/Cascade arrange the document together with
visible semantic windows without moving those browsers. Opening a buffer
reopens and focuses the document window if it was closed.

`lib/turbo_ui` distinguishes ordinary zoomable windows,
non-zooming tool windows, fixed-size dialogs, and topmost popup scopes through
plain capability records and balanced immediate-mode calls. Close is a request
returned to Draft application policy; modal dialogs exclusively route mouse,
keyboard, and focus; drop-down menus and combo lists share one popup capture.
Menu titles and rows derive both their visible underlined mnemonic and their
activation from one access-key value. Right-aligned command shortcuts remain
application policy, so Turbo Draft handles them before focused controls and
invokes the same direct operation as the corresponding menu branch.
No retained widget tree, callback table, or editor pointer crosses that layer.

F6 opens Project: its top list selects a runnable root by
mouse or Enter and its lower section displays the checked package/dependency
graph. F7 through F11 toggle declaration, reference/call, effect, denial, and
diagnostic views. F12 remains a quick root cycle and Shift-F12 cycles targets.
The Window menu tiles or cascades ordinary tileable windows without disturbing
auxiliary tools or fixed dialogs.
The semantic sections are currently read-only text projections, not trees with
source-jump navigation.

Build first checks the exact active-plus-dirty project buffer set. Only a
successful check continues that retained graph through MIR, per-package LLVM
modules, object emission, and native executable linking. The service returns
the resulting path to Draft. Run then suspends mouse reporting, the alternate
screen, and raw input in restoration order; Draft launches and waits through
`core/process`; and the
application resumes the terminal and invalidates the differential renderer.
Consequently an invalid edit can never cause Run to execute the older retained
program.

## Deliberate first-version limits

Checking is synchronous and conservatively treats every edited file as an
interface change. `core/process` currently runs
one exact executable path with inherited environment/current directory and no
arguments, pipes, shell, or background lifetime. Native IDE builds use `-O0`:
the current package-wide `-O2` pipeline makes routine IDE rebuilds take several
minutes, while the event/repaint fixes keep the interactive path responsive.
The compiler optimization selected for the open project remains independent.

Identifier completion, background checking, broad Codex worktree editing,
structured semantic navigation, richer build/run configurations, and body-only
invalidation remain later measurements or features. Open Workspace currently
uses a typed path rather than a directory browser. Ordinary Codex can
continue editing the normal files, and Draft `...` retains its existing
compiler-synthesis meaning.

## Verification boundary

`draft_project` package tests cover the optional versioned manifest grammar and
path constraints. `draft_compiler_service_tests` cover C ABI creation, canonical
paths, multi-file source transactions, source enumeration, production syntax
spans, semantic views, invalid-overlay diagnostics, retained last-good state,
topology changes, automatic root discovery, transactional workspace replacement,
root/target switching, traversal rejection, and native Build.
Draft package tests cover the application-side Host table, overlay assembly,
Draft-owned Run, and the buffer/root invariant. `draft_draftide_smoke` launches
the real Draft-built executable without `--root`, reads the repository manifest,
builds its selected program through the real shared compiler service, retrieves
syntax/semantic products, and paints a frame without entering an interactive
terminal. `draft_draftide_auto_root_smoke` repeats that boundary in the
manifest-free one-root `examples/hello` workspace.
