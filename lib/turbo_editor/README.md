# turbo_editor

`turbo_editor` is a reusable exact-byte document buffer and terminal-cell
editor view. It depends on `lib/turbo_ui`, but not on terminal input, rendering,
the Draft compiler, a process/event loop, or a desktop-window policy. A host may
place the same editor view in any `turbo_ui` content rectangle; Turbo Draft puts
it in an ordinary movable, resizable, zoomable window.

The package provides:

- owned path/current/saved-baseline storage and explicit load/save;
- Unicode grapheme-aware cursor movement with one-byte invalid-data recovery;
- selection, mouse positioning, literal search, and horizontal/vertical scroll;
- bounded undo/redo with a 128-operation/1 MiB history policy;
- dirty close protection and polling-based disk conflict detection;
- optional byte-offset syntax spans with a classic Turbo C/Pascal palette.

`poll_disk` reloads a changed clean buffer. If memory is dirty, it preserves the
buffer, marks `disk_conflict`, and returns `.conflict`; it never merges or
overwrites automatically. Normal `save` refuses that conflict. Its explicit
`overwrite_conflict` argument is reserved for a user-confirmed choice such as
Turbo editor's “Overwrite + quit” dialog.

The standalone [`turbo-editor`](../../examples/turbo-editor/) application is
the complete interactive example. Run focused native tests with:

```sh
build/draftc test . --root lib/turbo_editor --target aarch64-macos
```
