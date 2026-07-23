# Turbo UI and Turbo Draft

Build the reusable Turbo-style terminal UI first, then use it to construct
Turbo Draft. The UI is an ordinary Draft library of its own—not `core`, not an
editor-private module, and not coupled to the compiler.

Suggested source layout:

```text
lib/turbo_ui/                 reusable library
examples/turbo-ui-gallery/    visual and interactive proof
```

The IDE remains one terminal process with an embedded compiler and ordinary
`.draft` files. The executable and all editor/project/UI policy are written in
Draft. A small shared C++ library retains the bootstrap compiler behind an
opaque C ABI; it can disappear when the compiler is self-hosted without
rewriting the IDE. Unsaved buffers are in-memory overlays only. There is no
candidate or revision system, and IDE state does not belong under `.draft/`.

## 1. Build the standalone Turbo UI library

- [x] Define cell geometry, rectangles, intersection and clipping, explicit
  widget IDs, a Turbo palette, borders, titles, and shadows.
- [x] Define caller-supplied key, text, mouse, and resize events. Keep terminal
  reading, rendering, timing, and the event loop outside the library.
- [x] Add immediate-mode frame state: hot, active, and keyboard-focused IDs.
  Use ordinary procedures over a caller-owned `tui.Surface`; retain no widget
  objects, callback tree, application pointers, or hidden event loop.
- [x] Add rows, columns, padding and splitters; buttons with default/cancel
  roles; labels, checkboxes, radio buttons, UTF-8 text boxes, selection combo
  boxes, lists, scrollbars, dialogs, and status bars.
- [x] Add real menu overlays with Alt access, arrow navigation, actions,
  checked/radio/disabled rows, separators, and shortcut labels.
- [x] Add persistent active-window/focus routing, normal/tool/dialog capability
  sets, real modal capture, a separate popup layer, close requests, reversible
  zoom, visible mouse resize, and keyboard move/size.
- [x] Test geometry, clipping, focus traversal, values, text editing, menus,
  popups, modality, close/zoom/resize, and cell output using synthetic events
  without opening a terminal.

## 2. Complete terminal input and the gallery

- [x] Add scoped mouse-reporting lifetime to `core/terminal`, including
  position, button press/release, drag and wheel decoding, with guaranteed
  restoration on macOS, Linux and Windows.
- [x] Write the small adapter from `core/terminal` keys, mouse observations and
  resize notifications to `turbo_ui` events.
- [x] Build `turbo-ui-gallery` with the blue palette, menus, dialogs, lists,
  checkboxes, splitters and overlapping windows. It is the interactive API
  example and the proof that the library is independent of Turbo Draft.
- [x] Check the library and gallery on all four targets and run native smoke
  tests on matching hosts.

## 3. Build a useful editor with the library

- [x] Add ordinary open/edit/save buffers, Unicode-aware cursor movement,
  scrolling, selection, search, bounded undo/redo and dirty-file protection.
- [x] Start with one editor plus status/build area, then add file and buffer
  windows using the same public library API demonstrated by the gallery.
- [x] Watch disk changes. Reload only when no unsaved buffer conflicts;
  otherwise show an explicit conflict and never attempt automatic merging.

## 4. Embed the compiler directly

- [x] Build `draftide` as a hosted Draft executable. Keep only an opaque,
  fixed-layout C service over the C++ bootstrap compiler; Draft owns project
  selection, buffers, terminal/UI state, Build/Run policy, and the service
  handle lifetime. The service owns compiler products and never calls Draft.
- [x] Open an ordinary workspace directory. Discover executable roots without
  requiring a project file; use an optional versioned `draft.project` or
  `--root` only to select the initial root/source. Keep open buffers in Draft
  and the last successful `SourceManager`/`CompileWorkspaceResult` pair in the
  compiler service.
- [x] Check synchronously at first. Submit the active buffer plus every other
  dirty project buffer as one complete-file override transaction, and replace
  the stored graph only after success. A failed attempt publishes diagnostics
  but is not a user-visible candidate system.
- [x] Expose the production lexer token ranges to the editor and add classic
  Turbo C/Pascal-style syntax coloring for keywords, comments, strings,
  numbers, declarations and invalid text. Keep the color theme in the editor;
  do not implement a second Draft lexer or put language policy in `turbo_ui`.
- [x] Treat edits conservatively as interface changes initially. If a file,
  package or import changes graph topology, perform a complete fresh workspace
  load. Optimize body-only edits only after measurement.
- [x] Continue the retained checked graph through shared native artifact
  publication. Check current text before Build/Run and never silently execute
  the retained program when current text is invalid. Run the resulting path in
  Draft through the small `core/process` boundary.
- [x] Test successful and failed overlays, topology reloads, last-good retention,
  root/target switching, and continuation through MIR/LLVM/linking.

## 5. Add semantic IDE windows progressively

- [x] Derive package/dependency, declaration/reference/call, visibility,
  effect, denial and diagnostic views from existing checked compiler products;
  do not introduce another parser or semantic engine.
- [x] Allow one opened workspace to switch among several active roots. Root and
  target select the checked graph, resolution/build namespace and run setup.
- [x] Put a root selector above the F6 package/dependency view and preserve each
  buffer's owning root so F5 always checks/builds the right graph.
- [x] Keep “open directory” as the project model. The manifest remembers only
  initial operator selection; it does not enumerate packages or dependencies.
- [x] Make Files the compiler-discovered source browser and Buffers the open
  document list. Add Open File through Files and transactional Open Workspace
  with Save all / Discard / Cancel dirty-buffer policy.

## Explicitly later

These are future additions, not incomplete first-version work:

- Compiler-backed identifier completion, after the existing declaration and
  scope products expose a useful query.
- Background checking. If introduced, use one in-memory generation counter to
  discard stale work; it is not persisted source history.
- Broad Codex editing through an ordinary Git worktree and explicit conflict
  handling. Ordinary Codex remains outside the keystroke loop, while Draft
  `...` retains its existing precise compiler-synthesis meaning.
- Extend `draft.project` only when named Build/Run configurations, foreign
  providers, arguments, environment, or working directories are implemented.
  It must not list source files, redefine package discovery, or become a
  dependency manager.
- Replace the current read-only semantic text projections with selectable
  compiler-backed navigation when source-jump queries exist.
