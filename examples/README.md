# Draft examples

The examples collectively show every major Draft 1 feature family, but no
single program tries to be an exhaustive language test. Start with
[`language-tour`](language-tour/package.draft), then use the focused examples
for the subsystem you care about. Exact edge cases and invalid programs remain
in `tests/`, where diagnostics and source ranges can be checked directly;
[`runtime-checks`](runtime-checks/package.draft) is the broad native conformance
program.

The native backends target AArch64 macOS, AArch64 GNU/Linux, x86-64 GNU/Linux,
and x86-64 Windows/MSVC. On a matching host, a provider-free example can be
checked or built with:

```sh
build/draftc check examples/language-tour
build/draftc run examples/language-tour
```

The positional directory selects a package or recursive build scope. Its nearest
ancestor `draft.workspace` establishes the workspace boundary; without one, the
selected directory is a standalone workspace whose root is `.`. In a
multi-package workspace, select a child explicitly or let `build` discover all
packages with a surface package-level `main`:

```sh
build/draftc check examples/packages/app
build/draftc build examples/packages
build/draftc run examples/packages
```

Here `app` imports `lib/math` from the same workspace. `lib/math` is compiled as
its dependency but is not an executable target because it has no `main`. The
workspace's single named program is the durable `run` default; aggregate
`build` still discovers every executable rather than using that default.

## Qualification matrix

[`qualification.tsv`](qualification.tsv) classifies every tracked directory
containing Draft or assembly source. It is exhaustive data, not a naming
convention: ordinary executables are built and launched, dependency packages
are frontend-checked, the C library, foreign-provider, and vendored-raylib
packages have their own native link gates, resolved agent programs consume
committed pins, and
intentionally unresolved programs must fail for the declared reason. The same
rows identify every example-owned Draft test and benchmark.

After configuring and building the bootstrap, run the complete example slice
with:

```sh
ctest --test-dir build -L examples --output-on-failure
```

The ordinary unfiltered CTest command used by CI includes this label. Frontend
qualification runs for all supported targets on every host. Native execution,
C-client/provider linking, and Draft test/benchmark execution are registered on
matching macOS/Linux architecture pairs. Windows CI runs the same ordinary
executable inventory plus its C-client/provider/artifact gate; Draft test and
benchmark execution awaits the Windows validation-process runner. Unsupported
hosts omit native tests instead of reporting false skips.

## Start here

| Example | What it covers |
| --- | --- |
| [`language-tour`](language-tour/) | A readable ordinary-language tour: constants, `when`, named/default arguments, arrays and slices, structs, enums, variant `Result` and `Option`, tuples, distinct types, parametric types and procedures, pointers, `for`, `switch`, `defer`, assertions, `docs`, and console output. |
| [`console`](console/) | Static heterogeneous `console.println` packs, allocation-free `core/format`, standard output, process arguments, booleans, mixed integer widths, and exact 64/128-bit boundary spellings. |
| [`file-io`](file-io/) | Owned C path storage, explicit file handles, a zero-copy immutable-text write, byte reads, `defer` cleanup, and a complete create/read/remove round trip. |
| [`simple-editor`](simple-editor/) | A useful but deliberately disposable ed-like application: line storage, byte input, numbered navigation, insertion, deletion, dirty-buffer protection, file load/save, command parsing, and focused Draft tests. |
| [`tetris`](tetris/) | A full-screen terminal game with raw timed input, fragmented arrow-key decoding, portable resize watching, Unicode-capable `core/tui` cell painting and differential output, a normally OS-seeded deterministic `core/random` seven-bag stream with an explicit gameplay fallback, collision and wall kicks, scoring, levels, clean restoration, and deterministic tests. |
| [`turbo-ui-gallery`](turbo-ui-gallery/) | The independent visual proof for `lib/turbo_ui`: classic drop-down menus with underlined mnemonics and command shortcuts, buttons, check/radio controls, UTF-8 text and combo boxes, caller-owned expandable trees and collection views with proportional scrolling, splitters, true modal dialogs, close/zoom/resize window chrome, mouse and keyboard input, event-free repaint reconciliation, and differential rendering. |
| [`turbo-editor`](turbo-editor/) | A useful full-screen terminal editor built from the reusable Turbo packages: real File/Edit/Project/Window menus, separate file/open-buffer navigation, multiple exact-byte buffers, Unicode grapheme movement, reusable search and dirty-quit dialogs, mouse selection, bounded undo/redo, disk-conflict detection, and managed tool windows with Tile/Cascade arrangement. |
| [`raylib-asteroids`](raylib-asteroids/) | A complete graphical Asteroids game: normally OS-seeded deterministic `core/random` fixed-step simulation with an explicit gameplay fallback, inline object pools, toroidal collision and rendering, focused raylib 6.0 C records/imports, explicit shared-provider linking, keyboard input, frame timing, stack-formatted UI, and a headless software-render integration gate. |
| [`denials`](denials/) | A runnable positive `deny` example whose transitive call graph is compiler-checked to avoid console access, assertions, assembly, unchecked access, and context access. Negative denial cases live in `tests/denial_test.cpp`. |
| [`hello`](hello/) | The smallest provider-free compiler and native-backend smoke program: a fixed array, slice, loop, procedure call, and assertion. |

