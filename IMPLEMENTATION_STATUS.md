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
AArch64 macOS executables and library artifacts, link exact package assembly,
resolve every synthesis
grammar category through transactional pins, and consume those pins offline.

The release acceptance test is still unproved. In particular,
validation/evidence commands, the complete runtime and initial-core surfaces,
generated-source maps, and exact foreign-provider artifact binding do not yet
exist.

## Requirement audit

| Plan requirement | Current evidence | Assessment | Remaining proof or work |
| --- | --- | --- | --- |
| Small, direct C++20 bootstrap with warnings, sanitizers, and tests | `AGENTS.md`, root `CMakeLists.txt`, and the compiler/test targets | Implemented foundation | Run the final suite under the sanitizer configuration and both supported host compilers available to release engineering. |
| One explicit AArch64 macOS target profile | `src/target/profile.*` and `draft_target_profile_tests` | Implemented for the first target | Final native tests must run on the pinned SDK/toolchain rather than only checking profile data. |
| Complete lexer, parser, semicolon insertion, and folder packages | `src/source`, `src/syntax`, `src/workspace`, parser/package/workspace tests | Broadly implemented | Add grammar conformance fixtures covering every valid and invalid production, not only representative nodes. |
| Symbols, scopes, types, constants, layouts, parametrics, and `when` | `src/sema` and semantic/type/constant/interface tests | Broadly implemented | Audit every Draft 1 operator/type combination and complete any missing target validation, especially SIMD and C-layout edge cases. |
| Canonical interfaces and transitive denials | `src/sema/interface.*`, `effect.*`, `denial.*` and tests | Partially implemented | Flow-through procedure slots and exact foreign-provider summaries/artifact binding are absent. |
| Complete handwritten language through HIR, MIR, LLVM, and native execution | `src/sema/body_checker.*`, `src/mir`, `src/backend`, native examples | Partially implemented | Complete the runtime/core surface and release-native conformance matrix. |
| Runtime context, entry, TLS, failures, allocator, and OS support | entry/runtime lowering, lazy foreign-thread attachment, explicit child-thread Context installation, pthread-key-owned temporary allocation/reset/destruction, stable process argument/environment views with teardown, default allocator/logger/random providers, virtual-memory mappings, `core/runtime`, and `core/memory` | Partially implemented | The current OS file seam begins at fixed descriptors because Darwin open(2) requires a locked fixed-signature wrapper artifact; richer runtime/internal-state facilities remain to be completed. |
| Initial core package set from specification section 7 | Every named package exists as inspectable Draft source; allocator-explicit arenas/buffers/owned strings, virtual memory, containers, OS, pthread, compiler-backed atomic, and concurrent atomic examples check, lower, and execute natively | Implemented foundation | Complete the full test/benchmark runners and expand package APIs from the representative first surface as conformance programs require them. |
| Parsed inline assembly plus package assembly | `src/assembly`, `AARCH64_ASSEMBLY_PROFILE.md`, `examples/assembly`, `examples/external-assembly`, assembly/toolchain tests | Implemented for the first profile | `draft-aarch64-apple-v2` fixes and validates the closed straight-line scalar, memory, selection, conversion, baseline NEON, and barrier grammar. Labels, branches, calls, stack changes, and unwinding intentionally remain external-file features. |
| C imports/exports and native artifacts | `src/interop`, scalar/aggregate Darwin ABI and generated-header tests, `examples/c-interop`, `examples/c-library`, native artifact tests | Implemented except provider binding | Object, deterministic archive, dylib, assembly-bundle, LLVM IR, executable, and C-header paths work; exact logical-provider-to-pinned-library/object mapping remains. |
| Provider-independent docs, judgments, and synthesis obligations | agent metadata/obligation modules and tests | Implemented foundation | Context still lacks the full bounded semantic dependency closure, active denial facts, enclosing skeletons, tests, benchmarks, and dominating judgment claims. |
| Dependency-ordered synthesis and opaque interface completeness sets | staged resolver/compiler passes and resolver tests | Implemented for package dependency rounds | Add finer early compile-time dependencies inside one package and prove that each same-set expansion cannot type-check by observing another expansion. |
| Codex adapter behind a provider-neutral boundary | `src/elaborator/codex_cli.*` and adapter tests | Implemented first adapter | Pin the complete executable distribution identity, not merely the selected launcher bytes, and add bounded cancellation/timeout/retry policy. |
| Content-addressed generated source and atomic manifest commit | resolution/store/overlay modules and tests | Implemented foundation | Add generated-source maps and evidence references to the manifest schema. |
| Ordinary offline builds consume pins without a provider | staged offline compiler path, resolver tests, and locked executable/archive adapter tests | Implemented | Extend byte-for-byte release proof across every output kind. |
| `draft build --locked` with no ambient external search | Versioned external-input rows, resolved-program binding, content-tree verification, explicit toolchain/SDK CLI roots, clean process environment, absolute Clang/linker/archiver paths, and SDK/link flags | Implemented for compiler-owned artifacts | Add exact dependency/foreign object/archive/shared-library/runtime/provider-summary mappings; locked builds reject every unsupported external row. |
| Tests, benchmarks, judgments, and validation evidence | Surface syntax and a provider-free `judge` diagnostic only | Missing | Implement discovery/harness commands, canonical execution order, evidence keys/store, verdict aggregation, revocation, and locked evidence verification. |
| Generated-source diagnostics/source maps | Resolved whole-file overrides only | Missing | Preserve expansion-to-site mapping through diagnostics, debug locations, coverage, profiles, and disassembly; serialize maps in the manifest. |
| Crash-safe and deterministic release verification | Atomic pin-store tests and deterministic serializers | Partially implemented | Add fault-injection recovery, byte-for-byte clean-workspace rebuild tests, malformed-store fuzz/conformance cases, and final sanitizer/native gates. |

## Current executable commands

The driver currently exposes `lex`, `syntax`, `target`, `check`, `emit-llvm`,
`emit-c-header`, ordinary and locked `build` for executable, object, static
library, dynamic library, and assembly-bundle kinds, `resolve`, and the
provider-free `judge` boundary. The specification's `test`, `bench`, and
validation-selection commands are not implemented yet.

## Next release-critical slice

The next implementation slice binds foreign provider identities to exact locked
artifacts and summaries. The validation and evidence harness then needs to
exercise complete programs through the locked native seam rather than relying
only on unit-level compiler proofs.
