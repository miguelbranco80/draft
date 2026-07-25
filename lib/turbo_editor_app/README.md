# turbo_editor_app

`turbo_editor_app` is the reusable Draft application shell shared by the
standalone editor and Turbo Draft. It owns documents, terminal resources, the
event loop, File/Edit/Search/Run/Workspace/Window/Help menus, dialogs, desktop
windows, and optional compiler interaction.

The editor is an ordinary closable, zoomable, tileable window. It starts alone
and zoomed, with the active document path in its title. Program Files and Open
Files are explicit non-tileable navigation panes; semantic inspectors appear
only when requested. Dialogs remain fixed and modal. `lib/turbo_editor` stays a
smaller reusable document engine and editor view with no terminal, compiler,
window, or event-loop ownership.

An optional `draft_compiler_api.Host_Api` is a borrowed synchronous procedure
table rather than a compiler dependency. A zero table produces the standalone
editor; DraftIDE supplies the compiler service; tests supply direct fakes.
Typing invokes only the host's production-lexer color operation. Check, Build,
Run, Definition, and Usages explicitly enter package semantics.

The terminal loop drains queued input before one differential presentation,
coalesces pointer motion, skips repeated motion observations, resolves a lone
Escape after 25 ms, and drives held-scrollbar repeat from explicit deadlines.
Local DraftIDE enables hover motion; SSH sessions request click/drag/wheel input
without unpressed motion. Cleanup always disables reporting and drains queued
reports before returning cooked input to the shell.

Menu commands and global shortcuts call the same direct application operations.
Escape is local cancel; Alt-X and Ctrl-Q are explicit Workspace-wide exit
commands. New documents remain `Untitled N` until Save As installs a real path,
and exit routes an unnamed document to Save As or explicit Discard.
