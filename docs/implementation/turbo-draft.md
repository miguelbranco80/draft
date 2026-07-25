# Turbo Draft and the embedded compiler service

Status: first synchronous terminal IDE implemented on every current hosted
target; native execution is qualified on matching hosts.

Turbo Draft is a Draft program, not a C++ editor wrapped around Draft widgets.
`tools/draftide` contains the hosted Draft `main`; `lib/turbo_editor_app` owns
buffers, terminal resources, immediate-mode UI state, workspace interaction,
Build/Run policy, and the event loop. The same application package runs without
a compiler in `examples/turbo-editor`, which keeps the editor and Turbo UI
independently reusable.

The bootstrap compiler remains C++ and LLVM-backed, so one narrow shared
library bridges the bootstrapping boundary:

```text
tools/draftide                 hosted Draft executable and main
  -> lib/turbo_editor_app      editor, workspace UI, Build/Run policy
       -> Host_Api             provider-free Draft procedure table
            -> lib/draft_compiler
                 -> C ABI      opaque handle and fixed-layout records
                      -> CompilerSession -> draft_compiler -> LLVM
```

The C ABI owns no editor concept and never calls back into Draft. Draft creates
and destroys the opaque compiler-session handle and supplies caller-owned byte
buffers for every operation. The service borrows those buffers only during a
synchronous call. C++ retains `SourceManager`, `CompileWorkspaceResult`,
diagnostic text, syntax spans, tooling projections, structured package rows,
exact navigation rows, and temporary native build artifacts. No STL,
filesystem, compiler, or LLVM type crosses the boundary.
This seam can therefore be replaced by the self-hosted compiler without
rewriting the Draft application.

## Files and workspace selection

The IDE opens an ordinary directory. The compiler service uses the same upward
`draft.workspace` search as `draftc`, so that directory may be a workspace or a
package inside one. The nearest marker establishes the import and `.draft/`
state boundary. With no marker, the opened directory is a standalone
workspace. The service discovers executable roots below that boundary; with
one it selects it directly, and with several it selects the first deterministic
row and opens the program/package view so the user can choose. An explicitly
opened library package remains selectable for checking even without `main`.

When a marker names a default program, the IDE initially selects its `root`:

```text
draft-workspace-v1
default = editor

[program editor]
root = apps/editor
```

The IDE consumes the workspace boundary, exclusions, default program, and the
complete effective build/run layer for every root. The Programs & Packages
window shows the selected Program's effective target, optimization, artifact,
output, debug/assertion policy, native providers and assets, arguments,
environment, and working directory. The service formats this projection only
after applying manifest precedence, so the Draft application neither duplicates
the workspace parser nor guesses which values Build and F5 will use. `--source`
may select one
direct file inside the active package. Without it, the compiler opens the first
target-selected source in bytewise filename order; `package.draft` has no
privileged IDE meaning. No root selector is required because the opened path or
named default supplies the initial package. A named program's target
participates in target-qualified root inspection. An explicit `--target`
selection replaces manifest targets without changing the file.

These selections already contain the information needed for Build and Run. The
workspace is the filesystem and import-identity boundary. The active root is
the package whose closed import graph is checked, and a package-level `main`
makes that root executable. A workspace may contain several such roots, so the
IDE exposes root selection instead of pretending there is one implicit
executable. A library root remains useful for focused editing and checking, but
cannot produce an executable until it supplies a valid `main`.

No directory name other than `core` has package-resolution meaning. For a
workspace `/work`, `import lib/text` reads `/work/lib/text`; `lib` is only
an ordinary first path component. `import core/console` instead selects the
compiler distribution's pinned core root, regardless of the shell's current
directory. Consequently the workspace argument, not the directory from which
`draftide` happens to be launched, must enclose every ordinary package the
program imports. From this repository's `examples/` directory, for example,
open `..` with root `examples/turbo-editor`; opening `.` with root
`turbo-editor` would intentionally exclude the repository's top-level `lib/`.

Source remains normal `.draft` files. After checking, the service publishes the
target-selected workspace files in the reachable package graph as deterministic
workspace-relative names plus canonical I/O paths. Workspace Sources browses
that table;
Buffers separately lists the ordinary documents currently open in memory.
`turbo_editor.Buffer` owns each open file and its unsaved bytes. Switching roots
reuses an already-open buffer by path or opens another ordinary buffer; every
buffer records the compiler root that owns it. Reactivating an older buffer
first reselects that root, so Check or F5 cannot submit one root's bytes under
another root's graph. Dirty buffers are neither overwritten nor merged. Saving
writes the normal file, and polling reports an explicit conflict when disk and
dirty memory diverge. There is no IDE source database, candidate workspace,
revision history, or IDE state under `.draft/`.

