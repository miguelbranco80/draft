# Front end and semantic core

This document records bootstrap representations and algorithms for lexical classification, type inference, compile-time evaluation, effect summaries, and expression checking. Observable Draft behavior remains authoritative in the language specification.

## Type completion facets

Status: payload representation, declaration products, and the post-body ABI
facet are implemented for workspace compilation and rejecting direct clients;
the discovery-only direct compatibility composition remains.

`TypeStore` retains one `TypeCompletion` row beside every canonical `Type` row.
Allocation completes type identity. Member-name completeness, member-type
completeness, and target-natural layout readiness are independent states. A
fresh nominal aggregate therefore has a complete identity and waiting member
facets; `^Node` can have complete pointer layout before `Node` itself has a
complete inline layout. Tuples similarly retain complete member types while
waiting for a member's layout, then publish their own layout when that member
finishes.

`NotApplicable` is a semantic answer rather than a pending state: scalars have
no member set, while untyped and compile-time-only meta types have no runtime
natural layout. Product errors remain in the semantic graph and diagnostics;
they are not encoded as a counterfeit layout or as a second error type in the
payload table. Structural inspection names the exact facet required by each
query. Constant evaluation leaves a query pending when that facet is waiting;
it never reads a partial member vector or reports a not-applicable error for a
fact that later work can still publish. Final body checking instead diagnoses a
still-incomplete query at its source call.

The old bundled `complete_nominal` operation is gone: member-name closure,
member-type closure, and natural layout have separate one-way publication
operations. Target-natural struct, raw-union, tagged-union, and enum layout is a
pure producer over an immutable `TypeStore`; it returns a task-owned layout and
offset packet or the exact incomplete type facets in first-use order.

During ordinary complete workspace compilation, collection first allocates
nominal identities. One `TypeMembers` product publishes each nominal's selected
source-order member symbols, and its dependent `TypeMemberTypes` product fills
those stable symbols without redeclaring them. Non-nominal declaration products
resolve authored signatures and type aliases. Every product consumes only its
explicit completed graph prerequisites; a valid payload in a sequential task
snapshot cannot stand in for a missing edge. A separate `TypeNaturalLayout`
product consumes completed member types and publishes physical layout. Symbolic
parametric templates deliberately stop after member types because their
canonical concrete applications, not the template pattern, own runtime layout.
Imported interface nominals are already-complete upstream inputs and never
become local declaration work.

Array counts, SIMD widths, enum values, `@align` arguments, and parametric value
arguments remain fields of their owning declaration product; the graph does not
create a node per expression. The resolver handles the small literal/arithmetic
vocabulary directly, then uses the ordinary compile-time interpreter for full
Draft expressions such as a procedure call. The interpreter may return exact
declaration, constant, and type-facet prerequisites. A blocked attempt discards
both its partially built structural types and provisional diagnostics. Derived
array, tuple, distinct, and concrete-generic layout waits are reduced to the
authored nominal layout products that can actually unblock them. An imported
owner-evaluated generic proxy instead reports its portable owner request; the
coordinator attaches the declaration to the canonical command-local generic
demand described below.

Package and member `when` conditions also have one-site producers. The
coordinator publishes a completed package selection and appends only the chosen
branch before extending the dynamic product graph. For aggregate members, a
syntax-only scan records the initially reachable condition frontier without
declaring members. A member packet waits on those exact products; if a selected
false branch reveals an `else when`, the private member attempt reports that
site by `SyntaxReference`, the coordinator appends its condition product, and
only then retries the packet. Unselected branches remain opaque. The completed
condition frontier feeds `TypeMembers`; `TypeMemberTypes` depends on that stable
namespace and may add its own exact declaration, constant, generic-demand, or
type-facet prerequisites. Workspace interface-synthesis discovery uses these
same products and suspends exact producers on the package opaque set. Only the
lower-level direct Discover-mode API retains aggregate compatibility until step
6 gives it task-owned synthesis constraints.

