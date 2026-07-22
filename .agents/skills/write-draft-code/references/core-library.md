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
- [`core/c_abi`](#corec_abi)
- [`core/console`](#coreconsole)
- [`core/format`](#coreformat)
- [`core/heap`](#coreheap)
- [`core/io`](#coreio)
- [`core/map`](#coremap)
- [`core/memory`](#corememory)
- [`core/option` and `core/result`](#coreoption-and-coreresult)
- [`core/os`](#coreos)
- [`core/process`](#coreprocess)
- [`core/random`](#corerandom)
- [`core/runtime`](#coreruntime)
- [`core/testing`](#coretesting)
- [`core/terminal`](#coreterminal)
- [`core/tui`](#coretui)
- [`core/unicode`](#coreunicode)
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

## `core/c_abi`

Import directly as:

```draft
import core/c_abi
```

It provides target-selected C scalar aliases for the current profiles:
`char`, `signed_char`, `unsigned_char`, `short`, `unsigned_short`, `int`,
`unsigned_int`, `long`, `unsigned_long`, `long_long`,
`unsigned_long_long`, `size_t`, `ssize_t`, `intptr_t`, and `uintptr_t`.

Darwin and GNU/Linux are LP64, so C `long` is i64/u64. Windows is LLP64, so C
`long` is i32/u32 while pointers, `size_t`, `ssize_t`, and pointer-sized Draft
integers remain 64 bits; C `long long` remains i64/u64.

Use qualified names such as `c_abi.int` and `c_abi.size_t`. It does not provide
libc procedures, errno, C file APIs, `float`, `double`, or variadic helpers. A
future 32-bit C ABI still requires its own explicit target definitions.

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
memory.owned_string_copy_bytes(&owned, source_bytes)
memory.owned_string_copy_bytes_with_allocator(&owned, source_bytes, allocator)
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

These are ordinary variants:

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
seek, flush/fsync, rename, pipe, socket, signal, permission API, detailed
errno, or asynchronous I/O. The separate minimal child-process boundary is in
`core/process`.

## `core/process`

Blocking execution of one exact executable path:

```draft
process.Error // .none, .unavailable
process.Result{error, exited, exit_code, signal}
process.run(path: cstring) -> process.Result
```

`run` supplies no extra arguments, inherits the current directory, environment,
and standard handles, and waits for completion. A nonzero child exit still has
`error == .none`; inspect `exited`, `exit_code`, and `signal`. The path must be
an exact zero-terminated executable path: there is no PATH lookup, command
string, shell, quoting, pipes, redirection, detached/background lifetime,
cancellation, or asynchronous API. A POSIX `execv` failure appears as exit 127;
a Windows creation failure returns `.unavailable`. Keep the input cstring alive
through the synchronous call.

## `core/random`

Context-provided bytes and deterministic application streams:

```draft
random.fill(destination: []u8) -> bool
random.seed_u64() -> (u64, bool)

random.Generator{state}
random.init(&generator, seed)
random.next_u64(&generator) -> u64
random.bounded_u64(&generator, upper_bound) -> u64
random.unit_f32(&generator) -> f32
```

`fill` borrows and initializes caller storage through
`context.random_generator`; empty input succeeds, while a missing/failing
provider returns false. Hosted defaults use OS facilities, but a custom Context
can install any provider, so do not infer cryptographic security from this API.
`seed_u64` uses an explicit little-endian byte interpretation and returns
`(0, false)` rather than using a partially written provider result.

`Generator` is a copyable inline deterministic xorshift64* stream. Seed once,
then pass `^Generator` for advancement; seed zero is normalized to the package's
fixed nonzero default. `bounded_u64` is unbiased and requires a nonzero bound;
`unit_f32` lies in `[0, 1)`. This stream is for simulations, games, procedural
generation, and tests—not secrets, keys, nonces, or tokens.

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

Logging has no public convenience facade. Random byte and deterministic stream
operations live in `core/random`; runtime retains only their provider ABI.
There is no Context guard, allocator error type, or provider ownership
framework.

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
terminal.Session_State // inactive, active, suspended
terminal.begin_raw(&session, input: os.File) -> io.Error
terminal.suspend(&session) -> bool
terminal.resume(&session) -> bool
terminal.read(&session, destination, timeout: time.Duration)
    -> (usize, io.Error)
terminal.restore(&session) -> bool

terminal.Screen
terminal.Screen_State // inactive, active, suspended
terminal.begin_screen(&screen, output: os.File) -> io.Error
terminal.suspend_screen(&screen) -> bool
terminal.resume_screen(&screen) -> bool
terminal.write_screen(&screen, frame: []u8) -> io.Error
terminal.restore_screen(&screen) -> bool

terminal.Size{columns, rows}
terminal.query_size(terminal_file: os.File) -> (terminal.Size, io.Error)

terminal.Resize_Watcher
terminal.Resize_Watcher_State // inactive, active
terminal.begin_resize_watch(&watcher, terminal_file)
    -> (terminal.Size, io.Error)
terminal.poll_resize(&watcher) -> (terminal.Size, bool, io.Error)
terminal.end_resize_watch(&watcher) -> bool

terminal.Decoder
terminal.Key
terminal.Key{kind, byte_value, shift, alt, control}
terminal.Key_Kind // byte, escape, enter/tab/backspace, navigation/editing,
                  // page_up/page_down, f1-f12, or none
terminal.decode_key(&decoder, byte: u8) -> terminal.Key
terminal.flush_key(&decoder) -> terminal.Key

terminal.Input
terminal.Input_Kind // none, key, mouse
terminal.Mouse
terminal.Mouse_Action // move, press, release, wheel, none
terminal.Mouse_Button // primary, middle, secondary, none
terminal.decode_input(&decoder, byte: u8) -> terminal.Input
terminal.flush_input(&decoder) -> terminal.Input

terminal.Mouse_Reporting
terminal.Mouse_Reporting_State // inactive, active, suspended
terminal.begin_mouse_reporting(&reporting, &screen) -> io.Error
terminal.suspend_mouse_reporting(&reporting) -> bool
terminal.resume_mouse_reporting(&reporting) -> bool
terminal.restore_mouse_reporting(&reporting) -> bool

terminal.cursor_home
terminal.erase_line_tail
terminal.erase_screen_tail
terminal.reset_style
```

Session owns a saved native input mode but borrows the descriptor. Keep a
non-inactive Session at one stable address, pair successful `begin_raw` with
`restore`, and keep the descriptor open through restoration. `suspend` restores
the saved mode while retaining the obligation; `resume` reapplies raw mode.
Both are idempotent in their destination state and return false from inactive
state. `read` requires active state, rounds the duration upward to poll's
millisecond resolution, returns `(0, .none)` on timeout and `.end_of_input` on
hangup. Raw mode makes control keys ordinary bytes, so applications can handle
Ctrl-C and leave through normal cleanup.

Screen borrows its output descriptor and owns only the alternate-screen cleanup
obligation. Pair every non-inactive Screen with `restore_screen`.
`suspend_screen` leaves the alternate screen; `resume_screen` enters it again.
A failed begin or resume may leave Screen active because a control-sequence
prefix may already have changed terminal state. Applications construct frame
bytes in their own storage and publish them through `write_screen`. Public
`cursor_home`, `erase_line_tail`, `erase_screen_tail`, and `reset_style` byte
strings are available for fixed-buffer frame construction.

When coordinating both resources, acquire Session then Screen; suspend Screen
then Session; resume Session then Screen; and restore Screen then Session.
Always attempt both final restorations even if the first fails.

`query_size` borrows any terminal descriptor and reports cell columns/rows via
the selected POSIX `TIOCGWINSZ` request or Windows visible console rectangle.
Invalid descriptors return
`.invalid`, native failures return `.unavailable`, and successful zero
dimensions are preserved for application fallback policy.

`Resize_Watcher` borrows the queried descriptor and is a non-copyable cleanup
owner. Only one may be active in a process. On POSIX, begin installs an
atomic-only SIGWINCH handler only when the prior disposition is `SIG_DFL`;
explicit ignore/custom dispositions produce `.unavailable` and remain
untouched. A resize interrupts `terminal.read` as a successful zero-byte turn,
then `poll_resize` consumes the coalesced notification and queries the size.
End restores the complete saved action and retains active state if restoration
fails. Windows compares a native size query on every poll behind the same API.

Decoder preserves ordinary input as `.byte` keys and recognizes Enter, Tab,
Backspace, cursor, home/end, insert/delete, page, and F1-F12 forms across
fragmented reads. Escape alone is emitted only by `flush_key`, after the
caller's chosen timeout. An Escape-prefixed ordinary byte carries Alt; xterm
modifier parameters carry Shift/Alt/Control on semantic keys. It does not decode
UTF-8: each source byte remains available to the application or `core/utf8`.
Physical combinations that a byte protocol cannot distinguish—such as Enter
and Ctrl-M—are not guessed. `decode_input` additionally recognizes fragmented
xterm SGR mouse reports with zero-based coordinates, press/release, primary
drag state, wheel direction, and modifiers. `decode_key` consumes those reports
without emitting punctuation when a key-only application enabled no mouse.

`Mouse_Reporting` borrows an active Screen and owns the obligation to disable
ANSI button, drag, motion, and SGR modes. Disable/suspend it before suspending a
Screen, resume it after the Screen, and restore it before `restore_screen`.

The package deliberately has no frame/layout builder, general signal/job-
control layer, event loop, terminfo, or ncurses dependency.
The current native-mode implementation exists for AArch64 macOS, AArch64/
x86-64 GNU/Linux, and x86-64 Windows. POSIX owns exact termios, poll, and ioctl
contracts; Windows owns console modes, handle waits, and visible-window size.
The ANSI/VT screen and key policy is target-independent.

The built-in target denial summaries do not yet audit the terminal-specific
`tcgetattr`, `tcsetattr`, `cfmakeraw`, `poll`, `ioctl`, `sigaction`, or
`sigemptyset` calls. A reachable
native terminal operation beneath an active `deny` therefore fails closed as
an unknown foreign call until that explicit target coverage is added.

## `core/tui`

Allocation-backed Unicode-grapheme cell surfaces and ANSI differential
rendering:

```draft
tui.Color
tui.indexed_color(index: u8) -> tui.Color
tui.Style{foreground, background, bold, dim, underline, reverse}
tui.Cell{value, columns, continuation, style}

tui.Surface
tui.surface_init(&surface, columns, rows)
tui.surface_init_with_allocator(&surface, columns, rows, allocator)
tui.surface_destroy(&surface)
tui.clear(&surface, style)
tui.put(&surface, column, row, byte_value, style) -> bool
tui.put_rune(&surface, column, row, value, style) -> bool
tui.fill(&surface, column, row, width, height, value, style) -> bool
tui.write_ascii(&surface, column, row, text, style) -> bool
tui.write_ascii_bytes(&surface, column, row, bytes, style) -> bool
tui.write_utf8(&surface, column, row, text, style) -> bool
tui.write_utf8_bytes(&surface, column, row, bytes, style) -> bool
tui.cell_at(&surface, column, row) -> (tui.Cell, bool)

tui.Renderer
tui.renderer_init(&renderer, columns, rows)
tui.renderer_init_with_allocator(&renderer, columns, rows, allocator)
tui.renderer_destroy(&renderer)
tui.surface(&renderer) -> ^tui.Surface
tui.renderer_resize(&renderer, columns, rows)
tui.invalidate(&renderer)
tui.present(&renderer, &terminal_screen) -> io.Error
```

Surface and Renderer are non-copyable owners. Renderer owns its desired and
last-published surfaces plus a reusable output buffer; it borrows Screen only
for `present`. A successful present commits the desired frame. A write failure
invalidates history so the next attempt clears and repaints completely.
`renderer_resize` discards both frames when dimensions change; equal dimensions
are an exact no-op and preserve existing borrows. `present` requires an active
Screen even for an empty diff. Call `invalidate` after every resume attempt that
may have emitted terminal bytes—successful or failed—and after any out-of-band
terminal output.

Coordinates are zero-based terminal columns. `put` remains the printable-ASCII
byte operation; `put_rune` accepts one printable standalone scalar. `fill`
requires a one-column scalar because its width is measured in cells. ASCII and
UTF-8 writers validate their complete input and range before mutation and do
not clip. `write_utf8` and `write_utf8_bytes` copy multi-scalar graphemes into
Surface-owned storage, so caller text/bytes are never retained.

A wide glyph owns one leading cell plus a continuation. Replacing either half
clears the complete old glyph. `cell_at` exposes the first scalar, width, style,
and continuation state but not the Surface's private grapheme bytes. The zero
Style means terminal defaults; indexed colors cover ANSI palette entries
0-255. Rendering emits maximal changed runs with absolute cursor positions,
never splits a grapheme, and resets style at the update boundary.

There are no widgets, layout engine, text wrapping, clipping regions, input
dispatch, focus, event loop, mouse support, normalization, shaping, bidi,
locale tailoring, transparency, or retained application pointers. Build those
policies over Surface only when an application demonstrates the reusable shape.

## `core/unicode`

Pinned Unicode 17.0.0 grapheme and terminal-width policy:

```draft
unicode.Grapheme_Error // none, end_of_input, invalid_encoding, nonprinting
unicode.rune_columns(value) -> (columns: usize, printable: bool)
unicode.next_grapheme(text, byte_offset)
    -> (end_byte: usize, columns: usize, unicode.Grapheme_Error)
unicode.next_grapheme_bytes(bytes, byte_offset)
    -> (end_byte: usize, columns: usize, unicode.Grapheme_Error)
```

Both forms borrow strict UTF-8 and return one extended grapheme boundary
using UAX #29, including Hangul, Indic conjuncts, emoji ZWJ sequences, and
regional-indicator pairs. Width is deterministic across targets: East-Asian
W/F, emoji, flags, and keycaps are two columns; ambiguous characters are one.
Controls and mark/format-only clusters return `nonprinting` after consuming the
complete cluster. This is `core/tui` policy, not locale-sensitive font
measurement. There is no normalization, shaping, bidi, or line breaking.

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
pthread or Windows HANDLE after success. Mutexes and conditions must remain at
stable addresses; waits belong in predicate loops. Windows maps them to
exclusive SRW locks and condition variables. Some lifecycle operations return
`bool`, while lock/wait/signal failures assert.

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
- general signal-safe terminal recovery beyond the scoped resize handler;
- sockets, argument-bearing/redirected/background processes, general signals,
  dynamic libraries, or event loops;
- Unicode normalization, general property queries, case mapping, shaping, bidi,
  locale tailoring, or line breaking;
- general formatting/parsing and string builders;
- ownership-aware algorithms, iterators, smart pointers, or automatic cleanup;
- broad math, compression, crypto, image, or GUI packages.

Do not hide these gaps behind invented APIs. For a generally reusable need,
design the smallest portable core abstraction plus explicit target-specific
implementation and tests. For application policy, keep the operation local.
