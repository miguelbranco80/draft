# Draft: Core language

Part of the [Draft language specification](../../README.md).

[← Overview](../../README.md) · [Next: Types, memory, and runtime →](02-types-memory-runtime.md)

<a id="section-3"></a>

## 3. Folder packages

A directory is one package: a namespace, visibility boundary, compilation unit,
and synthesis-context boundary. Every Draft source file in the directory declares
the same short package name. Every program target selects one package inside one
canonical workspace root; an aggregate command may build several such targets
independently. A `draft.workspace` marker establishes that root for packages
below it. When no marked ancestor exists, a directly selected package is a
standalone workspace. The marker may record operator build/run defaults, but it
does not enumerate source files or change language semantics.

An import path resolves relative to that workspace root or through one explicit
import-prefix mapping to a dependency root; `core/...` resolves in the selected
compiler distribution. Ambiguous mappings, paths escaping a mapped root, and
ambient searches through unrelated directories, environment paths, or the
process working directory are errors. A package's semantic identity is its
normalized root-relative import path paired with its root identity. The
workspace root has the fixed identity `workspace`; a dependency root uses its
pinned content identity; and the `core/...` root uses the selected compiler
distribution's content identity. Physical filesystem paths are not semantic.

```draft
package jpeg

import core/io
import core/heap as heap
import codec/bits as bits
```

Imports are explicit and file-local. An alias is an ordinary name in that
file. Imports do not re-export names, and the package import graph is acyclic.

