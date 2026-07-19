# Bootstrap implementation limits

These are explicit crash-safety and bounded-context contracts of the bootstrap compiler. They are not general Draft language limits unless the normative specification says otherwise.

## Recursive implementation resource limits

Status: bootstrap crash-safety contract; not a Draft language limit.

The direct recursive implementation has separate budgets for graphs that are
independent in the language. Parsed declarations, members, types, expressions,
and statements share a 512-level syntax-nesting budget. After parsing, an
acyclic package-import chain is limited to 256 loaded levels, and the type
resolver independently limits forward declaration dependencies to 256 active
declarations. Compile-time constant bindings have their own 256-binding
dependency budget; compile-time procedure calls retain the separate 64-call
recursion budget plus the existing execution-step and value-size limits.

These guards report stable source diagnostics before the host C++ stack becomes
the accidental limit. Cycle diagnostics remain distinct: each graph checks its
visited or active state before applying the acyclic-depth bound. Structural
walks over HIR, MIR, interface types, effects, and constants either follow the
already bounded syntax/type shape or install a visited/cycle row before
following children.

## Native host and instrumentation limits

Status: explicit post-AArch64-Linux qualification boundary.

The bootstrap compiler executable and both locked tool distributions are
currently qualified on an AArch64 macOS host. It can produce and natively
validate programs for AArch64 macOS and AArch64 GNU/Linux, but running the
compiler on Linux is not yet a locked configuration. That host port needs an
ELF dependency-closure verifier for Clang, lld, and llvm-ar before their
process-loader dependencies can be claimed as content-pinned inputs.

AddressSanitizer is qualified only for the macOS target. Linux and every other
instrumentation request remain fail-closed until the compiler pass, runtime,
deployment, clean-process environment, and evidence identity are specified and
tested together. Linux ELF debug information is embedded in the primary
artifact; split debug packages are not implemented.
