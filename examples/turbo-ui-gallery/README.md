# Turbo UI gallery

This interactive gallery proves that `lib/turbo_ui` is a standalone reusable
Draft package rather than editor-private machinery. It demonstrates the classic
blue Turbo palette, drop-down menus with checked/radio/disabled rows and
separators, buttons, checkboxes, radio buttons, UTF-8 text input, combo boxes,
lists, scrollbars, splitters, true modal dialogs, and overlapping windows with
close, zoom, visible resize, mouse input, keyboard focus, terminal resize, and
differential cell rendering.

Build and run on a native AArch64 macOS host from the repository root:

```sh
build/draftc build . --root examples/turbo-ui-gallery -o /tmp/turbo-ui-gallery
/tmp/turbo-ui-gallery
```

Press Escape to quit when no menu or dialog owns it. Use Alt-F/Alt-O for menus,
Alt-F3 to request closing the active window, or Ctrl-F5 followed by arrows to
move it (Shift-arrows resize; Enter accepts; Escape restores). Drag a title or
the visible lower-right handle with the mouse, and use Tab/Shift-Tab plus Enter
to navigate controls.

The deterministic noninteractive path is suitable for native smoke testing:

```sh
/tmp/turbo-ui-gallery --smoke
```
