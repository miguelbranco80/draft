# turbo_editor_app

`turbo_editor_app` is the reusable Draft application shell shared by the
standalone editor example and Turbo Draft. It owns buffers, terminal resource
lifetime, the event loop, immediate-mode windows, and Build/Run interaction.

Its optional `draft_compiler_api.Host_Api` is a borrowed procedure table rather
than a compiler dependency. A zero table produces the standalone editor;
Turbo Draft supplies the real compiler service; tests supply small fakes. The
host is synchronous and borrows all byte ranges only for each call.

This split keeps `lib/turbo_editor`, `lib/turbo_ui`, and the useful standalone
editor independent of LLVM and the bootstrap compiler.
