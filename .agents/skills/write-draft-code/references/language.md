# Draft language coding reference

Use this as a compact map of the implemented Draft 1 surface. Read the linked
normative section before relying on an edge case. Prefer copying a pattern from
`examples/language-tour` or another compiling example over inventing syntax.
The primary authorities are the
[`core language specification`](../../../../docs/specification/01-core-language.md)
and [`types, memory, and runtime specification`](../../../../docs/specification/02-types-memory-runtime.md).

## Contents

- [Packages and declarations](#packages-and-declarations)
- [Procedures and bindings](#procedures-and-bindings)
- [Literals and source rules](#literals-and-source-rules)
- [Runtime types and views](#runtime-types-and-views)
- [Aggregates and layout](#aggregates-and-layout)
- [Expressions and conversions](#expressions-and-conversions)
- [Control flow and cleanup](#control-flow-and-cleanup)
- [Constants, targets, and compile-time selection](#constants-targets-and-compile-time-selection)
- [Parametric declarations](#parametric-declarations)
- [Assertions and unchecked access](#assertions-and-unchecked-access)
- [Program entry](#program-entry)
- [Features Draft deliberately lacks](#features-draft-deliberately-lacks)

## Packages and declarations

Read specification sections 3–4 in `docs/specification/01-core-language.md`.

One directory is one package. Every selected `.draft` file in that directory
must declare the same short package name. Imports are explicit and file-local;
another file must repeat an import it uses. Imports do not re-export names, and
the package graph must be acyclic.

```draft
package image

docs "Image storage and transformation policy."

import core/result
import codec/bits as bits
```

Use `pub` for Draft package visibility. It does not export a C linker symbol.
Files are discovered automatically; there is no source-list build file.

Declarations use three principal forms:

```draft
count: usize                 // mutable, explicitly typed, zero initialized
limit := 128                 // mutable, inferred runtime type
Buffer_Size :: 4096          // immutable compile-time value
```

`name: T = value` combines an explicit runtime type and initializer. A local
`name: T = ---` deliberately omits zero initialization; reading unwritten bytes
is undefined behavior. `---` is not a general value and is invalid for globals.

Use `_` to discard a binding without suppressing evaluation. Parenthesized
patterns destructure one tuple. A comma without parentheses declares or assigns
parallel values instead:

```draft
(_, value) := read_pair()
left, right: u32
left, right = right, left
```

The number of tuple pattern elements must match the tuple. Parallel assignment
evaluates lvalue addresses/indices left to right, evaluates all right-hand
values left to right, and only then performs stores left to right. It does not
implicitly destructure one tuple result.

Package declarations are visible throughout the package and private unless
`pub`. A conventional `package.draft` is organizational, not semantically
special.

## Procedures and bindings

`proc` is the only callable abstraction. Absence of `->` means no result.
Group parameter names only when they share one type:

```draft
distance :: proc(left, right: Point) -> u64 {
    return squared_distance(left, right)
}
```

Parameters are immutable. Create an explicit local copy to mutate a value.
Arguments and operands evaluate left to right. A direct procedure call may use
named arguments after all positional arguments, and may omit parameters with
compile-time constant defaults:

```draft
adjust :: proc(value: i64, scale: i64 = 2, offset: i64 = 0) -> i64 {
    return value * scale + offset
}

plain := adjust(10)
shifted := adjust(offset = 3, value = 10)
```

Names/defaults belong to the declaration, not its procedure type or ABI. A
procedure value therefore requires the complete positional list. Defaults are
checked in declaration scope, cannot depend on runtime parameters or unresolved
parametric bindings, and require a concrete parameter type. Do not put defaults
on grouped, discard, or static-pack parameters or in standalone procedure
types.

Nested procedures are static procedures, not closures. They may use package
declarations, imports, predeclared names, context, and enclosing compile-time or
parametric names. They may not capture an outer invocation's runtime parameters,
locals, iteration bindings, or static argument pack; pass ordinary values
explicitly or declare a separate pack on the nested procedure.

```draft
outer :: proc(input: []u8) -> usize {
    count_nonzero :: proc(bytes: []u8) -> usize {
        count: usize
        for byte in bytes {
            if byte != 0 {
                count += 1
            }
        }
        return count
    }
    return count_nonzero(input)
}
```

An ordinary procedure pointer is `proc(...) -> T` and carries the hidden Draft
context in its physical ABI. A `c proc(...) -> T` is a distinct C procedure
pointer with no Draft context. A C procedure type may use bare final `..` for a
native variadic tail; read `interop-and-targets.md` before declaring or calling
one. Neither procedure kind is a closure.

## Literals and source rules

Source is UTF-8, but identifiers are ASCII letters/digits/underscore with a
non-digit first character. `_` alone is the discard pattern. `file` and
`folder` are reserved attachment keywords, not legal identifier names.

Integer literals support decimal, `0b`, `0o`, `0x`, and digit separators.
Decimal floats require digits on both sides of `.`; exponent-only forms such as
`1e6` are also valid. Strings and runes accept the documented escapes; raw
backtick strings preserve UTF-8 bytes and may span lines. A rune contains
exactly one Unicode scalar.

```draft
mask :: 0xff00_ff00
ratio :: 1.25e-3
newline :: '\n'
text :: `exact
multiline bytes`
```

The lexer inserts semicolons at newline/EOF after an identifier, literal,
`break`, `continue`, `return`, `)`, `]`, `}`, postfix `^`, or `---`. It does not
insert inside `(` or `[`, after a comma, after an operator that needs an
operand, or before an immediately attached `file`/`folder` clause of `docs`,
`judge`, or `...`. Keep `} else {` together. Break a long expression after an
operator or comma, not after a complete operand. An explicit `;` is required
between clauses of a three-clause `for` and otherwise has the same statement-
ending effect.

`^` is exclusively pointer syntax: it begins pointer types and follows an
expression to dereference it. Write binary `left ~ right` for bitwise XOR and
unary `~value` for bitwise complement. Prefix and binary position distinguish
the two `~` operators without a newline-sensitive rule. `pointer^= value` is
postfix dereference followed by ordinary assignment; XOR assignment is `~=`.

Composite literals are:

```draft
bytes := [4]u8{1, 2, 3, 4}
point := Point{x = 10, y = 20}
overlay := C_Value{bits = 42}       // exactly one union field
pair := (left, right)
mode: Mode = .read
result: result.Result[u64, Error] = .ok(42)
```

Omitted fixed-array elements and struct fields receive zero values. In an
`if`, `for`, or `switch` header, parenthesize a composite literal so its `{`
cannot be confused with the statement body.

Enum and variant cases use contextual `.case` or `.case(payload)` syntax.
Write `error == .none`, not `error == io.Error.none`; the latter is not Draft
member syntax. Add a type annotation when no surrounding expected type can
determine which enum or variant owns the case.

## Runtime types and views

Read specification section 5 in `docs/specification/02-types-memory-runtime.md`.

The scalar vocabulary includes:

- `bool`; storage booleans `b8`, `b16`, `b32`, `b64`.
- Signed `i8` through `i128`; unsigned `u8` through `u128`.
- Natural `int`, `uint`; pointer-sized `isize`, `usize`, `uintptr`.
- IEEE `f16`, `f32`, `f64`; distinct Unicode scalar `rune`.
- Native/endian storage spellings such as `u32le`, `u32be`, `f64le`.
- `byte` as an alias of `u8`.

Concrete numeric types never convert implicitly. Untyped constants convert
only when representable in an expected type.

Pointer and view types are:

```text
^T          nullable pointer to one T
[^]T        unchecked pointer to sequential T values
rawptr      untyped data address
cstring     zero-terminated C byte pointer
[]T         non-owning mutable slice {data, len}
string      non-owning immutable byte slice
[N]T        inline fixed array; N is part of the type
simd[N]T    target-approved fixed vector
```

Use `&value` for an address and postfix `pointer^` for dereference. Use
`ptr_offset(pointer, count)` and `ptr_sub(left, right)` for pointer movement;
ordinary integer operators do not perform pointer arithmetic. `[^]T` indexing
is inherently unchecked. Construct a bounded slice with `pointer[:length]`.

Array, slice, and string indexing and half-open slicing are bounds checked by
default. A slice or string never owns or extends the lifetime of its backing
storage. Array assignment copies the entire inline array; slice assignment
copies only the view.

`string` is immutable bytes, not an owning Unicode string. Indexing returns one
byte. Use `core/utf8` to validate, decode, count, or encode Unicode scalars when
scalar boundaries matter; its offsets and widths remain byte counts. Use
`core/unicode` for extended-grapheme boundaries and deterministic terminal
column widths, and `tui.write_utf8` to paint complete graphemes. A combining
mark cannot be painted as a standalone terminal cell.

`raw_data(text)` returns a `[^]u8` pointing at a string's existing first byte.
It performs no copy or termination and inherits the backing storage's lifetime.
The pointer permits unchecked addressing but does not prove writability;
writing through it is undefined unless the program independently knows those
backing bytes are writable. Prefer a typed core operation over using this
escape hatch directly.

For file output, `core/os.write_text` and `write_text_all` are the ordinary
typed wrappers. They keep `raw_data` and its native nonmutation proof inside
the OS package.

Use `nil` only for pointer-like and procedure-pointer values. Draft has no
implicit pointer or integer truthiness; write `pointer != nil`.

## Aggregates and layout

Draft supports tuples, structs, enums, aliases, distinct types, variants, and
unions. Empty arrays and empty aggregates are invalid in Draft 1.

```draft
Index :: usize
Duration :: distinct i64

Mode :: enum u8 {
    read,
    write,
}

Outcome :: variant {
    failed: Error,
    complete: usize,
}

C_Value :: c union {
    bits: u64,
    pointer: rawptr,
}

Cache_Line :: align(64) struct {
    bytes: [64]u8,
}

Wire_Header :: struct {
    tag: u8,
    packed sequence: u32be,
    checksum: u16,
}
```

Tuples and structs are products; variants are sums with one active alternative;
unions overlay fields without an active tag. Access tuple members as `.0`, `.1`,
and so on. Switch over a variant to obtain a typed payload binding.

An alias has the target's identity. A `distinct` type has the target's layout
and operators but a separate identity; cross its boundary with an explicit
cast. Enums require a zero-valued member for a valid zero value. The first
implicit enum member is zero; values then increment.

Use `c struct`, `c union`, or `c enum` for selected-target C layout.
`align(N)` may raise struct or union alignment; combine the modifiers as
`c align(N) struct` or `c align(N) union`. These are direct modifiers, not
annotations. Do not guess C layout: read the target profile and add ABI tests
against Clang.

`packed field: T` is a per-field rule for an ordinary Draft struct. It removes
alignment padding before that occurrence and gives it effective alignment one;
following natural fields align normally. The value still has type `T` and may
be read or assigned. Taking `&record.field` is rejected when the occurrence is
under-aligned because `^T` promises natural alignment. Do not use `packed` in a
`c struct`, union, tuple, enum, or variant. Inspect the authored rule with
`type_member_is_packed(T, index)` and the resulting byte position with
`type_member_offset(T, index)`.

`bits(N) field: T` allocates an integer-like field from a continuous bit stream
inside an ordinary Draft struct:

```draft
Header :: struct {
    bits(3) kind: u8,
    bits(6) delta: i16,
    bits(1) active: bool,
    payload_size: u16,
}
```

`N` is a positive compile-time `usize` value which must be complete when the
field declaration is checked. The logical type may be `bool`, a concrete
signed or unsigned integer, an enum whose members fit, or a `distinct` wrapper
around one of those; `bool` requires width one. Consecutive bit fields share
bytes and may cross a byte boundary. Bit zero is the low bit of the first byte
on every target. An ordinary or `packed` field closes a partial byte before
applying its own alignment rule.

Unsigned, boolean, and unsigned-backed enum reads zero-extend; signed reads
sign-extend from `N`. Assignment retains the low `N` bits and preserves adjacent
fields. A bit field is readable and assignable but has no address, so
`&record.field` is invalid. Do not use `bits(N)` in a `c struct`, union, tuple,
enum, or variant. Inspect it with `type_member_bit_width(T, index)` and
`type_member_bit_offset(T, index)`; ordinary fields report width zero and their
byte offset multiplied by eight. Structural member indices are zero-based and
follow source order.

`core/option.Option[T]` and `core/result.Result[T, E]` are ordinary variants,
not magic. A zero `Result` selects its first `.err` alternative.

## Expressions and conversions

Postfix call/index/member/dereference bind most tightly; then prefix; then
multiplicative/shift/bitwise-AND; additive/bitwise-OR/binary-`~` XOR;
comparison; `&&`; `||`; and the conditional expression. Comparisons do not
chain.

Assignment is a statement. Compound assignment evaluates its lvalue once.
`&&` and `||` short-circuit. The conditional expression evaluates only its
selected value:

```draft
larger := left if left > right else right
```

Use `cast[T](value)` for explicit conversion. Important rules include:

- Integer-to-integer conversion uses target-width modular bits.
- Integer/float conversion to a float rounds to nearest with ties to even.
- Float-to-integer truncates and traps for NaN/out-of-range.
- Conversion into `rune` traps for a non-Unicode scalar.
- Enums cast only to or from their exact backing integer type; conversion into
  an enum traps unless a declared member has the value. Reach a different
  integer type with an explicit second cast, for example
  `cast[int](cast[u8](result))` for an `enum u8`.
- Compatible data pointer/raw-pointer/`uintptr` casts preserve address bits.
- `bool` and boolean-storage casts map false/true to zero/one and accept any
  nonzero storage value as true.
- Native and matching endian storage scalars convert explicitly by value.
- Procedure pointers do not cast.

Concrete integer arithmetic wraps; divide/remainder by zero, signed minimum
divided by `-1`, and invalid shifts trap. Floating arithmetic follows declared
IEEE precision without fast-math. A required constant that would trap is a
compile error.

Equality is not defined for aggregates, slices, or strings; use explicit
library/application comparison. Draft has no operator overloading.

The currently implemented predeclared intrinsic vocabulary includes `len`,
`size_of`, `align_of`, `cast`, `raw_data`, `ptr_offset`, `ptr_sub`, `assert`,
`static_assert`, and the structural type-inspection queries. `context` and
`target` are predeclared values. Do not invent an introspection spelling;
inspect the compiler/specification before using a less common query.

## Control flow and cleanup

Draft supports bare lexical blocks, runtime `if`, conditional expressions, four
`for` forms, `switch`, `break`, `continue`, `return`, `defer`, compile-time
`when`, `unchecked`, and `deny`.

```draft
for {
    service_one()
}

for offset < len(input) {
    offset += consume(input[offset:])
}

for index: usize = 0; index < len(output); index += 1 {
    output[index] = 0
}

for value, index in input {
    output[index] = transform(value)
}
```

Iteration values are copies. Mutate an element through an index or pointer.
The iterable expression is evaluated once.

`switch` evaluates its subject once, never falls through, and supports
comma-separated labels plus an empty default `case:`. Enum and variant switches
must be exhaustive or have a default.

```draft
switch outcome {
case .ok(value):
    consume(value)
case .err(error):
    report(error)
}
```

`defer` accepts a call statement only. It evaluates and saves the callee,
arguments, and ordinary context immediately, then runs saved calls LIFO on any
normal scope exit: fallthrough, `break`, `continue`, or `return`. A runtime trap
does not run defers. In `return expression`, the result is saved before defers
run.

## Constants, targets, and compile-time selection

`::` values may include numbers, booleans, strings, enums, aggregates, variants,
procedures, and types. Constants have no mutable storage or stable
address. Untyped integers are arbitrary precision and untyped decimal floats
are exact rationals until conversion.

Compile-time procedures return values, not syntax. They may use control flow,
parametric code, type/layout queries, and target facts, but not runtime globals,
context, foreign calls, or assembly.

Exact type values have the compile-time-only type `type`; they can be constants,
cross package interfaces, supply later annotations or parametric arguments, and
compare with `==`/`!=`, but cannot reach runtime. A structural constructor such
as `[^]u8`, `proc(value: i32) -> i32`, or `simd[4]u32` is the exact type value
when expression grammar places it in a compile-time comparison.
`type_of(expression)` observes the checked static type without evaluating the
expression. Structural inspection uses `type_kind`, `type_name`, scalar
bit-width/byte-order queries, element and member queries, underlying and
discriminator queries, procedure-signature queries, and C-representation or
requested-alignment queries. Procedure inspection includes
`type_is_variadic`; a static `..type` pack produces fixed specializations and
therefore is not a variadic procedure type. Use the exact vocabulary and applicability table
in the specification's “Compile-time type values and structural inspection”
section rather than guessing a reflection API. These are structural language
facts, not target ABI classification or runtime reflection.

Use `when` to select already parsed declarations, members, or statements:

```draft
when target.os == .linux {
    Page_Size :: 4096
} else {
    Page_Size :: target.page_size
}
```

Every branch is parsed; only the selected branch is name/type checked and
contributes contents. `when` introduces no lexical scope and cannot construct
syntax. The target exposes `identity`, `arch`, `os`, `abi`, `byte_order`,
`object_format`, `file_tag`, `pointer_bits`, `page_size`, and compile-time
`has_feature`. Its five categorical fields use the distinct compiler-defined
enums `Target_Architecture`, `Target_Operating_System`, `Target_ABI`,
`Target_Byte_Order`, and `Target_Object_Format`; their contextual alternatives
do not cross those type boundaries. Each field or `has_feature` result is an
ordinary compile-time scalar and may be assigned to a runtime binding; the
whole `target` object has no runtime representation.
The current architecture alternatives are `.aarch64` and `.x86_64`; operating
systems are `.macos`, `.linux`, and `.windows`; ABIs are `.darwin_arm64`,
`.aapcs64_gnu`, `.sysv_amd64`, and `.win64`; object formats are `.macho`,
`.elf`, and `.coff`. Each list has that stable source order.

Inside a parametric procedure, a statement `when` may depend on the concrete
type/value arguments. The template is checked symbolically in both branches,
then each instance retains only its selected branch. The compiler recognizes
exact refinement through `type_of(value) == T` and kind refinement through
`type_kind(type_of(value)) == .signed_integer`; `!=`, reversed operands, and
chained `else when` carry the corresponding complements. Use an exact test when
an operation needs a complete type such as `string`; use a kind test for the
operations guaranteed by a numeric category. A final
`static_assert(false, "unsupported type")` rejects only concrete instances
which reach it.

## Parametric declarations

Put compile-time type/value parameters in brackets after the declaration name:

```draft
Buffer[T: type, N: usize] :: struct {
    values: [N]T,
}

sum[T: number] :: proc(values: []T) -> T {
    result: T
    for value in values {
        result += value
    }
    return result
}
```

The closed type constraints are `type`, `integer`, `float`, and `number`.
Constraint membership is semantic, not representation-based: enums, endian
scalars, `rune`, boolean storage, and distinct wrappers do not automatically
satisfy `integer`/`number`.

Applications use brackets: `Buffer[u8, 256]`, `sum[u64](values)`. Procedure
arguments can infer type and simple one-to-one integer value parameters; use
explicit arguments when inference is not uniquely specified. There is no
template metaprogramming or syntax reflection. Pass behavior outside the closed
constraints explicitly through procedure values or records containing them.

A Draft procedure body may have one named final static pack parameter:

```draft
print_values :: proc(prefix: string, values: ..type) {
    for value, index in values {
        when type_of(value) == string {
            consume_text(prefix, value)
        } else when type_kind(type_of(value)) == .signed_integer {
            consume_integer(cast[i128](value), index)
        } else {
            static_assert(false, "unsupported argument type")
        }
    }
}
```

Call it directly with the fixed prefix and any number of tail values. Defaults
may fill fixed parameters; a call with pack-tail values supplies its fixed
prefix positionally because no positional value may follow a named argument.
Each tail
keeps its own concrete type; untyped integers/floats default independently to
`int`/`f64`. `len(values)` is a compile-time `usize`, and static iteration
expands in source order with a compile-time `usize` index. The pack is not a
slice, tuple, runtime value, type list, or C varargs ABI. Do not take the
procedure as a value, use the pack outside `len`/iteration, put `..type` in a
standalone/C/foreign/exported signature, or invent `[Types: ..type]` syntax.
The static expansion is not a runtime loop target, so its own body does not
make `break` or `continue` valid.
Explicit bracket parameters compose normally, as in
`render[T: type] :: proc(first: T, values: ..type)`.

Use parameter-dependent `static_assert` for relationships such as capacity or
layout requirements. It constrains concrete instantiations but does not grant
operations absent from the declared constraint.

Draft guarantees dependent integer-value inference only for a single
one-to-one occurrence through a narrow reversible expression (direct use,
unary sign/complement, a suitable cast, add/subtract/XOR by a known constant).
Repeated occurrences, multiplication, division, remainder, shifts, AND, and OR
require an explicit value argument.

## Assertions and unchecked access

`assert(condition, optional_message)` is a runtime intrinsic using the active
context. A build may disable it, in which case neither argument evaluates.
Never depend on assertion side effects. It is invalid in a `c proc`.

`static_assert(condition, optional_message)` requires compile-time values, is
never disabled, and remains valid in `c proc`.

`unchecked { statements }` suppresses dynamic bounded-access checks in its
region. It does not make invalid memory, races, casts, arithmetic, or lifetimes
safe. Multi-pointer indexing is always unchecked. Prefer checked slices and
prove a performance need before using it. An active `deny unchecked` restores
checks where possible and rejects inherently unchecked reachable work.

Read `references/agent-features.md` before using `deny` or other agent-facing
constructs.

## Program entry

A hosted executable root has exactly one explicit package-level,
non-parametric ordinary `main`:

```draft
main :: proc() {
    run()
}
// or
main :: proc() -> int {
    return run()
}
```

Arguments and environment come from `core/os`, not `main` parameters. There are
no hidden package initializers or destructors. Initialize application state
explicitly inside `main`; normal return runs ordinary defers. Libraries require
no `main`.

## Features Draft deliberately lacks

Do not synthesize syntax for familiar features that are absent. Draft 1 has no
methods, closures, inheritance, interfaces/traits, exceptions, implicit
destructors, automatic ownership/moves, operator overloading, declaration
overloading, Draft-defined C variadic bodies, macro expander,
AST construction API, textual preprocessor, hidden package initialization,
strict-aliasing assumption, implicit numeric conversions, implicit truthiness,
aggregate equality, general compile-time declaration generation, or raw-string
inline assembly.

Future GPU procedures, additional layout forms, and raw-string assembly in
`docs/specification/07-future-ideas.md` are non-normative and unimplemented.
