# turbo_editor

`turbo_editor` is a reusable exact-byte document buffer and terminal-cell
editor view. It depends on `lib/turbo_ui`, but not on terminal input, rendering,
the Draft compiler, a process/event loop, or a desktop-window policy. A host may
place the same editor view in any `turbo_ui` content rectangle; Turbo Draft puts
it in an ordinary movable, resizable, zoomable window.

The package provides:

- owned path/current/saved-baseline storage and explicit load/save;
- an incrementally maintained sorted line-start table, so viewport and cursor
  lookup do not rescan the complete document on every frame;
- Unicode grapheme-aware cursor movement with one-byte invalid-data recovery,
  Draft-identifier word movement, file Home/End, viewport-sized paging, and a
  preferred visual column retained across short lines;
- selection, mouse positioning, wrapped forward/reverse literal search,
  one-based line navigation, and independent horizontal/vertical scroll (wheel
  movement never snaps back to the cursor);
- single-match and whole-document replacement. Replace All scans only original
  bytes, reports its exact count, and never re-matches replacement text;
- bounded undo/redo with a 128-record/1 MiB history policy; each replacement is
  a joined record pair and therefore remains one atomic user undo/redo step;
- dirty close protection and polling-based disk conflict detection;
- optional byte-offset syntax spans with a classic Turbo C/Pascal palette;
  painting advances one monotonic span cursor across the visible viewport.

`poll_disk` reloads a changed clean buffer. If memory is dirty, it preserves the
buffer, marks `disk_conflict`, and returns `.conflict`; it never merges or
overwrites automatically. Normal `save` refuses that conflict. Its explicit
`overwrite_conflict` argument is reserved for a user-confirmed choice such as
Turbo editor's “Overwrite + quit” dialog.

The standalone [`turbo-editor`](../../examples/turbo-editor/) application is
the complete interactive example. Run focused native tests with:

```sh
build/draftc test lib/turbo_editor --target aarch64-macos
```
