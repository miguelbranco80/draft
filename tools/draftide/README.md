# Turbo Draft

Turbo Draft is the full-screen terminal IDE written in Draft. The bootstrap
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
- **Compile**: Compiler Options, Check, Build, and Build All Programs.
- **Run**: Run and Run Configuration.
- **Window**: desktop navigation/arrangement, Workspace Files, and document or
  last-successful semantic windows.
- **Help**: the complete shortcut reference and About.

Open File, Save As, and Open Workspace use one real modal directory browser.
**Window > Workspace Files** opens an independent persistent browser rooted at
the active Workspace. Enter navigates a directory or opens a file; Up stops at
the Workspace root. A file inside that Workspace but outside the active root
package opens as an editing-only document without changing compiler scope. A
path outside the active Workspace requires an explicit **Switch and Open**
decision. Workspace replacement or closure never discards dirty documents
without Save All/Discard/Cancel.

The principal shortcuts are also listed by **Help > Keyboard Shortcuts**:

- Ctrl-N/O/W/S and F2: new, open, close document, and save.
- Ctrl-X/C/V/A, Delete, Ctrl-Z/Y: selection and editing commands.
- Ctrl-F, F3/Shift-F3, Ctrl-R, F4, Ctrl-G: find, replace, and line navigation.
- F12/Shift-F12 and Alt-Left/Alt-Right: definition, usages, and history.
- Alt-F9, F9, F5: Check, Build, and Run.
- F6 and F8: Compiler Options and Diagnostics.
- Ctrl-F6/Ctrl-Shift-F6, Alt-F3, Ctrl-F5: desktop window commands.
- Alt-F/E/C/R/W/H: open the six menus; Alt-X exits; Escape only cancels the
  current menu, dialog, or window operation.

F5 checks the exact visible edits, builds using the selected root's effective
workspace configuration, restores the primary terminal, runs the executable
with the structured Run Configuration, and waits for Enter before resuming the
IDE. Relative working-directory overrides are Workspace-relative. An
editing-only file outside the active root is rejected, and an invalid current
edit is never replaced by an older executable.

Typing runs only the production lexer for syntax color. Package semantics run
on explicit Check/Build/Run or semantic navigation. Local terminals enable
hover motion; OpenSSH sessions request click/drag/wheel input without flooding
the remote PTY with unpressed movement.

For a noninteractive compiler/UI composition proof:

```sh
build/draftide . --smoke
```

See [the implementation boundary](../../docs/implementation/turbo-draft.md)
for ownership, compiler transactions, rendering, and verification details.
