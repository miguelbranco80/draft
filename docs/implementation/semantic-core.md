# Front end and semantic core

This document records bootstrap representations and algorithms for lexical classification, type inference, compile-time evaluation, effect summaries, and expression checking. Observable Draft behavior remains authoritative in the language specification.

## Contextual `c`

Status: implementation representation; no intended semantic change.

The lexer records `c` distinctly because it introduces a C procedure calling
convention. The parser also accepts that token wherever an ordinary contextual
name is required, because the specification deliberately uses `c` as the local
alias in `import core/c as c` and in qualified names such as `c.int`. A line
ending in the alias therefore receives ordinary identifier semicolon behavior.

## Procedure-flow and external-audit summaries

Procedure effect interfaces retain path-shaped procedure flow slots. Calls,
ordinary local and typed-field copies, hidden-Context provider fields, and
cross-package interface composition substitute finite named targets at the
exact call site. Transitive declaration calls are effects as well, so denying a
declaration reaches through helpers and procedure-valued fields. This is a
semantic may-target set: all source assignments are unioned, and optimization
is never allowed to narrow it. Returned procedure leaves carry either their
factory-input slots or a flattened imported call contract, keeping those two
parameter scopes distinct across package interfaces. Typed pointer-field writes
carry the same value contracts back through local or imported callees; a signed
address/dereference balance prevents writes to an addressed local copy from
being misreported as caller mutation. Body replay makes chained write-through
helpers independent of declaration order. Higher-order FlowCall rows recursively
retain the typed arguments supplied to the callback, including their own finite
procedure contracts, and the same shape crosses package interfaces. Callees and
arguments are snapshotted in source evaluation order before a call's write-back
becomes visible, so later argument effects cannot rewrite earlier values.

External artifact summaries use the strict
`draft-provider-denial-summary-v1` line format documented in section 12. The
summary declares the exact canonical artifact digest; the manifest separately
pins the summary bytes under the same logical provider name. Compilation keeps
the parsed audit and its digest in the compiled result, allowing the native
adapter to reject a manifest summary row that semantic checking did not consume.
An omitted symbol is unknown, and compiler-, package-assembly-, and target-owned
providers cannot be overridden by an external audit.

## Nominal generic inference and transitive interfaces

Status: Draft 1 semantic rule and bootstrap representation.

Procedure inference unifies nominal template applications by their retained
template identity and ordered arguments. It never compares aggregate layouts or
member names: unrelated structs remain different even when their bytes match.
After an earlier argument fixes a type parameter, later arguments receive the
substituted expected type, allowing ordinary literals in calls such as
`array.append(&values, 42)` without weakening the rule that every parameter must
be inferred uniquely.

An imported nominal type may occur only inside another declaration's procedure
signature. Interface import therefore creates a private compiler-owned type
owner for its retained member packet even when the immediate dependency does
not publish a type declaration for it. This makes transitive values fully
usable—for example, the `runtime.Allocator` returned by `core/heap` still has
its `procedure` and `user` members—and lets a subsequent interface export carry
the same member information onward.

Generic aggregate construction applies that rule recursively. When a public
template contains a specialization from a transitive dependency, the canonical
interface packet is sufficient to create the concrete nested nominal even if
the consumer did not directly import the original template declaration. The
importer installs provenance before following members so recursive types
converge on the in-progress TypeId. Recursive instantiation snapshots aggregate
rows before appending new ones; sanitizer coverage enforces this append-only
table discipline.

## Unique dependent-value inference

Status: implemented for a closed, provably one-to-one Draft 1 subset.

Procedure-call inference first substitutes values learned from earlier runtime
arguments, then may invert one remaining dependent integer expression. The
solver requires exactly one occurrence of exactly one unresolved value
parameter. It follows only operations which are one-to-one over the node's exact
typed domain: unary positive, negation, bitwise complement, same-width or
widening integer casts, addition or subtraction by a constant expression, and
XOR by a constant expression. Fixed-width inversion uses the same
two's-complement wrapping as ordinary dependent-expression evaluation. The
candidate is substituted back into the complete canonical expression and
evaluated before it is accepted.

This supports calls inferred from shapes such as `[N + 1]T`, `[10 - N]T`, and
`Buffer[N + 1]`, including imported templates whose parameter leaves have been
remapped through package-interface ordinals. It also checks a later dependent
argument against a value inferred by an earlier argument.

The compiler does not guess through non-injective forms. Repeated occurrences
such as `N + N`, multiplication/division/remainder, shifts, AND, and OR require
an explicit generic argument unless a future solver can prove a unique solution
for the complete declared domain. Rejecting those forms is part of the
uniqueness rule, not a heuristic failure.

## Owner-evaluated generic layout constants

Status: implemented for array/SIMD recipes in local generic types, generic
procedure signatures, nested procedure-dependent nominal and structural alias
value arguments, and concrete cross-package public type applications.

The compact dependent-integer tree remains the canonical representation for
arithmetic and casts such as `[N + 1]T`. It must not pretend to represent a call
to an ordinary compile-time procedure: that procedure may contain conditionals,
loops, switches, recursion, aggregate values, and further calls, all of which
belong to the full interpreter rather than a second reduced evaluator.

When a parameter-dependent array or SIMD count needs that full interpreter, its
symbolic Type row carries an explicit owner-evaluated marker and the defining
SemanticPackage retains the source recipe in a package-local side table. A
canonical package interface exports only the marker. It never exports FileId,
NodeId, ScopeId, a private procedure SymbolId, or an ambient source pointer.

