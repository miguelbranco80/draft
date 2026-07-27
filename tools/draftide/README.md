# DraftIDE

DraftIDE is the full-screen terminal IDE written in Draft. The bootstrap
compiler service is the only C++ component; ordinary files, editor state,
menus, dialogs, terminal/UI policy, compiler commands, and `main` are Draft.

Build it and open a workspace directory or any package inside one:

```sh
cmake --build build --target draftide --parallel
build/draftide examples/raylib-asteroids
```

DraftIDE uses the same model as `draftc`:

- **Workspace** is the filesystem/import boundary established by the nearest
  optional `draft.workspace`, or by the opened directory when no marker exists.
- **Root package** is the exact package operated on by Check and Build. A root
  containing `main` is a runnable Program.
- **Document** is one open file or unnamed buffer. A file may be edited without
  belonging to the active root package.
- **Compiler Options** selects the root package and target and shows the
  effective build configuration.
- **Run Configuration** edits exact argument rows, `NAME=value` environment
  rows, and the working directory separately from compiler policy.

The six menus have conventional, non-overlapping responsibilities:

- **File**: New/Open File/Open Workspace, close document/workspace, Save/Save
  As/Save All, and Exit.
- **Edit**: undo/redo, clipboard commands, find/replace, line and semantic
  navigation, and navigation history.
- **Compile**: Compiler Options, provider-free Check/Build/Build All Programs,
  unsaved `//?`/`//!` agent-comment rewriting, explicit Resolve Synthesis, and
  explicit Judge Claims.
- **Run**: Run and Run Configuration.
- **Window**: desktop navigation/arrangement, Workspace Files, Diagnostics,
  persistent Build Output, and document or last-successful semantic windows.
- **Help**: the complete shortcut reference and About.

Open File, Save As, and Open Workspace use one real modal directory browser.
**Window > Workspace Files** opens an independent persistent browser rooted at
the active Workspace. Enter or double-click navigates a directory or opens a
file; one click selects, and Up stops at the Workspace root. A file inside that
Workspace but outside the active root package opens as an editing-only document
without changing compiler scope. A path outside the active Workspace requires
an explicit **Switch and Open** decision. Workspace replacement or closure
never discards dirty documents without Save All/Discard/Cancel.

All DraftIDE chrome is flat: windows, menu popups, and compact one-row buttons
do not spend terminal cells on black drop shadows. Dialog actions align at the
right edge, file-browser toolbars align at the left, and collection scrollbars
appear only when the current content exceeds the visible rows.

The principal shortcuts are also listed by **Help > Keyboard Shortcuts**:

- Ctrl-N/O/W/S and F2: new, open, close document, and save.
- Ctrl-X/C/V/A, Delete, Ctrl-Z/Y: selection and editing commands.
- Ctrl-F, F3/Shift-F3, Ctrl-R, F4, Ctrl-G: find, replace, and line navigation.
- F12/Shift-F12 and Alt-Left/Alt-Right: definition, usages, and history.
- Ctrl-E: rewrite the active file from the selected `//?` or `//!` block.
- Alt-F9, F9, F5: Check, Build, and Run.
- F6 and F8: Compiler Options and Diagnostics.
- Ctrl-F6/Ctrl-Shift-F6, Alt-F3, Ctrl-F5: desktop window commands.
- Alt-F/E/C/R/W/H: open the six menus; Alt-X exits; Escape only cancels the
  current menu, dialog, or window operation.

The Find and Replace dialogs remain open across their repeatable actions, so
Next/Previous or replacement work can continue until Cancel, Escape, or the
window close control is used. Standalone F3, Shift-F3, and F4 perform the same
document operations without opening a dialog.

F5 checks the exact visible edits, builds using the selected root's effective
workspace configuration, and only then restores the primary terminal and runs
the executable
with the structured Run Configuration, and waits for Enter before resuming the
IDE. Relative working-directory overrides are Workspace-relative. An
editing-only file outside the active root is rejected, and an invalid current
edit is never replaced by an older executable.

A successful Build does not open a popup; the status line reports its duration
and artifact path. Every Build replaces the persistent Build Output transcript,
whether or not that window is visible. Compiler failure shows and raises
Diagnostics, whose selectable rows open the exact source range. DraftIDE does
not hide Diagnostics after a later success. An F5 build failure stays inside
the IDE; a launched program's nonzero exit or signal remains in Program Output
and the Run Result rather than becoming a compiler diagnostic.

**Compile > Resolve Synthesis** is the only IDE command allowed to ask the
default Codex provider for missing or stale `...` expansions. **Compile > Judge
Claims** separately evaluates `judge` sites and records evidence. Both save
dirty files in the active root's reachable graph first (including a package
shared by multiple roots), show progress before their synchronous provider
work, update Build Output, and raise Diagnostics for compiler/provider errors.
A completed negative judgment with no compiler error raises Build
Output for the verdict. Build and F5 never invoke either command: an unresolved
site fails normally and asks for Resolve. A successful Resolve still requires a
separate Build or F5.

**Compile > Expand Agent Comment** (Ctrl-E) is an intentionally ephemeral
active-file prototype. Put the cursor on any line of a maximal contiguous
same-marker group, such as

```draft
//? create a parser for the following command syntax
//? and return structured errors rather than asserting
```

The text after those markers is concatenated and sent with the complete active
file, its exact selected range and marker kind, active logical path, and a
private read-only tree of reachable workspace-owned Draft sources. Unsaved
active/dirty buffers replace disk bytes. Other scattered annotations remain in
the file context, but the selected group is the immediate request. Codex may
rewrite anywhere in the active file, including imports and package declarations,
and returns exactly one complete replacement file. It cannot edit or create
other files. Its instructions ask it to preserve unrelated behavior and to
represent a required cross-file seam honestly with a precise TODO, minimal
compiling scaffold/no-op, or retained annotation rather than inventing an API.

`//?` records persistent intent and is meant to remain in the returned file.
`//!` is a transient work annotation which Codex may remove, retain, or turn
into an ordinary comment when appropriate. DraftIDE does not enforce either
policy. It privately checks the first complete candidate with the provider-free
compiler. Compiler errors cause exactly one advisory Codex reconsideration
with the candidate and compact workspace-relative diagnostics; errors may have
already existed, and the second result is not checked or rejected. DraftIDE
applies the resulting bytes verbatim without saving. The status bar animates
while one worker owns the compiler callback; input and disk polling pause until
it rejoins. There is no proposal state, accept/reject command, other-file edit,
or final validity gate. The complete replacement is one ordinary history
transaction, so Ctrl-Z restores the exact previous file. The
operation creates no file or resolution pin; Check remains the explicit way to
validate the new visible bytes.

Typing runs only the production lexer for syntax color. Package semantics run
on explicit Check/Build/Run or semantic navigation. Local terminals enable
hover motion; OpenSSH sessions request click/drag/wheel input without flooding
the remote PTY with unpressed movement.

For a noninteractive compiler/UI composition proof:

```sh
build/draftide . --smoke
```

See [the implementation boundary](../../docs/implementation/draftide.md)
for ownership, compiler transactions, rendering, and verification details.
