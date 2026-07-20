# Draft examples

The examples collectively show every major Draft 1 feature family, but no
single program tries to be an exhaustive language test. Start with
[`language-tour`](language-tour/package.draft), then use the focused examples
for the subsystem you care about. Exact edge cases and invalid programs remain
in `tests/`, where diagnostics and source ranges can be checked directly;
[`runtime-checks`](runtime-checks/package.draft) is the broad native conformance
program.

The first backend targets AArch64 macOS. On that host, a provider-free example
can be checked or built with:

```sh
build/draftc check examples/language-tour
build/draftc build examples/language-tour -o /tmp/draft-language-tour
/tmp/draft-language-tour
```

The positional directory is always the workspace. A package in that directory
is root `.`, so the commands above need no conventionally named child. In a
multi-package workspace, select a child explicitly or let `build` discover all
packages with a surface package-level `main`:

```sh
build/draftc check examples/packages --root app
build/draftc build examples/packages
```

Here `app` imports `lib/math` from the same workspace. `lib/math` is compiled as
its dependency but is not an executable target because it has no `main`.

## Start here

| Example | What it covers |
| --- | --- |
| [`language-tour`](language-tour/) | A readable ordinary-language tour: constants, `when`, arrays and slices, structs, enums, tagged `Result` and `Option`, tuples, distinct types, parametric types and procedures, pointers, `for`, `switch`, `defer`, assertions, `docs`, and console output. |
| [`console`](console/) | `core/console`, allocation-free `core/format`, standard output, process arguments, booleans, and the exact minimum `i64` and maximum `u64` spellings. |
| [`file-io`](file-io/) | Owned C path storage, explicit file handles, immutable-text writes, byte reads, `defer` cleanup, and a complete create/read/remove round trip. |
| [`simple-editor`](simple-editor/) | A useful but deliberately disposable ed-like application: line storage, byte input, numbered navigation, insertion, deletion, dirty-buffer protection, file load/save, command parsing, and focused Draft tests. |
| [`denials`](denials/) | A runnable positive `deny` example whose transitive call graph is compiler-checked to avoid console access, assertions, assembly, unchecked access, and context access. Negative denial cases live in `tests/denial_test.cpp`. |
| [`hello`](hello/) | The smallest provider-free compiler and native-backend smoke program: a fixed array, slice, loop, procedure call, and assertion. |

Console output is core-library policy, not special language syntax. Immutable
strings are copied into a bounded stack buffer by `core/console`, decimal
integers are formatted by ordinary Draft code in `core/format`, and bytes reach
the existing `core/os.write` boundary. No new compiler or LLVM intrinsic is
needed. The first API deliberately covers text, `bool`, `u64`, and `i64`;
floating-point, rune, and general composite formatting remain future library
work rather than missing backend features.

## Core library and runtime examples

| Example | What it covers |
| --- | --- |
| [`core-runtime`](core-runtime/) | `Option`, `Result`, the runtime `Context`, scoped context replacement, provider records, and the C-to-Draft context bridge. |
| [`core-memory`](core-memory/) | Typed allocation, alignment, resizing, temporary allocation, arenas, owned buffers and strings, and virtual memory. |
| [`core-array`](core-array/) | Owning growable arrays plus small uses of the heap, I/O interface, testing, benchmarking, and duration APIs. |
| [`core-map`](core-map/) | Explicit string-key map initialization, insertion, replacement, lookup, removal, and destruction. |
| [`core-utf8`](core-utf8/) | Allocation-free strict UTF-8 validation, forward/reverse scalar decoding, counting, and encoding into caller-owned storage. |
| [`core-os`](core-os/) | Arguments, environment, process facts, file descriptors, path conversion, and raw byte file I/O. |
| [`core-thread`](core-thread/) | Thread creation and joining, mutexes, condition variables, per-thread context setup, temporary storage, and language TLS. |
| [`core-atomic`](core-atomic/) | Atomic integer, pointer, and memory-order operations on one thread. |
| [`core-atomic-thread`](core-atomic-thread/) | Atomic coordination across spawned threads. |

## Language, packages, and backend examples