Named-constant evaluation has a single-product entry point. It accepts
an immutable table of already published constants, evaluates only its named
root, and returns canonical unfinished-declaration, local-constant, and exact
type-facet blockers. A reference to another unpublished declaration is not
entered recursively; the coordinator adds the corresponding product edge. An
ambiguous `Alias :: Name` value product explicitly depends on declaration
classification and completes without publishing a value when the row is a type
alias. Blocked attempts discard their private mutations and diagnostics.
Complete workspace compilation and rejecting direct analysis schedule every
final package-scope constant as a real `ConstantValue` product. The
product-aware finalizer consumes the published constant table and rechecks
required storage/target contracts without recursively recomputing named
constants. The direct sequential coordinator also exhausts independent products
after a source error, then runs only the diagnostic validation closure; it does
not manufacture values for failed or blocked products. A long acyclic constant
chain therefore advances through graph edges rather than consuming recursive
constant-evaluator depth. The discovery-only direct compatibility API retains
aggregate constant evaluation until step 6.
Ready constants imported through dependency interfaces are not duplicated as
consumer products. The finalizer copies their already translated values under
the consumer-local proxy IDs before body checking and validation context use.
When one frozen wave completes declarations and constants together, the
coordinator fixes the declaration TypeStore prefix before interning
constant-task structural types. This publication order prevents equal numeric
task-local TypeIds from being reinterpreted as unrelated declaration rows.

## Compile-time type values and inspection

Status: exact type values and the complete Draft 1 structural query vocabulary
are implemented in body checking, constant evaluation, and package interfaces.

`TypeStore` contains one layout-less `MetaType` row for the predeclared `type`
type. `ConstantValue::Type` carries a TypeId index while a package is being
checked. The body checker and constant interpreter both call the single direct
query implementation in `sema/type_inspection`; query applicability is not
reimplemented by LLVM lowering or core packages. `type_of` checks an operand's
static type without retaining it as an evaluated operand, so an unused call,
trap, or runtime read cannot become observable through inspection.
Structural type constructors used as expressions, including pointers, views,
arrays, tuples, procedures, and SIMD vectors, resolve to the same exact `TypeId`
and meta-type constant as a named type; they do not pass through the runtime
expression or lowering paths. A `::` constant whose value is a
`ConstantValue::Type` may in turn supply that exact TypeId to an annotation,
structural type, parametric type/procedure argument, or cast. TypeResolver probes
the ordinary constant interpreter when a name in type syntax is not a declared
type, so computed values and their aliases have one meaning in every type
position. Ready imported type constants take the same path after interface
rewriting; the meta-type stored on either Symbol is never mistaken for the
represented type.
Package-constant evaluation uses a non-executing declared-result reader to fold
the type value, then reuses the ordinary BodyChecker expression path for every
package initializer and selected declaration, aggregate-member, or procedure
`when` condition that contains `type_of`, short-circuiting, or a conditional
value branch. The constant evaluator already checked every operand of an
ordinary selected expression, so it is not redundantly sent through
runtime-oriented HIR. The preflight checks complete call arity, arguments,
slicing, members, and nested intrinsics without lowering or executing an
inspected expression; the result reader is not a second source type checker.

HIR has no value for the whole predeclared `target` object. BodyChecker instead
imports each direct target field or query as a typed constant from the ordinary
constant evaluator, both in validation and executable source; this is how
`os := target.os` materializes the compile-time scalar without manufacturing a
runtime target record. Validation-only HIR may additionally import an
already-evaluated binding. Importing exact leaves preserves grouping,
contextual alternatives, and target-to-target comparisons without duplicating
the compiler-defined target enum types in BodyChecker. The diagnosing evaluator
handles target leaves so a short-circuited invalid feature still receives the
authoritative feature diagnostic. The bridge never imports an enclosing
logical or conditional result, because doing so could repeat short-circuiting
and hide the independent operand the pass is meant to validate. Invalid
children propagate without secondary reflection or operator diagnostics once
the exact operand error has been reported. If validation ever produces an
invalid HIR value without such a child diagnostic, the preflight boundary emits
a fail-closed static-type error; a selected declaration or statement branch
cannot disappear silently.

The five categorical target fields use separate compiler-defined enum rows in
every `TypeStore`. The constant evaluator converts each profile spelling to its
stable member ordinal and exact enum `TypeId` before comparison or reflection;
sharing an integer representation therefore cannot make `target.os` compatible
with `target.arch`. `type_of`, `type_name`, and member inspection observe those
ordinary named enum types. Contextual alternatives receive that exact type from
either binary operand, and integer-to-enum casts validate the compiler-defined
member table just as source enums validate their declared members. Direct
target-member recognition unwraps transparent parentheses around the target
base, while preserving the normal rule that a lexical declaration named
`target` shadows the predeclared object. `target.has_feature` exposes its
declared bool result to non-evaluating `type_of`, but validation still checks its
arity, complete argument subtrees, compile-time string, and known spelling.