Every regular `.draft`, `.s`, `.S`, and `.asm` file directly in a selected
package directory participates without a source-list build file. A filename of
the form `name@<target.file_tag>.<extension>` participates only for the exact
[target profile](02-types-memory-runtime.md#target-profile) identified by
`target.file_tag`; an unqualified filename participates for every supported
profile. After removing the target qualifier, `*_test.draft` and
`*_bench.draft` participate only in their explicit validation commands and as
typed synthesis context; they are not part of ordinary builds or resolution
success criteria. Compile-time `when` handles smaller target differences inside
a Draft source file.

The target profile fixes the assembler dialect, preprocessing rule, and tool
contract for each assembly extension; `.S` has no host-dependent implicit
meaning. Symbols crossing between Draft and package assembly use explicit
[`foreign`](04-native-interop.md#foreign-imports) or
[`export`](04-native-interop.md#c-exports) C-ABI declarations. Assembly cannot
name private Draft symbols.

Declarations are visible throughout their package and private to it by
default:

```draft
Marker :: struct {
    kind:   Marker_Kind,
    offset: usize,
}

pub decode :: proc(input: []u8, output: []Pixel) -> Decode_Result {
    return decode_pixels(input, output)
}
```

`pub` exposes a declaration to importing packages. C linker visibility is
defined separately by C export declarations.

A conventional `package.draft` may collect central public types and package
documentation, but it is an ordinary source file. Likewise, package directory
names such as `app`, `cmd`, or `lib` are ordinary names and carry no language or
build meaning.

The compiler derives a canonical public interface from all `pub` declarations.
Tools, dependent packages, and agents consume this interface without reading
every implementation file.

<a id="section-4"></a>

## 4. Declarations and procedures

Declarations follow an Odin-like form:

```draft
count: u32
limit := 128
Buffer_Size :: 4096
```

`_` is the discard pattern. It may appear wherever a declaration, assignment,
destructuring operation, iteration, or payload pattern would otherwise bind a
name:

```draft
(_, marker) := read_marker(input)
```

Each `_` creates no binding or storage. The corresponding expression is still
evaluated and type-checked. A parenthesized pattern such as `(a, b)` destructures
a tuple of the same arity; `_` may occupy any element. In a declaration,
`a, b: T` groups names sharing one explicit type. In an assignment, an
unparenthesized comma separates equal numbers of lvalues and right-hand
expressions and never destructures a tuple. A procedure parameter list separates
parameter declarations with commas, and each declaration may similarly group
names before one type. Iteration has its own `for value, index in iterable`
binding grammar.

An untyped integer initializing an inferred runtime binding defaults to `int`;
an untyped floating value defaults to `f64`. If it is not representable, an
annotation is required. A numeric `::` constant remains untyped until a use
requires a concrete type.

`proc` is the single callable abstraction. The absence of a result arrow means
the procedure returns no value:

```draft
log_message :: proc(message: string) {
}

checksum :: proc(input: []u8) -> u64 {
    return checksum_bytes(input)
}
```

A named procedure parameter may have a compile-time constant default. The
default applies only to that one binding, so a grouped parameter declaration
cannot carry `=` and `_` cannot have a default:

```draft
adjust :: proc(value: i64, scale: i64 = 2, offset: i64 = 0) -> i64 {
    return value * scale + offset
}

plain := adjust(10)
shifted := adjust(offset = 3, value = 10)
```

A call may bind fixed parameters by name in any declaration order. Positional
arguments must precede named arguments, every fixed parameter is supplied at
most once, and every omitted parameter must have a default. The name/default
contract is available only through a direct procedure declaration; a procedure
value has only its structural signature and therefore accepts the complete
positional list. Defaults are checked in declaration scope, converted to the
declared parameter type, and required to be compile-time constants. They cannot
depend on runtime parameters or unresolved parametric bindings, or have a
parameter type which remains symbolic. Defaults are not valid in standalone
procedure types or on a static pack.

A bare final `..` has a separate native-interoperation meaning in a `c proc`:
it marks an unnamed C variadic tail. It has no binding name or Draft value and
is distinct from both a named `values: ..type` static pack and the `...`
synthesis construct. Its declaration and call rules are specified in
[native interop](04-native-interop.md#c-variadic-imports-and-calls).

Procedures live in packages or lexical scopes. Structs contain data; package
procedures operate on that data explicitly:

```draft
pub resize :: proc(image: ^Image, width, height: u32) -> Error {
    return resize_image(image, width, height)
}
```

Nested procedures keep implementation details close to their caller. They do
not capture enclosing runtime state; required state is passed explicitly.

```draft
decode :: proc(input: []u8) -> Decode_Result {
    read_marker :: proc(input: []u8, offset: ^usize) -> Marker_Result {
        return parse_marker(input, offset)
    }

    offset: usize
    marker := read_marker(input, &offset)
    return decode_from_marker(input, marker)
}
```

Nesting changes lexical visibility, not procedure representation. A nested
procedure may reference package and imported declarations, predeclared names,
the Draft context, and enclosing compile-time declarations and parametric
type or value parameters that are in lexical scope. It may not reference an
enclosing invocation's parameters, locals, or iteration bindings, even when
those bindings are immutable. Those values must be passed as explicit
parameters. An enclosing static argument pack is likewise not capturable: it
describes the outer specialization's runtime arguments as well as compile-time
types and length. A nested procedure may declare and specialize its own pack.

A normal call propagates the current hidden runtime-context pointer to the
nested procedure; that is ordinary calling-convention behavior rather than
capture. A nested procedure has no environment object, has the same
procedure-pointer representation as an equivalent package procedure, and may
outlive the outer invocation when passed or returned. Nesting neither requests
nor implies inlining.

Parameters are immutable bindings. A mutable local copy is explicit. Values
have ordinary value semantics unless their type is a pointer or an explicitly
defined view.

### Source text and literals

Source is UTF-8. Identifiers match `[A-Za-z_][A-Za-z0-9_]*`; `_` alone is the
discard pattern. `true` and `false` are `bool` literals. `file` and `folder`
are reserved attachment keywords and cannot be used as identifiers; their
attachment role is described below. Tokens use longest match.

The lexer inserts `;` at a newline or end of file after an identifier or
literal, `break`, `continue`, `return`, `)`, `]`, `}`, postfix `^`, or `---`;
trailing comments are ignored. An explicit `;` is equivalent and separates the
clauses of a three-clause `for`. No semicolon is inserted inside `(` or `[`,
after an operator that requires a following operand, or after a comma. The
parser treats immediately following `file` and `folder` clauses as
continuations of an active `docs`, `judge`, or `...` construct and suppresses
insertion before them; an explicit semicolon or the first other non-trivia token
ends the attachment group. Consequently, `} else {` remains on one line.

`c` is contextual rather than globally reserved. Before `proc` it selects the C
calling convention; before `struct`, `enum`, or `union` it selects C layout,
as defined by sections 5 and 12. Elsewhere it is an ordinary identifier, so a
project may still choose it as a local binding or import alias; those uses
receive ordinary semicolon behavior. The distributed C scalar package is named
`core/c_abi` and does not rely on that contextual-name freedom. `align` is the reserved
aggregate-layout modifier defined by section 5; Draft has no general annotation
prefix or user-defined declaration modifier. `packed` is the reserved
per-field struct-layout modifier defined by section 5. `bits` is recognized as
the `bits(N)` field modifier only at the start of a struct field and only when
immediately followed by `(`; elsewhere it remains an ordinary contextual name
with ordinary semicolon behavior.

`^` is exclusively pointer syntax: it begins pointer types and follows an
expression to dereference it. Bitwise XOR is binary `~`; unary `~` remains
bitwise complement. Prefix and binary position distinguish the two `~`
operators in the same way that they distinguish unary and binary `-`, so line
breaks require no operator-specific lookahead rule. There is no `^=` compound
operator: `pointer^= value` is the whitespace-insensitive spelling of
`pointer^ = value`, an assignment through the pointer.

Integer literals are decimal or use `0b`, `0o`, or `0x`; underscores may
separate digits. A decimal floating literal is digits, `.`, digits, and an
optional exponent, or digits followed by a required exponent; an exponent is
`e` or `E`, an optional sign, and digits. Thus `.5` and `1.` are invalid. Quoted
strings and rune literals accept `\\`, `\"`,
`\'`, `\n`, `\r`, `\t`, `\0`, `\xNN`, and `\u{...}` escapes; a rune literal
contains one Unicode scalar. Raw backtick strings preserve their UTF-8 bytes
across lines. Escaped Unicode scalars in strings are encoded as UTF-8, while
byte escapes permit arbitrary string bytes.

A rune literal has standalone type `rune`. When an expression position instead
expects a signed or unsigned integer type, including a distinct integer type,
the literal adopts that type if its scalar value is exactly representable.
Expected types come from assignments, returns, fixed procedure arguments,
aggregate members, switch subjects, conditional branches, and the concrete
operand of a binary operator, so both `byte_value == 'q'` and
`'q' == byte_value` are valid when `byte_value` is `u8`. A context-free literal
still has type `rune`, and two rune literals do not invent an integer context.
This rule does not apply to floating, enum, endian-storage, or boolean-storage
types and does not implicitly convert a variable or named constant whose type
is `rune`. For non-ASCII text the literal denotes the Unicode scalar rather
than its UTF-8 encoding; for example, `'é'` is 233 and not either byte of its
two-byte UTF-8 representation.

Composite literals have these Draft 1 forms: `[N]T{values}` for fixed arrays,
`T{field = value, ...}` for structs, `T{field = value}` for one selected union
field, `(a, b)` for tuples, and contextual `.case` or `.case(value)` for
enum and variant alternatives. Arrays reject excess elements and zero-fill
an omitted tail; struct fields are unique and omitted fields take their zero
value. In an `if`, `for`, or `switch` header, an unparenthesized `{` begins the
statement body, so a composite literal there must be parenthesized.

### Expressions and evaluation

Operators bind from highest to lowest as follows:

| Level | Forms | Association |
| --- | --- | --- |
| Postfix | call, index or slice, member `.`, dereference `^` | left |
| Prefix | `+ - ! ~ &` | right |
| Multiplicative | `* / % << >> &` | left |
| Additive | `+ - | ~` | left |
| Comparison | `== != < <= > >=` | none |
| Logical AND | `&&` | left |
| Logical OR | `||` | left |
| Conditional | `value if condition else value` | right |

Assignment and compound assignment are statements, not expressions. The
compound forms are `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `~=`, `<<=`, and
`>>=`; each evaluates its lvalue once, applies the corresponding binary
operator, and stores the result.

Unary `+` and `-` require a numeric value, unary `~` requires an integer and
computes its bitwise complement, `!` requires `bool`, `&` takes the address of
addressable storage, and postfix `^` dereferences a data pointer. Binary `+`,
`-`, `*`, and `/` require one common concrete numeric type after contextual
conversion of untyped constants; `%`, binary `~` (XOR), and the other bitwise
operators require one common integer type. A shift accepts an integer count and
returns the left operand's integer type. `&&` and `||` accept and return `bool`.

Member selection automatically dereferences one pointer-to-one base. If `value`
has type `^T`, `value.member` is equivalent to `value^.member`; the same rule
applies to a numeric tuple selector such as `pair.0` and independently at each
dot in a chain. The selected member retains the addressability, alignment, nil
behavior, and effects of the explicit spelling. The rule does not apply in any
other expression context and does not apply to `[^]T`, `rawptr`, or `cstring`.
A multi-pointer must first select an element, for example
`records[index].field`. Explicit postfix `^` remains available for reading,
writing, or taking the address of the complete pointee, and
`value^.member` remains valid.

When every numeric operand is untyped, integer-only arithmetic, remainder,
bitwise, and shift operations use arbitrary-precision integers and produce an
untyped integer; `/` truncates toward zero, `%` is `a - (a / b) * b`, and
bitwise operations and right shift use an infinite two's-complement
representation. If either untyped operand
is a decimal floating constant, `+`, `-`, `*`, and `/` use exact rational
arithmetic and produce an untyped floating constant, while remainder, bitwise,
and shift operators are invalid. Constant division by zero and a negative shift
are compile errors. Comparisons of untyped numeric constants use the same
domains and produce `bool`. Contextual conversion occurs only when a concrete
type is later required.

Equality is defined for booleans, numeric and boolean-storage scalars, `rune`,
matching enums, and matching data or procedure pointers; pointers compare
address bits and may compare with contextual `nil`. Ordering is defined for
numeric values and `rune`. Endian storage values support equality but require
conversion to their native counterpart for arithmetic or ordering. Aggregates,
slices, and strings use library comparison. User-defined operator overloading
is absent in Draft 1.

A call evaluates its callee and then every explicit argument in source order,
regardless of the physical parameter selected by a named argument. Omitted
defaults are closed constants and introduce no runtime evaluation. An
assignment evaluates lvalue address and index expressions from left to right,
then right-hand sides from left to right, then performs stores from left to
right. A conditional expression evaluates its condition first and then only its
selected value. Other operands evaluate left to right; `&&` and `||`
short-circuit.

Concrete numeric types do not convert implicitly; an untyped numeric constant
converts when representable in the required type. The exact contextual typing
of a rune literal described above is also a source-literal rule, not a
conversion from concrete `rune`. An assignment, return, or argument position
supplies its expected type through the complete expression; a compound
assignment uses its left operand's type. `cast[T](value)` is the sole explicit
value-conversion form in Draft 1:

- Integer-to-integer conversion reduces the mathematical value modulo the
  target width and interprets the resulting bits using the target signedness.
- Integer-to-float and float-to-float conversion round to the target IEEE type,
  using nearest with ties to even.
- Float-to-integer conversion truncates toward zero and traps for NaN or an
  out-of-range result.
- Pointer-to-pointer, pointer-to-`rawptr`, and `rawptr`-to-pointer conversion
  preserve the address. Pointer or `rawptr` conversion to or from `uintptr`
  preserves the address bits.

Aliases use their target type's rules. A `distinct` value converts explicitly
to or from its underlying type; conversion into `rune` traps unless the result
is a Unicode scalar value. Enums convert explicitly to or from their backing
type, and conversion into an enum traps unless the value names a member.
Native and endian-specific scalar counterparts convert by value. Conversion
from `bool` to `b8`, `b16`, `b32`, or `b64` produces zero or one; conversion
back is false for zero and true otherwise.

A user-defined distinct type has its underlying layout and zero value but a
separate identity. Underlying operators remain available with the distinct type
substituted for their operand and result types; comparisons still return
`bool`. Untyped constants may fill those operand positions, but concrete
underlying and distinct values never mix implicitly. The built-in `rune` permits
equality and ordering only; arithmetic requires conversion through `i32` and a
checked conversion back.

The data-pointer rules cover `^T`, `[^]T`, `rawptr`, and `cstring`. Procedure
pointers do not cast; only a compatible procedure or `nil` can initialize one.

Other source and target pairs are invalid in Draft 1.

Concrete signed and unsigned integer arithmetic wraps modulo the type width;
signed values use two's-complement representation. `/` truncates toward zero;
`%` has the dividend's sign. Either operation by zero traps. Signed minimum
divided by `-1` traps, while its remainder is zero. Left shift discards high
bits; signed right shift is arithmetic and unsigned right shift is logical. A
negative or out-of-range shift count traps.

Floating operations use IEEE behavior, round each result to its declared type
using nearest with ties to even, and use no excess precision or fast-math
transformations in Draft 1. Typed compile-time operations use the same rules as
runtime operations; a required constant that would trap is a compile error.
A runtime trap never returns, runs no `defer`, and terminates the current
execution through a mechanism fixed by the target profile.

### Constants and compile-time evaluation

`::` binds an immutable compile-time value. Numeric and aggregate constants,
types, and procedures use the same declaration form:

```draft
Buffer_Size :: 4096

Decode_Mode :: enum {
    Baseline,
    Progressive,
}

decode :: proc(input: []u8, output: []Pixel) -> Decode_Result {
    return decode_pixels(input, output)
}
```

A data constant has no mutable storage or stable address. Taking
`&Buffer_Size` is invalid. Using a constant in runtime code materializes its
value where required; string and aggregate data may be emitted into read-only
storage without giving the constant source-level identity.

Untyped integer constants retain arbitrary precision during compilation and
are checked for representability when converted to a machine type. Untyped
decimal floating constants remain exact rationals derived from their spelling
until contextual conversion, then round once under the runtime IEEE rule.
Constant values may include booleans, numbers, strings, enums, arrays, structs,
variants, unions, procedure identities, and types.

A type value has the compile-time-only type `type`. It may be named by a
constant, compared for exact identity, inspected, and transmitted through a
package interface, but it has no runtime representation. The compiler rejects
any path that would store or pass a `type` value at runtime. Structural queries
and their exact applicability are defined in
[the type and memory specification](02-types-memory-runtime.md#compile-time-type-values-and-structural-inspection).

An expression is eligible for compile-time evaluation when all inputs and
transitive calls are compile-time values and no runtime-only dependency is
reached. The compiler must evaluate an eligible expression when a constant is
required and otherwise reports that it is not a constant. Cycles among constant
declarations are errors; terminating procedural recursion is permitted within
compiler resource limits.

Evaluation may use conditionals, loops, `switch`, parametric code, target and
type queries, and layout queries. Runtime globals, Draft context, foreign
calls, native assembly, and other runtime-only state are unavailable. Resource
exhaustion is an error only when a constant is required. Optional folding may
be abandoned and must preserve any runtime trap.

Constants provide array and SIMD lengths, enum values, `case` labels, alignment
and layout parameters, parametric arguments, `when` and `static_assert`
conditions, assembly immediates, and static or thread-local initializers.
Target-dependent evaluation is cached separately for each selected target
profile.

```draft
make_mask :: proc(bits: uint) -> uint {
    result: uint
    for i: uint = 0; i < bits; i += 1 {
        result |= 1 << i
    }
    return result
}

Header_Mask :: make_mask(12)
```

### Control flow

Braces delimit compiler-defined regions rather than acting as a general macro
or metaprogramming mechanism. The language has a small closed set of braced
grammar categories: statement blocks, declaration or member lists, expression
regions belonging to constructs that explicitly permit them, `switch` case
lists, and assembly instruction lists. The construct before the brace and the
syntactic position of the complete construct determine which category is
parsed; later name and type checking does not have to reinterpret the parsed
category.

A bare block is valid wherever a statement is valid. It introduces a lexical
scope for local names, lifetimes, and `defer`, but it produces no value and
cannot occupy an expression position:

```draft
prepare()

{
    scratch: [4096]u8
    decode_into(input, scratch[:])
}

finish()
```

A bare block is not a namespace. Its local declarations cannot be qualified or
used after the block, and packages remain the language's named namespace
construct. Other braced constructs specify whether they introduce a lexical
scope or contribute declarations to the surrounding scope. In particular,
`when` contributes its selected contents to the surrounding scope, while
`deny` chooses an expression, statement, declaration, or member region from
its syntactic position as described in [section 13](05-denials-validation.md#section-13).

`if` requires a `bool`; integers, pointers, and aggregates have no implicit
truthiness. Statement branches use lexical blocks:

```draft
if width == 0 || height == 0 {
    return Error.Invalid_Size
} else if width > Max_Width {
    return Error.Too_Wide
} else {
    initialize_image()
}
```

The conditional expression evaluates one branch and requires both values to
have a common result type:

```draft
larger := a if a > b else b
```

An outer expected type is propagated into both values. Without one, a direct
`nil`, contextual enum alternative, or contextual variant alternative
may discover its type from the concrete opposite value, symmetrically on either
side. If both values require context and there is no outer expected type, the
expression is ambiguous. Parentheses and denial wrappers do not change this
discovery rule. A nested conditional requests outer context exactly when both
of its own value branches require context.

`for` is the loop construct. It supports infinite, conditional, clause, and
array, slice, or string iteration forms:

```draft
// Infinite.
for {
    service_next_request()
}

// Conditional.
for offset < len(input) {
    offset += consume_from(input, offset)
}

// Initialization, condition, and post statement.
for i: usize = 0; i < len(output); i += 1 {
    output[i] = 0
}

// Array or slice values with explicitly requested indices.
for value, index in input {
    output[index] = transform(value)
}

// Strings expose bytes and byte offsets, not decoded Unicode scalars.
for byte, offset in text {
    consume_byte(byte, offset)
}

// A flat tuple pattern destructures each array or slice element.
for (key, value), index in entries {
    consume_entry(key, value, index)
}
```

One iteration binding receives the value. A second binding explicitly requests
the index. The iterable expression is evaluated once before the loop. For an
array or slice of `T`, the first binding has type `T`; the second has type
`usize` and takes successive values starting at zero and less than `len`. For a
`string`, the first binding has type `u8` and the index is the corresponding
zero-based byte offset. String iteration observes the exact immutable bytes,
including embedded zero and every byte of a multi-byte UTF-8 encoding; it does
not validate or decode Unicode.

For an array or slice whose element type is a tuple, the value position may be
one parenthesized, flat tuple pattern. Its arity must equal the element tuple's
arity. Each retained name receives a copy of the corresponding tuple member,
and the optional index remains the separate binding after the closing `)`.
Nested patterns are not part of this form. `_` may discard the complete value,
any tuple member, or the index:

```draft
for value in input {
    consume(value)
}

for _, index in input {
    consume_index(index)
}

for (key, _), index in entries {
    consume_key(key, index)
}
```

Iteration values are copies; mutation of the underlying element uses explicit
indexing or a pointer. `break` exits the nearest loop or `switch`; `continue`
begins the next iteration of the nearest loop.

The same binding spelling statically expands a procedure's final `..type`
argument pack, but that form is compile-time specialization rather than a
runtime loop. Its exact binding, ordering, and direct-call rules are specified
under [parametric types and procedures](02-types-memory-runtime.md#parametric-types-and-procedures).

`switch` evaluates its subject once. Constant `case` labels use the subject's
type, comma-separated labels share a body, and an empty `case:` is the default:

```draft
switch marker.kind {
case .Start, .Restart:
    begin_segment(marker)
case .End:
    finish_segment(marker)
case:
    report_unknown(marker)
}
```

Cases never fall through. Each case body is a lexical scope; its locals and
`defer` calls follow the ordinary scope-exit rules. A `switch` over an enum or
variant must cover every alternative or provide a default. Variant
cases may bind the active payload; the binding is local to that case:

```draft
switch result {
case .ok(marker):
    consume(marker)
case .err(error):
    report(error)
}
```

`return` leaves the current procedure and supplies its declared result value.
`defer` accepts a procedure-call statement. Its callee and arguments are
evaluated from left to right when `defer` executes and saved by value; an
ordinary callee also saves the current hidden context pointer, while a `c`
callee has none. Deferred calls run in last-in, first-out order whenever the
scope exits through fallthrough, `break`, `continue`, or `return`.
For `return expression`, the result is evaluated and saved first; exiting-scope
deferred calls then run before the saved value is transferred to the caller.

`when` selects a declaration list, type member list, or statement list during
compilation according to its syntactic position. Unlike `if`, it does not emit
a runtime branch:

```draft
when target.os == .linux {
    Page_Size :: 4096
} else {
    Page_Size :: target.page_size
}
```

The condition follows the constant-evaluation rules above and must produce a
`bool`. When the condition is independent of an enclosing parametric
declaration, every branch is parsed but only the selected branch contributes
its contents and proceeds through name and type checking.

A statement `when` inside a parametric procedure may instead depend on that
declaration's compile-time type or value parameters. The source template is
checked symbolically in every possible branch, using the type refinements
defined by the type specification. Each concrete instantiation evaluates the
condition after substitution and retains only its selected branch. A rejected
operation or `static_assert` in another concrete branch cannot affect that
instance. Parameter-dependent selection is still compilation, not a runtime
branch, and no symbolic query or type value reaches MIR.

`when` introduces no lexical scope; declarations and members in the concrete
selected branch belong to the surrounding scope or type.

The filename qualifier in [section 3](01-core-language.md#section-3) handles
implementations that differ by whole files. `when` handles smaller target,
feature, ABI, and layout differences without a preprocessor or source-list
build file. It selects already parsed syntax and cannot construct or rewrite
syntax.

### Assertions

`assert` is a predeclared compiler intrinsic for runtime invariants. It accepts
a `bool` and an optional `string` message:

```draft
assert(len(input) > 0)
assert(offset < len(input), "offset is outside the input")
```

When the condition is false, `assert` reports the condition text, optional
message, and caller source location through `context.assertion_failure_proc`,
then does not return. The handler receives the condition text, message, source
file, line, and column; an omitted message is the empty string. If the handler
is `nil` or returns, the intrinsic traps directly. A build may disable runtime
assertions explicitly with `--assertions=off`; in that mode neither the
condition nor the message is evaluated. Programs must not rely on assertion
side effects. Disabling an assertion does not give the optimizer permission to
assume its condition, and always-required runtime validation uses ordinary
control flow and errors.
Runtime `assert` is invalid inside a `c proc`, which has no active Draft
context; `static_assert` remains available there.

`static_assert` checks a compile-time invariant:

```draft
static_assert(size_of(Header) == 16)
static_assert(align_of(Packet) >= 8, "Packet alignment is too small")
```

Its condition must be a compile-time `bool`, and its optional message must be a
compile-time `string`. A false condition is a compilation error, and
`static_assert` is never disabled. Both forms are language intrinsics rather
than ordinary package procedures so the compiler can preserve the original
expression and source location. Test expectations remain ordinary testing
library operations that may record a failure and continue a test; a failed
`assert` terminates the current execution.
