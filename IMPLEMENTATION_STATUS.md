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
resolve every synthesis grammar category through transactional pins, and
consume those pins offline.

The release acceptance test is still unproved. In particular, complete
runtime/initial-core conformance, judgment-provider evidence, and
generated-source maps do not yet exist.

## Requirement audit

| Plan requirement | Current evidence | Assessment | Remaining proof or work |
| --- | --- | --- | --- |
| Small, direct C++20 bootstrap with warnings, sanitizers, and tests | `AGENTS.md`, root `CMakeLists.txt`, and the compiler/test targets | Implemented foundation | Run the final suite under the sanitizer configuration and both supported host compilers available to release engineering. |
| One explicit AArch64 macOS target profile | `src/target/profile.*` and `draft_target_profile_tests` | Implemented for the first target | Final native tests must run on the pinned SDK/toolchain rather than only checking profile data. |
| Complete lexer, parser, semicolon insertion, and folder packages | `src/source`, `src/syntax`, `src/workspace`, parser/package/workspace tests | Broadly implemented | Add grammar conformance fixtures covering every valid and invalid production, not only representative nodes. |
| Symbols, scopes, types, constants, layouts, parametrics, and `when` | `src/sema` and semantic/type/constant/interface tests | Broadly implemented | Audit every Draft 1 operator/type combination and complete any missing target validation, especially SIMD and C-layout edge cases. |
| Canonical interfaces and transitive denials | `src/sema/interface.*`, `effect.*`, `denial.*`, artifact-summary parser/verifier, and tests; target v4 system-symbol summaries | Implemented first-release closure | Direct, returned, typed-field, interprocedural write-back, hidden-Context, and recursively higher-order flows compose at exact call sites and across package interfaces. Transitive declarations, compiler/runtime bridges, target System symbols, package assembly, and artifact-bound external audits are included. Expand malformed-contract and deep-recursion conformance coverage. |
| Complete handwritten language through HIR, MIR, LLVM, and native execution | `src/sema/body_checker.*`, `src/mir`, `src/backend`, native examples | Partially implemented | Complete the runtime/core surface and release-native conformance matrix. |
| Runtime context, entry, TLS, failures, allocator, and OS support | entry/runtime lowering, lazy foreign-thread attachment, explicit child-thread Context installation, pthread-key-owned temporary allocation/reset/destruction, stable process argument/environment views with teardown, default allocator/logger/random providers, virtual-memory mappings, typed pathname open/read/write/close/remove through a package-assembly ABI shim, `core/runtime`, and `core/memory` | Partially implemented | Richer runtime/internal-state facilities and a release-native conformance matrix remain to be completed. |
| Initial core package set from specification section 7 | Every named package exists as inspectable Draft source; allocator-explicit arenas/buffers/owned strings, virtual memory, containers, OS, pthread, compiler-backed atomic, concurrent atomic, and validation examples check, lower, and execute natively | Implemented foundation | Expand package APIs from the representative first surface as conformance programs require them. |
| Parsed inline assembly plus package assembly | `src/assembly`, `AARCH64_ASSEMBLY_PROFILE.md`, `examples/assembly`, `examples/external-assembly`, assembly/toolchain tests | Implemented for the first profile | `draft-aarch64-apple-v2` fixes and validates the closed straight-line scalar, memory, selection, conversion, baseline NEON, and barrier grammar. Labels, branches, calls, stack changes, and unwinding intentionally remain external-file features. |
| C imports/exports and native artifacts | `src/interop`, `src/backend/foreign_inputs.*`, `foreign_summaries.*`, scalar/aggregate Darwin ABI and generated-header tests, `examples/c-interop`, `examples/c-library`, `examples/foreign-provider`, native artifact tests | Implemented for first target | All output kinds work. Built-in providers are profile-owned and have closed symbol summaries; every other logical provider maps to one exact pinned object/archive/dylib and optional exact summary, both reverified before use. |
| Provider-independent docs, judgments, and synthesis obligations | agent metadata/obligation modules and tests | Implemented foundation | Context still lacks the full bounded semantic dependency closure, active denial facts, enclosing skeletons, tests, benchmarks, and dominating judgment claims. |
| Dependency-ordered synthesis and opaque interface completeness sets | staged resolver/compiler passes and resolver tests | Implemented for package dependency rounds | Add finer early compile-time dependencies inside one package and prove that each same-set expansion cannot type-check by observing another expansion. |
| Codex adapter behind a provider-neutral boundary | `src/elaborator/codex_cli.*` and adapter tests | Implemented first adapter | Pin the complete executable distribution identity, not merely the selected launcher bytes, and add bounded cancellation/timeout/retry policy. |
| Content-addressed generated source and atomic manifest commit | resolution/store/overlay modules and tests | Implemented foundation | Add generated-source maps and evidence references to the manifest schema. |
| Ordinary offline builds consume pins without a provider | staged offline compiler path, resolver tests, and locked executable/archive adapter tests | Implemented | Extend byte-for-byte release proof across every output kind. |
| `draft build --locked` with no ambient external search | Versioned external-input rows, resolved-program binding, content-tree verification, explicit toolchain/SDK/provider/summary CLI roots, clean process environment, absolute Clang/linker/archiver paths, provider snapshots, consumed summary verification, SDK/link flags, and optional exact-key test/benchmark evidence gates | Implemented through foreign link artifacts, audits, and validation evidence | Add runtime-asset mappings; locked builds reject unsupported external roles. |
| Tests, benchmarks, judgments, and validation evidence | Typed core-nominal discovery, target-qualified file selection, canonical package/declaration order, compiler-owned isolated native harnesses, private result pipe, direct process runner, process-isolated benchmark warmup/sampling, canonical content-addressed evidence, exact environment/tool/policy keys, append-only attempt history, failure revocation, locked evidence gates, validation tests, and `examples/validation` | Implemented for tests and first benchmark profile | Judgment execution remains the provider-free typed boundary described by the initial plan. Add richer instrumentation profiles, statistical aggregation/tolerances, and Codex judgment evidence. |
| Generated-source diagnostics/source maps | Resolved whole-file overrides only | Missing | Preserve expansion-to-site mapping through diagnostics, debug locations, coverage, profiles, and disassembly; serialize maps in the manifest. |
| Crash-safe and deterministic release verification | Atomic pin-store tests and deterministic serializers | Partially implemented | Add fault-injection recovery, byte-for-byte clean-workspace rebuild tests, malformed-store fuzz/conformance cases, and final sanitizer/native gates. |

## Current executable commands

The driver currently exposes `lex`, `syntax`, `target`, `check`, `emit-llvm`,
`emit-c-header`, ordinary and locked `build` for executable, object, static
library, dynamic library, and assembly-bundle kinds, `test`, `bench --verify`,
`resolve`, and the provider-free `judge` boundary. Test and benchmark commands
compile their otherwise excluded files into a compiler-owned AArch64 macOS
harness, execute selected procedures in canonical order, and append immutable
evidence attempts under `.draft/evidence`. Locked builds may require matching
active evidence with `--require-test-evidence` and
`--require-benchmark-evidence`; they verify it without executing validation.

## Next release-critical slice

The next implementation slice returns to runtime/core conformance and the final
release gates: sanitizer builds, pinned-native execution, deterministic clean
rebuilds, malformed-store recovery, and the remaining generated-source mapping
and judgment-provider boundaries.
