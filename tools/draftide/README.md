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

The first path is the workspace, not the current package. Without `--root`,
Turbo Draft reads `<workspace>/draft.project`:

```text
draft-project-v1
root = examples/turbo-editor
source = package.draft
```

`root` selects one package relative to the workspace; `source` is optional and
defaults to `package.draft`. A package-level `main` makes the root runnable.
`--root` and `--source` are explicit command-line overrides. Imports such as
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

The top row contains real File, Edit, Project, and Window drop-down menus; use
Alt-F/Alt-E/Alt-P/Alt-W as well as the mouse. F5 checks, builds, and runs the
currently selected root. F6 opens Project, with
a selectable run-root list above the package/dependency graph. F7-F11 open
declaration, reference, effect, denial, and diagnostic windows. Those semantic
sections are currently read-only reports rather than source-jump navigation.
F12 changes root and Shift-F12 changes target. Alt-F3 closes the active tool
window, Ctrl-F5 enters keyboard move/size mode, and Ctrl-F6 selects the next
window. Editing, selection, search,
save, undo/redo, mouse input, and dirty-file conflict rules are inherited from
`lib/turbo_editor_app` and `lib/turbo_editor`.

For a noninteractive integration proof:

```sh
build/draftide . --smoke
```

The complete ownership and compiler-transaction design is documented in
[`docs/implementation/turbo-draft.md`](../../docs/implementation/turbo-draft.md).
