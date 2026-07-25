# Turbo Draft

Turbo Draft is the full-screen terminal IDE written in Draft. The only C++
component is the bootstrap compiler service behind an opaque C ABI; ordinary
files, documents, menus, terminal/UI state, compiler commands, and `main` are
Draft code.

Build and open a Workspace or any package inside it:

```sh
cmake --build build --target draftide --parallel
build/draftide examples/raylib-asteroids
```

DraftIDE searches upward for the nearest `draft.workspace`. That file may set a
default Program and Build/Run settings, but it does not enumerate source files.
Without a marker, the opened directory is a standalone Workspace. The compiler
discovers packages and executable Programs normally; `--source <file>` is only
an optional initial-file override.

The UI uses four deliberately separate concepts:

- **Workspace**: filesystem/import boundary and optional `draft.workspace`.
- **Program**: active root package; it is runnable when it contains `main`.
- **Document**: one open file or unsaved `Untitled N` buffer.
- **Program and Run Settings**: effective target, optimization, artifact,
  providers, arguments, environment, and working directory used by Build/F5.

DraftIDE starts with one editor window whose title is the active file's
Workspace-relative path. File contains ordinary document actions: New, Open,
Files in Active Program, Open Files, Save, Save As, Save All, and Exit. Open and
Save As accept an absolute path or one relative to the Workspace. A file outside
the active Program remains editing-only. Workspace contains Switch Workspace,
Program and Run Settings, and Packages and Imports. Compiler inspection windows
live under Window rather than obscuring startup.

The main shortcuts are visible in Help > Keyboard Shortcuts:

- Ctrl-N/O/S and Ctrl-X/C/V/A: document and selection commands.
- Ctrl-F, F3/Shift-F3, Ctrl-H, Ctrl-R, Ctrl-G: find/replace/navigation.
- Alt-F9, F9, F5: Check, Build, Build and Run.
- F2, Alt-0, F6, F8: Program Files, Open Files, Program settings, Diagnostics.
- F12/Shift-F12 and Alt-Left/Alt-Right: semantic definition/usages/history.
- Ctrl-F6/Ctrl-Shift-F6, Alt-F3, Ctrl-F5: desktop window commands.
- Alt-X or Ctrl-Q: Exit. Escape only cancels the current menu/dialog/operation.

Alt-F/E/S/R/K/W/H opens File, Edit, Search, Run, Workspace, Window, or Help;
an open menu accepts its underlined command letter directly. Conventional
terminals cannot distinguish Ctrl-Shift-S from Ctrl-S, so Save As and Replace
All use their visible menu access paths instead of advertising fake shortcuts.

F5 checks the exact visible edits, builds with the active Program's effective
Workspace configuration, restores the primary terminal, runs the executable
with its configured arguments/environment/working directory, and waits for
Enter before resuming the IDE. An invalid current edit is never replaced by an
older retained executable.

Typing runs only the production lexer for syntax color; package semantics run
on explicit Check/Build/Run or semantic navigation. Local terminals enable
hover motion. When an OpenSSH environment is detected, DraftIDE requests only
click/drag/wheel reports so pointer movement does not flood the remote PTY.

For a noninteractive compiler/UI composition proof:

```sh
build/draftide . --smoke
```

See [the implementation boundary](../../docs/implementation/turbo-draft.md)
for ownership, compiler transaction, rendering, and verification details.
