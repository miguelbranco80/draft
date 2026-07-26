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
row and exposes the choice through Compiler Options. Startup still
shows only the active document; compiler inspectors never obscure it. An
explicitly opened library package remains selectable for checking even without
`main`.

When a marker names a default program, the IDE initially selects its `root`:

```text
draft-workspace-v1
default = editor

[program editor]
root = apps/editor
```

The IDE consumes the workspace boundary, exclusions, default Program, and the
complete effective policy for every root. Compiler Options contains an explicit
root-package selector, an explicit target selector, and the effective build
projection: optimization, artifact, output, debug/assertion policy, native
providers, and assets. Run Configuration separately owns exact argument rows,
`NAME=value` environment rows, and the working directory; it can restore the
workspace defaults as one operation. Packages and Imports is a semantic
inspector rather than routine configuration. The service formats the build
projection only after applying manifest precedence, so the Draft application
neither duplicates the workspace parser nor guesses which values Build will
use. `--source` may select one
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
target-selected files in the active root's reachable graph as deterministic
Workspace-relative names plus canonical I/O paths. Packages and Imports exposes
that semantic graph; Open Documents separately lists the ordinary documents
currently open in memory. Workspace Files is a third, deliberately different
projection: it lazily enumerates the current filesystem directory beneath the
compiler-published Workspace root, whether or not those entries participate in
the active package graph.
`turbo_editor.Buffer` owns each open file and its unsaved bytes. Switching roots
reuses an already-open buffer by path or opens another ordinary buffer; every
buffer records the compiler root that owns it. Reactivating an older buffer
first reselects that root, so Check or F5 cannot submit one root's bytes under
another root's graph. Dirty buffers are neither overwritten nor merged. Saving
writes the normal file, and polling reports an explicit conflict when disk and
dirty memory diverge. There is no IDE source database, candidate workspace,
revision history, or IDE state under `.draft/`.

File > Open File, Save As, and Open Workspace share one deterministic modal
directory browser. Location is a pathname field, entries are directories-first
and bytewise sorted, and file operations have a distinct Name field. Workspace
Files owns a separate persistent snapshot so opening that dialog cannot move
the tool pane. Its read-only location begins at the compiler-published Workspace
path, Up is disabled at that root, and Enter or a same-row double click calls the
same navigation/document-opening command; one click only selects. The pane uses
a Workspace-relative breadcrumb, one-row toolbar controls, and an empty reserved
scrollbar column until scrolling is possible. A path inside the Workspace but
outside the active root package therefore remains editing-only; a path outside
the active Workspace requires the explicit Switch and Open decision. Such a
switch and Open Workspace construct and validate a complete replacement
compiler session before swapping it behind the stable host handle. Dirty
buffers require
an explicit Save All, Discard, or Cancel choice; save never silently overwrites
a disk conflict. Close Workspace follows the same dirty-document policy and
leaves one ordinary unnamed editing buffer.

Both browsers retain at most 512 rows for one directory. They consume the whole
native enumeration and keep the first rows in the authored directories-first,
bytewise order, so even a truncated directory is deterministic rather than a
filesystem-order subset. Workspace Files enumerates only the current directory;
it never recursively scans a large Workspace merely because the pane is open.

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

## Foreground coloring and semantic transactions

Ordinary editing synchronously lexes only the active complete buffer. This
operation uses the production Draft lexer, including its recovery tokens, but
does not discover packages, refresh workspace configuration, type-check source,
propagate effects, lower IR, or emit native code. It replaces only buffer-local
syntax spans. Declaration coloring is a small token-context classification over
that same stream, not a second lexer. Diagnostics and semantic inspection views
remain the result of the latest explicit semantic operation while an edit is
pending.

Check, Build, Run, Go to Definition, and Find Usages are explicit semantic
operations. They submit the active buffer plus every other dirty buffer
belonging to the checked graph as one synchronous source transaction. Clean
inactive files remain ordinary disk inputs, avoiding conservative invalidation
of unchanged packages. Definition and usage navigation first checks pending
edits so exact source ranges can never come from a stale retained graph.

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
editor has one current buffer and one latest explicit diagnostic set. Background
semantic work, generation counters, and cancellation are intentionally absent;
the keystroke path remains fast by doing only lexical work.

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
close, zoom, z-order, and arrangement policy. Open Documents and Workspace Files
are auxiliary non-tileable tool panes, so Tile/Cascade arrange the document
together with visible semantic windows without moving either browser. The
document window title uses the root package's Workspace-relative file label
where available, not a generic title or a space-consuming canonical host path.
Opening a file reopens and focuses the document window if it was closed.

