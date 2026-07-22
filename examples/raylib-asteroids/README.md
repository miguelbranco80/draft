# Draft Asteroids with raylib

This example is a complete vector-style Asteroids game and Draft's first
graphical native-library application. It deliberately separates three concerns:

- [`game`](game/) is a pure fixed-step Draft simulation with inline storage and
  deterministic tests;
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

## Build on macOS or Linux

From the repository root, first build the vendored library as a shared desktop
provider. Audio is disabled because this first game uses no sound:

```sh
cmake -S vendor/raylib -B build/raylib-desktop \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_SHARED_LIBS=ON \
  -DPLATFORM=Desktop \
  -DCUSTOMIZE_BUILD=ON \
  -DSUPPORT_MODULE_RAUDIO=OFF \
  -DSUPPORT_CUSTOM_FRAME_CONTROL=OFF \
  -DSUPPORT_BUSY_WAIT_LOOP=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/raylib-desktop --parallel
```

The two frame-control settings are required when using raylib's customized
build. Its CMake customization defaults otherwise enable application-owned
frame control and a full busy-wait loop. Application-owned frame control makes
`EndDrawing` omit buffer presentation, frame pacing, and input/window event
polling, which is not the contract used by this example.

On macOS, resolve CMake's dylib symlink to the regular provider file, build the
optimized Draft application, and expose that directory to the loader:

```sh
RAYLIB=$(realpath build/raylib-desktop/raylib/libraylib.dylib)
build/draftc build examples/raylib-asteroids --root app \
  --target aarch64-macos -O2 \
  --provider "raylib=shared-library:$RAYLIB" \
  -o build/draft-asteroids
DYLD_LIBRARY_PATH=$(dirname "$RAYLIB") build/draft-asteroids
```

On x86-64 Linux, use the corresponding target and loader variable (use
`aarch64-linux` on an ARM64 Linux host):

```sh
RAYLIB=$(realpath build/raylib-desktop/raylib/libraylib.so)
build/draftc build examples/raylib-asteroids --root app \
  --target x86_64-linux -O2 \
  --provider "raylib=shared-library:$RAYLIB" \
  -o build/draft-asteroids
LD_LIBRARY_PATH=$(dirname "$RAYLIB") build/draft-asteroids
```

## Build on Windows

In a Visual Studio x64 developer PowerShell, CMake publishes `raylib.lib` and
`raylib.dll`. The import library is the exact Draft linker input; the DLL must
sit beside the executable at runtime:

```powershell
cmake -S vendor/raylib -B build/raylib-desktop -A x64 `
  -DBUILD_EXAMPLES=OFF `
  -DBUILD_SHARED_LIBS=ON `
  -DPLATFORM=Desktop `
  -DCUSTOMIZE_BUILD=ON `
  -DSUPPORT_MODULE_RAUDIO=OFF `
  -DSUPPORT_CUSTOM_FRAME_CONTROL=OFF `
  -DSUPPORT_BUSY_WAIT_LOOP=OFF
cmake --build build/raylib-desktop --config Release --parallel 4
$raylib = (Resolve-Path build/raylib-desktop/raylib/Release/raylib.lib).Path
build/Release/draftc.exe build examples/raylib-asteroids --root app `
  --target x86_64-windows -O2 `
  --provider "raylib=archive:$raylib" `
  -o build/draft-asteroids.exe
Copy-Item build/raylib-desktop/raylib/Release/raylib.dll build/
build/draft-asteroids.exe
```

For a single-config Windows generator, omit the `Release` directory component
when selecting the `.lib` and `.dll`.

## Validate without a display

The game rules need no provider:

```sh
build/draftc test examples/raylib-asteroids --root game
```

The repository integration test additionally builds raylib's software
framebuffer backend, links the real Draft application, and renders one complete
headless frame through `--smoke`:

```sh
ctest --test-dir build -R draft_raylib_asteroids_example --output-on-failure
```