At interface publication, every type-valued constant recursively rewrites its
package-local TypeId into the same `InterfaceTypeId` graph used by public
declaration types. Import performs the inverse rewrite into the consumer's
TypeStore. Interface hashes include that canonical interface index. No
package-local ID crosses the boundary, including type values nested in a
compile-time aggregate or generic value packet.

## Raw string-data intrinsic

Status: exact string-to-pointer typing and explicit runtime lowering are
implemented.

The body checker recognizes `raw_data` as a predeclared intrinsic rather than a
library declaration. It requires exactly one `string` operand and assigns the
ordinary `[^]u8` result type. The checked HIR retains the operand because the
pointer is a runtime value derived from that particular string view; constant
evaluation may inspect the call's static result type but never folds the call
or reads its bytes. No implicit string-to-slice or string-to-pointer conversion
exists beside this named operation.

The result borrows the string's backing lifetime and does not establish that
the storage is writable. Those facts remain source-level semantic contracts;
the HIR type is intentionally the existing multi-pointer type rather than a
second const-qualified pointer family. Denial summaries record the intrinsic
as its own reachable effect, so `deny raw_data` applies through local helpers
and imported package interfaces exactly like the other compiler-defined
operations.

The compiler-defined `Type_Kind`, `Type_Byte_Order`, and `Calling_Convention`
enums are canonical builtin TypeStore rows. Their source-facing alternatives
are maintained beside the query implementation rather than manufactured as
source symbols. A folded enum result may reach runtime as its ordinary integer
representation. Before MIR, the body checker walks every executable HIR value
and rejects automatic storage, arguments, results, aggregates, and procedure
values whose type contains the `type` meta-value; global initializer checking
enforces the same boundary for package storage. A procedure whose signature
contains `type` remains available to constant evaluation and receives ordinary
body checking, but is marked compile-time-only and omitted from MIR. LLVM keeps
its final diagnostic only as an internal fail-closed check, not as the first
user-facing enforcement point.

## Instance-dependent `when` and refinement

Status: implemented for parametric procedure bodies and concrete generic
instances.

Package discovery may record a provisional selection for a body `when` whose
condition is already ready, allowing nested synthesis sites to be found. It
does not require a pending statement condition, because body-local declarations
do not exist in the package scope. BodyChecker always makes the authoritative
selection again at the statement's source-ordered lexical point; preceding
compile-time constants, ordinary locals visible through `type_of`, lexical
shadowing, and concrete instance substitutions are therefore exact. A body
`when` that names a type/value parameter or an ordinary value whose type graph
contains one remains unselected in package discovery. The symbolic body checker
then checks both branch HIR containers. An explicit refinement stack records
the subject SymbolId, optional exact TypeId, allowed `TypeKind` set, and
accumulated exact exclusions. Only the recognized
`type_of(subject)` and `type_kind(type_of(subject))` comparisons add facts;
arbitrary dependent booleans remain selection-only expressions. Refinements
affect expression capability checks but never mutate declaration types or
public constraint rows.

Concrete procedure instances reuse the ordinary constant interpreter with
their type/value substitution overlay, check only the selected transparent
branch, and emit no symbolic type query. A dedicated dependent-`when` depth
marks every symbolic possibility, including index/value conditions which grant
no type refinement. A `static_assert` in any such possibility is deferred and
evaluated only when a concrete instance selects it; ordinary runtime branch
facts do not defer assertions. Body agent sites are recorded only by the
symbolic source pass, with the active branch-refinement path, and are not
duplicated while instances validate the accepted expansion.

Effect closure includes every concrete HIR procedure. For a specialization
requested across a package boundary, the completed interface publishes an
instance row containing the public template name, canonical ordered generic
arguments, canonical ordered pack types, stable monomorphized linker name, and
that exact body's effect/return/write contract. Consumer proxies refresh from
this row rather than inheriting the symbolic public template summary; two
substitutions may select different calls and therefore legitimately expose
different denials.

## Static heterogeneous argument packs

Status: implemented for local, nested, and cross-package direct calls, symbolic
body checking, exact effects, and native lowering.

