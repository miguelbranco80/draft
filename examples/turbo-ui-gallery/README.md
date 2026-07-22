# Turbo UI gallery

This interactive gallery proves that `lib/turbo_ui` is a standalone reusable
Draft package rather than editor-private machinery. It demonstrates the classic
blue Turbo palette, menus, buttons, checkboxes, lists, scrollbars, splitters,
dialogs, overlapping draggable/resizable windows, mouse input, keyboard focus,
terminal resize, and differential cell rendering.

Build and run on a native AArch64 macOS host from the repository root:

```sh
build/draftc build . --root examples/turbo-ui-gallery -o /tmp/turbo-ui-gallery
/tmp/turbo-ui-gallery
```

Press Escape to quit. Drag a title to move a window, drag its lower-right
corner to resize it, click controls with the mouse, and use Tab/Shift-Tab plus
Enter to navigate by keyboard.

The deterministic noninteractive path is suitable for native smoke testing:

```sh
/tmp/turbo-ui-gallery --smoke
```
