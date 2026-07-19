---
name: write-draft-code
description: Implement, review, debug, or test code written in the Draft systems language. Use for `.draft` source, core packages, examples, runtime-facing Draft code, target-qualified source, tests and benchmarks, C/native interop, denials, `docs`, `judge`, or `...`; also use when deciding whether current Draft or its core library supports an operation, adding a core API, or invoking `draftc`. Covers the implemented language, explicit ownership, current standard-library surface, supported targets, repository conventions, and validation workflow. Do not use for a C++ bootstrap-only change that neither changes Draft behavior nor touches Draft source.
---

# Write Draft Code

Write production-quality Draft against this checkout's actual language and
core library. Treat the specification as semantic authority, `core/` as the
library API, and compiling examples/tests as the executable usage record.

## Establish the contract

1. Read the repository `AGENTS.md` before editing.
2. Read the relevant normative section under `docs/specification/`. Never infer
   Draft behavior from C, C++, Go, Odin, Jai, or Rust familiarity.
3. Inspect every imported `core/<package>` source file. Core APIs are ordinary
   Draft declarations and are intentionally small; do not hallucinate a richer
   standard library.
4. Inspect the closest compiling example and the target contract when the code
   touches layout, ABI, assembly, runtime, or operating-system behavior.
5. Separate a language rule from a bootstrap limitation and from a missing
   library operation. Put each change in its owning layer.

## Load only the needed references

Read [language.md](references/language.md) for any nontrivial Draft code. Then
load the task-specific references:

- Read [memory-and-ownership.md](references/memory-and-ownership.md) for
  pointers, views, allocation, containers, resources, context, globals, TLS,
  atomics, or threads.
- Read [core-library.md](references/core-library.md) before using or extending
  `core`; it lists the current public surface and conspicuous absences.
- Read [interop-and-targets.md](references/interop-and-targets.md) for
  target-qualified files, C imports/exports, callbacks, assembly, artifacts,
  or platform code.
- Read [agent-features.md](references/agent-features.md) for `docs`, `judge`,
  `...`, `deny`, tests, benchmarks, or provider-free resolution.
- Read [workflow-and-testing.md](references/workflow-and-testing.md) before
  adding packages/examples or choosing validation and documentation updates.

Each reference points back to the authoritative repository source. When a
reference and current specification disagree, follow the specification and
update the stale skill reference in the same coherent change.

## Write the direct Draft version

- Use plain structs, enums, tagged unions, fixed arrays, slices, explicit loops,
  and package procedures. Draft has no methods, inheritance, exceptions,
  operator overloading, declaration overloading, or default arguments.
- Show ownership in names, types, initialization, and cleanup. A pointer,
  slice, string, `io.Reader`, or `io.Writer` is a view or handle, not ownership.
- Keep an owning value at one stable address once callbacks or allocators retain
  its pointer. Do not copy resource handles or owning containers by value.
- Initialize and destroy owning core containers explicitly. Pair resource
  acquisition with `defer` immediately when the cleanup contract permits it.
- Check every recoverable error. Use `assert` only for programmer invariants,
  never for an expected file, input, allocation-policy, or platform failure.
- Use explicit `cast[T](value)` for concrete conversions. Do not assume C-like
  implicit integer, enum, pointer, or truthiness conversions.
- Use `string` for immutable borrowed bytes, `[]u8` for mutable borrowed bytes,
  and an explicit owner such as `memory.Buffer`, `memory.Owned_String`, an
  arena, or an application record for storage.
- Keep target differences in exact `@<target.file_tag>` files when a whole
  implementation differs; use compile-time `when` for small selected facts.
- Begin every nontrivial `.draft` file with a module contract comment before
  `package`: purpose, inputs/outputs, owned resources, invariants, allowed
  dependencies, and relevant specification sections.
- Give important types and nontrivial procedures contract comments covering
  ownership, lifetime, mutability, preconditions, results/state changes,
  errors, determinism, and the non-obvious algorithm or semantic reason.
  Comment stages and subtle branches; do not narrate syntax.

## Avoid false familiarity

Do not assume the current library provides paths, directory walking, seek,
metadata, rename, terminal control, sockets, subprocesses,
Unicode algorithms, general formatting, iterators, smart pointers, hidden
destructors, or automatic moves. Confirm in `core/`. If a generally useful
operation is missing, add the smallest honest core or platform seam with tests;
otherwise keep application policy local.

Do not bypass a missing fixed-arity C binding with an invented variadic Draft
declaration. Draft 1 rejects variadic foreign procedures; use a small
fixed-signature wrapper supplied by an explicit provider or package assembly.

Do not add `...` merely to avoid implementing code. Complete handwritten
programs require no provider. Saved expansions are ordinary checked Draft
source, and `build` never invokes a provider.

## Validate in increasing scope

Run commands from the repository root. Prefer the narrowest command that proves
the current step, then broaden:

```sh
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/package
build/draftc expand path/to/package --out /tmp/expanded-source
build/draftc resolve path/to/package --build -o /tmp/program
build/draftc test path/to/package
build/draftc build path/to/package -o /tmp/program
/tmp/program
```

Pass `--target aarch64-linux` when checking or building the Linux profile;
macOS is the compatibility default. Run both target checks for portable code.
Only native AArch64 hosts can execute current Draft artifacts. See
[workflow-and-testing.md](references/workflow-and-testing.md) for negative
compiler tests, CMake/CTest, sanitizer, example, and documentation routing.

For a resolved package, `expand --out` writes a provider-free complete-source
view with one `.draft-map` sidecar per selected source. The destination must be
absent. Treat this as derived inspection output; the files to commit are
`.draft/resolution.json` and its referenced `.draft/generated` objects.

Use `resolve --build` when source must be resolved and emitted in one command.
It commits only a completely checked source transaction, then continues that
same semantic graph into the native backend; it does not make tests or judgments
implicit. Plain `build` remains provider-free.

Before completion, inspect the diff, re-read changed Draft code linearly, audit
comments and ownership, verify deterministic ordering, run the relevant wider
suite, update the owning docs and this skill when its guidance changed, and
commit the coherent verified slice under the repository rules.
