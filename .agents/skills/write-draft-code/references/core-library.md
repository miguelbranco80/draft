# Current Draft Core Library

This is an applied index of the public core surface in this checkout. Core
packages are ordinary Draft source under [`core/`](../../../../core); inspect
the imported package before calling it. The specification's library contract
is in [`docs/specification/02-types-memory-runtime.md`](../../../../docs/specification/02-types-memory-runtime.md#section-7).

This file intentionally records conspicuous absences. A familiar-looking name
from another standard library is not evidence that Draft provides it.

## Contents

- [How to use core](#how-to-use-core)
- [`core/array`](#corearray)
- [`core/atomic`](#coreatomic)
- [`core/benchmark`](#corebenchmark)
- [`core/c`](#corec)
- [`core/console`](#coreconsole)
- [`core/format`](#coreformat)
- [`core/heap`](#coreheap)
- [`core/io`](#coreio)
- [`core/map`](#coremap)
- [`core/memory`](#corememory)
- [`core/option` and `core/result`](#coreoption-and-coreresult)
- [`core/os`](#coreos)
- [`core/runtime`](#coreruntime)
- [`core/testing`](#coretesting)
- [`core/terminal`](#coreterminal)
- [`core/thread`](#corethread)
- [`core/time`](#coretime)
- [`core/utf8`](#coreutf8)
- [Missing facilities](#missing-facilities)

## How to use core

Import each package explicitly in every file that uses it:

```draft
import core/array
import core/io
import core/memory
import core/os
```

Core is deliberately small and direct:

- There are no methods; call `array.append(&values, value)`.
- Owning types have explicit initialization and destruction.
- Recoverable failure is an explicit return value.
- Generic containers copy values and do not invoke element destructors.
- Most allocation failures are assertions in the current API.
- Package implementation comments are part of the API contract and are often
  more precise than the short inventory below.

Only `core/atomic` operations and narrow `core/runtime` bridges receive
compiler-special lowering. Everything else is ordinary Draft code.

## `core/array`

Public surface:

```draft
array.Dynamic[T]
array.init(&value)
array.init_with_allocator(&value, allocator)
array.destroy(&value)
array.reserve(&value, minimum)
array.append(&value, element)
array.remove(&value, index)
array.clear(&value)
array.view(&value) -> []T
```

`Dynamic[T]` owns one contiguous allocation and captures one allocator at
initialization. Do not copy a live value. Growth invalidates slices and element
pointers. `remove` preserves order; `clear` keeps capacity. Neither operation
destroys an owning element.

There is currently no `push`, `pop`, `insert`, iterator, clone, shrink, or
fallible allocation operation.

## `core/atomic`

Public types and operations:

```draft
atomic.Order // .relaxed, .acquire, .release,
             // .acquire_release, .sequentially_consistent
atomic.Value[T]

atomic.init(&value, initial)
atomic.load(&value, order)
atomic.store(&value, replacement, order)
atomic.exchange(&value, replacement, order)
atomic.fetch_add(&value, operand, order)
atomic.fetch_sub(&value, operand, order)
atomic.fetch_and(&value, operand, order)
atomic.fetch_or(&value, operand, order)
atomic.fetch_xor(&value, operand, order)
atomic.compare_exchange(&value, &expected, desired, success, failure)
atomic.fence(order)
```

Supported objects are naturally aligned 1-, 2-, 4-, or 8-byte integers and
pointers; fetch operations require integers. `init` is non-atomic and must
happen before publication. After publication, never access `.storage`
directly.

Orders must be compile-time constants. Current bootstrap lowering requires a
direct call through the imported package; taking these procedures as values or
explicitly specializing them is rejected. Failed compare-exchange updates
`expected` with the observed value.

There is no wait/notify, weak CAS, atomic reference, fetch min/max, or dynamic
memory-order selection.

## `core/benchmark`

`benchmark.Benchmark` exposes `maximum_time`, `last_time`, `samples`,
`failures`, and harness-owned `user`. Public operations are:

```draft
benchmark.require_max_time(&state, duration)
benchmark.measure(&state, operation) // operation: proc()
benchmark.failed(&state) -> bool
```

The package measures one call with `time.monotonic_now`. It has no warmup,
batching, statistics, report formatter, callback arguments, or error channel.
The validation runner owns policy beyond this minimal record.

## `core/c`

Import conventionally as:

```draft
import core/c as c
```

It provides LP64 aliases for the current AArch64 Darwin and GNU targets:
`char`, `signed_char`, `unsigned_char`, `short`, `unsigned_short`, `int`,
`unsigned_int`, `long`, `unsigned_long`, `long_long`,
`unsigned_long_long`, `size_t`, `ssize_t`, `intptr_t`, and `uintptr_t`.

It does not provide libc procedures, errno, C file APIs, `float`, `double`, or
variadic helpers. Do not assume these aliases describe a future Windows or
32-bit C ABI.

## `core/console`

All operations return `io.Error`:

```draft
console.write_to(handle, text)
console.print(text)
console.println("count:", count, true)
console.println()
console.newline()
console.eprint(text)
console.eprintln(text)
console.print_u64(value)
console.print_i64(value)
console.print_bool(value)
```

Console text is written directly from immutable string storage without copying
or allocation. Output completes partial writes and performs no hidden
buffering, terminal detection, Unicode normalization, or newline conversion.
`console.println` is a static `..type` pack, not runtime varargs: it accepts zero
or more strings, booleans, and ordinary signed/unsigned integers of every width,
separates them with one ASCII space, appends one line feed, and returns the first
write error. Floats, runes, enums, endian scalars, distinct values, aggregates,
format strings, interpolation, input, color, and terminal control are absent.

## `core/format`

Allocation-free decimal formatting:

```draft
format.unsigned_decimal(destination: []u8, value: u64) -> ([]u8, bool)
format.signed_decimal(destination: []u8, value: i64) -> ([]u8, bool)
format.unsigned_decimal_128(destination: []u8, value: u128) -> ([]u8, bool)
format.signed_decimal_128(destination: []u8, value: i128) -> ([]u8, bool)
```

The returned slice aliases `destination` and begins at index zero. A false
result means the buffer was too small; the returned slice is empty and the
destination contents are unspecified. There is no parsing, hex/binary, width,
padding, floating-point, rune, or general formatting facility.

## `core/heap`

`core/heap` is a thin convenience layer over `context.allocator`:

```draft
heap.allocator() -> runtime.Allocator
heap.allocate(size, alignment) -> rawptr
heap.resize(storage, old_size, new_size, alignment) -> rawptr
heap.free(storage, size, alignment)
```

The caller retains size, alignment, and ownership metadata. There is no
allocation-size query, typed handle, automatic cleanup, or fallible result.
Prefer `core/memory` typed and owning facilities where they fit.

## `core/io`

`io.Error` has `.none`, `.end_of_input`, `.unavailable`, and `.invalid`.

```draft
io.Reader_Proc :: proc(rawptr, []u8) -> (usize, io.Error)
io.Writer_Proc :: proc(rawptr, []u8) -> (usize, io.Error)

io.Reader{procedure, user}
io.Writer{procedure, user}
io.read(reader, destination) -> (usize, io.Error)
io.write(writer, source) -> (usize, io.Error)
```

The callback record borrows provider-owned `user` state. Callbacks are
ordinary Context-bearing Draft procedures. The wrappers invoke the callback
once; they do not read exactly, write all, close, flush, seek, buffer, or add
asynchrony. A nil procedure is an assertion failure.

## `core/map`

Public surface:

```draft
map.Key_Ops[K]{hash, equal}
map.Map[K, V]
map.init(&value, key_ops)
map.init_with_allocator(&value, key_ops, allocator)
map.destroy(&value)
map.set(&value, key, item)
map.get(&value, key) -> (V, bool)
map.remove(&value, key) -> bool

map.u64_keys
map.u64_key_ops()
map.string_keys
map.string_key_ops()
```

`Map` uses open addressing with linear probing and captures one allocator. Its
hash and equality callbacks are ordinary Draft procedures and must agree
semantically. `get` returns `(zero V, false)` when absent. `set` replaces by
structural assignment.

String keys are borrowed byte views; their storage must outlive membership.
The built-in string hash is deterministic but not collision-hardened for
adversarial keys.

There is no iteration, reserve, clear, contains, entry API, stable reference,
clone, or element destructor. Rebuild, replace, remove, and destroy do not
clean up owning keys or values.

## `core/memory`

Raw and typed allocation:

```draft
memory.allocate(size, alignment)
memory.allocate_with_allocator(size, alignment, allocator)
memory.resize(storage, old_size, new_size, alignment)
memory.resize_with_allocator(storage, old_size, new_size, alignment, allocator)
memory.free_bytes(storage, size, alignment)
memory.free_bytes_with_allocator(storage, size, alignment, allocator)
memory.new[T]()
memory.new_with_allocator[T](allocator)
memory.free(pointer)
memory.free_with_allocator(pointer, allocator)
```

Zero-size allocation returns `nil`; freeing `nil` is harmless. Exact original
size, alignment, and allocator are caller obligations. Required nonzero
allocation failures assert. Custom allocators are not required to zero fresh
storage.

Temporary group allocation:

```draft
memory.temporary_allocate(size, alignment)
memory.temporary_new[T]()
memory.reset_temporary()
```

Reset or thread exit invalidates every temporary pointer together.

Arena API:

```draft
memory.Arena
memory.arena_init(&arena, block_size)
memory.arena_init_with_allocator(&arena, block_size, backing)
memory.arena_allocator(&arena) -> runtime.Allocator
memory.arena_reset(&arena)
memory.arena_destroy(&arena)
```

Keep the Arena stable after exposing its allocator. Reset releases all blocks
but keeps policy reusable; destroy also clears policy.

Fixed owning buffer:

```draft
memory.Buffer[T]
memory.buffer_init(&buffer, length)
memory.buffer_init_with_allocator(&buffer, length, allocator)
memory.buffer_copy(&buffer, source)
memory.buffer_copy_with_allocator(&buffer, source, allocator)
memory.buffer_view(&buffer) -> []T
memory.buffer_destroy(&buffer)
```

The view borrows until destruction. Element copies and destruction are
structural, not deep.

Owned terminated bytes:

```draft
memory.Owned_String
memory.owned_string_copy(&owned, source)
memory.owned_string_copy_with_allocator(&owned, source, allocator)
memory.owned_string_bytes(&owned) -> []u8
memory.owned_string_cstring(&owned) -> cstring
memory.owned_string_destroy(&owned)
```

Both views die at destruction. Embedded zeros remain bytes but terminate C's
view.

Virtual memory:

```draft
memory.Virtual_Protection // .none, .read, .read_write,
                          // .read_execute, .read_write_execute
memory.Virtual_Region
memory.virtual_reserve(size) -> (memory.Virtual_Region, bool)
memory.virtual_protect(&region, protection) -> bool
memory.virtual_commit(&region) -> bool
memory.virtual_release(&region) -> bool
```

Operations cover the complete region. There is no decommit, subrange API,
mapped-file API, guard-page policy, or fallible general allocator.

## `core/option` and `core/result`

These are ordinary tagged unions:

```draft
option.Option[T]        // .none, .some(T)
result.Result[T, E]     // .err(E), .ok(T)
```

Use `switch` and payload binding. Zero `Option` is `.none`; zero `Result` is
`.err(zero E)`. There is no propagation operator, unwrap, map, chaining,
default helper, or compiler magic.

## `core/os`

Process surface:

```draft
os.args() -> []string
os.environment() -> []string
os.process_id() -> u64
os.exit(status)
os.page_size() -> usize
```

Argument and environment slices borrow process-lifetime storage. Environment
entries preserve ordered `name=value` bytes and duplicates.

File surface:

```draft
os.File{descriptor}
os.standard_input
os.standard_output
os.standard_error
os.open_read_only
os.open_write_only
os.open_read_write
os.open_append
os.open_create
os.open_truncate
os.open_exclusive
os.default_creation_permissions
os.file_from_descriptor(descriptor)
os.open(path: cstring, flags, mode) -> (os.File, io.Error)
os.open_for_reading(path: cstring) -> (os.File, io.Error)
os.create_for_writing(path: cstring) -> (os.File, io.Error)
os.read(handle, destination) -> (usize, io.Error)
os.write(handle, source) -> (usize, io.Error)
os.write_all(handle, source) -> io.Error
os.write_text(handle, source: string) -> (usize, io.Error)
os.write_text_all(handle, source: string) -> io.Error
os.close(&handle) -> bool
os.remove(path: cstring) -> bool
```

`File` is a non-RAII descriptor handle; copies alias. Paths are `cstring`, not
ordinary Draft strings. Read, `write`, and `write_text` perform one system call
and may be partial; the corresponding `*_all` operations retry until the
complete source is accepted or an error prevents progress. Text writes use
`raw_data` internally and neither copy nor retain the string. EOF is
`.end_of_input`. Native error detail is currently collapsed to `.unavailable`
or `bool`.

There is no path type, environment lookup, directory traversal, metadata,
seek, flush/fsync, rename, pipe, socket, subprocess, signal, permission API,
detailed errno, or asynchronous I/O.

## `core/runtime`

Public provider records:

- `Allocator_Operation`, `Allocator_Proc`, and `Allocator`;
- `Log_Level`, `Logger_Proc`, and `Logger`;
- `Random_Generator_Proc` and `Random_Generator`;
- `Assertion_Failure_Proc`;
- the versioned `runtime.Context` record.

`runtime.empty_context()` returns a zero Context with nil providers; it is not
usable for allocation, logging, randomness, or assertion reporting until the
caller populates those capabilities.

Public runtime bridges are:

```draft
runtime.default_context() -> runtime.Context
runtime.install_thread_context(&context)
runtime.reset_temporary_allocator()
runtime.call_with_context(&context, ordinary_procedure, arguments...)
```

`call_with_context` is compiler-specialized at each exact callback signature;
the zero-parameter source declaration is not a C variadic ABI. The supplied
context is dynamic call state and is not permanently installed as thread
default. These bridges are primarily for C callbacks and `core/thread`; use
ordinary calls in ordinary Draft code.

There are no public logging/random convenience procedures, Context guard,
allocator error type, or provider ownership framework.

## `core/testing`

`testing.Test` contains harness-owned counters and `user`. Public operations:

```draft
testing.expect(&test, condition)
testing.expect_message(&test, condition, message)
testing.failed(&test) -> bool
```

Expectations record failures rather than trap. `expect_message` currently does
not persist or render its message. There are no equality helpers, fixtures,
skip, fatal expectation, reporter callback, or expected-failure mechanism.

## `core/terminal`

Interactive input and full-screen surface:

```draft
terminal.Session
terminal.begin_raw(&session, input: os.File) -> io.Error
terminal.read(&session, destination, timeout: time.Duration)
    -> (usize, io.Error)
terminal.restore(&session) -> bool

terminal.Screen
terminal.begin_screen(&screen, output: os.File) -> io.Error
terminal.write_screen(&screen, frame: []u8) -> io.Error
terminal.restore_screen(&screen) -> bool

terminal.Decoder
terminal.Key
terminal.Key_Kind // none, byte, escape, up/down/right/left,
                  // home/end, delete, page_up/page_down
terminal.decode_key(&decoder, byte: u8) -> terminal.Key
terminal.flush_key(&decoder) -> terminal.Key

terminal.cursor_home
terminal.erase_line_tail
terminal.erase_screen_tail
terminal.reset_style
```

Session owns a saved native input mode but borrows the descriptor. Keep an
active Session at one stable address, pair successful `begin_raw` with
`restore`, and keep the descriptor open through restoration. `read` rounds the
supplied duration upward to poll's millisecond resolution; a timeout is
`(0, .none)`, hangup is `.end_of_input`, and native failures are `.unavailable`.
Raw mode makes control keys ordinary bytes, so applications can handle Ctrl-C
and leave through normal cleanup.

Screen borrows its output descriptor and owns only the alternate-screen cleanup
obligation. Pair every active Screen with `restore_screen`; unlike begin_raw, a
failed `begin_screen` may leave Screen active because a prefix of the ANSI enter
sequence may already have changed terminal state. Applications construct frame
bytes in their own storage and publish them through `write_screen`. Public
`cursor_home`, `erase_line_tail`, `erase_screen_tail`, and `reset_style` byte
strings are available for fixed-buffer frame construction.

Decoder preserves ordinary input as `.byte` keys and recognizes common ANSI
arrow, home/end, delete, and page sequences across fragmented reads. Escape
alone is emitted only by `flush_key`, after the caller's chosen timeout. It does
not decode UTF-8: each source byte remains an ordinary byte for the application
or `core/utf8` to interpret. Modified navigation CSI forms collapse to the base
navigation kind. Function keys have no Key_Kind; common SS3 function-key
sequences are consumed as unsupported input rather than leaking their final byte
as an ordinary command.

The package deliberately has no frame/layout builder, terminal-size query,
signal recovery, mouse protocol, event loop, terminfo, or ncurses dependency.
The current native-mode implementation exists for AArch64 macOS and AArch64
GNU/Linux through exact target-qualified termios and poll layouts; its ANSI
screen and key policy is target-independent.

## `core/thread`

Public surface:

```draft
thread.Entry_Proc :: proc(user: rawptr)
thread.Thread
thread.Mutex
thread.Condition

thread.spawn(entry, user) -> (thread.Thread, bool)
thread.join(&thread) -> bool
thread.current_id() -> uintptr
thread.yield()
thread.mutex_init(&mutex) -> bool
thread.mutex_destroy(&mutex) -> bool
thread.mutex_lock(&mutex)
thread.mutex_try_lock(&mutex) -> bool
thread.mutex_unlock(&mutex)
thread.condition_init(&condition) -> bool
thread.condition_destroy(&condition) -> bool
thread.condition_wait(&condition, &mutex)
thread.condition_signal(&condition)
thread.condition_broadcast(&condition)
thread.sleep(duration) -> bool
```

Spawn copies the active Context but gives the child thread-owned temporary
storage. The `user` pointer is borrowed until completion. Join clears the
handle after success. Mutexes and conditions must remain at stable addresses;
waits belong in predicate loops. Some lifecycle operations return `bool`,
while lock/wait/signal failures assert.

There is no detach, thread return value, cancellation, timed condition wait,
sleep retry, builder, once, read/write lock, semaphore, affinity, naming, or
automatic join.

## `core/time`

```draft
time.Duration             // distinct i64 nanoseconds
time.nanosecond
time.microsecond
time.millisecond
time.second
time.monotonic_now() -> time.Duration
```

This is monotonic duration measurement, not wall-clock/calendar time. There is
no wall clock, date, timer, timeout, deadline, conversion, formatting, or
checked-overflow layer. Sleeping is in `core/thread`.

## `core/utf8`

Allocation-free strict UTF-8 operations:

```draft
utf8.Decode_Error // .none, .end_of_input, .invalid_encoding
utf8.replacement_rune

utf8.decode_next(text: string, offset) -> (rune, usize, Decode_Error)
utf8.decode_next_bytes(bytes: []u8, offset) -> (rune, usize, Decode_Error)
utf8.decode_previous(text: string, offset) -> (rune, usize, Decode_Error)
utf8.decode_previous_bytes(bytes: []u8, offset) -> (rune, usize, Decode_Error)
utf8.valid(text) -> bool
utf8.valid_bytes(bytes) -> bool
utf8.rune_count(text) -> (usize, bool)
utf8.rune_count_bytes(bytes) -> (usize, bool)
utf8.rune_size(value) -> usize
utf8.encode(destination, value) -> (usize, bool)
```

Offsets and widths are byte counts. Forward offsets and reverse exclusive
boundaries must be within the input. A clean end returns `replacement_rune`,
zero, and `.end_of_input`. Malformed input returns `replacement_rune`, one, and
`.invalid_encoding`, so a caller that chooses replacement can always progress.
Validation and counting stop at the first malformed byte; the count excludes
that byte. `encode` writes only when the complete canonical sequence fits and
otherwise leaves the destination unchanged.

The package borrows all storage and performs no allocation or mutation of
input. It does not provide iteration syntax, normalization, Unicode properties,
grapheme segmentation, case mapping, locale handling, or display width.

## Missing facilities

The current core does not yet supply several facilities a larger application
may need:

- path manipulation, directories, metadata, random-access files, or rename;
- window-size queries, mouse input, or signal-safe terminal recovery;
- sockets, subprocesses, signals, dynamic libraries, or event loops;
- Unicode normalization, properties, case mapping, and grapheme algorithms;
- general formatting/parsing and string builders;
- ownership-aware algorithms, iterators, smart pointers, or automatic cleanup;
- broad math, compression, crypto, image, or GUI packages.

Do not hide these gaps behind invented APIs. For a generally reusable need,
design the smallest portable core abstraction plus explicit target-specific
implementation and tests. For application policy, keep the operation local.
