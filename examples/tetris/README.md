# Tetris

This is a complete, small full-screen terminal game written in Draft. It uses
`core/terminal` for portable raw input and screen lifetime, and `core/tui` for
owned cell surfaces and differential ANSI/VT output. There is no ncurses,
third-party dependency, background thread, or platform-specific application
code.

Build and run it on a matching native target, for example AArch64 macOS:

```sh
build/draftc run examples/tetris --target aarch64-macos -O2
```

On GNU/Linux, select `--target aarch64-linux` or `--target x86_64-linux` for
the host architecture. On x86-64 Windows, select `--target x86_64-windows` and
use a Windows 10-or-newer console with virtual-terminal processing. Use a
terminal of 50 columns by 24 rows or larger. The game follows terminal resizes;
a smaller terminal displays a bounded size notice instead of corrupting the
screen.

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
score and level progression, timed gravity, colored pieces over an explicit
black cell background, and compact deterministic wall kicks. The kick rules
are deliberately smaller than guideline SRS, and the first game has no hold
slot, lock delay, or high-score file. Interactive play normally seeds one
`core/random.Generator` from the hosted OS-backed provider and uses an explicit
clock/process fallback when that capability is unavailable. Gameplay consumes
only the deterministic local stream, and tests supply fixed seeds.

## Why the example is split

- [`game.draft`](game.draft) is deterministic simulation with no I/O,
  allocation, or clock access.
- [`input.draft`](input.draft) maps `core/terminal`'s streaming keys—including
  fragmented ANSI arrows—to game actions one byte at a time.
- [`render.draft`](render.draft) paints a complete desired Unicode-capable cell surface;
  `core/tui` retains the prior surface and publishes only changed cell runs.
- [`package.draft`](package.draft) owns raw-mode lifetime and the timed event
  loop, queries terminal size, and resizes the bounded TUI viewport. Every
  ordinary exit path leaves the alternate screen and restores the saved mode.
- [`tetris_test.draft`](tetris_test.draft) checks gameplay, input, and rendering
  without requiring an interactive terminal.

Run the deterministic tests with:

```sh
build/draftc test examples/tetris --target aarch64-macos
```

For automation whose standard input is not a terminal, the executable provides
one narrow conformance path:

```sh
build/draftc run examples/tetris --target aarch64-macos -O2 -- --smoke
```

Raw mode turns Ctrl-C into an ordinary byte so the program can clean up. An
uncatchable kill or a terminal process failure can still interrupt cleanup; the
terminal's conventional `reset` command recovers such an exceptional session.