A consumer which later supplies concrete generic arguments forms a stable
instantiation request. Compiler orchestration sends that request to the package
which owns the template and procedure bodies. The owner evaluates the recipe
with the normal compile-time interpreter and returns a complete concrete
InterfaceTypeGraph; the consumer imports that graph exactly like any other
owner-produced generic result. A clean consumer rebuild removes the provisional
unknown-layout marker before body checking. Requests and results are ordered and
keyed by package identity, public template identity, and canonical argument
graphs. Private requester nominals returning through the owner graph resolve to
their original local TypeId. This mirrors cross-package generic procedure
instantiation and preserves private implementation details without making
consumer semantics read dependency source.

The same boundary applies when a nominal application itself supplies a full
procedure-dependent value such as `Buffer[increment(N)]`. Its ParametricArgument
carries an explicit owner-evaluated marker while the defining SemanticPackage
retains the recipe. A symbolic nominal instance has members for type checking but
an unknown layout. Substitution composes the captured environment through nested
templates; once concrete, it evaluates the argument and constructs the ordinary
canonical nominal instance. Interfaces again export only the marker.

A structural alias application such as `Bytes[increment(N)]` cannot use that
nominal-instance representation because its final identity is only the
canonical identity of the resulting array, pointer, tuple, procedure, or scalar.
The bootstrap therefore creates a deliberately non-interned placeholder which
copies the provisional structural shape, has unknown layout, and indexes a
package-local record containing the alias template plus its ordered arguments.
Structural interning explicitly ignores these placeholders, so an unresolved
application can never poison a later concrete lookup. Once all arguments are
exact, the placeholder disappears and the ordinary TypeStore constructor
returns the canonical structural TypeId; no alias-specific nominal identity is
invented.

Concrete cross-package structural results need a durable lookup key even though
the resulting Type row has no provenance. The owner annotates only the root of
the published InterfaceTypeGraph with the public template identity and canonical
arguments. A consumer records that application key beside the imported
canonical TypeId. Later rebuilds therefore reuse the exact array/tuple/etc.
without changing structural identity. Pending whole-application markers and
their package-local recipe indices never cross this boundary. Completed body
interfaces retain all self-contained application graphs published during the
layout fixed point instead of discarding them when effects are refreshed.

Owner requests close transitively across package imports. If package A requests
a concrete type from B and B's resulting layout requests a private recipe from
C, the workspace publishes C's graph first, cleanly rebuilds B against the
enriched C interface, and retries the original request. Canonical request
digests detect a repeated non-progressing set, while an explicit owner stack
guards the acyclic package invariant. Previously published type graphs are
self-contained and remain attached to B's interface across the rebuild.

## Procedure-dependent generic procedure arguments

Status: local and cross-package concrete specialization implemented.

An explicit generic procedure application may use a full compile-time call in
its value packet, for example `inner[increment(N)]()`. During the non-lowered
outer-template pass, the body checker validates the full expression's ordinary
type and carries an explicit deferred substitution marker; it does not invent a
compact integer expression or prematurely manufacture a callee instance. Every
concrete outer specialization checks the original syntax again, evaluates it
with the normal typed interpreter and its active type/value bindings, and then
instantiates the exact callee. Cross-package orchestration therefore transfers
only the final integer and never the caller's private helper or source recipe.

Symbolic signatures may retain the callee's unresolved value parameter or a
whole structural-alias placeholder during the template-only pass. They exist
only as type-checking evidence and are never lowered. A full-call substitution
is applied only when the saved recipe actually references that parameter; this
prevents an unrelated outer template environment from trying to evaluate a
callee-owned recipe. The concrete pass substitutes exact counts and is
authoritative for runtime-bearing HIR, including signatures whose array/SIMD or
structural-alias shape depends on the procedure-produced argument.

## Shift-count validation domain

Status: complete Draft 1 scalar runtime rule.

A shift count retains its independently checked source integer type through HIR;
there is no language-level coercion to the shifted operand's type. MIR validates
the count in unsigned `u128`, which exactly represents every nonnegative Draft 1
integer count and every scalar operand width. Sign-extending a negative signed
count into that domain produces a large unsigned value, so the single
`count < width` comparison rejects both negative and out-of-range counts. Only
the proven-safe count is then represented at the shifted operand's width for
LLVM's same-width shift instruction. This avoids truncating a width such as 128
into a narrow count type such as `i8` while retaining the source rule that every
invalid count traps before the shift executes.

## Conditional context discovery

Status: complete Draft 1 contextual-value inference rule.

An outer expected type continues to flow into both conditional value branches.
Without one, a direct `nil` or contextual enum/tagged-union alternative may take
its type from the opposite, independently typed branch regardless of whether it
appears on the left or right. This is a type-checking dependency only: runtime
evaluation still executes the condition first and then exactly one value branch.
If both branches require context, the compiler does not guess an owner or pointer
kind; an outer expected type remains mandatory. Grouping and denial wrappers do
not change this rule, and a nested conditional requests outer context exactly
when both of its own value branches require it.

## Runtime assertion build mode

Status: implemented and bound to resolved-program identity.

`--assertions=off` is a target-independent compile configuration, not an LLVM
optimization flag. Source still parses, resolves, type-checks, and participates
in effect and denial analysis normally. MIR lowering recognizes the typed
`assert` intrinsic before visiting its operands and emits nothing. This is the
only placement that directly proves the specified behavior: calls, stores,
traps, and message construction used solely by an assertion cannot survive,
and no condition is converted into an optimizer assumption.

The versioned `draft.resolved-program.v4` hash records the assertion mode beside
compiler content v129. `build`, `resolve`, and `judge` expose the same explicit
flag so a provider-free manifest cannot be replayed under a different
mode. Test and benchmark compilations deliberately override the release choice
to assertions on and receive their own resolved validation digest; disabling
release assertions must never weaken the validation program that authorizes a
release.
