# Turbo editor

`turbo-editor` is a useful full-screen terminal editor written in Draft with
the standalone `lib/turbo_ui` and `lib/turbo_editor` packages. It supports
multiple file buffers, exact-byte open/save, Unicode grapheme cursor movement,
mouse selection, scrolling, literal search, bounded undo/redo, dirty-buffer
quit protection, and explicit disk-change conflicts. The Files and Buffers
windows are ordinary draggable/resizable `turbo_ui` windows.
If a dirty file changes on disk, normal Save refuses to overwrite it; Escape
opens an explicit Overwrite/Discard/Cancel choice.

Build and run from the repository root on AArch64 macOS:

```sh
build/draftc build examples/turbo-editor -O2 -o /tmp/turbo-editor
/tmp/turbo-editor path/to/file.draft another-file.txt
```

Use the mouse or arrows to move, Shift plus movement to select, Ctrl-S to save,
Ctrl-F to search, Ctrl-Z/Ctrl-Y for undo/redo, F2/F3 for the Files/Buffers
windows, and Escape to quit. An unsaved buffer requires Save + quit, Discard,
or Cancel. The editor never automatically merges a disk change with unsaved
memory.

For a noninteractive native smoke:

```sh
/tmp/turbo-editor --smoke
```
