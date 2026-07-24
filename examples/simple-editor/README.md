# Simple editor

This is a deliberately small, line-oriented application for exercising Draft
as an application language. It is useful enough to edit and save a text file,
but it is not intended to become the future full-screen Draft editor.

Build and run it on a native AArch64 macOS host with:

```sh
build/draftc run examples/simple-editor -- path/to/file.txt
```

An unavailable path starts with an empty buffer; `w` creates or replaces it.
The command set is:

| Command | Operation |
| --- | --- |
| `NUMBER` | Select and print a one-based line. |
| `p` | Print the current line. |
| `l` | List every line with its one-based number. |
| `n` or an empty command | Select and print the next line. |
| `i` | Insert input before the current line. |
| `a` | Append input after the current line. |
| `d` | Delete the current line. |
| `w` | Write the buffer to the original path. |
| `q` | Quit only when there are no unsaved changes. |
| `q!` | Quit and discard unsaved changes. |
| `h` | Print command help. |

During `i` or `a`, enter `.` alone to finish and `..` to insert a literal dot
line.

The editor stores file contents as arbitrary bytes split at LF. Saving writes
one LF after every active line, so it does not preserve a missing final newline.
It intentionally has no raw terminal mode, screen painting, undo, search,
atomic replacement, metadata preservation, Unicode decoding, or grapheme-aware
editing.

Run its focused representation, parser, and file round-trip tests with:

```sh
build/draftc test examples/simple-editor --target aarch64-macos
```
