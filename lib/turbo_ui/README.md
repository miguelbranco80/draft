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

| Constructor | Move | Resize | Close | Zoom | Tile/Cascade |
| --- | --- | --- | --- | --- | --- |
| `window_init` | yes | yes | yes | yes | yes |
| `tool_window_init` | yes | yes | yes | no | no |
| `dialog_window_init` | yes | no | yes | no | no |

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
Application menus can invoke the identical policies through
`request_close_active_window` and `toggle_active_window_zoom`; neither helper
silently applies application-specific close semantics.
When a title lives in mutable/owned byte storage, pass an empty static title to
`begin_window` and immediately call `window_title_bytes` before painting the
content. This keeps Draft 1's string immutability boundary explicit while still
supporting file paths and generated document labels without allocation.

Chrome geometry follows `core/unicode`'s pinned terminal-width policy. The
text-default zoom arrow occupies one cell immediately before the upper-right
border corner; an emoji-presentation selector is deliberately absent. A theme
selects shadowed or flat decoration for every button, window, and popup in one
frame. Enabled shadows use an explicit one-column shade glyph, so exposing an
a previous-frame shadow changes glyph content as well as background style during
differential repainting.

`turbo_theme` authors its 16 colors in the IBM PC numbering used by Turbo-era
interfaces and translates them to ANSI palette indices at style construction.
In particular, logical blue and red must not be passed directly to ANSI, whose
indices for those two colors are reversed.

`tile_windows` and `cascade_windows` provide deterministic whole-desktop
arrangement for visible windows whose independent `tileable` flag is set. Both
deliberately leave fixed dialogs, auxiliary tool panes, and hidden windows
untouched. Manual move/resize capability does not imply arrangement
participation; an application may change `flags.tileable` explicitly.

Hit testing follows the same frame-and-shadow footprint that painting exposes.
A front shadow therefore occludes rear controls instead of visually covering a
resize handle while allowing that handle to receive the click; a flat theme's
hit region ends at the frame. Window chrome capture normally ends on release; a
later no-button move or new primary press also cancels stale capture when a
terminal or multiplexer lost the release.

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
- compound lists and preorder trees with custom immediate-mode rows, read-only
  text viewports, proportional scrollbars, and draggable splitters;
- `status_bar`; and
- `dialog_begin`, which applies dialog content styling to a normal window
  transaction.

Button bounds describe their complete caller-reserved footprint. A footprint
at least two rows high uses its final row and column for a real shade-glyph
shadow when the active theme enables shadows; only the face is hit-tested, and
a held face moves down/right over the shadow. Flat themes use the complete face
without movement. Use `button_width(label)` for a shadow-capable footprint and
`compact_button_width(label)` for an exact one-row face; byte length is not
terminal width. `.default_action` and `.cancel_action` roles accept an otherwise-
unhandled Enter or Escape in the current scope. An optional printable ASCII
access key underlines the same label character and activates it with Alt. The
default action has distinct angle-bracket chrome, keyboard focus is visibly
selected, and labels are centered by terminal columns. Text boxes preserve
valid UTF-8 while accepting byte-stream terminal input, move/delete by
grapheme, scroll horizontally, and report `changed`, `submitted`, and `full`.
`Text_Box_Kind.text` accepts ordinary UTF-8 bytes; `unsigned_integer` accepts
only ASCII decimal digits for fields such as one-based line numbers. The caller
still owns range and semantic validation when the field is submitted.
`status_bar_bytes` complements the immutable-string form for caller-formatted
counts without allocation; its slice is borrowed only for the current frame.
The two `status_bar_sections` forms keep transient feedback on the left and a
session identity right-aligned only when both complete sections and their gap
fit, dropping the identity first on a narrow terminal.
Combo boxes keep their item slice borrowed and separate the owner-window field
pass from the popup pass so choices remain above every window.

`List_State` owns cursor, viewport offset, and the first row of an armed
double-click pair. `list_view` handles keyboard, mouse, explicit single- or
double-click activation, and its integrated scrollbar, then returns the visible
model range. The caller supplies semantic click counts because terminal
protocols carry no double-click timing. Simple `[]string` clients use `list`;
richer clients call
`list_row_begin` and paint markers, byte labels, indentation or columns into the
returned row. Wheel and scrollbar movement change only the viewport; keyboard
selection is what keeps the cursor visible. `Text_View_State` and
`text_view_bytes` provide the same scrolling behavior for allocation-free
read-only line views. Vertical scrollbars have arrow cells, a proportional
thumb, page regions, grab-relative dragging, and held-arrow capture. Lists may
keep an unneeded reserved scrollbar column visually empty for compact panes;
text and tree views accept the same shared `Scrollbar_Visibility` policy.
The UI owns no clock: an application schedules deterministic `repeat_event()`
pulses while `scrollbar_repeat_active(&ui)` is true.

`tree_view` applies that same list behavior to a caller-owned flat preorder
table of `Tree_Node` records. Each node retains its own expansion bit; the
caller also supplies one scratch index array for the frame, so hiding
descendants allocates nothing and does not create a retained widget tree. Right
expands or enters a branch, Left closes it or selects its parent, and Enter
toggles it.
`tree_row_begin` paints indentation and a classic `+`/`-` disclosure marker
before returning an application-customizable content rectangle.

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
if turbo_ui.menu_action(
    &ui, &menus, &file, 102, "Save", "Ctrl-S",
    access_key = cast[u8]('s'),
) {
    save()
}
turbo_ui.menu_separator(&ui, &file)
turbo_ui.menu_check(
    &ui, &menus, &file, 103, "Diagnostics", &show_diagnostics,
    access_key = cast[u8]('d'),
)
turbo_ui.end_menu(&ui, &menus, file)
```

Drop-down rows support actions, checked values, radio choices, disabled items,
separators, underlined access keys, and right-aligned shortcut labels. The
access key must be a printable ASCII letter present in its label. Alt+an
underlined title letter opens that menu; pressing an underlined row letter acts
while it is open. Left/Right switch titles; Up/Down/Home/End move over enabled
rows; Enter acts; Escape or an outside press dismisses. Shortcut text such as
`Ctrl-S` describes an application command and is deliberately not registered
by the immediate-mode menu: the application handles that key at global scope
and invokes the same direct operation as the menu branch. With a mouse, either
click a title and then click one item, or hold on the title, drag through the
drop-down, and release on the intended item. Leaving the actionable rows and
menu content preserves the last explicit highlight; re-entering changes it
only when the pointer reaches another enabled row. Inert separators likewise
preserve the previous actionable row rather than selecting a different
command. Menus and combo lists share the single explicit popup layer, so
transient input capture has one inspectable rule.

Some immediate-mode decisions, such as keyboard focus traversal and a menu
highlight that skips a separator, become known only after the affected row was
painted. After `end_frame`, call `needs_repaint`; when true, traverse once more
with `no_event()` before presenting. This reconciles visible cells without
reapplying the input event.

The independent [`turbo-ui-gallery`](../../examples/turbo-ui-gallery/) is the
complete interactive reference. Tests use in-memory surfaces and synthetic
events without opening a terminal:

```sh
build/draftc test lib/turbo_ui --target aarch64-macos
```
