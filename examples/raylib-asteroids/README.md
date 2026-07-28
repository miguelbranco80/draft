# Draft Asteroids with raylib

This example is a complete vector-style Asteroids game and Draft's first
graphical native-library application. It deliberately separates three concerns:

- [`game`](game/) is a pure fixed-step Draft simulation with inline storage, a
  deterministic `core/random.Generator`, and reproducible tests;
- [`raylib`](raylib/) is the small audited raylib 6.0 C ABI surface the program
  actually consumes; and
- [`app`](app/) owns the window, input, time accumulator, UI text, and drawing.

The repository vendors the official raylib 6.0 library source under
[`vendor/raylib`](../../vendor/raylib/). There is no raylib package-manager or
download step. The desktop build still uses the operating system's native
window/OpenGL development APIs; raylib's GLFW dependency is included in the
vendored tree.

## Play

- `A`/left and `D`/right turn.
- `W`/up applies thrust.
- Space fires.
- `R` restarts after game over.
- Escape or the window close control quits.

The commands below use a source-built `build/draftc` (or
`build/Release/draftc.exe` on Windows). If you followed the top-level
downloaded-release setup, use the extracted release's `bin/draftc` or
`bin/draftc.exe` instead.

## Build on macOS or Linux

From the repository root, first build the vendored library as a shared desktop
provider. The output-directory setting gives every single-config generator the
stable location recorded in `draft.workspace`; the manifest selects the exact
regular versioned library for the active Draft target. Audio is disabled because
this first game uses no sound:

```sh
RAYLIB_OUTPUT="$PWD/build/raylib-provider"
cmake -S vendor/raylib -B build/raylib-desktop \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_SHARED_LIBS=ON \
  -DPLATFORM=Desktop \
  -DCUSTOMIZE_BUILD=ON \
  -DSUPPORT_MODULE_RAUDIO=OFF \
  -DSUPPORT_CUSTOM_FRAME_CONTROL=OFF \
  -DSUPPORT_BUSY_WAIT_LOOP=OFF \
  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$RAYLIB_OUTPUT" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/raylib-desktop --parallel
```

The two frame-control settings are required when using raylib's customized
build. Its CMake customization defaults otherwise enable application-owned
frame control and a full busy-wait loop. Application-owned frame control makes
`EndDrawing` omit buffer presentation, frame pacing, and input/window event
polling, which is not the contract used by this example.

The CMake step owns raylib. Draft's manifest owns the root, O2 choice, provider
mapping, working directory, and loader environment. The post-CMake operation is
therefore an ordinary run. On Apple Silicon macOS:

```sh
build/draftc run examples/raylib-asteroids --target aarch64-macos
```

On x86-64 Linux (use `aarch64-linux` on an ARM64 Linux host):

```sh
build/draftc run examples/raylib-asteroids --target x86_64-linux
```

## Build on Windows

In a Visual Studio x64 developer PowerShell, publish `raylib.lib` and
`raylib.dll` to the same stable provider directory. The manifest uses the import
library for linking and launches the program with that directory as its current
working directory, where the Windows loader can find the DLL:

```powershell
$raylibOutput = (New-Item -ItemType Directory -Force build/raylib-provider).FullName
cmake -S vendor/raylib -B build/raylib-desktop -A x64 `
  -DBUILD_EXAMPLES=OFF `
  -DBUILD_SHARED_LIBS=ON `
  -DPLATFORM=Desktop `
  -DCUSTOMIZE_BUILD=ON `
  -DSUPPORT_MODULE_RAUDIO=OFF `
  -DSUPPORT_CUSTOM_FRAME_CONTROL=OFF `
  -DSUPPORT_BUSY_WAIT_LOOP=OFF `
  -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$raylibOutput" `
  -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE="$raylibOutput" `
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$raylibOutput" `
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE="$raylibOutput"
cmake --build build/raylib-desktop --config Release --parallel 4
build/Release/draftc.exe run examples/raylib-asteroids `
  --target x86_64-windows
```

There is deliberately no CMake command in `draft.workspace`: CMake produces the
vendored C library; Draft consumes one explicit result. If raylib changes its
published versioned filename, update that one provider row rather than adding
artifact discovery or scripting to `draftc`.

## Validate without a display

The game rules need no provider:

```sh
build/draftc test examples/raylib-asteroids/game
```

The repository integration test additionally builds raylib's software
framebuffer backend, links the real Draft application, and renders one complete
headless frame through `--smoke`:

```sh
ctest --test-dir build -R draft_raylib_asteroids_example --output-on-failure
```
