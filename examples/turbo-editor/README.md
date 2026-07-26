# Turbo editor

`turbo-editor` is a useful full-screen terminal editor written in Draft with
the standalone `lib/turbo_ui` and `lib/turbo_editor` packages. It supports
multiple file buffers, exact-byte open/save, Unicode grapheme cursor movement,
mouse selection, scrolling, literal search and replacement, bounded undo/redo,
dirty-buffer quit protection, and explicit disk-change conflicts. Open
Documents is an ordinary draggable `turbo_ui` tool window.
If a dirty file changes on disk, normal Save refuses to overwrite it; Escape
cancels the local operation and Alt-X opens the explicit
Save/Discard/Cancel quit choice.

Build and run from the repository root on AArch64 macOS:

```sh
build/draftc run examples/turbo-editor -O2 -- \
  path/to/file.draft another-file.txt
```

Use the mouse or arrows to move, Shift plus movement to select, Ctrl-S/F2 to
save, Ctrl-F/F3/Shift-F3 to search, Ctrl-R/F4 to replace, Ctrl-G to go to a
line, Ctrl-Z/Ctrl-Y for undo/redo, Alt-0 for Open Documents, F1 for the complete
shortcut list, and Alt-X to quit. An unsaved buffer requires Save + exit,
Discard, or Cancel. The editor never automatically merges a disk change with
unsaved memory.

For a noninteractive native smoke:

```sh
build/draftc run examples/turbo-editor -O2 -- --smoke
```
