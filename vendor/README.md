# Vendored source

Draft examples may vendor compact native dependencies when that keeps a
complete program reproducible without a package manager. Each dependency owns
a provenance note in its directory. The note records the exact upstream
release, archive digest, retained files, license, and the reason the source is
present.

Vendoring is intentionally simple: these are ordinary reviewed source files,
not a second dependency resolver or a general cache. Update one dependency in
one coherent commit, rebuild its native integration example, and retain the
upstream license.

| Directory | Upstream | Used by |
| --- | --- | --- |
| [`raylib`](raylib/) | raylib 6.0 | `examples/raylib-asteroids` |

