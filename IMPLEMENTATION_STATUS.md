# Draft bootstrap compiler implementation status

Status: living completion audit for the first implementation plan.

This file records what evidence currently proves and, more importantly, what it
does not prove. Passing unit tests are evidence for the behavior they exercise;
they are not treated as proof that an entire specification section is complete.
The authoritative requirements remain the language specification and
`IMPLEMENTATION_PLAN.md`.

## Release-level assessment

The bootstrap is a substantial end-to-end compiler, but it is not yet the full
first solid release described by the plan. It can parse and semantically check a
large Draft subset, lower representative programs through MIR and LLVM, build
AArch64 macOS executables, link exact package assembly, resolve every synthesis
grammar category through transactional pins, and consume those pins offline.

The release acceptance test is still unproved. In particular, physical
verification and invocation of locked external inputs, validation/evidence
commands, complete runtime TLS/context behavior, the complete initial core
distribution, generated-source maps, and several ABI artifact/output paths do
not yet exist.

## Requirement audit

| Plan requirement | Current evidence | Assessment | Remaining proof or work |
| --- | --- | --- | --- |
| Small, direct C++20 bootstrap with warnings, sanitizers, and tests | `AGENTS.md`, root `CMakeLists.txt`, and the compiler/test targets | Implemented foundation | Run the final suite under the sanitizer configuration and both supported host compilers available to release engineering. |
| One explicit AArch64 macOS target profile | `src/target/profile.*` and `draft_target_profile_tests` | Implemented for the first target | Final native tests must run on the pinned SDK/toolchain rather than only checking profile data. |
| Complete lexer, parser, semicolon insertion, and folder packages | `src/source`, `src/syntax`, `src/workspace`, parser/package/workspace tests | Broadly implemented | Add grammar conformance fixtures covering every valid and invalid production, not only representative nodes. |
| Symbols, scopes, types, constants, layouts, parametrics, and `when` | `src/sema` and semantic/type/constant/interface tests | Broadly implemented | Audit every Draft 1 operator/type combination and complete any missing target validation, especially SIMD and C-layout edge cases. |
| Canonical interfaces and transitive denials | `src/sema/interface.*`, `effect.*`, `denial.*` and tests | Partially implemented | Flow-through procedure slots and exact foreign-provider summaries/artifact binding are absent. |
| Complete handwritten language through HIR, MIR, LLVM, and native execution | `src/sema/body_checker.*`, `src/mir`, `src/backend`, native examples | Partially implemented | Complete the closed parsed-assembly vocabulary, foreign aggregate ABI coverage, runtime/core surface, and release-native conformance matrix. |
| Runtime context, entry, TLS, failures, allocator, and OS support | entry/runtime lowering and `core/runtime`, `core/memory` | Partially implemented | Foreign-thread attachment, Draft TLS establishment, thread-owned temporary arena, default logger/random provider, environment exposure, and runtime teardown are missing. |
| Initial core package set from specification section 7 | `core/runtime`, `c`, `option`, `result`, and `memory` | Incomplete | `heap`, `array`, `map`, `io`, `os`, `atomic`, `thread`, `testing`, `benchmark`, and `time` are absent; memory arenas/owned buffers/strings/virtual memory are also absent. |
| Parsed inline assembly plus package assembly | `src/assembly`, `examples/assembly`, `examples/external-assembly`, assembly/toolchain tests | Partially implemented | External assembly is complete for the first seam. Inline assembly still needs the remainder of the target profile's closed instruction/addressing/SIMD vocabulary. Labels and calls intentionally remain external-file features. |
| C imports/exports and native artifacts | `src/interop`, ABI tests, `examples/c-interop` | Partially implemented | Exact provider-to-library mapping, aggregate C ABI coverage, and object/static/shared-library/C-header/assembly artifact commands are missing. |
| Provider-independent docs, judgments, and synthesis obligations | agent metadata/obligation modules and tests | Implemented foundation | Context still lacks the full bounded semantic dependency closure, active denial facts, enclosing skeletons, tests, benchmarks, and dominating judgment claims. |
| Dependency-ordered synthesis and opaque interface completeness sets | staged resolver/compiler passes and resolver tests | Implemented for package dependency rounds | Add finer early compile-time dependencies inside one package and prove that each same-set expansion cannot type-check by observing another expansion. |
| Codex adapter behind a provider-neutral boundary | `src/elaborator/codex_cli.*` and adapter tests | Implemented first adapter | Pin the complete executable distribution identity, not merely the selected launcher bytes, and add bounded cancellation/timeout/retry policy. |
| Content-addressed generated source and atomic manifest commit | resolution/store/overlay modules and tests | Implemented foundation | Add generated-source maps and evidence references to the manifest schema. |
| Ordinary offline builds consume pins without a provider | staged offline compiler path and resolver tests | Implemented | Extend the proof through native artifact production under the exact locked inputs. |
| `draft build --locked` with no ambient external search | Versioned external-input manifest rows, resolved-program binding, and `draft.content-tree.v1` hashing | Partially implemented | Add resolve/build CLI bindings, verify physical roots, require all selected artifacts/toolchain/SDK/runtime rows, pass explicit subprocess paths and SDK/link flags, and reject ambient search. |
| Tests, benchmarks, judgments, and validation evidence | Surface syntax and a provider-free `judge` diagnostic only | Missing | Implement discovery/harness commands, canonical execution order, evidence keys/store, verdict aggregation, revocation, and locked evidence verification. |
| Generated-source diagnostics/source maps | Resolved whole-file overrides only | Missing | Preserve expansion-to-site mapping through diagnostics, debug locations, coverage, profiles, and disassembly; serialize maps in the manifest. |
| Crash-safe and deterministic release verification | Atomic pin-store tests and deterministic serializers | Partially implemented | Add fault-injection recovery, byte-for-byte clean-workspace rebuild tests, malformed-store fuzz/conformance cases, and final sanitizer/native gates. |

## Current executable commands

The driver currently exposes `lex`, `syntax`, `target`, `check`, `emit-llvm`,
`build`, `resolve`, and the provider-free `judge` boundary. The specification's
`test`, `bench`, validation selection, artifact-output, and genuinely locked
build modes are not implemented yet.

## Next release-critical slice

The next implementation slice connects the locked manifest contract to native
building. It must accept explicit physical roots, verify them against canonical
content identities, eliminate ambient tool/SDK/library search, and demonstrate
that a resolved program rebuilds natively with provider access disabled. A
version-only Clang gate is useful development protection but is not sufficient
evidence for a locked build.