| Example | What it covers |
| --- | --- |
| [`runtime-checks`](runtime-checks/) | The dense native conformance matrix: scalar and storage types, endian values, aggregates, raw and tagged unions, globals and TLS, relocation-bearing initializers, parametrics, casts, procedure pointers, pointer operations, operators, loop forms, switches, assignment order, and `defer`. This is comprehensive verification, not the recommended tutorial. |
| [`runtime-traps`](runtime-traps/) | Runtime-selected mandatory trap paths for division, shifts, conversions, Unicode and enum checks, and bounds checks. The native test runner expects each selector to trap. |
| [`nested-procedures`](nested-procedures/) | Static nested procedures, recursion, lexical compile-time bindings, parametric nesting, escaping procedure pointers, context propagation, and collision-free backend names. |
| [`packages`](packages/) | Folder packages, file-local imports and aliases, `pub` declarations, package constants, qualified names, and compile-time `when`. |
| [`packages-generic`](packages-generic/) | Cross-package parametric types and procedures, inferred value parameters, layout computation, transitive instantiation, and private consumer types. |
| [`assembly`](assembly/) | Typed parsed AArch64 assembly with integer, flags, memory, floating conversion, and SIMD register classes. |
| [`external-assembly`](external-assembly/) | Target-qualified Mach-O/ELF `.s` discovery and one C-ABI symbol implemented by separate assembly files. |
| [`c-interop`](c-interop/) | A small foreign libc import and a Draft procedure exported with a C linker name. |
| [`c-library`](c-library/) | The full Draft-as-C-library fixture: generated headers, C-compatible records, enums, raw unions, callbacks, aggregates, TLS, and re-entry into ordinary Draft code. It is driven by the C client integration test rather than as a standalone executable. |
| [`foreign-provider`](foreign-provider/) | A foreign block supplied by an explicitly configured external object provider. It requires the matching provider artifact when built. |

## Agent and validation examples

| Example | What it covers |
| --- | --- |
| [`agent-acceptance`](agent-acceptance/) | End-to-end declaration, member, expression, and statement synthesis; `judge`; committed content-addressed expansions; tests and benchmarks that consume generated declarations; evidence; generated-source correlation; and provider-free rebuilding. See the [acceptance guide](../docs/releases/agent-acceptance.md). |
| [`agent-pending`](agent-pending/) | The smallest intentionally unresolved synthesis and judgment workspace, with a self-contained [live-run guide](agent-pending/README.md). It demonstrates the first Codex-backed `resolve`, generated-source inspection, and the subsequent provider-free build. |
| [`judgment-tour`](judgment-tour/) | Provider-free package, struct-member, procedure, branch, loop, switch-case, and target-selected judgments; attached evidence; selective execution; ordinary assertions; and a pointer-taking procedure as Draft's current method-style pattern. |
| [`agent-judgment-mix`](agent-judgment-mix/) | Intentionally unresolved declaration, member, expression, statement, and parsed-assembly synthesis guided by package, type-member, and dominating procedure judgments, with a self-contained live-run guide. |
| [`validation`](validation/) | Discovery and execution of `*_test.draft` and `*_bench.draft` procedures through `core/testing` and `core/benchmark`. |

## Feature map

| Draft area | Primary examples |
| --- | --- |
| Declarations, expressions, literals, arrays, slices, control flow, tuples, pointers, aggregates, enums, tagged unions, and distinct types | `language-tour`, `runtime-checks` |
| Compile-time evaluation, target facts, layout, `when`, and parametrics | `language-tour`, `packages`, `packages-generic`, `nested-procedures` |
| Globals, TLS, context, allocation, collections, atomics, and threads | `core-runtime`, `core-memory`, `core-array`, `core-map`, `core-atomic`, `core-thread`, `runtime-checks` |
| Console, formatting, files, arguments, environment, and a complete line-oriented application | `console`, `file-io`, `core-os`, `simple-editor` |
| Explicit UTF-8 validation, scalar traversal, and encoding over byte strings | `core-utf8` |
| Parsed and external assembly, SIMD, C imports, C exports, and foreign providers | `assembly`, `external-assembly`, `c-interop`, `c-library`, `foreign-provider` |
| `docs`, `judge`, synthesis, deterministic resolution, and provider-free builds | `language-tour`, `judgment-tour`, `agent-judgment-mix`, `agent-acceptance`, `agent-pending` |
| Denials, assertions, traps, tests, and benchmarks | `denials`, `runtime-traps`, `validation` |

This map covers positive surface-language capabilities. Parser recovery,
ill-typed programs, exact diagnostics, denied transitive effects, ABI corner
cases, determinism, and evidence corruption are intentionally test-suite
concerns rather than runnable examples.