File > Open File accepts an ordinary pathname without changing the Workspace.
Relative paths start at the Workspace when the compiler host is attached;
absolute paths retain their native meaning. A path in Workspace Sources keeps
its selected Program association, while any other file is explicitly
editing-only and cannot enter a compiler override by accident. Reopening the
same exact path reuses its in-memory buffer and unsaved bytes.

File > Open Folder accepts another directory without restarting. The native
service constructs and validates a complete replacement compiler session before
swapping it behind the stable host handle. Draft then replaces its source table
and buffers. Dirty buffers require an explicit Save all, Discard, or Cancel
choice; save never silently overwrites a disk conflict. The first version uses
a typed folder path rather than introducing a second filesystem browser.

The manifest is operator configuration, not a language or dependency manifest.
The compiler service resolves workspace defaults followed by the matching
program overrides into one immutable root record. One backend-owned build-policy
operation interprets that merged record for both `draftc` and the compiler
service; target, artifact, optimization, provider, summary, asset, and relative
path grammar therefore have no IDE-specific copy. Root discovery retains the
already-resolved record used for target-specific inspection instead of reading
provider summaries again while publishing the root table. Draft receives only
the resulting artifact kind/path and copied run arguments, environment, and
working directory. The manifest does not list source files, redefine package
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

On each successful check, the service derives deterministic inspection data
from existing compiler products:

- package/import rows from `WorkspaceGraph`;
- declarations and visibility from package symbol tables;
- references and direct calls from HIR and effect-call summaries;
- closed procedure effects from effect summaries;
- denial regions from semantic denial records;
- diagnostics from the latest check attempt; and
- exact definition and ordered usage ranges resolved from symbol tables, HIR,
  imported-symbol identities, and retained syntax nodes.

Package/import and navigation data cross the C ABI as fixed records, with
labels, paths, and source text copied separately into Draft ownership. The
other sections remain formatted text. Navigation is accepted only when the
latest check succeeded against the visible bytes; a rejected edit may retain a
last-good graph for summary windows, but F12 never consults it. Imported proxy
symbols are canonicalized to their provider declaration, and member ranges are
narrowed through retained syntax nodes rather than parsing formatted dumps.
These are views, not another parser, indexer, or semantic engine. The Draft UI
copies them into its own bounded storage and presents independent movable,
resizable windows. The document editor uses that same ordinary window model:
its content is the reusable `turbo_editor` view, while the application owns its
close, zoom, z-order, and arrangement policy. Sources and Buffers are auxiliary
non-tileable tool panes, so Tile/Cascade arrange the document together with
visible semantic windows without moving those browsers. Opening a buffer
reopens and focuses the document window if it was closed.

Workspace Sources, Buffers, executable roots, the package tree, and read-only semantic
sections share `lib/turbo_ui`'s collection viewport policy. Applications retain
cursor and offset only; the reusable layer owns keyboard movement, distinct
activation, wheel scrolling, proportional scrollbar geometry, and visible-row
mapping.
Rich rows paint markers and byte labels directly after `list_view`, so reuse
does not require callbacks, label allocation, or a retained item model. The
tree specialization consumes a caller-owned flat preorder table and scratch
visible-index mapping; expansion, Left/Right parent navigation, and `+`/`-`
disclosure remain reusable UI policy while graph identity stays in the compiler.

`lib/turbo_ui` distinguishes ordinary zoomable windows,
non-zooming tool windows, fixed-size dialogs, and topmost popup scopes through
plain capability records and balanced immediate-mode calls. Close is a request
returned to Draft application policy; modal dialogs exclusively route mouse,
keyboard, and focus; drop-down menus and combo lists share one popup capture.
Window chrome uses the same pinned Unicode column widths as `core/tui`: the
text-default zoom arrow occupies one cell before the upper-right corner, while
the emoji-presentation form would occupy two. Shadows use explicit one-column
shade cells so differential erasure changes glyph content as well as style.
Menu titles and rows derive both their visible underlined mnemonic and their
activation from one access-key value. Right-aligned command shortcuts remain
application policy, so Turbo Draft handles them before focused controls and
invokes the same direct operation as the corresponding menu branch.
No retained widget tree, callback table, or editor pointer crosses that layer.

