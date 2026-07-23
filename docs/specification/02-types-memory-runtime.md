# Draft: Types, memory, and runtime

Part of the [Draft language specification](../../README.md).

[← Core language](01-core-language.md) ·
[Next: Design context and agent synthesis →](03-agent-synthesis.md)

<a id="section-5"></a>

## 5. Runtime types

Every runtime type has a defined size, alignment, representation, and native
lowering. A type need not fit in one register to work with assembly; aggregates
can be passed through fields, ABI decomposition, addresses, or memory operands.

### Target profile

Every build selects one versioned target profile. It fixes the LLVM target
triple and data layout, pointer width, byte order, scalar sizes and alignments,
object format, platform C ABI, CPU features, relocation and code models, TLS
model, trap and termination lowering, and whether a parsed-assembly dialect is
available. The
built-in `target` value exposes
the stable compile-time fields `identity: string`,
`arch: Target_Architecture`, `os: Target_Operating_System`,
`abi: Target_ABI`, `byte_order: Target_Byte_Order`,
`object_format: Target_Object_Format`, `file_tag: string`,
`pointer_bits: uint`, and `page_size: usize`.
The categorical types are distinct compiler-defined enums. Draft 1 defines
`Target_Architecture` with `.aarch64` and `.x86_64`;
`Target_Operating_System` with `.macos`, `.linux`, and `.windows`; `Target_ABI`
with `.darwin_arm64`, `.aapcs64_gnu`, `.sysv_amd64`, and `.win64`;
`Target_Byte_Order` with `.little` and `.big`; and `Target_Object_Format` with
`.macho`, `.elf`, and `.coff`, in the stated source order. A later target profile may
append alternatives but cannot reorder or conflate existing types. Consequently
`target.os == target.arch` and `target.os == .aarch64` are type errors even when
the underlying labels or integer representations happen to coincide.
`file_tag` is a unique filename-safe ASCII identifier for source selection.
`target.has_feature(feature: string) -> bool` requires a compile-time string.
It returns whether the profile enables a feature known to its architecture;
an unrecognized feature name is a compile error.
Each direct field and query result is an ordinary compile-time scalar and may
materialize where runtime code requires that scalar value. The complete
`target` object has no runtime representation and cannot itself be stored or
passed.

A Draft 1 compiler is required to support only one profile and rejects other
targets. Additional profiles extend target coverage without changing language
syntax or target-independent semantics. The selected profile identity is
recorded as a build input.

### Scalar and machine types

```text
bool                              logical boolean
b8 b16 b32 b64                   fixed-width boolean storage

i8 i16 i32 i64 i128             signed integers
u8 u16 u32 u64 u128             unsigned integers
int uint                         target-natural integers
isize usize                      pointer-sized signed/unsigned integers
uintptr                          integer able to hold pointer bits

f16 f32 f64                     IEEE floating-point values
byte                             alias of u8
rune                             distinct i32 Unicode scalar value
```

Multi-byte integer and floating types have explicit `le` and `be` storage
variants, such as `u32le` and `f64be`, for protocols and mapped data. Registers
use the target-native representation; loads, stores, and conversions preserve
the declared byte order.

Untyped numeric constants exist during compilation and convert to concrete
machine types before code generation.

### Pointers, procedures, and views

```text
^T                               nullable pointer to one T
[^]T                             C-like pointer to sequential T values
rawptr                           untyped address, equivalent to C void*
uintptr                          integer representation of an address
proc(...) -> T                   procedure pointer/signature

[N]T                             fixed array with inline storage
[]T                              slice: {data: [^]T, len: usize}
string                           immutable byte slice
cstring                          zero-terminated C byte pointer
simd[N]T                        fixed vector with native vector lowering
```

A procedure type is identified by its ordered fixed parameter types, result
type, calling convention, and whether it has a C variadic tail. Source
parameter names and default constants belong to a named declaration's call
contract; they do not affect procedure-type equality, assignment
compatibility, representation, or ABI. Consequently, taking a procedure as a
value deliberately loses named/default calling convenience.

`nil` is the zero-address literal for pointer-like and procedure-pointer types.
A zero slice may contain `{nil, 0}`. Pointer arithmetic is performed through
explicit `ptr_offset` and `ptr_sub` operations.

For a `^T` or `[^]T` value, `ptr_offset(pointer, count: isize)` advances by
`count` elements and returns the same pointer type. `ptr_sub(left, right)`
requires matching pointer types, returns `isize`, and requires both addresses
to lie in the same allocation. The address difference `left - right` in bytes
must be an exact multiple of `size_of(T)`; otherwise the operation
has undefined behavior. The signed quotient must be representable as `isize` or
the operation also has undefined behavior. Ordinary integer operators do not
operate on pointers. Pointer-to-`rawptr` and
pointer-to-`uintptr` conversions are explicit.
Dereferencing a nil, invalid, or insufficiently aligned address has undefined
behavior. An offset that overflows or leaves its allocation other than at the
one-past address also has undefined behavior. Calling a nil procedure pointer
or an address that does not name a compatible procedure is undefined behavior.

`^T` addresses one value. `[^]T` supports unchecked indexing.
`pointer[:length]` constructs a `[]T` from a `[^]T` and a `usize` length; nil is
valid only with zero length, and otherwise the caller must provide that many
live, aligned elements. Slices and strings are small value views and do not own
their backing storage.

`raw_data(text: string) -> [^]u8` is the explicit escape from ordinary string
immutability for native pointer-and-length interfaces. For a nonempty string,
the result points at its first byte and exactly `len(text)` consecutive bytes
may be read during the lifetime of the backing storage. An empty string exposes
no dereferenceable byte and its returned address is not required to be non-nil.
The operation does not allocate, copy, append a zero terminator, validate an
encoding, or extend the backing lifetime. A string slice therefore exposes its
already adjusted first byte rather than the original string's beginning.

