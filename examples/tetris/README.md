# Tetris

This is a complete, small full-screen terminal game written in Draft. It uses
the Draft core library and ordinary ANSI/VT output directly—there is no
ncurses, third-party dependency, heap allocation, or platform-specific code in
the application.

Build and run it on AArch64 macOS:

```sh
build/draftc build examples/tetris --target aarch64-macos -O2 -o /tmp/draft-tetris
/tmp/draft-tetris
```

On an AArch64 GNU/Linux host, select `--target aarch64-linux` instead. Use a
terminal of roughly 50 columns by 24 rows or larger.

## Controls

| Key | Action |
| --- | --- |
| A / left arrow | Move left |
| D / right arrow | Move right |
| S / down arrow | Soft drop |
| W / up arrow | Rotate clockwise |
| Z | Rotate counterclockwise |
| Space | Hard drop |
| P | Pause or resume |
| R | Restart after game over |
| Q / Ctrl-C | Quit cleanly |

The program uses a shuffled seven-piece bag, a ghost piece, line clearing,
score and level progression, timed gravity, color, and compact deterministic
wall kicks. The kick rules are deliberately smaller than guideline SRS, and
the first game has no hold slot, lock delay, high-score file, or terminal-size
query.

## Why the example is split

- [`game.draft`](game.draft) is deterministic simulation with no I/O,
  allocation, or clock access.
- [`input.draft`](input.draft) maps `core/terminal`'s streaming keys—including
  fragmented ANSI arrows—to game actions one byte at a time.
- [`render.draft`](render.draft) constructs a complete ANSI frame in a fixed
  stack buffer, then publishes it with one write.
- [`package.draft`](package.draft) owns raw-mode lifetime and the timed event
  loop. Every ordinary exit path leaves the alternate screen and restores the
  saved terminal mode.
- [`tetris_test.draft`](tetris_test.draft) checks gameplay, input, and rendering
  without requiring an interactive terminal.

Run the deterministic tests with:

```sh
build/draftc test examples/tetris --target aarch64-macos
```

For automation whose standard input is not a terminal, the executable provides
one narrow conformance path:

```sh
/tmp/draft-tetris --smoke
```

Raw mode turns Ctrl-C into an ordinary byte so the program can clean up. An
uncatchable kill or a terminal process failure can still interrupt cleanup; the
terminal's conventional `reset` command recovers such an exceptional session.