Signature resolution records one `StaticArgumentPack` beside the owning
procedure instead of adding a slice, tuple, or meta-type member to its source
`TypeKind::Procedure`. The row owns the source binding, fixed-prefix count, and
a unique symbolic element TypeId. The source procedure is parametric even when
it has no bracketed parameters, so it follows the same non-lowered template
boundary as an ordinary generic procedure.

Call checking evaluates the fixed prefix and tail in source order. The tail is
defaulted independently, producing an ordered TypeId vector. The instance key
is the ordinary type/value substitution packet plus that vector. Instantiation
appends one ordinary Parameter symbol and signature member per tail element;
static iteration aliases its lexical value binding directly to that parameter.
Each concrete index is a lexical compile-time `usize`. The resulting HIR and
MIR contain fixed procedure calls and sequential blocks only—there is no pack
value, runtime type tag, loop, or backend variadic operation.

The symbolic source pass expands the loop body once using the unique element
TypeParameter and a symbolic index. Concrete checking expands one lexical block
per element and evaluates dependent `when` conditions with exact types and
indices. `len(pack)` uses a narrow constant-evaluator overlay containing only
the marker SymbolId and length; other attempts to evaluate the marker fail.
This avoids pretending that the pack is an array or string in compile-time
state. Static iteration blocks are sequential compile-time-selection HIR, not
runtime loop HIR, so they do not create a `break`/`continue` target. A nested
procedure cannot capture an enclosing pack because its elements are runtime
parameters of the outer specialization; it may declare and specialize its own
pack normally.

Public interfaces carry the pack name and fixed-prefix count separately from
the procedure type. Consumer calls send canonical graphs for every ordered tail
type to the defining package. Stable instance names hash explicit generic
arguments followed by a domain-separated pack count and those graphs. Final
instance interface rows retain the same pack-type graphs beside the exact
effect, return, and write contracts.

## Declaration products and demand-driven body work

Status: bootstrap phase ownership and incremental command-local scheduling.

Semantic analysis publishes an immutable package-interface payload for each
package. Initial collection and interface binding happen once. Each package
`when` records the foreign/export/deny context needed to append its selected
branch; when the condition becomes ready, only that branch is added to the same
declaration table. Unconditional declarations and existing IDs are not
recollected. Complete workspace compilation publishes type identity, selected
member names, member types, conditional choices, natural layout, and final named
constants as individual semantic products before the package-interface barrier.
Each ready task mutates only a private snapshot; deterministic publication moves
its completed packet into retained package state. Interface-synthesis discovery
uses those same products: a ready declaration/member/condition/constant stop is
blocked on one package `OpaqueSynthesisSet`, and no incomplete facet enters the
retained package state. Every stopped product retains its exact private semantic
package and constant inputs. A reached compile-time procedure is body-checked
once into that same packet so lexical bindings, expected types, and branch facts
are available to its provider site. A `TypeMembers` task with structural
synthesis additionally checks and retains the authored member prefix up to the
hole, but publishes neither that prefix nor a nominal member facet. The
coordinator source-orders and deduplicates metadata rows, then builds each
obligation against its owning packet; it never merges task-local IDs or reruns a
declaration, condition, constant, or body product. A declaration/member hole may
defer an otherwise untypeable reached body until accepted source creates the
successor generation. Rejecting direct semantic test and subsystem entry points
use the same product operations through a package-local sequential coordinator;
there is no aggregate interface-discovery compatibility path.

Body workers never append directly to the coordinator's retained value. Each
returns an exact semantic append packet and one local HIR arena; the coordinator
publishes those packets into its canonical append-only package tables. Workspace
compilation retains that live `PackageBodyWorkState` after finalization: its
work rows, procedure results, and semantic products preserve one append-only
index domain for later discoveries. Direct subsystem entry points return that
same state rather than transferring its semantic tables and products into a
reduced carrier. Every SymbolId, ScopeId, and TypeId in those arenas belongs to
the accompanying package. Workspace consumers retain HIR-local IDs:
effects, denials, metadata/obligation context, native interop, validation,
parsed assembly, and MIR all select procedure arenas directly. A transitive
agent-context walk carries the owning arena beside each found procedure, so an
expression ID is never interpreted in a sibling product. The former reduced
body result, HIR projection, and package MIR container are deleted; direct
subsystem tests traverse the same product-owned arenas and lowering operations
as workspace compilation.