The result type is intentionally an ordinary multi-pointer: Draft has no
const-qualified pointer family. `raw_data` does not prove that storage is
writable. A write through the result is undefined behavior unless the program
independently knows that the affected backing bytes are live and writable;
string literals and other read-only image storage provide no such guarantee.
Multi-pointer indexing retains its ordinary inherently unchecked semantics.

Scalar integers, floats, pointers, procedure pointers, and legal SIMD vectors
bind directly to target register classes. Other values use their declared
aggregate layout.

### Arrays, slices, and strings

`[N]T` is a built-in fixed array. `N` is a positive compile-time `usize` in Draft 1;
the elements are stored contiguously and inline with stride `size_of(T)`. Fixed
arrays do not allocate. Assignment and parameter passing copy the complete
value under the ordinary value rules. Length is part of the type, and nesting
expresses multidimensional storage:

```draft
values := [4]u32{10, 20, 30, 40}
matrix: [3][4]f32
```

`[]T` is a built-in non-owning `{data: [^]T, len: usize}` view. Copying a slice
copies only that view and does not copy or extend the lifetime of its backing
storage. `string` is the corresponding immutable byte view; indexing a string
produces a `byte`.

Slicing uses half-open bounds. `values[low:high]` contains elements beginning at
`low` and ending before `high`; `values[:]` selects the complete value, and
either bound may be omitted. Slicing an array produces a slice borrowing the
array's storage. Slicing a slice or string produces another view of the same
backing storage. Bounds follow the checked and `unchecked` rules in
[section 6](02-types-memory-runtime.md#bounds-and-addresses). All index and slice-bound
expressions require `usize` after contextual conversion of an untyped constant.

`len(value)` is a predeclared query. It is a compile-time value for a fixed
array and does not evaluate the fixed-array operand. It is a runtime `usize`
for a slice or string. Growable owning arrays and maps are explicit core-library
types described in
[section 7](02-types-memory-runtime.md#growable-arrays-and-maps) rather than
built-in language types.

### Aggregate and layout types

- Tuples, including tuple-valued procedure results.
- Structs and fixed arrays with inline storage.
- Enums with explicit or inferred integer backing types.
- Type aliases and `distinct` types.
- Variants with an explicit discriminator.
- Unsafe unions for untagged storage overlay and C compatibility.

Aliases and distinct types use the same declaration shape:

```draft
Index    :: usize
Duration :: distinct i64
```

`(T, U)` is a tuple type, `(a, b)` is a tuple value, and `.0`, `.1`, and so on
select its members. Parentheses without a comma only group; a Draft 1 tuple has at
least two members. A procedure returning `-> (T, U)` has one tuple result, so
`return (a, b)` returns it and a parenthesized binding pattern may destructure
the call result.

Struct and tuple members remain in source order. Each member begins at the
smallest offset satisfying its natural alignment; aggregate alignment is the
largest member alignment, and total size is rounded to that alignment. A
tuple result uses the same value layout, although the target ABI may decompose
it at a call boundary. Draft 1 rejects zero-length arrays and aggregates with no
members or alternatives.

A default-layout struct may lower the placement alignment of selected fields to
one byte with the field prefix `packed`:

```draft
Packet :: struct {
    kind: u8,
    packed sequence: u32be,
    checksum: u16,
}
```

`packed name: T` places that field at the current byte offset without inserting
alignment padding and contributes alignment one, while preserving the logical
type, size, value operations, and source name of `T`. A grouped declaration such
as `packed low, high: u32` applies the rule to both fields. The next natural
field still rounds its own offset normally, and final size is rounded to the
largest effective field alignment. In the example the offsets are 0, 1, and 6,
the struct alignment is 2, and its size is 8.

Packed fields are ordinary readable and assignable fields. Their loads and
stores use the alignment guaranteed by the containing occurrence rather than
pretending that the field begins at `align_of(T)`. `&value.field` is valid only
when that exact occurrence nevertheless guarantees `align_of(T)`; otherwise it
is a compile-time error because `^T` promises natural alignment. `packed` is not
valid on tuples, unions, variants, enums, or `c struct`; C-compatible packed
layout requires an explicit foreign representation rather than a Draft layout
rule being presented as the platform C ABI.

A default-layout struct may allocate selected integer-like fields from a
continuous source-order bit stream with `bits(N)`:

```draft
Header :: struct {
    bits(3) kind: u8,
    bits(6) delta: i16,
    bits(1) active: bool,
    payload_size: u16,
}
```

`N` is a positive compile-time `usize` expression which must be complete when
the field declaration is checked. A bit field's logical type must be `bool`, a
concrete signed or unsigned integer, an enum, or a `distinct` wrapper around
one of those. `bool` requires width one. An integer width cannot exceed its
selected-target logical width. Every declared enum value must fit the selected
width using the enum backing type's signedness. Endian scalars, boolean-storage
integers, `rune`, floats, pointers, and aggregate types are not bit-field types.

Consecutive bit fields consume consecutive bits without implicit padding. Bit
zero is the least-significant bit of the first byte, independently of target
scalar byte order, and fields may cross byte boundaries. `type_member_offset`
for a bit field is the byte containing its first bit. An ordinary field closes
the active bit stream at the next byte and then applies its own natural or
`packed` alignment. Bit fields contribute alignment one; final struct size is
still rounded to the largest effective field alignment. The example has bit
offsets 0, 3, and 9; `payload_size` begins at byte two; size is four and
alignment is two.

Reading an unsigned, boolean, or unsigned-backed enum bit field zero-extends
its stored bits. Reading a signed integer or signed-backed enum sign-extends
from `N`. Assignment retains the low `N` bits of the logical value, so a later
read may differ when the assigned value is outside the field's representable
range. Assigning one field preserves every neighboring bit. A grouped
declaration such as `bits(3) red, green: u8` allocates two consecutive
three-bit fields.

Bit fields are readable and assignable but never addressable: `&value.field`
is a compile-time error because no byte-addressed `^T` can name that storage.
`bits(N)` is not valid on tuples, unions, variants, enums, or `c struct`.

`variant { ... }` is a tagged sum; `union { ... }` overlays every named field
at offset zero. A union uses the maximum member alignment and the maximum
member size rounded to that alignment.

A variant stores discriminator `D` first. Payload-free alternatives have
size zero and alignment one; its remaining layout is exact:

```text
payload_alignment = max(alignment of every alternative)
payload_size       = round_up(maximum alternative size, payload_alignment)
payload_offset     = round_up(size_of(D), payload_alignment)
variant_alignment  = max(align_of(D), payload_alignment)
variant_size       = round_up(payload_offset + payload_size, variant_alignment)
```

An enum member is `Name` or `Name = constant`. The first implicit enum value is
zero; each later implicit value is one greater than the preceding value,
including an explicit one. Duplicate values are invalid. Variant
discriminators start at zero and increase in source order.
The inferred enum backing is the smallest fixed-width unsigned integer that fits
all values, or the smallest fixed-width signed integer when any value is
negative. A variant uses the smallest fitting unsigned discriminator.
`enum u16 { ... }` and `variant u16 { ... }` select these types explicitly; every
value must fit the selected backing type.

`c struct`, `c union`, and `c enum` delegate layout and ABI lowering to the
selected target's C ABI. C layout replaces the default aggregate layout and,
for an enum without an explicit backing type, the default backing rule. An
explicit enum backing may combine with `c enum` only when the target C ABI
supports it; otherwise the declaration is invalid.

`align(N) struct` and `align(N) union` require a positive, power-of-two
compile-time `usize`. The request may raise but not reduce alignment; size and
array stride are rounded to the resulting alignment. C layout and requested
alignment compose as `c align(N) struct` or `c align(N) union`: C layout is
computed first and the requested alignment is applied afterward. These are a
closed set of direct type-constructor modifiers, not annotations:

```draft
Header :: c struct {
    kind: u32,
}

C_Value :: c union {
    bits:    u64,
    pointer: rawptr,
}

C_Cache_Line :: c align(64) struct {
    bytes: [64]u8,
}

Cache_Line :: align(64) struct {
    bytes: [64]u8,
}
```

### Compile-time type values and structural inspection

Every Draft type has one exact compile-time value. Those values have the
predeclared meta-type `type`, may be stored in constants, passed as ordinary
compile-time arguments, compared with `==` and `!=`, and cross package
interfaces. They have no runtime layout or runtime type descriptor and must be
folded before MIR. `type_of(expression)` returns the expression's exact static
type without evaluating the expression. An otherwise untyped integer or float
operand defaults to `int` or `f64` at this boundary.

A constant or compile-time expression which produces a type value may supply
any type position, including an annotation, a structural element/result type,
a parametric type or procedure argument, and an explicit cast target. An
imported public type-valued constant has the same meaning after its exact type
graph is reconstructed in the consumer package.

`type_kind(T)` returns the compiler-defined `Type_Kind` enum. Its stable
alternatives are `.void`, `.bool`, `.boolean_storage`, `.signed_integer`,
`.unsigned_integer`, `.float`, `.rune`, `.endian_scalar`, `.raw_pointer`,
`.c_string`, `.string`, `.pointer`, `.multi_pointer`, `.slice`, `.array`,
`.tuple`, `.procedure`, `.simd`, `.struct`, `.enumeration`, `.variant`,
`.union`, `.distinct`, and `.type`. Untyped values and unresolved type
parameters are not concrete type values and cannot produce a folded query
result. A query in a parameter-dependent compile-time expression may remain
symbolic until a concrete instantiation supplies the exact type; it never
creates a runtime type descriptor.

The remaining structural queries are compile-time intrinsics:

- `type_name(T) -> string` returns a deterministic canonical spelling.
- `type_bit_width(T) -> usize` applies to scalar types with a defined bit
  width.
- `type_byte_order(T) -> Type_Byte_Order` returns `.native`, `.little`, or
  `.big` for scalar storage types.
- `type_element(T) -> type` applies to pointers, multi-pointers, slices,
  arrays, and SIMD vectors; `type_element_count(T) -> usize` applies to
  concrete arrays and SIMD vectors.
- Member indices are zero-based and follow source order.
- `type_member_count(T) -> usize`, `type_member_name(T, index) -> string`, and
  `type_member_type(T, index) -> type` inspect tuples, structs, enums, variants,
  and unions in source order. Tuple member names are their decimal
  indices.
- `type_member_offset(T, index) -> usize` returns the natural-layout byte
  offset of a tuple, struct, variant alternative, or union member.
  Enum alternatives have no member offset.
- `type_member_is_packed(T, index) -> bool` applies to structs and reports the
  authored field placement rule. It does not infer packing from a coincidental
  byte offset.
- `type_member_bit_width(T, index) -> usize` applies to structs and returns `N`
  for a `bits(N)` field or zero for an ordinary/`packed` field.
  `type_member_bit_offset(T, index) -> usize` returns the absolute
  struct-relative bit position of a struct field; ordinary and `packed` fields
  therefore return `type_member_offset(T, index) * 8`.
- `type_member_value(T, index)` returns an enum alternative's backing-typed
  integer value.
- `type_underlying(T) -> type` applies to distinct types, enums, and endian
  scalars. `type_discriminator(T) -> type` applies to variants.
- `type_parameter_count(T) -> usize`, `type_parameter_type(T, index) -> type`,
  `type_result(T) -> type`, and
  `type_calling_convention(T) -> Calling_Convention` inspect procedure types.
  Calling conventions are `.draft` and `.c`. `type_is_variadic(T) -> bool`
  reports whether a C procedure has the bare `..` native variadic tail; static
  `..type` packs specialize to fixed procedure types and therefore report
  false.
- `type_is_c_repr(T) -> bool` applies to structs, enums, and unions.
  `type_requested_alignment(T) -> usize` applies to structs and unions and
  returns zero when no `align(N)` was requested.

An inapplicable query or an out-of-range index is a compile-time error at the
query. These operations expose language structure and natural Draft layout;
target ABI argument/return classification remains part of the selected target
profile and is not a type-inspection query.

Draft 1 supports only SIMD lane counts and element types named by the selected target
profile. Unsupported combinations are compile errors.

### Variants, `Option`, and `Result`

A tuple is a product: every member exists simultaneously. A variant is a
sum: one named alternative is active.

`core/option` and `core/result` supply conventional ordinary parametric
variants. They have no compiler-defined behavior:

```draft
package option

pub Option[T: type] :: variant {
    none,
    some: T,
}
```

```draft
package result

pub Result[T: type, E: type] :: variant {
    err: E,
    ok:  T,
}
```

Their default representation is an explicit discriminator plus enough aligned
inline storage for the largest alternative. Consequently, a zero-initialized
`Result` is `.err` with the zero value of `E`, never a fabricated success.

```draft
import core/result

read_marker :: proc(input: []u8) -> result.Result[Marker, Parse_Error] {
    if invalid(input) {
        return .err(Parse_Error.Invalid_Marker)
    }
    marker := parse_marker(input)
    return .ok(marker)
}

consume_marker :: proc(input: []u8) {
    result := read_marker(input)
    switch result {
    case .ok(marker):
        consume(marker)
    case .err(error):
        report(error)
    }
}
```

Nullable pointers model zero-address absence. `option.Option[T]` models a
semantic choice between `.none` and `.some(value)` for any permitted `T`.
Draft 1 keeps the explicit discriminator representation and performs no niche
layout optimization.

### Parametric types and procedures

A declaration may introduce explicit compile-time type and value parameters in
brackets after its name:

```draft
Pair[T: type, U: type] :: struct {
    first:  T,
    second: U,
}

Buffer[T: type, N: usize] :: struct {
    data: [N]T,
}

swap[T: type] :: proc(a, b: ^T) {
    temporary := a^
    a^ = b^
    b^ = temporary
}
```

Every bracketed parameter is compile-time. `T: type` accepts any concrete
runtime type; `N: usize` accepts a compile-time `usize` value. Within the
declaration, `T` and `N` are ordinary names.

Parametric application uses brackets consistently:

```draft
pair: Pair[u32, string]
buffer: Buffer[u8, 256]
swap[u32](&left, &right)
```

Procedure arguments may infer parametric arguments when inference has a unique
solution, so `swap(&left, &right)` may infer `T`. Explicit arguments remain
available when inference is impossible or undesirable.

A procedure body may additionally end its runtime parameter list with one
static heterogeneous argument pack:

```draft
inspect_all :: proc(prefix: string, values: ..type) {
    static_assert(len(values) >= 0)
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

`..type` is valid only on one named final parameter of a Draft procedure with a
body. It is not valid in a standalone procedure type, a C procedure, a foreign
declaration, or an exported procedure. It cannot share a grouped name list.
Bracketed type/value parameters, when present, retain their existing syntax:
`render[T: type] :: proc(destination: ^T, values: ..type)`. There is no
bracketed pack parameter and no source-level pack-of-types object.

A direct call supplies the fixed prefix followed by zero or more pack values.
Defaults may fill omitted fixed parameters. Named arguments may select fixed
parameters, but because positional arguments cannot follow a named argument, a
call that supplies a pack tail uses positional fixed arguments first.
The callee and every argument are evaluated exactly once from left to right.
Each tail expression is checked without a common expected type; an untyped
integer therefore defaults to `int` and an untyped floating value defaults to
`f64`. Explicit bracket arguments and ordinary inference determine any named
parametric parameters independently of the tail. Taking a pack procedure as a
procedure value or calling it indirectly is invalid.

The pack binding is compile-time structure, not a runtime slice, tuple, erased
value, or variadic ABI. Its only value-like operations are `len(values)`, which
is a compile-time `usize`, and `for value, index in values`. Static iteration
expands once per supplied tail value in source order. `value` denotes that
ordinary argument with its exact concrete static type; the optional `index` is
a compile-time `usize` beginning at zero. `_` may discard either binding. The
same dependent-`when` refinement rules described below apply to `value`, and
an index-dependent `when` is selected independently in each expansion. Static
iteration is not itself a runtime control target: a `break` inside its source
body applies only to an enclosing runtime loop or switch, and a `continue`
applies only to an enclosing runtime loop.

Before executable HIR or MIR, every ordered tuple of explicit generic arguments
and pack element types produces an ordinary fixed-signature procedure. Equal
ordered type tuples reuse a specialization regardless of the runtime values;
different order or types are different specialization identities. This rule is
the same across package interfaces. A synthesis or judgment site in the source
body is collected once from the symbolic pack body and is not repeated for
each element or concrete specialization.

Nominal applications unify only when they have the same template identity and
their ordered type and value arguments unify; identical layout or members do
not make distinct nominal templates interchangeable. Substitutions learned
from earlier procedure arguments are applied before later arguments are
matched.

For a dependent integer value, Draft 1 guarantees inference from a single
occurrence of that parameter in a checked one-to-one pattern built from direct
use; unary `+`, unary `-`, or bitwise complement; a same-width or widening
integer cast; addition or subtraction by a known constant; the corresponding
known-constant-minus-parameter form; or XOR by a known constant. Inversion uses
each expression node's exact checked integer domain, including ordinary
fixed-width two's-complement wrapping. The inferred candidate is substituted
into the complete original expression and re-evaluated before acceptance. Repeated
occurrences such as `N + N`, multiplication, division, remainder, shifts, AND,
and OR require an explicit argument unless a later language revision defines a
broader solver.

Constraint names are compiler-defined and valid only in parametric parameter
declarations. The initial vocabulary is closed:

- `type` accepts any concrete runtime type.
- `integer` accepts `i8`, `i16`, `i32`, `i64`, `i128`, `u8`, `u16`, `u32`,
  `u64`, `u128`, `int`, `uint`, `isize`, `usize`, and `uintptr`.
- `float` accepts `f16`, `f32`, and `f64`.
- `number` accepts the union of `integer` and `float`.

Aliases inherit membership from their target. Boolean storage, enums, `rune`,
endian storage types, and `distinct` wrappers do not satisfy these constraints
merely because of their representation. `number` guarantees comparison, binary
arithmetic `+`, `-`, `*`, and `/`, and their compound assignments. `integer`
additionally guarantees remainder, bitwise, and shift operators. A parametric
body is checked symbolically using only the operations guaranteed by its
declared constraints:

```draft
sum[T: number] :: proc(values: []T) -> T {
    result: T
    for value in values {
        result += value
    }
    return result
}
```

A parameter-dependent statement `when` may refine one named value without
changing its declaration or the procedure's public constraint. The recognized
refinement predicates are exact-type comparisons
`type_of(value) == Some_Type` and broad-kind comparisons
`type_kind(type_of(value)) == .signed_integer`; `!=` and either operand order
are equivalent with the true and false facts exchanged. In the matching exact
branch, ordinary checking treats `value` as `Some_Type`. In a matching kind
branch, it grants only the operations common to that `Type_Kind` alternative:
for example, `.signed_integer` and `.unsigned_integer` grant the `integer`
constraint operations, while `.float` grants the `float` operations. The
`else` branch receives the exact complement, and an `else when` chain
accumulates earlier exclusions in source order.

Other parameter-dependent boolean conditions remain valid compile-time
selection but grant no additional type operations during symbolic checking.
The compiler checks the symbolic template once under these facts, then checks
only the selected branch of each concrete instance. A terminal
`static_assert(false, message)` is therefore a valid way to reject every
concrete type not accepted by earlier branches. This refinement stack is not a
generic constraint solver and never changes interface identity.

Relationships involving parametric type or value parameters use `static_assert`
inside the declaration. An assertion independent of those parameters is
evaluated immediately. A parameter-dependent assertion becomes an instantiation
obligation and is evaluated after each concrete substitution, or earlier when
the declared constraints prove it. A false assertion rejects that instantiation
and reports both declaration and instantiation locations. It does not grant the
body operations absent from its declared constraints.

Behavior outside the closed constraint vocabulary is passed explicitly through
procedures or data containing procedure pointers:

```draft
maximum[T: type] :: proc(
    a, b: T,
    less: proc(left, right: T) -> bool,
) -> T {
    return b if less(a, b) else a
}
```

Instantiation produces concrete native layouts and calls. Instantiations are
monomorphized when layout or direct-call behavior differs; the compiler may
merge machine-identical instantiations without changing program semantics.
Instantiation introduces no boxing or implicit dynamic dispatch.

A synthesis site inside a parametric declaration receives its named
compile-time parameters, declared constraints, active dependent-`when`
refinements, and expected type. It is resolved once in that symbolic
environment and must type-check under those facts. Ordinary monomorphization
then specializes the pinned generated source; it does not issue another
provider request for each concrete instance.

Parametric declarations do not inspect, construct, or rewrite syntax. They
substitute only their declared type and value parameters into an already parsed
declaration.

<a id="section-6"></a>

## 6. Memory, storage, globals, and context

### Storage and initialization

Local variables have automatic storage for the lifetime of their lexical
activation unless optimized into registers or equivalent storage.

Variables are initialized to their zero value by default. Zero clears scalar
and storage bytes; pointers become `nil`; aggregate members become their zero
values; an enum must declare a zero-valued member; and the first alternative of
a variant has discriminator zero and zeroed payload storage.

`---` is a special initializer, not a general expression or first-class value.
It is valid only on an automatic local declaration and permits the compiler to
omit normal zero initialization. Its value bytes remain uninitialized until
written. Reading an uninitialized byte as data, or using a typed value with an
uninitialized value byte, is undefined behavior; the compiler diagnoses a read
when it can prove one. Taking an address or creating a view is not a read, so
ordinary procedures, foreign code, or assembly may initialize the storage
through a pointer:

```draft
x: int       // zero initialized
y: int = --- // potentially uninitialized
```

Package globals accept zero initialization or compile-time constant
initializers. Runtime startup performs those initializations only:

```draft
pub counters: Counters
crc_table: [256]u32 = make_crc_table_constant()
```

Each linked image contains one static instance of every non-thread-local
package variable it defines. That instance has a stable address usable by
native code through a pointer passed by Draft code or an explicit C-ABI
boundary. Package assembly cannot name a private Draft symbol.

Runtime setup is an ordinary procedure invoked explicitly by the program:

```draft
initialize :: proc(config: ^Config) -> Error {
    return initialize_subsystems(config)
}

main :: proc() {
    config: Config
    error := initialize(&config)
    if error != Error.None {
        report(error)
        return
    }
    run_application(config)
}
```

Assignment copies a value according to its declared layout. Pointers, slices,
and strings copy their view rather than the referenced storage. Resource
ownership is expressed by library types and explicit procedures.

A storage-backed variable, naturally aligned field, or array element is
addressable with `&`; an unmaterialized temporary is not. A packed field remains
an assignment target but permits `&` only when its exact containing occurrence
still guarantees the field type's natural alignment. Taking an address
materializes automatic storage for its existing lexical lifetime; it does not
extend that lifetime. Address formation and dereference preserve the declared
type alignment.

#### Thread-local storage

`thread_local` is valid only on a package variable. Each thread receives one
instance with the declaration's zero or compile-time constant initializer, and
that instance has a stable address for the lifetime of the thread:

```draft
thread_local scratch: Scratch_Buffer
```

Thread-local variables otherwise follow the same visibility, typing, address,
and explicit-runtime-setup rules as package globals. The target runtime and
entry shim, `core/thread`, or the foreign-thread runtime bridge establish their
storage; the language does not run hidden package initializers.

### Memory validity and aliasing

A storage instance is a contiguous byte range supplied by automatic, package,
thread-local, allocated, mapped, or foreign storage. Automatic storage lives for
its lexical activation, package storage for the linked image, and thread-local
storage for the thread. Allocator, mapping, and foreign APIs define the lifetime
of storage they supply. A pointer or view does not extend that lifetime; access
after it ends is undefined behavior. An allocation in pointer operations means
one such storage instance.

An address lies within an allocation when it is in that instance's current live
byte range. Pointer offset and subtraction additionally admit the address
exactly one byte past the range after element scaling. Their same-allocation
rule is satisfied only when one live instance contains both addresses under
that definition. Because pointers carry no provenance, reuse may make the same
address bits fall within a later storage instance; validity is determined at
the operation, never extended from an earlier lifetime.

Pointers are machine addresses with no hidden ownership or provenance. Integer
and pointer conversions preserve address bits but do not create storage or
extend its lifetime. A typed load requires live, sufficiently aligned storage
containing a valid representation of its type. A typed store requires live,
sufficiently aligned storage and establishes the stored representation. Padding
written by a typed store is unspecified. `byte` and `u8` may inspect or copy any
initialized object byte, including padding.

Draft 1 has no strict-aliasing assumption: pointers of different types may refer to
the same bytes. Reading a union member interprets the shared bytes as that
member and is valid when those bytes form a valid value of its type. Every bit
pattern is valid for integers, floats, boolean-storage types, endian scalars,
and pointer values. `bool` requires zero or one, `rune` a Unicode scalar, an
enum a declared value, and a variant a declared discriminator with a valid
active payload. Loading any other typed representation is undefined behavior.

### Bounds and addresses

Array, slice, and string indexing is bounds-checked by default. Constant indices
are checked during compilation; dynamic indices are checked at runtime. A
failed dynamic check calls the context-free `core/runtime.bounds_failure` entry,
whose private runtime ABI is:

```text
c proc(kind: u8, first, second, length: usize,
       source_file: cstring, line, column: usize)
```

Kind zero reports indexing (`first` is the index and `second` is zero); kind one
reports half-open slicing (`first` is low and `second` is high). If the entry
returns, the compiler traps. The `unchecked` construct is a statement whose
braced statement list introduces an ordinary lexical scope and disables dynamic
bounds checks:

```draft
unchecked {
    unchecked_value := slice[index]
}
```

In Draft 1, `unchecked` means bounds-check suppression only. An active
`deny unchecked` re-establishes the checked-or-proven boundary under the
transitive denial rules in
[section 13](05-denials-validation.md#section-13). A compiler option may change
the outer default for a complete build, but it cannot weaken a denial.

Multi-pointer indexing is unchecked because `[^]T` carries no length. Creating
a slice from it introduces a length and the usual slice checks.

### Allocation

The allocator ABI is defined by `core/runtime`. Typed allocation helpers,
arenas, owned strings, virtual memory, and allocation strategies are ordinary
facilities in `core/memory`. They use `context.allocator` when no allocator is
supplied explicitly.

```draft
import core/memory

tree := memory.new[Tree]()                                // context.allocator
defer memory.free(tree)                                   // context.allocator

arena_tree := memory.new_with_allocator[Tree](arena_allocator)
defer memory.free_with_allocator(arena_tree, arena_allocator)
```

The explicit forms use distinct names to keep allocator selection and ownership
visible at each call site; Draft has no declaration overloading. `allocate`,
`resize`, the corresponding `*_with_allocator` operations, and `free_bytes*`
expose the raw byte substrate used by containers and arenas.

Owning growable arrays and maps are separate core-library types. Each initialized
container stores its allocator; initialization and destruction are explicit.

### Draft context

`core/runtime` defines the required Draft context ABI as ordinary public
declarations:

```draft
package runtime

pub Assertion_Failure_Proc :: proc(
    condition, message, source_file: string,
    line, column: usize,
)

pub Context :: c struct {
    allocator:              Allocator,
    temp_allocator:         Allocator,
    assertion_failure_proc: Assertion_Failure_Proc,
    logger:                 Logger,
    random_generator:       Random_Generator,
    user_ptr:               rawptr,
    user_index:             int,
    _internal:              rawptr,
}
```

For a hosted target, `runtime.default_context` supplies a general-purpose
allocator, a thread-owned temporary allocator, a diagnostic assertion handler,
the target runtime's default logger and random generator, zero user fields, and
runtime-owned internal state. Temporary allocations remain valid until an
explicit reset/scope operation or thread exit; no call boundary resets them.
The selected target runtime defines the concrete providers. A versioned
compiler distribution supplies that target's hosted-runtime object; it is not
ordinary package source, a foreign provider, or an input to generated-source
resolution. The host linker, SDK, and their physical paths used to emit an
artifact do not become language inputs.

The compiler and runtime agree on the `Context` layout from `core/runtime`, but
no source package name is injected implicitly. Source code that names the type
imports `core/runtime` normally. Independently, the built-in `context` value
exposes the active record's fields inside an ordinary procedure.

Executable startup constructs the root runtime context before invoking `main`.
Every ordinary `proc` receives the active runtime-context pointer under the ABI
defined in [section 12](04-native-interop.md#draft-and-c-procedure-abis), and normal calls
automatically propagate it. Every context field is visible unless an enclosing
`deny` region removes access to it.

A procedure begins with its caller's context pointer. A runtime lexical scope
that assigns a context field or takes the address of one uses a compiler-managed
local copy of the runtime `Context`, initialized from the surrounding value.
Later field accesses and normal calls from that scope use the local pointer;
leaving the scope discards the copy. A scope that neither assigns nor takes a
field address need not create a copy. Context assignment through the local
record never mutates the caller's active record:

```draft
context.user_index = 456

{
    context.allocator = arena_allocator
    context.user_index = 123
    decode(input)
}

assert(context.user_index == 456)
```

Compile-time `when` grouping does not introduce a runtime scope. A C calling
convention procedure establishes a Draft context explicitly before calling
ordinary Draft procedures when needed.

### Runtime concurrency

Concurrent execution may be created by `core/thread`, a foreign host, or the
target runtime. Two accesses to overlapping bytes conflict when at least one
writes. A data race is two conflicting accesses in different threads, at least
one non-atomic, where neither happens-before the other; a data race is undefined
behavior. Happens-before is the transitive closure of within-thread sequencing
and synchronization edges defined by `core/thread` and `core/atomic`.

Draft 1 adopts the atomic read, coherence, and memory-order rules of ISO C11 sections
5.1.2.4 and 7.17 for the relaxed, acquire, release, acquire-release, and
sequentially consistent orders exposed by `core/atomic`; consume ordering is
absent.

`core/atomic` supplies compiler-backed atomic values and load, store,
read-modify-write, compare-exchange, and fence operations for target-supported
integer and pointer types. Atomic storage is accessed only through those
operations. Distinct atomic objects may not overlap, and accessing their storage
through an ordinary lvalue is undefined behavior. Each atomic object has one
modification order. `core/thread` defines the synchronization edges of start,
join, and its synchronization objects. An exact-artifact foreign-provider
summary may declare corresponding acquire, release, start, or join edges for
imported operations;
without such a declaration, a foreign call adds no synchronization edge. An
assembly memory clobber is only a compiler dependency; it does not make ordinary
accesses atomic or cure a data race.

Atomic order arguments are compile-time `core/atomic.Order` values. The
compiler rejects release loads, acquire stores, releasing compare-exchange
failure orders, and a failure order stronger than the corresponding success
order. A relaxed fence is valid but creates no synchronization edge. The
bootstrap AArch64 core surface currently requires these intrinsics to be direct
package calls and rejects taking them as procedure values or explicitly
specializing them; that is an implementation limitation recorded in the
[runtime implementation](../implementation/runtime-and-core.md), not an
additional C11 memory-semantic rule.

A thread created by `core/thread` initializes Draft TLS before entry and
begins with an independent copy of the spawning thread's active `Context`. The
runtime gives it thread-owned temporary-allocation and internal state; other
fields are copied, and referenced objects remain shared under the data-race
rules. On a foreign-created thread, the context-free runtime bridge initializes
Draft TLS on first use before entering ordinary Draft code. The supplied
context pointer is retained only for the dynamic call; TLS remains until thread
exit. Normal return from a thread entry runs its ordinary deferred calls; there
are no hidden package destructors.

### Program entry

`draft build path/to/workspace --root path/to/package` treats the selected
package as one executable root. Omitting `--root` discovers every surface
package-level `main` below the workspace and builds each selected package as an
independent program target; `--root .` names the workspace-directory package.
A hosted executable root must contain exactly one package-level, non-parametric
ordinary procedure named `main`. The declaration must appear explicitly in
surface source, although its body may contain synthesis sites. `main` uses the
ordinary Draft calling convention and receives the hidden root
runtime-context pointer.

The permitted signatures are:

```draft
main :: proc() {
    run_application()
}
```

or:

```draft
main :: proc() -> int {
    run_application()
    return 0
}
```

Returning no value produces process exit status zero. An `int` result supplies
the platform process exit status. Process arguments and environment are
ordinary library queries rather than special `main` parameters:

```draft
import core/os

main :: proc() -> int {
    args := os.args()
    environment := os.environment()
    return run(args, environment)
}
```

The compiler emits the platform entry shim required by the target ABI. The shim
establishes static and thread-local storage, constructs the root runtime
`Context`,
makes process arguments and environment available to the OS package, invokes
Draft `main` with the hidden context pointer, and converts its result to the
platform exit status. Runtime application setup remains an explicitly called
ordinary procedure inside `main`; the shim performs no package initialization.

Libraries do not require `main`. Freestanding, kernel, and embedded build
targets name an explicit entry procedure and calling convention in target
configuration instead of using the hosted-process contract. Before an ordinary
entry, the configured shim must establish Draft TLS and supply a non-null
`Context`. A `c proc` entry receives neither and must use the runtime bridge
before entering ordinary Draft code.

<a id="section-7"></a>

## 7. Core libraries

Library declarations are never imported implicitly. Primitive types, `nil`,
the built-in `context` and `target` names, and specified compiler intrinsics
such as `len`, `size_of`, `align_of`, `cast`, `raw_data`, `assert`, and
`static_assert` are predeclared language names rather than library APIs.

Core packages ship with the compiler distribution but are imported explicitly,
file-locally, and used through their package name or alias. Except for the ABI
contract of `core/runtime` and target intrinsics exposed by `core/atomic`, their
declarations are ordinary Draft code that the compiler, tools, and agents
inspect like project code.

The initial core set includes:

| Package | Role |
| --- | --- |
| `core/runtime` | Context, allocator, failure, entry, thread-bridge, and ABI contracts. |
| `core/option` | `option.Option[T]`, an ordinary optional-value variant. |
| `core/result` | `result.Result[T, E]`, an ordinary success-or-error variant. |
| `core/memory` | Typed allocation, arenas, owned buffers and strings, and virtual memory. |
| `core/heap` | Heap allocator backends and direct heap operations. |
| `core/array` | `array.Dynamic[T]`, an owning growable contiguous array. |
| `core/map` | `map.Map[K, V]` and explicit hash-map operations. |
| `core/random` | Context-provided random bytes and deterministic seeded streams. |
| `core/format` | Allocation-free conversion of values into caller-owned byte buffers. |
| `core/console` | Checked human-facing text and scalar output over standard process handles. |
| `core/terminal` | Suspendable raw/screen sessions, timed reads, cell-size queries/watchers, and streaming key decoding. |
| `core/tui` | Owned Unicode-grapheme cell surfaces and deterministic ANSI differential rendering. |
| `core/unicode` | Pinned grapheme segmentation and deterministic terminal display widths. |
| `core/utf8` | Allocation-free strict UTF-8 validation, decoding, encoding, and scalar counting. |
| `core/io` | Stream and input/output interfaces and utilities. |
| `core/os` | Process, argument, environment, file, and operating-system facilities. |
| `core/process` | Exact-path synchronous child-process execution and explicit completion status. |
| `core/atomic` | Compiler-backed atomic values and memory-order operations. |
| `core/thread` | Threads, synchronization, and Draft TLS/context establishment. |
| `core/c_abi` | C ABI scalar aliases for interoperation declarations. |
| `core/testing` | Test harness types and assertions. |
| `core/benchmark` | Benchmark harnesses and performance budgets. |
| `core/time` | Time values, units, clocks, and durations. |

This list can grow without expanding the language grammar or the set of
predeclared names.

`core/format` and `core/console` are ordinary Draft packages rather than
compiler formatting or printing intrinsics. The initial formatting surface
provides base-ten `u64`, `i64`, `u128`, and `i128` conversion into caller-owned
byte slices. `console.println(values: ..type)` writes zero or more strings,
booleans, and ordinary signed or unsigned integers separated by one ASCII space,
then one line feed. It returns the first `core/io.Error`; an unsupported type is
a compile-time error for that concrete call. The other console operations keep
their fixed text/scalar signatures. More formatting policy can grow in these
packages without changing the language or backend.

`core/utf8` explicitly interprets a `string` or `[]u8` as UTF-8 without changing
the built-in types: strings remain arbitrary immutable bytes, indexing still
returns one byte, and `rune` remains one Unicode scalar. The package validates,
decodes in either direction, counts scalars, and encodes into caller-owned
storage. Decoding uses strict shortest-form UTF-8 and reports malformed input;
normalization and locale policy remain separate library concerns.

`core/unicode` segments strict UTF-8 strings into Unicode 17.0.0 extended
grapheme clusters and assigns each printable cluster a deterministic terminal
width. UAX #29 defines boundaries; East_Asian_Width W/F scalars, emoji
presentation sequences, regional indicators, and keycaps occupy two columns,
while ambiguous-width characters occupy one. Controls and clusters with no
printable base are rejected. This is a portable `core/tui` layout policy, not a
locale-sensitive claim about every font or terminal, and it performs no
normalization, shaping, bidi reordering, or line breaking.

`core/random.fill` obtains bytes through the active `Context.random_generator`.
The hosted default provider is operating-system-backed, but the Context may
install a deterministic or otherwise non-security provider; the core API makes
no cryptographic claim on its behalf. `seed_u64` gives those bytes one explicit
little-endian interpretation. Separately, `random.Generator` is an inline,
explicitly seeded deterministic stream with full-width, unbiased bounded, and
unit-`f32` draws. It is suitable for repeatable simulation and procedural work,
not secrets, keys, nonces, or tokens.

`core/time` defines `Duration` as a distinct signed integer type and constants
such as `time.nanosecond: Duration`; the distinct-operator rules make
`200 * time.nanosecond` a `Duration`.

`core/process.run` is the initial hosted child-process operation. It borrows one
exact zero-terminated executable path, supplies no additional arguments,
inherits the parent environment, current directory, and standard handles, waits
synchronously, and distinguishes host process-operation failure from a normal
exit or signal termination. A POSIX child whose exact `execv` fails reports
exit 127; Windows creation failure reports `.unavailable`. It performs no path
search or shell interpretation. More
argument, I/O, and lifetime policy can grow in the package without becoming a
language or compiler feature.

### Growable arrays and maps

Fixed arrays `[N]T` and slices `[]T` are built-in non-allocating types.
`array.Dynamic[T]` is the ordinary core type for an owning, growable contiguous
array. It is initialized with the current or an explicit allocator and destroyed
explicitly. Mutation uses package procedures such as `array.append`,
`array.reserve`, and `array.remove`.

`map.Map[K, V]` is likewise an ordinary core type rather than a language map
primitive. `map.init` receives a `map.Key_Ops[K]` value defining hashing and
equality, plus an optional allocator. Core supplies key operations for common
scalar and string types. Draft 1 defines no map literal or `value[key]` map-indexing
syntax; lookup and mutation use `map.get`, `map.set`, and `map.remove`.

```draft
import core/array
import core/map

values: array.Dynamic[u32]
array.init(&values)
defer array.destroy(&values)
array.append(&values, 42)

counts: map.Map[string, u32]
map.init(&counts, map.string_keys)
defer map.destroy(&counts)
map.set(&counts, "blocks", 3)
(count, found): (u32, bool) = map.get(&counts, "blocks")
```

Both containers store their allocator after initialization. Omitting the
allocator uses `context.allocator`; this remains an ordinary visible semantic
dependency. Their layouts, procedures, and key-operation definitions are
available to synthesis agents through the normal imported-package interface.
