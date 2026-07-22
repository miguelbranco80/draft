# turbo_ui

`turbo_ui` is a reusable immediate-mode terminal UI library for Draft. It
paints into a caller-owned `core/tui.Surface` and consumes one caller-supplied
semantic event per frame. It does not read the terminal, present frames, own an
event loop, know about the compiler, or retain application objects.

The application owns persistent values such as window bounds and z-order,
checkbox state, list selection, split positions, documents, and dialogs. The
library retains only hot/active/focused identity and direct-manipulation capture
in an explicit `turbo_ui.State`.

```draft
theme := turbo_ui.turbo_theme()
turbo_ui.begin_frame(&ui, tui.surface(&renderer), event, theme)

if turbo_ui.button(
    &ui,
    100,
    turbo_ui.Rect{column = 2, row = 2, width = 12, height = 1},
    "Build",
) {
    build_project()
}

turbo_ui.end_frame(&ui)
```

Use `begin_window_frame` when supplying overlapping `Window_State` values, draw
them in the index order returned by `window_order`, and pair each visible
`begin_window` with `end_window`. The caller adapts `core/terminal` observations
to `turbo_ui.Event`; the gallery is the complete interactive reference.

Tests use in-memory surfaces and synthetic events:

```sh
build/draftc test . --root lib/turbo_ui --target aarch64-macos
```
