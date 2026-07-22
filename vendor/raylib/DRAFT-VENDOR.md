# raylib provenance

This directory contains the buildable library subset of the official raylib
6.0 source release. Draft vendors it so the graphical interoperation example
can be built from one checkout without a system package manager.

- Upstream project: <https://github.com/raysan5/raylib>
- Release: `6.0`, published 2026-04-23
- Source archive: <https://github.com/raysan5/raylib/archive/refs/tags/6.0.tar.gz>
- Archive SHA-256: `2b3ee1e2120c7a0796b33062c7e9a694dd8a8caa56a96319ac8c8ecf54a90d0b`
- License: zlib/libpng; see [`LICENSE`](LICENSE)

The retained files are the upstream `src/` and `cmake/` trees plus
`CMakeLists.txt`, `CMakeOptions.txt`, `raylib.pc.in`, `README.md`, and
`LICENSE`. Upstream examples, games, documentation supplements, CI metadata,
and media assets are omitted because they are not inputs to the library build.
No retained upstream source has been modified.

To update this dependency, download a tagged official archive, verify and
record its digest, replace the retained subset exactly, then rebuild both the
desktop and `PLATFORM=Memory` shared-library forms. The latter is the headless
native integration oracle used by Draft's Asteroids example.