Open Documents, executable roots, the package tree, both filesystem browsers,
and read-only semantic sections share `lib/turbo_ui`'s collection viewport
policy. Applications retain cursor and offset only; the reusable layer owns
keyboard movement, distinct activation, wheel scrolling, proportional scrollbar
geometry, and visible-row mapping.
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
Disabled menu rows neither acquire the pointer highlight nor discard the last
enabled selection; crossing separators or leaving/re-entering a popup likewise
preserves that selection. Buttons expose one measured minimum footprint which
includes brackets and the shadow column, preventing application-authored labels
from being clipped by layout guesses.
No retained widget tree, callback table, or editor pointer crosses that layer.

The event loop drains immediately queued terminal reads before one renderer
publication. It preserves press/release/wheel/key order, coalesces consecutive
motion across read boundaries, and skips repeated motion observations which
cannot change hover or drag state. A private decoder state is exposed only as
`terminal.decoder_pending`; the application uses that fact for a 25-ms Escape
ambiguity deadline even while mouse reports continue. Scrollbar arrows retain
capture and consume explicit application-timed repeat pulses. Local sessions
request all-motion reports for hover. An OpenSSH environment selects the core
terminal layer's button-and-drag mode, so unpressed cell crossings do not flood
the network and event loop; clicks, releases, wheels, and drags remain intact.
Escape is solely a popup/dialog/window-operation cancel key; Alt-X starts the
explicit exit policy.

F1 opens a modal, scrollable reference generated from the application's single
authored shortcut table. F2 saves, Alt-0 opens Open Documents, F6 opens Compiler
Options, and F8 opens Diagnostics. The Window menu exposes Workspace Files
without reserving another global key. The top-level vocabulary is exactly File,
Edit, Compile, Run, Window, and Help; every menu row and global shortcut invokes
the same named application operation. Packages and Imports is an explicitly
requested structured semantic view; Click or Enter
toggles a package and Left/Right closes, opens, or enters branches. Other
declaration, reference/call, effect, and denial inspectors live under Window.
F12 resolves the symbol at the exact editor byte offset and
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
Draft. Run rejects non-executable kinds and active editing-only files, then
suspends mouse reporting, the alternate screen, and raw input in restoration
order. Draft copies the selected program's arbitrary-length argument/environment
rows and working directory; an IDE-entered relative working directory is
resolved from the Workspace exactly like a manifest value. It launches and
waits through `core/process.run_with_options`. Child output is
written directly to the restored primary terminal and remains visible behind
an explicit Enter prompt; only then does the application resume raw input and
the alternate screen and invalidate the differential renderer.
Consequently an invalid edit can never cause Run to execute the older retained
program.

Normal IDE exit disables mouse reporting, discards reports already queued in
raw input, and only then restores the screen and cooked input. This prevents
SGR report fragments from becoming visible shell command bytes after a long
synchronous build or check.

## Deliberate first-version limits

Checking is synchronous and conservatively treats every edited file as an
interface change. `core/process` has exact arguments, environment overrides,
and working-directory selection but no pipes, redirection, shell, or background
lifetime. IDE build speed now follows the manifest optimization choice; a
program configured as O2 deliberately pays the ordinary O2/ThinLTO cost.

Identifier completion, background checking, broad Codex worktree editing,
named configuration variants, and body-only invalidation remain later
measurements or features.
Ordinary Codex can continue editing the normal files, and Draft `...` retains
its existing compiler-synthesis meaning.

## Verification boundary

`draft_workspace_selection_tests` cover marker discovery, manifest grammar,
and path constraints. `draft_compiler_service_tests` cover C ABI creation,
canonical paths, multi-file source transactions, source enumeration, independent
production-lexer coloring, structured package rows, definition/usage navigation, stale-check
navigation rejection, semantic views, invalid-overlay diagnostics,
retained last-good state, topology changes, automatic root discovery,
transactional workspace replacement,
root/target switching, traversal rejection, effective build configuration and
session-summary projection, live manifest/summary refresh, target-specialized
named-root discovery, run-setting copying, configured output kinds, and native Build. Draft package
tests cover the application-side Host table, lexical-only typing path, overlay
assembly, configured Draft-owned Run, read-only buffer enforcement, real
document/clipboard/save-as commands, directory browsing, structured run-setting
editing, exhaustive menu/help behavior, 80x24 dialog layout, semantic history,
and the buffer/root invariant. `draft_draftide_smoke` launches
the real Draft-built executable, reads the repository workspace marker,
builds its selected program through the real shared compiler service, retrieves
syntax/semantic products, and paints a frame without entering an interactive
terminal. `draft_draftide_auto_root_smoke` repeats that boundary in the
manifest-free one-root `examples/hello` workspace.