Definite-initialization and agent loop-range inference complete inside the
isolated procedure task before publication. Initialization diagnostics
therefore belong to the exact body product, while derived loop facts travel in
the same task-local semantic-site suffix and are remapped once during canonical
publication. Package finalization checks only target-wide type constraints and
never reconstructs HIR.

The body coordinator no longer retains one `BodyChecker` while walking a
package and its growing instance vector. Seed materialization is separate, then
each package-level procedure or concrete specialization is checked by a fresh
checker over the published append-only body state. A body may publish new
specialization records, but those records become later roots. Each concrete
record retains its complete type/value substitution environment in addition to
its public specialization key; this distinction matters for a nested generic
procedure whose body uses compile-time bindings inherited from an enclosing
specialization. Its concrete parameter scope retains the static-pack marker and
element parameters, so the fresh checker reconstructs the complete active pack
without consulting the discoverer's transient state. This is the independent
reconstruction boundary for procedure products. Lexically nested declarations
also publish later roots instead of recursively checking their bodies. A nested
root snapshots the enclosing concrete environment, including the pack marker
which must remain an illegal capture, while its lexical symbol and signature
remain in the completed enclosing root. Each authored or concrete HIR procedure
is therefore produced by one root invocation.

Those roots are live rows in the command's semantic product graph rather than
facts recorded after package checking. Initial authored rows depend on the
completed package interface. A nested procedure or locally discovered concrete
specialization is appended between frozen waves and depends on the exact root
which published its environment. Root diagnostics are task-local and merge in
product order. Invalid recoverable HIR completes its scheduling row but leaves
the containing body state invalid, allowing later authored roots to be checked
without permitting effects or lowering to consume the package.

The coordinator retains the published package and constants while a
`ProcedureBodyTaskInput` owns one private view frozen at explicit counts for
every append-only table. Its TypeStore and SymbolTable are append-only overlays:
existing rows are read directly from the canonical tables, new rows remain
task-owned, and bindings added to an existing scope are recorded separately
without mutating that scope. ConstantTable uses the same prefix-overlay model
for immutable package constants and task-local lexical constants.
Declaration-closed file scopes, imports, imported documentation, native
bindings, and package conditional regions are direct immutable views and are no
longer represented in the body append packet. Owned-scope, aggregate/enum,
parametric, specialization, imported semantic, dependent-type recipe,
semantic-site, and declaration-denial tables use read-only-prefix/local-suffix
views as well: a task's raw vectors own only its new rows. Imported rows in this
boundary are symbols, types, concrete procedures, outbound type requests, and
effect/return/write contracts. Recipe rows describe required integer expressions
and deferred element counts, value expressions, and type applications.
Aggregate layout publication, procedure-specialization promotion,
required-integer refinement, and site enrichment use global combined-table
indices but map mutable access explicitly to the task suffix; prefix rows remain
immutable. The worker returns only a
`ProcedureBodySemanticAppend`, one procedure-local HIR arena, diagnostics, and
discovered roots; it never aliases or returns a replacement for
`PackageBodyWorkState`. The coordinator validates the work index, root symbol,
and complete prefix, then appends type/symbol rows and every semantic side table
in product order before exposing discovered roots. It also records every
canonical TypeId first appended by that publication against the exact procedure
product. Declaration-baseline types use the package-interface barrier instead.
This producer table lets later type facets depend on one body product without a
coarse all-package-body edge.

The result boundary is procedure-local and every currently ready root receives
the same frozen prefix. Suffix IDs remain private to each task. After the whole
wave joins, the publisher translates them into the grown canonical tables,
interns equal structural types, and merges equal procedure and nominal type
specializations without retaining duplicate roots, scopes, parameters,
members, or nominal TypeIds. The closed-wave executor invokes those isolated
workers with a bounded count; task-indexed results and diagnostics are consumed
only after join. Qualification compares one-worker and four-worker product
graphs, diagnostics, semantic table sizes, and final LLVM bytes.

Named constant products carry both `ConstantValue` and checked `TypeId`.
Package-interface finalization installs that type payload into its retained
declaration symbol before validation. Constant evaluation also retains a ready
value's type in evaluator-owned state, so reading an immutable published
constant never rewrites a declaration merely to recover an inferred type.