Console output is core-library policy, not special language syntax. Immutable
strings reach `core/os.write_text` without allocation or copying; decimal
integers are formatted into caller-owned byte buffers by ordinary Draft code in
`core/format`. `console.println` uses a source-level `..type` pack which becomes
one fixed-signature specialization per ordered type tuple; no printing
intrinsic, runtime type erasure, or variadic ABI is involved. It covers text,
`bool`, and every ordinary signed/unsigned integer width. Floating-point, rune,
enum, distinct, endian-scalar, and composite formatting remain future library
policy rather than missing backend features.

## Core library and runtime examples

| Example | What it covers |
| --- | --- |
| [`core-runtime`](core-runtime/) | `Option`, `Result`, the runtime `Context`, scoped context replacement, provider records, and the C-to-Draft context bridge. |
| [`core-memory`](core-memory/) | Typed allocation, alignment, resizing, temporary allocation, arenas, owned buffers and strings, and virtual memory. |
| [`core-array`](core-array/) | Owning growable arrays plus small uses of the heap, I/O interface, testing, benchmarking, and duration APIs. |
| [`core-map`](core-map/) | Explicit string-key map initialization, insertion, replacement, lookup, removal, and destruction. |
| [`core-utf8`](core-utf8/) | Allocation-free strict UTF-8 validation, forward/reverse scalar decoding, counting, and encoding into caller-owned storage. |
| [`core-os`](core-os/) | Arguments, environment, process facts, file descriptors, direct variadic C `open`, path conversion, raw byte I/O, and zero-copy literal/sliced/empty string writes. |
| [`core-thread`](core-thread/) | Thread creation and joining, mutexes, condition variables, per-thread context setup, temporary storage, and language TLS. |
| [`core-atomic`](core-atomic/) | Atomic integer, pointer, and memory-order operations on one thread. |
| [`core-atomic-thread`](core-atomic-thread/) | Atomic coordination across spawned threads. |

## Language, packages, and backend examples

| Example | What it covers |
| --- | --- |
| [`runtime-checks`](runtime-checks/) | The dense native conformance matrix: scalar and storage types, endian values, aggregates, selective packed and cross-byte bit fields, unions and variants, globals and TLS, relocation-bearing initializers, parametrics, casts, procedure pointers, pointer operations, operators, loop forms, switches, assignment order, and `defer`. This is comprehensive verification, not the recommended tutorial. |
| [`runtime-traps`](runtime-traps/) | Runtime-selected mandatory trap paths for division, shifts, conversions, Unicode and enum checks, and bounds checks. The native test runner expects each selector to trap. |
| [`nested-procedures`](nested-procedures/) | Static nested procedures, recursion, lexical compile-time bindings, parametric nesting, escaping procedure pointers, context propagation, and collision-free backend names. |
| [`packages`](packages/) | Folder packages, file-local imports and aliases, `pub` declarations, package constants, qualified names, and compile-time `when`. |
| [`packages-generic`](packages-generic/) | Cross-package parametric types and procedures, inferred value parameters, layout computation, transitive instantiation, and private consumer types. |
| [`thinlto-benchmark`](thinlto-benchmark/) | A focused native benchmark contrasting identical dependency-carrying work through an ordinary cross-package Draft procedure and a package-local reference loop; O0 exposes the call boundary while O2 ThinLTO imports it. |
| [`assembly`](assembly/) | Typed parsed AArch64 assembly with integer, flags, memory, floating conversion, and SIMD register classes; compile-time selection leaves a valid no-assembly program on x86-64. |
| [`external-assembly`](external-assembly/) | Target-qualified Mach-O/AArch64 ELF/x86-64 ELF/AMD64 COFF `.s` discovery and one C-ABI symbol implemented by separate assembly files. |
| [`c-interop`](c-interop/) | A small foreign libc import and a Draft procedure exported with a C linker name. |
| [`c-library`](c-library/) | The full Draft-as-C-library fixture: generated headers, C-compatible records, enums, unions, callbacks, aggregates, TLS, and re-entry into ordinary Draft code. `draftc build examples/c-library` uses its durable dynamic-library policy; the C client integration test exercises the result. |
| [`foreign-provider`](foreign-provider/) | A foreign block supplied by an explicitly configured external object provider. It requires the matching provider artifact when built. |

