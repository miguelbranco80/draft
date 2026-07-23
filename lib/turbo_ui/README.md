# turbo_ui

`turbo_ui` is a reusable immediate-mode terminal desktop library written in
Draft. It paints into a caller-owned `core/tui.Surface` and consumes one
caller-supplied semantic event per frame. It does not read the terminal,
present frames, allocate controls, retain labels/items, own an event loop, or
know about an editor or compiler.

The application owns all durable values: window records, menu state, text
storage, selections, checkboxes, documents, and whether a requested close is
allowed. `turbo_ui.State` retains only interaction identity, per-window focus,
one modal scope, one popup scope, and direct-manipulation capture.

## Desktop and window contract

Use `begin_window_frame` for a desktop with overlapping windows. Its `desktop`
rectangle normally excludes the menu and status rows. Supply one visible
`modal_window` ID when a dialog must block the rest of the desktop.

Three constructors select ordinary capability sets:

| Constructor | Move | Resize | Close | Zoom |
| --- | --- | --- | --- | --- |
| `window_init` | yes | yes | yes | yes |
| `tool_window_init` | yes | yes | yes | no |
| `dialog_window_init` | yes | no | yes | no |

The flags remain explicit in `Window_State` and may be adjusted by an
application. `show_modal_window` records the owning active window before a
dialog is activated. A dialog becomes modal only when its ID is also passed to
`begin_window_frame`; decoration and input policy are deliberately separate.

Draw windows back-to-front using `window_order`. `begin_window` returns a
content rectangle and a `close_requested` result. Closing never silently hides
or destroys a window: the application can reject it, ask about dirty data, or
call `hide_window`. The frame includes a close box, reversible zoom box, and
visible lower-right resize handle. Alt-F3 requests close; Ctrl-F5 enters
keyboard move/size mode, arrows move, Shift-arrows resize, Enter accepts, and
Escape restores the entry rectangle.

`tile_windows` and `cascade_windows` provide deterministic whole-desktop
arrangement for visible movable/resizable windows. Both deliberately leave
fixed dialogs and hidden windows untouched; the application still controls
participation through each window's capability flags.

```draft
desktop := turbo_ui.Rect{row = 1, width = surface.columns}
if surface.rows > 2 {
    desktop.height = surface.rows - 2
}
modal := turbo_ui.No_Widget
if confirm.visible {
    modal = confirm.id
}
turbo_ui.begin_window_frame(
    &ui, &surface, event, theme, windows[:], desktop, modal,
)

order: [8]usize
count := turbo_ui.window_order(windows[:], order[:])
for position: usize = 0; position < count; position += 1 {
    window := &windows[order[position]]
    frame := turbo_ui.begin_window(&ui, window, "Files")
    if frame.visible {
        paint_files(frame.content)
    }
    turbo_ui.end_window(&ui, frame)
    if frame.close_requested {
        request_file_window_close()
    }
}
```

## Controls

Controls are ordinary procedure calls over caller-owned values:

- `label`, `button`, `checkbox`, and `radio_button`;
- `text_box`, a bounded single-line UTF-8 editor over caller storage;
- `combo_box` plus its topmost `combo_box_popup` pass;
- `list`, `scrollbar_vertical`, and draggable `splitter`;
- `status_bar`; and
- `dialog_begin`, which applies dialog content styling to a normal window
  transaction.

Buttons may be `.default_action` or `.cancel_action`; an otherwise-unhandled
Enter or Escape activates that role in the current scope. Text boxes preserve
valid UTF-8 while accepting byte-stream terminal input, move/delete by
grapheme, scroll horizontally, and report `changed`, `submitted`, and `full`.
Combo boxes keep their item slice borrowed and separate the owner-window field
pass from the popup pass so choices remain above every window.

Every live control and window has a stable, nonzero `Widget_Id`. IDs are the
only link between frames; no widget objects, callback tree, or application
pointers are retained. Tab/Shift-Tab traverse focusable controls in direct call
order within the active window, dialog, or popup.
Custom controls should call `focus_area` for pointer capture and gate direct
keyboard handling with `has_focus`; `focused_id` alone intentionally also
remembers focus belonging to inactive windows.

## Menus and popups

Menus use two direct passes. Titles are painted before windows; the matching
drop-downs are painted after windows. This makes the overlay relationship
explicit and prevents a drop-down from being clipped by its owner.

```draft
turbo_ui.menu_bar(&ui, &menus, turbo_ui.Rect{width = surface.columns, height = 1})
turbo_ui.menu_title(
    &ui, &menus, 100, 101,
    turbo_ui.Rect{column = 1, width = 8, height = 1},
    "File", cast[u8]('f'),
    turbo_ui.Rect{column = 1, row = 1, width = 26, height = 6},
)
turbo_ui.menu_bar_end(&ui, &menus)

// Draw ordinary windows here.

file := turbo_ui.begin_menu(
    &ui, &menus, 100, 101,
    turbo_ui.Rect{column = 1, row = 1, width = 26, height = 6},
)
if turbo_ui.menu_action(&ui, &menus, &file, 102, "Save", "Ctrl-S") {
    save()
}
turbo_ui.menu_separator(&ui, &file)
turbo_ui.menu_check(&ui, &menus, &file, 103, "Diagnostics", &show_diagnostics)
turbo_ui.end_menu(&ui, &menus, file)
```

Drop-down rows support actions, checked values, radio choices, disabled items,
separators, and right-aligned shortcut labels. Alt+access-key opens a title;
Left/Right switch titles; Up/Down/Home/End move over enabled rows; Enter acts;
Escape or an outside press dismisses. With a mouse, either click a title and
then click one item, or hold on the title, drag through the drop-down, and
release on the intended item. Leaving every item clears the transient
highlight; re-entering selects only the row actually under the pointer. Menus
and combo lists share the single explicit popup layer, so transient input
capture has one inspectable rule.

Some immediate-mode decisions, such as keyboard focus traversal and a menu
highlight that skips a separator, become known only after the affected row was
painted. After `end_frame`, call `needs_repaint`; when true, traverse once more
with `no_event()` before presenting. This reconciles visible cells without
reapplying the input event.

The independent [`turbo-ui-gallery`](../../examples/turbo-ui-gallery/) is the
complete interactive reference. Tests use in-memory surfaces and synthetic
events without opening a terminal:

```sh
build/draftc test . --root lib/turbo_ui --target aarch64-macos
```
