# Draft: Denials and validation

Part of the [Draft language specification](../../README.md).

[← Native interop](04-native-interop.md) · [Next: Compiler architecture →](06-compiler.md)

<a id="section-13"></a>

## 13. Compiler denials

`deny` establishes a lexical compiler-enforced boundary using ordinary resolved
names and built-in language constructs:

```draft
import core/heap as heap

deny heap, asm, context.allocator {
    ...
}
```

`deny selectors { region }` is one explicit construct, not a general braced
macro. Its syntactic position selects one of the braced grammar categories
defined in [section 4](01-core-language.md#control-flow). In an expression position,
the region contains exactly one expression and the denial construct produces
that expression's value:

```draft
checksum := deny heap, context.allocator {
    compute_checksum(input)
}
```

In a statement position the region is a statement block with an ordinary
nested lexical scope for names, lifetimes, and `defer`. An expression region
introduces no declaration scope. In a declaration or member position the region
contributes declarations or members to the surrounding package or type rather
than introducing a namespace. The compiler checks the restriction while
compiling each enclosed declaration or member and retains it in the compiled
contract for every later instantiation, call, or use. A synthesis site can
occupy any of these categories under the same active denial.

Each selector has its normal program meaning:

- An imported package alias selects declarations from that package.
- A declaration name selects that declaration.
- A predeclared intrinsic such as `assert` selects that intrinsic.
- A package global selects access to that global.
- The built-in `context` name selects all context-field access; a context field
  such as `context.allocator` selects access to that field.
- A built-in such as `asm` or `unchecked` selects that language construct.

The selected entities are prohibited throughout the semantic dependency graph
reachable from the denied region. `deny heap` therefore rejects a direct call,
a call through an ordinary helper, or a generated helper that reaches the
imported `heap` package. `deny context.allocator` rejects a reachable procedure
that accesses that field. `deny asm` rejects reachable native assembly.
Parsed `asm` and symbols implemented by `.s`, `.S`, or `.asm` files record use
of the built-in `asm` entity in compiler summaries. `unchecked` denotes an
access whose bounds safety is neither statically proven nor dynamically
checked, including multi-pointer indexing. Proof-based removal of a redundant
dynamic check is therefore not unchecked. Within `deny unchecked`, the compiler
emits checks for direct bounded array, slice, and string accesses that it cannot
prove safe, regardless of an outer unchecked policy. Inherently unchecked
operations, already-compiled unchecked callees, and other reachable unchecked
operations are rejected. `deny assert` rejects reachable runtime assertions
without affecting `static_assert`. An `assert` summary also records its access
to `context.assertion_failure_proc`, so denying `context` rejects it. A build-
wide unchecked default is represented as an outer `unchecked` region and cannot
weaken a denial.

`deny raw_data` rejects direct or reachable extraction of a string's backing
pointer, including extraction performed by a helper from another package. The
selector does not imply `deny unchecked`: obtaining the pointer performs no
indexing, while a later multi-pointer access remains governed independently by
the ordinary checked/unchecked rules. Conversely, `deny unchecked` does not
forbid a `raw_data` value that is passed without an inherently unchecked Draft
access to a native read-only pointer-and-length interface.

Nested denials add restrictions. A nested region cannot restore an entity
denied by an enclosing region. The same rule applies to handwritten and
synthesized syntax.

The compiler resolves selectors before checking the denial. Package aliases,
renamed declarations, parametric instantiations, and C linker aliases all map to
stable semantic identities. A name-based selector must be in ordinary lexical
scope; built-in construct selectors such as `asm` and `unchecked` are always
available.

Compiled interfaces contain denial summaries of referenced declarations,
globals, context fields, and built-in constructs. A summary has flow-through
slots for procedure pointers received directly, returned, or reached through a
typed parameter or hidden-context field. Calling such a value contributes its
slot rather than an unknown edge. Calls, returns, copies, and typed-field stores
propagate slots, and the call site substitutes the finite target set established
by semantic assignments in the selected build graph. Unknown writes, type
erasure, or dynamically supplied targets produce an unknown edge. Optimization
may not narrow this set. These contracts are inferred and add no source syntax.

A foreign link provider may supply an explicit denial summary for each imported
symbol, including reachable Draft entries and flow-through callbacks. The
compiler uses a supplied summary as the semantic boundary for that invocation;
it does not treat a hash of one artifact as authentication of the transitive
native runtime. Without a summary the foreign body contributes an unknown edge.
An unknown edge rejects an active denial; compiler and runtime providers expose
their effects through compiler-owned contracts.

The compiler checks a denial after resolving each handwritten or generated
fragment and again after composing package and link-time summaries.

The compiler may internally represent facts as calls, context accesses, global
accesses, or syntax constructs. Source code uses only `deny` and normal names.

<a id="section-14"></a>

## 14. Validation and performance

Runtime `assert`, compile-time `static_assert`, tests, judgments, and benchmarks
serve different scopes. Assertions protect local invariants, tests execute
objective behavior, judgments review semantic claims that are difficult to
encode mechanically, and benchmarks enforce measured budgets.

Tests are ordinary procedures in `*_test.draft` files. `draft test` discovers
procedures whose names begin with `test_` and whose signature accepts
`^testing.Test`:

```draft
import core/testing

test_decode :: proc(t: ^testing.Test) {
    image := decode(test_input)
    testing.expect(t, image.width == 32)
}
```

Benchmarks are ordinary procedures in `*_bench.draft` files. `draft bench`
discovers `bench_` procedures accepting `^benchmark.Benchmark`:

```draft
import core/benchmark
import core/time as time

bench_decode :: proc(b: ^benchmark.Benchmark) {
    run_decode :: proc() {
        decode(test_input)
    }

    benchmark.require_max_time(b, 200 * time.nanosecond)
    benchmark.measure(b, run_decode)
}
```

The testing and benchmarking APIs are ordinary core-library declarations
visible to the compiler and agent like other package APIs. Test and benchmark
files participate in `draft test`, `draft bench`, and resolution-profile
validation, but not in an ordinary package build. The active profile selects
the suites, which run only after the selected build graph forms one coherent
resolved program.

Performance budgets are ordinary `Benchmark` API calls. Library procedures may
register time, cycle, allocation, and code-size limits when the target runner
supports those measurements. Resolution fails when the resolved program fails
a registered budget on the configured benchmark profile. `draft bench
--verify` reruns budgets for CI or release validation; ordinary builds neither
require nor rerun prior evidence.

A validation profile may require target-supported diagnostic instrumentation,
including address and lifetime checks, undefined-operation checks, allocator
poisoning, or race checks. A required but unavailable instrument fails
validation rather than being silently omitted. Its kind, options, runtime, and
tool versions are part of the evidence key; instrumented and uninstrumented
evidence are not interchangeable. Instrumentation diagnoses violations without
changing language semantics.

Benchmark evidence records the target triple, CPU features, runner and
benchmark-library versions, compiler flags, warmup and sampling policy, sample
counts, aggregation and tolerance rules, resolved-program identity, and observed
distribution. Evidence is reusable only when the configured
environment-matching policy accepts the current runner. Tests execute in
canonical package and declaration order with harness-provided isolation.
`draft build` never reruns tests, judgments, or benchmarks.
