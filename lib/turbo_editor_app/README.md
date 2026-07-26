# turbo_editor_app

`turbo_editor_app` is the reusable Draft application shell shared by the
standalone editor and Turbo Draft. It owns documents, terminal resources, the
event loop, File/Edit/Compile/Run/Window/Help menus, dialogs, desktop windows,
filesystem browsing, and optional compiler interaction.

The editor is an ordinary closable, zoomable, tileable window. It starts alone
and zoomed, with the active document path in its title. Open Documents is a
non-tileable tool pane; semantic inspectors appear only when requested. Fixed
dialogs are modal. `lib/turbo_editor` remains the smaller reusable document
engine and editor view with no terminal, compiler, window, or event-loop
ownership.

The package is split by owned transition rather than widget type:

- `package.draft` owns the long-lived `App` model and document/tooling tables;
- `commands.draft` owns document, workspace, search, and shortcut actions;
- `file_browser.draft` owns deterministic directory snapshots and path movement;
- `run_settings.draft` owns typed argument, environment, and working-directory
  configuration;
- `views.draft` paints dialogs and compiler/document inspectors;
- `desktop.draft` authors the complete menu vocabulary, window routing, and
  status bar;
- `host_integration.draft` copies compiler-service products and performs
  Check/Build/Run and semantic navigation; and
- `terminal_runtime.draft` alone owns the terminal session and event loop.

An optional `draft_compiler_api.Host_Api` is a borrowed synchronous procedure
table rather than a compiler dependency. A zero table produces the standalone
editor; DraftIDE supplies the compiler service; tests supply direct fakes.
Typing invokes only the host's production-lexer color operation. Check, Build,
Run, Definition, and Usages explicitly enter package semantics.

Menu commands and global shortcuts call the same named operations. Disabled
commands remain visible but inert, separators and blank popup space preserve
selection, Escape is local cancel, and Alt-X is the sole application exit
shortcut. New documents remain `Untitled N` until Save As installs a real path;
closing a dirty document or workspace always requires an explicit save,
discard, or cancel decision.

The terminal loop drains queued input before one differential presentation,
coalesces pointer motion, resolves a lone Escape after 25 ms, and drives held
scrollbar repeat from explicit deadlines. Local DraftIDE enables hover motion;
SSH sessions request click/drag/wheel input without unpressed motion. Cleanup
always disables reporting and drains queued reports before returning cooked
input to the shell.