## Agent and validation examples

| Example | What it covers |
| --- | --- |
| [`agent-acceptance`](agent-acceptance/) | End-to-end declaration, member, expression, and statement synthesis; `judge`; committed content-addressed expansions; tests and benchmarks that consume generated declarations; evidence; native debug output; and provider-free rebuilding. See the [acceptance guide](../docs/releases/agent-acceptance.md). |
| [`agent-pending`](agent-pending/) | The smallest intentionally unresolved synthesis and judgment workspace, with a self-contained [live-run guide](agent-pending/README.md). It demonstrates the first Codex-backed `resolve`, generated-source inspection, and the subsequent provider-free build. |
| [`judgment-tour`](judgment-tour/) | Provider-free package, struct-member, procedure, branch, loop, switch-case, and target-selected judgments; attached evidence; selective execution; ordinary assertions; and a pointer-taking procedure as Draft's current method-style pattern. |
| [`agent-judgment-mix`](agent-judgment-mix/) | Intentionally unresolved declaration, member, expression, statement, and parsed-assembly synthesis guided by package, type-member, and dominating procedure judgments, with a self-contained live-run guide. |
| [`validation`](validation/) | Discovery and execution of `*_test.draft` and `*_bench.draft` procedures through `core/testing` and `core/benchmark`. |

## Feature map

| Draft area | Primary examples |
| --- | --- |
| Declarations, expressions, literals, arrays, slices, control flow, tuples, pointers, aggregates, enums, variants, unions, and distinct types | `language-tour`, `runtime-checks` |
| Compile-time evaluation, target facts, layout, `when`, named/default arguments, parametrics, structural type inspection, and static heterogeneous argument packs | `language-tour`, `console`, `packages`, `packages-generic`, `nested-procedures` |
| Globals, TLS, context, allocation, collections, atomics, and threads | `core-runtime`, `core-memory`, `core-array`, `core-map`, `core-atomic`, `core-thread`, `runtime-checks` |
| Console, formatting, files, arguments, environment, raw timed terminal input, differential TUI cells, native window/input/drawing, and complete interactive applications | `console`, `file-io`, `core-os`, `simple-editor`, `tetris`, `turbo-ui-gallery`, `turbo-editor`, `raylib-asteroids` |
| Explicit UTF-8 validation, scalar traversal, and encoding over byte strings | `core-utf8` |
| Parsed and external assembly, SIMD, C imports, C exports, and foreign providers | `assembly`, `external-assembly`, `c-interop`, `c-library`, `foreign-provider`, `raylib-asteroids` |
| `docs`, `judge`, synthesis, deterministic resolution, and provider-free builds | `language-tour`, `judgment-tour`, `agent-judgment-mix`, `agent-acceptance`, `agent-pending` |
| Denials, assertions, traps, tests, and benchmarks | `denials`, `runtime-traps`, `validation`, `thinlto-benchmark` |

This map covers positive surface-language capabilities. Parser recovery,
ill-typed programs, exact diagnostics, denied transitive effects, ABI corner
cases, determinism, and evidence corruption are intentionally test-suite
concerns rather than runnable examples.
