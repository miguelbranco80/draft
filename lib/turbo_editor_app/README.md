# turbo_editor_app

`turbo_editor_app` is the reusable Draft application shell shared by the
standalone editor and DraftIDE. It owns documents, terminal resources, the
event loop, File/Edit/Compile/Run/Window/Help menus, dialogs, desktop windows,
filesystem browsing, and optional compiler interaction.

The editor is an ordinary closable, zoomable, tileable window. It starts alone
and zoomed, with the active document path in its title. Open Documents and the
workspace-rooted Workspace Files browser are non-tileable tool panes; semantic
inspectors appear only when requested. Fixed dialogs are modal.
`lib/turbo_editor` remains the smaller reusable document engine and editor view
with no terminal, compiler, window, or event-loop ownership.

Workspace Files uses a compact one-row toolbar, Workspace-relative breadcrumb,
and scrollbar-on-demand. Enter or a true same-row double click navigates a
directory or opens a document; one click only selects.

DraftIDE supplies one flat application theme to every window, menu popup, and
button. Modal action rows are compact and right-aligned; toolbar actions remain
left-aligned. The editor, lists, trees, and read-only text views retain a stable
reserved scrollbar column but paint it only when content can actually scroll.
The reusable `lib/turbo_ui` default remains the classic shadowed presentation
for other clients.

The package is split by owned transition rather than widget type:

- `package.draft` owns the long-lived `App` model and document/tooling tables;
- `commands.draft` owns document, workspace, search, and shortcut actions;
- `file_browser.draft` owns the modal file selector and the independent,
  persistent Workspace Files snapshot, including deterministic path movement;
- `run_settings.draft` owns typed argument, environment, and working-directory
  configuration;
- `views.draft` paints dialogs and compiler/document inspectors;
- `desktop.draft` authors the complete menu vocabulary, window routing, and
  status bar;
- `comment_expansion_job.draft` owns the one worker, immutable request
  snapshots, and atomic completion publication for the `//?`/`//!` prototype;
- `host_integration.draft` copies compiler-service products and performs
  provider-free Check/Build/Run, unsaved agent-comment rewriting, explicit
  Resolve/Judge, structured diagnostic activation, persistent result recording,
  and semantic navigation; and
- `terminal_runtime.draft` alone owns the terminal session and event loop.

An optional `draft_compiler_api.Host_Api` is a borrowed synchronous procedure
table rather than a compiler dependency. A zero table produces the standalone
editor; DraftIDE supplies the compiler service; tests supply direct fakes. The
agent-comment callback remains synchronous at this boundary but is invoked by
one App-owned Draft worker while the terminal thread makes no other host call.
Typing invokes only the host's production-lexer color operation. Check, Build,
Run, Resolve, Judge, Definition, and Usages explicitly enter package semantics.
Comment expansion, Resolve, and Judge occupy distinct optional callbacks, so an
ordinary Build or F5 path has no flag or fallback which could enter provider
work.

Build and Check update an app-owned result transcript and timed status without
changing result-window visibility on success. Compiler failure raises
Diagnostics; selecting a row opens its exact half-open range, including a
retained read-only source for generated/compiler-owned bytes. F5 prepares the
artifact and run configuration before the terminal is suspended, so failed
compilation never enters Program Output. Once launched, exit/signal/launch
failures remain runtime results and do not mutate Diagnostics.

Resolve and Judge save dirty documents in the active root's reachable path set
before queuing their synchronous calls. This includes a package shared by two
roots but excludes unrelated editing-only documents. Resolve reports pin counts
and commit state; Judge reports selected claims, evidence count, and verdict.
Both replace Build Output. Compiler/provider errors raise Diagnostics, while a
completed negative verdict with no diagnostic raises Build Output. The event
loop presents the pending status before beginning potentially long provider
work.

The prototype comment command is intentionally smaller. Put the cursor on any
line in a maximal contiguous same-marker `//?` or `//!` block, then press Ctrl-E
or choose **Compile > Expand Agent Comment**. Text after the selected markers
is joined with newlines and sent with the marker kind, exact range, complete
active source, active logical path, and a private read-only snapshot of
reachable workspace-owned Draft sources. Active and dirty open buffers override
disk, so Codex sees the same unsaved workspace state as Check; compiler/
dependency sources and unrelated workspace files are absent. Other annotations
in the active source are context, while the exact selected block is the
immediate request.

Codex returns one complete replacement for the active file and cannot edit or
create any other file. Its dedicated instructions permit imports, declarations,
and local changes, require unrelated behavior to be preserved, and explain how
to leave an honest TODO, minimal compiling seam/no-op, or retained annotation
when the requested work needs a change outside this single-file boundary.
`//?` is intended to remain as persistent intent; Codex may remove, retain, or
turn `//!` into an ordinary comment. The app deliberately does not interpret or
enforce those choices. The compiler host privately checks the first candidate
and, on errors, may make one advisory Codex reconsideration using compact
logical-path diagnostics. The host does not validate or reject the second
candidate. The app replaces the returned active document verbatim as one
unsaved history transaction, so one ordinary Ctrl-Z restores the exact prior
bytes. A byte-identical result is a successful no-op. Failure updates Build Output
without replacing semantic Diagnostics. While Codex runs, the status bar
animates and terminal input is consumed without dispatch; disk polling and
every other compiler-session call resume after the worker joins. Returned bytes
have no compiler-validity guarantee.

Menu commands and global shortcuts call the same named operations. Disabled
commands remain visible but inert, separators and blank popup space preserve
selection, Escape is local cancel, and Alt-X is the sole application exit
shortcut. A successful Save explicitly replaces any earlier formatted status
with `Saved`, so its confirmation is visible until subsequent feedback replaces
it. New documents remain `Untitled N` until Save As installs a real path;
closing a dirty document or workspace always requires an explicit save,
discard, or cancel decision.

Find and Replace are repeatable modal sessions. Next, Previous, Replace, Replace
All, and Find Next update and repaint the document underneath while keeping the
active dialog and query focus; only Cancel, Escape, or the window close control
ends the session. Standalone F3, Shift-F3, and F4 still focus the editor.

The terminal loop drains queued input before one differential presentation,
coalesces pointer motion, resolves a lone Escape after 25 ms, and drives held
scrollbar repeat from explicit deadlines. Local DraftIDE enables hover motion;
SSH sessions request click/drag/wheel input without unpressed motion. Cleanup
always disables reporting and drains queued reports before returning cooked
input to the shell.
