# turbo_editor_app

`turbo_editor_app` is the reusable Draft application shell shared by the
standalone editor example and Turbo Draft. It owns buffers, terminal resource
lifetime, the event loop, File/Edit/Project/Window menus, modal search/quit
dialogs, managed tool windows, and Build/Run interaction.

The document editor is an ordinary closable, zoomable, tileable desktop window.
It starts zoomed behind the auxiliary panes to retain a large editing area.
Files and Buffers are non-tileable auxiliary panes; dialogs remain fixed. The
Window menu can reopen the editor after close, and opening any source or buffer
also reopens and focuses it. `lib/turbo_editor` itself remains only a reusable
buffer plus bounded editor view—the application supplies the window content
rectangle and owns all desktop policy.

Its optional `draft_compiler_api.Host_Api` is a borrowed procedure table rather
than a compiler dependency. A zero table produces the standalone editor;
Turbo Draft supplies the real compiler service; tests supply small fakes. The
host is synchronous and borrows all byte ranges only for each call. Turbo
Draft's Project window renders the service's structured package/import rows
through `turbo_ui.tree_view`; the standalone editor simply leaves that optional
table empty.

This split keeps `lib/turbo_editor`, `lib/turbo_ui`, and the useful standalone
editor independent of LLVM and the bootstrap compiler.