The compiler closes procedure work through workspace-wide ready waves. Once
package interfaces are complete, import order does not serialize independent
authored bodies: pending roots from every package share one bounded worker set.
Checking a caller can still demand a concrete public generic body from its
dependency. A portable procedure-demand packet contains canonical interface
type graphs, exact value arguments, ordered static-pack types, the full semantic
digest, and the stable native instance name. The owning package sorts and
deduplicates each current ready set before looking up individual products. A
new owner body has an explicit graph edge to the exact completed consumer body
which exposed it. No consumer-local TypeId crosses the boundary, and
materialized owner TypeIds live only in append-only package semantic tables.

Each retained package owns one append-only row for every external demand seen
while its package interface remains selected. Authored roots are always
selected; current demands select their exact owner rows; and dynamically
discovered children follow their earlier prerequisite. An unseen demand checks
one new product. A removed demand only changes this selected projection, so
completed semantic
rows, HIR, product IDs, and diagnostics remain immutable and can be selected
again without rechecking. A matching package-local specialization is promoted
in place to the canonical external name while retaining its SymbolId and HIR.
A source or dependency-interface change replaces the complete
`CompiledPackage`; no generation counter or separate body key is
needed.

Only selected procedures publish transitive requests and enter metadata,
effects, interfaces, validation, or lowering. Each result retains the outbound
requests it created. A selected-HIR symbol scan also finds reuse of a concrete
import proxy created by another product, so deactivating the first product does
not lose a still-live transitive owner demand. This is direct semantic
selection, not an aggregate reuse/rebuild policy or a second body
representation.

After each global worker join, package-local semantic suffixes publish in
PackageId and work order, then graph states and diagnostics publish in
SemanticProductId order. Selection and outbound demands are recomputed only
between waves. The fixed point is complete when no body is pending and neither
the selected program nor its canonical demand sets change. This makes worker
count a scheduling choice while retaining direct, deterministic package-local
ID interning.

That fixed point exposes the complete source-semantic TypeId prefix. The
coordinator appends one `TypeAbiClassification` product per TypeId and evaluates
all packages in one bounded read-only ready wave. Every row depends explicitly
on the selected target. A declaration-baseline type also depends on its package
interface; a type appended by body publication instead depends on the exact
procedure product recorded as its producer. Classification returns `Illegal` as
a completed semantic answer, not as a failed task. The coordinator publishes
the TypeId-indexed table and graph states in product order, and native
validation, C-header generation, and LLVM lowering consume that table rather
than invoking the classifier independently.

MIR reads that ABI-complete semantic TypeStore immutably. A source pointer value
retains its checked Pointer TypeId. A compiler-created local, global, member, or
index address instead uses the canonical `rawptr` representation and records
the addressed semantic TypeId directly on its MIR instruction. Verification
checks that pair and uses it for atomic pointee rules. Lowering therefore cannot
append an unclassified synthetic-pointer suffix after the ABI wave.

Compile-time expression type preflight and early compile-time procedure checks
operate on private copies. Their HIR is disposable and their only permitted
lasting product is explicit early synthesis context. They cannot become a
hidden preliminary body pass or contaminate the declaration baseline. This is
command-lifetime reuse, not a persistent compiler cache: a new `draftc` process
reconstructs all semantic state from authored and committed generated source.

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
being misreported as caller mutation. Concrete flow-SCC closure makes chained
write-through helpers independent of declaration order. Higher-order FlowCall
rows recursively retain the typed arguments supplied to the callback, including
their own finite procedure contracts, and the same shape crosses package
interfaces. Callees and arguments are snapshotted in source evaluation order
before a call's write-back becomes visible, so later argument effects cannot
rewrite earlier values.

Initial direct discovery exposes every syntactically named call edge. Procedure
return and caller-visible pointer-write contracts then close over explicit
dependency-first SCCs. A returned finite procedure target may add a concrete
edge; only that graph refinement rebuilds the SCC condensation, and only members
of one changing component are revisited. During target discovery an absent local
return path is lattice bottom rather than an arbitrary callback. After concrete
edges stabilize, any path still absent becomes a final unknown fact and closes
through the same components. This removes the selected-body/global flow replay
while preserving source-order-independent may-target semantics.