The event loop drains immediately queued terminal reads before one renderer
publication. It preserves press/release/wheel/key order, coalesces consecutive
motion across read boundaries, and skips repeated motion observations which
cannot change hover or drag state. A private decoder state is exposed only as
`terminal.decoder_pending`; the application uses that fact for a 25-ms Escape
ambiguity deadline even while mouse reports continue. Scrollbar arrows retain
capture and consume explicit application-timed repeat pulses. Escape is solely
a popup/dialog/window-operation cancel key; Alt-X starts the explicit quit
policy.

F1 opens a modal, scrollable reference generated from the application's single
authored shortcut table. F6 opens Programs & Packages: its top list selects a
runnable root by mouse or Enter, its middle section displays the complete
effective Build/Run configuration, and its lower section displays an expandable
checked package/dependency tree.
Click or Enter toggles a package; Left/Right close, open, or enter branches.
F7 through F11 toggle declaration, reference/call, effect, denial, and
diagnostic views. F12 resolves the symbol at the exact editor byte offset and
selects its definition; Shift-F12 replaces the References window with a
structured, ordered Usages list. Alt-Left/Alt-Right traverse application-owned
path/range history after later compiler checks invalidate service-local file
IDs. Compiler-distributed core and dependency text is copied while its FileId
is current and opened as a visibly read-only `turbo_editor.Buffer`; mutation is
rejected by the document engine itself. The Window menu tiles or cascades
ordinary tileable windows without disturbing auxiliary tools or fixed dialogs.

Build first checks the exact active-plus-dirty workspace buffer set. Only a
successful check continues that retained graph through MIR, per-package LLVM
modules, object/assembly emission, and publication of the configured artifact.
Optimization, assertion mode, artifact kind/output, debug information,
providers/summaries, and runtime assets come from the selected root's effective
manifest configuration. Check and Build reread the small saved manifest, and
also reread provider-summary inputs, before using the retained graph. A changed
target, assertion, or summary invalidates that graph; unchanged structural
configuration retains it. The service returns the resulting kind and path to
Draft. Run rejects non-executable kinds, then suspends mouse reporting, the
alternate screen, and raw input in restoration order. Draft copies the selected
program's arbitrary-length argument/environment rows and working directory,
launches and waits through `core/process.run_with_options`. Child output is
written directly to the restored primary terminal and remains visible behind
an explicit Enter prompt; only then does the application resume raw input and
the alternate screen and invalidate the differential renderer.
Consequently an invalid edit can never cause Run to execute the older retained
program.

Normal IDE exit disables mouse reporting, discards reports already queued in
raw input, and only then restores the screen and cooked input. This prevents
all-motion SGR report fragments from becoming visible shell command bytes after
a long synchronous build or check.

## Deliberate first-version limits

Checking is synchronous and conservatively treats every edited file as an
interface change. `core/process` has exact arguments, environment overrides,
and working-directory selection but no pipes, redirection, shell, or background
lifetime. IDE build speed now follows the manifest optimization choice; a
program configured as O2 deliberately pays the ordinary O2/ThinLTO cost.

Identifier completion, background checking, broad Codex worktree editing,
named configuration variants, and body-only invalidation remain later
measurements or features. Open Folder currently uses a typed path rather than
a directory browser.
Ordinary Codex can continue editing the normal files, and Draft `...` retains
its existing compiler-synthesis meaning.

## Verification boundary

`draft_workspace_selection_tests` cover marker discovery, manifest grammar,
and path constraints. `draft_compiler_service_tests` cover C ABI creation,
canonical paths, multi-file source transactions, source enumeration, production
syntax spans, structured package rows, definition/usage navigation, stale-check
navigation rejection, semantic views, invalid-overlay diagnostics,
retained last-good state, topology changes, automatic root discovery,
transactional workspace replacement,
root/target switching, traversal rejection, effective program configuration,
live manifest/summary refresh, target-specialized named-root discovery,
run-setting copying, configured output kinds, and native Build. Draft package
tests cover the application-side Host table, overlay assembly, configured
Draft-owned Run, read-only buffer enforcement, shortcut/help behavior,
semantic history, and the buffer/root invariant. `draft_draftide_smoke` launches
the real Draft-built executable, reads the repository workspace marker,
builds its selected program through the real shared compiler service, retrieves
syntax/semantic products, and paints a frame without entering an interactive
terminal. `draft_draftide_auto_root_smoke` repeats that boundary in the
manifest-free one-root `examples/hello` workspace.
