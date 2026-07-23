# Turbo Draft

Turbo Draft is a full-screen terminal IDE written in Draft. The only C++ part
is the bootstrap compiler service behind an opaque C ABI; project selection,
files, editor state, syntax-colored UI, Build/Run policy, and `main` live here
or in reusable Draft libraries.

Build and run on a native supported host:

```sh
cmake --build build --target draftide --parallel
build/draftide .
```

The first path is the workspace, not the current package. Turbo Draft discovers
packages containing `main`; if there are several, Project opens so you can
choose the active Build/Run root. A workspace may optionally provide
`<workspace>/draft.project` to choose the initial row:

```text
draft-project-v1
root = examples/turbo-editor
source = package.draft
```

When the file exists, `root` selects one package relative to the workspace;
`source` is optional and defaults to `package.draft`. Without it, no project
file is required. `--root` and `--source` are explicit command-line overrides.
Imports such as
`lib/turbo_editor_app` are resolved below the workspace, while `core/terminal`
comes from the compiler's pinned core distribution. Thus, when launching from
this repository's `examples/` directory, keep the repository as the workspace:

```sh
../build/draftide ..
```

Opening `.` with root `turbo-editor` would make `examples/` the workspace and
would therefore exclude the top-level `lib/`. Turbo Draft needs no package list:
it discovers packages containing `main` and F12 switches among those executable
roots. The explicitly selected root may also be a library for focused editing,
although it is not runnable.

The top row contains real File, Edit, Project, and Window drop-down menus. Their
access letters are underlined: use Alt-F/Alt-E/Alt-P/Alt-W to open a menu, then
press an underlined command letter, or use the mouse. The right-aligned command
shortcuts are active application-wide rather than only when the editor owns
focus: Ctrl-S saves, Ctrl-F opens Find, and F2 opens Files. F5 checks, builds,
and runs the currently selected root. F6 opens Project, with a selectable
run-root list above an expandable package/dependency tree. Click or Enter
toggles a package; Left closes a branch or selects its parent, and Right opens
a branch or enters its first child. F7-F11 open declaration, reference, effect,
denial, and diagnostic windows. Those remaining semantic sections are currently
read-only reports rather than source-jump navigation.
Files lists the compiler-discovered workspace sources reachable from the active
root; arrow keys browse without changing documents, and Enter opens a row.
Buffers is the separate list of already-open documents and marks dirty rows.
File > Open Workspace accepts another directory and requires Save all, Discard,
or Cancel before replacing dirty buffers. File > Open File focuses Files; its
first version selects compiler-known project sources rather than browsing the
complete filesystem.
F12 changes root and Shift-F12 changes target. Alt-F3 closes the active tool
window, Ctrl-F5 enters keyboard move/size mode, and Ctrl-F6 selects the next
window. The editor itself is an ordinary closable, zoomable desktop window;
Window > Editor or opening a source brings it back. Tile and Cascade arrange
that editor together with visible semantic windows, while leaving Files,
Buffers, and fixed dialogs in their application-selected positions. Editing,
selection, search,
save, undo/redo, mouse input, and dirty-file conflict rules are inherited from
`lib/turbo_editor_app` and `lib/turbo_editor`.

For a noninteractive integration proof:

```sh
build/draftide . --smoke
```

The complete ownership and compiler-transaction design is documented in
[`docs/implementation/turbo-draft.md`](../../docs/implementation/turbo-draft.md).