The effect phase uses the resulting immutable direct graph. It identifies
strongly connected components without recursion on the compiler's C++ stack,
then closes the condensation graph from callees to callers. Only procedures
inside one legal recursive component iterate together; independent and acyclic
components never participate in a package-wide effect retry. Component members
use canonical procedure-row order, and independent ready components use their
smallest member as the deterministic tie-break. Exact call-site summaries are
derived only after every component is closed and are the summaries consumed by
lexical denial checking.

Direct and closed procedure contracts have distinct immutable payload types.
Each direct row is discovered independently against the same bottom source-row
domain and immutable native/imported terminal contracts; no earlier source body
can enrich a later direct product. Flow/effect closure receives those rows plus
the exact selected procedure-owned HIR arenas and owns every derived return,
write, finite-target, and transitive-effect fact. Each HIR-local expression ID
remains paired with its owning procedure, including in the call-site table
consumed by denials. Denial traversal reads the same selected procedure products
and never reconstructs a package HIR. The compiler retains the direct payload
separately from the closed SCC result so downstream code cannot accidentally
treat a local fact as transitive.

Final imported effect/return/write contracts are a separate immutable closure
input built from already-closed dependency interfaces. Imported procedures are
terminal direct-summary rows in the consumer graph, including an explicit
unknown row when no audit exists. They therefore participate in the same
dependency-first condensation order as local and native procedures, while no
post-body pass clears, repopulates, or changes `SemanticPackage` tables.
Provider obligation context consumes this same exact payload, so the compiler
does not describe a preliminary imported contract to Codex after closing a
different one for denials.

The compiler attaches these retained payloads to live semantic-product rows.
One `DirectEffectSummary` row maps to one selected HIR-bearing body product and
runs in a bounded worker wave. `ClosedEffectScc` rows map one-for-one to the
dependency-first components above; each row names its source direct products,
its exact earlier component dependencies, the target, and completed imported
component products. One independently checked `DenialResult` row then names its
exact body and owning closed component. Diagnostics remain task-local until
canonical graph publication, so a denial violation is the `Error` state of the
specific procedure product rather than an unstructured package failure.

A body or interface source transition supersedes the affected direct,
closed-SCC, and denial rows transitively through package consumers while
retaining reusable procedure-body products. The rebuilt payload vectors align
with the successor product IDs. The algorithmic package-wide flow and effect
replay paths are gone.

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

A consumer which later supplies concrete generic arguments reports a portable
owner request from its private declaration attempt. Before the request can
become work, every runtime-bearing argument graph blocks on its exact requester
natural-layout products. The coordinator then imports the argument graphs into
the defining package and canonicalizes one command-local key from the owner
PackageId, template SymbolId, and ordered owner-local ParametricArguments. Equal
requests from unrelated consumers therefore share one `TypeNaturalLayout`
product without a digest or requester-local identity.

The owner task evaluates the recipe with the normal compile-time interpreter
and publishes one complete package-independent `InterfaceTypeGraph`. If that
attempt reaches another package-owned recipe, it reports another portable
request and gains an explicit edge to the nested canonical demand. The consumer
retries only its blocked declaration product and imports completed dependency
graphs into that task-local package; no package declaration table is rebuilt.
The result is not appended to an already-complete `PackageInterface`.
Private requester nominals returning through the owner graph resolve to their
original local TypeId. This preserves private implementation details without
making consumer semantics read dependency source.

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
arguments. Import records that application key beside the canonical structural
TypeId in the retried task-local package, so ordinary type lookup consumes the
exact array/tuple/etc. without changing structural identity. Pending whole-
application markers and their package-local recipe indices never cross this
boundary. The completed graph remains the immutable result of its canonical
natural-layout demand rather than becoming mutable package-interface state.

Owner requests close transitively across package imports. If package A requests
a concrete type from B and B's resulting layout requests a private recipe from
C, B's product reports the portable nested request. The coordinator interns
that request into C's exact canonical key and adds the resulting product as a
dependency of B's product. Once C publishes its graph, B retries from its
private owner snapshot with that graph imported. Product states and graph cycle
diagnostics replace request digests, progress sets, owner recursion, and package
rebuilds.

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

The versioned `draft.resolved-program.v6` hash records the selected root and
assertion mode beside compiler content v140. `build`, `resolve`, and `judge`
expose the same explicit flag so a provider-free manifest cannot be replayed
under a different mode. Test and benchmark compilations deliberately override
the release choice to assertions on and receive their own resolved validation
digest; disabling
release assertions must never weaken the validation program that authorizes a
release.
