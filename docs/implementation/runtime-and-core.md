# Hosted runtime and core packages

This document records the hosted runtime, Context implementation,
allocator/core facilities, process and thread support, and compiler-backed
atomic surface. Portable language behavior remains in the specification;
exact machine and OS facts live in the selected target profile.

## Compiler-distributed source and runtime

Every compiler client links an immutable byte table containing the exact
target-qualified source below `core/`. Workspace loading selects and parses
those bytes directly from memory. It does not search the checkout, the current
directory, an environment variable, or a path beside `draftc`. The generator
sorts relative filenames and computes one build-time content identity from the
framed names and bytes; that identity is the semantic root identity for every
`core/...` package.

The hosted runtime remains a separate compiler-distributed input because it is
already target machine code rather than Draft source. CMake compiles one object
for each supported target and embeds those exact object and assembly bytes in
the compiler. A native build therefore needs neither the repository's `core/`
tree nor an installed runtime sidecar.

The five factual Markdown references shared with the repository's Draft coding
skill form a third, independent distribution asset. `draftc` embeds that compact
read-only bundle for internal code-producing Codex operations, not the skill's
repository workflow or any core implementation source. An external coding
agent installs the repository/marketplace skill in its own environment.
Ordinary checking, building, and running use neither the external skill nor
Codex.

## Initial hosted runtime context layout

Status: bootstrap runtime ABI; synchronized with `core/runtime` by tests.

The generated core content identity names the exact target-qualified
Darwin/Linux/Windows OS, memory, thread, time, package-assembly, formatting,
terminal, and console source bundle. It includes the typed
immutable-string write path:
core code may pass existing string storage to a synchronous nonmutating native
write without manufacturing a mutable byte slice or copying through a bounded
buffer.

All current native profiles use a root Context of 96 bytes with 8-byte
alignment. Its fields
begin at offsets 0, 16, 32, 40, 56, 72, 80, and 88, in the source order declared
by `core/runtime.Context`. Allocator, logger, and random-generator provider
records each contain a procedure pointer and a provider-state pointer. The
assertion callback is an ordinary Draft procedure pointer, so its physical call
prepends the active Context pointer.

One target-selected compiler-distributed runtime object defines runtime failure
helpers and root process state. Every package LLVM unit which uses those
services references the hidden link-unit symbols; only unit zero of the root
package adds the small hosted `main`/`wmain` wrapper when building an
executable. This gives all ordinary calls one coherent Context and prevents
per-package runtime state from emerging as a bootstrap artifact. Changing this
layout or helper contract changes the generated core identity and requires a
corresponding runtime ABI review.

`context` is a predeclared, addressable value in every ordinary Draft procedure.
When `core/runtime` is imported, its type is exactly the public
`runtime.Context`; otherwise the compiler uses a private ABI-identical nominal
type. A lexical block that assigns a Context field or takes the address of one
starts with a complete copy of the surrounding Context. Calls in that block use
the copy, and leaving the block restores the surrounding pointer. A `c proc`
has no implicit Context and may not name `context`.

Two compiler-owned bridges cover that C boundary. `runtime.default_context`
lazily initializes Draft TLS from the process-default Context and returns the
calling thread's snapshot through the selected C ABI's indirect aggregate-result
convention. `runtime.call_with_context` statically checks a non-nil `^Context`,
an ordinary Draft callback, and the callback's exact arguments, initializes
Draft TLS when entered from a foreign-created thread, then lowers directly to
that callback with the explicit hidden Context pointer. Named callbacks retain
their ordinary effect summaries; indirect callbacks remain unknown edges.
These are narrow versioned runtime exceptions, not permission to use Context in
arbitrary C signatures. The supplied pointer remains dynamic-call state and is
not installed as the thread default.

The hosted default allocator implements the three `core/runtime` operations
against the selected host heap. POSIX targets use `calloc`, `realloc`, and
`posix_memalign`. Windows keeps a malloc base pointer in one private header
before every aligned result; allocate/resize/release can then share one exact
zeroing and ownership rule for every alignment. Both paths preserve the old
allocation when a growing replacement fails. The root and each lazy thread
Context use this provider for general allocation.

The temporary provider owns a direct list of separately aligned allocations in
thread-specific state. POSIX selects a pthread key; Windows selects one
race-published fiber-local-storage key with a destructor, which also covers
foreign-created threads. Individual free is a no-op, resize allocates and
preserves the live prefix, explicit reset releases the list, native thread exit
destroys the state, and hosted main releases its state before process teardown.
`core/memory` exposes explicit temporary helpers and reset without hiding a
call-boundary reset. The runtime installs a stderr logger. POSIX random bytes
come from `arc4random_buf`; Windows fills exact byte ranges from UCRT `rand_s`
words, including a bounded final partial word.

The compiler-distributed runtime source is compiled for each target by the LLVM
toolchain selected to build the bootstrap. It uses the selected libc's physical
pthread typedefs. Darwin's `pthread_once_t` is a 16-byte signature record
initialized with its fixed `PTHREAD_ONCE_INIT` value and `pthread_key_t` is 64
bits. Under the selected glibc contract, `pthread_once_t` is one
zero-initialized 32-bit word
and `pthread_key_t` is 32 bits. Their declarations, globals, loads, and calls
all use the matching type and alignment. Windows uses an atomic FLS-key state
machine instead of fabricating a pthread layout. These target facts never enter
Draft's source-visible type system.

## Random bytes and deterministic streams

`core/random.fill` is the small ordinary-Draft facade over the active Context
provider. It forwards a nonempty mutable slice as `{pointer, count}`, treats an
empty slice as a completed no-op, and reports a nil or failing provider as
`false`. `seed_u64` requests exactly eight bytes and assembles them explicitly
in little-endian order, so custom-provider tests do not depend on host byte
order. Provider failure does not reinterpret a possibly written prefix.

`random.Generator` is intentionally independent of that ambient capability.
Initialization normalizes xorshift64*'s zero absorbing state, then every draw is
pure specified wrapping `u64` arithmetic. Bounded draws reject the incomplete
low residue range before applying remainder, avoiding modulo bias. `unit_f32`
uses the high 24 bits so every result is exactly representable and below one.
Games seed a Generator once—normally from the hosted provider—and use only the
local stream afterward, preserving deterministic simulation tests and avoiding
an operating-system call for every gameplay choice.

## Initial core memory facilities

Status: ordinary Draft library surface over the allocator and target-selected
Darwin/GNU/Windows ABIs.

`core/memory.Arena` is a direct linked list of backing blocks with an absolute
address-aligned bump cursor. Its allocator performs allocate and preserving
resize, treats individual free as a logical no-op, and releases complete blocks
on explicit reset/destroy. Block metadata and bytes use the caller-selected
backing allocator, so no compiler ownership mechanism is hidden behind the
handle.

`memory.Buffer[T]` owns one fixed-length typed allocation. `Owned_String` owns a
zero-terminated copy made from either an immutable `string` or a borrowed byte
slice and exposes a mutable bounded byte view plus `cstring`; Draft's built-in
`string` remains an immutable non-owning view and the library does not fabricate
one through an undocumented cast. Both handles store their allocator and
require explicit destruction.

## Formatting and console output

Status: ordinary Draft library surface over `core/os` byte and text writes; no
formatting or printing intrinsic.

`core/format` converts `u64`, `i64`, `u128`, and `i128` values to shortest
base-ten byte slices inside caller-owned storage. The result borrows that
storage and formatting allocates nothing. Each signed path computes magnitude
in the same-width unsigned type, so the minimum signed value is handled without
signed overflow. A too-small destination returns an empty slice and `false`.

`core/os.write_all` and `core/os.write_text_all` complete borrowed byte-slice
and immutable-string writes by retrying partial native writes. Both reject a
successful zero-byte write on a nonempty suffix so callers cannot spin without
progress. The text path uses the specified `raw_data(string)` bridge and passes
the existing pointer plus length to the synchronous native write operation; it
does not allocate, copy, terminate, or reinterpret encoding. The platform
contract promises that the native call neither mutates nor retains those bytes.
Native conformance replaces only that platform seam with a deterministic
three-byte writer. It checks literal, sliced, empty, and 16-KiB strings, an
immediate native error, an error after a partial prefix, and successful zero
progress. The same fixture inspects MIR and reachable effects to prove that the
production console path reaches the one raw-data extraction in `core/os`
without introducing a console-side representation escape.

`core/console` sends strings and booleans through that text path, sends formatted
integers through caller-owned byte slices, and returns `core/io.Error`.
`console.println` uses a static `..type` pack: the source loop selects string,
bool, signed-integer, or unsigned-integer formatting through dependent `when`,
while every compiled call is an ordinary fixed-signature procedure. Arguments
are separated by one ASCII space, zero arguments produce only one line feed,
and the first write error stops the sequence. There is no runtime format
registry, erased `any`, allocation, copy buffer, or variadic ABI.

## UTF-8 byte/scalar conversion

Status: ordinary allocation-free Draft library code; no compiler or backend
intrinsic.

`core/utf8` preserves the language's deliberate separation between byte strings
and Unicode scalar values. Its string and byte-slice entry points copy at most
four candidate bytes into fixed stack storage, apply the complete shortest-form
UTF-8 table, and return `(rune, byte_width, Decode_Error)`. Successful callers
advance by the returned width. Malformed input returns U+FFFD with a one-byte
recovery width, which makes an explicit replacement loop progress without
hiding the error; an exact end boundary instead reports `end_of_input` with
zero width.

Reverse decoding examines at most four possible starts and accepts only a
strict forward decode ending at the supplied exclusive boundary. Validation
and scalar counting stop at the first malformed byte. Encoding first proves
that caller-owned output storage can hold the complete canonical sequence, so
a failed write changes no bytes. The package performs no allocation,
normalization, grapheme segmentation, display-width calculation, or locale
handling.

## Interactive terminal sessions

Status: ordinary Draft library policy over target-selected Darwin/glibc/Windows
console operations; no ncurses, event framework, or compiler intrinsic.

`core/terminal.Session` is a move-by-convention owner of one restoration
obligation, not of the underlying `os.File`. `begin_raw` first obtains the
complete native terminal mode, derives the platform's conventional raw mode
and applies it immediately, then publishes Session state only after both native
operations succeed. Raw mode makes control bytes—including
Ctrl-C—application input, allowing normal application cleanup to restore the
terminal. The explicit inactive/active/suspended state also supports process
job control without hiding signals: `suspend` restores the saved mode while
retaining the descriptor borrow, and `resume` derives and reapplies raw mode.
Failed transitions retain their source state. `restore` is idempotent when
inactive and ends an already-suspended obligation without another native call.

`terminal.read` combines one target readiness wait with one ordinary `os.read`.
Durations are rounded upward to millisecond resolution and saturated at the
largest positive C `int`. POSIX uses `poll`; Windows waits on the descriptor's
console handle. Timeout is the successful `(0, .none)` case, hangup is
`.end_of_input`, and the initial core error vocabulary collapses other native
failures to `.unavailable`.

`terminal.discard_pending_input` is the explicit destructive cleanup operation
for byte-producing terminal protocols. After an application disables mouse or
another report mode, it consumes already-queued bytes until input has remained
quiet for fifty milliseconds. A 250-millisecond total bound prevents continuous
input from delaying terminal restoration indefinitely. The Session remains
active throughout, so the application can then restore cooked mode; every later
cleanup must still be attempted if discarding fails.

`core/terminal.Screen` separately owns the obligation to leave an ANSI/VT
alternate screen and show the cursor. It borrows the output descriptor and
becomes active before writing the enter sequence, because a failed complete
write may already have published a state-changing prefix. Its matching
inactive/active/suspended states let an application leave the alternate screen
before yielding terminal control and re-enter it afterward. Applications build
their own complete frame bytes and publish them through `write_screen`; small
public cursor-home, erase, and synchronized-update fragments support that
construction without introducing a layout engine or fixed library buffer.
Entering, suspending, resuming, or leaving a Screen first emits the synchronized
update end marker, so a renderer write which failed after only its begin marker
cannot retain a hidden terminal frame across job control or cleanup.

An application coordinating both owners acquires Session then Screen, suspends
Screen then Session, resumes Session then Screen, and restores Screen then
Session. It attempts both final restorations even if the first fails. This
ordering exposes the primary screen and saved input mode to external work and
does not re-enter the alternate screen when raw input could not be reacquired.

`terminal.query_size` uses one exact eight-byte exchange record. Darwin and
Linux pass that compatible winsize shape through their target-owned variadic
`ioctl`; Windows derives it from `GetConsoleScreenBufferInfo`'s visible window.
The public result widens rows and columns to `usize`, preserves a successful
zero dimension, and deliberately omits pixel dimensions because cell layout is
the portable text-application contract.

`terminal.Resize_Watcher` turns those snapshots into one explicit change
stream. One move-by-convention watcher owns the process observation slot and a
borrowed output descriptor. On Darwin and Linux, begin reads the existing
SIGWINCH disposition and installs a context-free handler only when that
disposition is `SIG_DFL`; an explicit ignore or application handler is never
replaced. The handler performs one relaxed store to a 32-bit `core/atomic`
value. Its action deliberately omits `SA_RESTART`, so an interrupted
`terminal.read` returns a successful empty turn when that flag is pending and
the event loop can call `poll_resize`. Notifications coalesce, and the first
poll always re-queries to close the setup race. End restores the complete saved
action before releasing the process slot; failed restoration retains ownership
for retry. Windows has no corresponding process signal, so the same public
poll compares one synchronous visible-window query each turn. This preserves
one portable API without translating unrelated console input records into the
byte-oriented Session stream.

The allocation-free `Decoder` preserves ordinary input—including control and
UTF-8 bytes—as byte keys and recognizes Enter, Tab, Backspace, cursor,
home/end, insert/delete, page, F1-F12, and xterm SGR mouse forms across
arbitrary read boundaries. `decode_input` returns a tagged key or mouse result;
the original `decode_key` consumes and discards complete mouse sequences for
key-only applications. Mouse results carry zero-based cells, press/release,
drag motion, wheel direction, and explicit Shift/Alt/Control state. A lone
Escape remains pending until the application calls `flush_input` or
`flush_key` after its own timeout. Escape-prefixed bytes and xterm numeric
parameters preserve explicit modifiers, while combinations the byte protocol
cannot distinguish are not guessed.

`Mouse_Reporting` is a separate move-by-convention restoration obligation that
borrows an active `Screen`. Its selected `Mouse_Reporting_Mode` is explicit
application policy: `buttons_and_drag` enables button, drag, and SGR reports
with ANSI modes 1000/1002/1006, while `all_motion` additionally enables mode
1003 so unpressed pointer movement can drive hover. The former is the default
because it avoids a continuous input stream over remote PTYs; an application
which visibly uses hover opts into the latter. Both modes disable the complete
1000/1002/1003/1006 set in reverse order so cleanup is also correct after a
partially written begin sequence. The stored mode survives suspend/resume.
Active/suspended states compose with screen job control: suspend reporting
before the screen, resume it after the screen, and restore it before final
screen restoration. Before restoring cooked input, an application that enabled
reporting discards queued reports through the still-active Session so those
bytes cannot become shell input. The Windows virtual-terminal console and POSIX
terminals share those byte sequences. Unicode text interpretation, broader
signal/job-control policy, and event-loop composition remain application
concerns.

Darwin stores a 72-byte, eight-aligned termios record and uses 32-bit `nfds_t`;
the selected glibc contract stores a 60-byte, four-aligned record and uses
64-bit `nfds_t`. Both use the common eight-byte `pollfd` and `winsize` layouts.
Darwin's public signal action is 16 bytes; the selected 64-bit glibc action is
152 bytes because its signal mask occupies 128 bytes. Windows stores the saved
input/output mode as 32-bit console flags and uses one-pointer SRW/condition
records. Target source contains compile-time
size/alignment assertions, while native qualification links the selected libc
or Kernel32 boundary.

## Minimal terminal cell rendering

Status: ordinary Draft allocation and ANSI policy over `core/terminal`; no
widgets, layout framework, callbacks, or compiler intrinsic.

`core/unicode` owns generated, sorted interval tables pinned to Unicode 17.0.0.
The source generator records SHA-256 hashes for UnicodeData,
GraphemeBreakProperty, DerivedCoreProperties, emoji-data, and EastAsianWidth.
One allocation-free state machine implements extended grapheme rules GB3-13,
including Hangul, Indic conjunct, emoji-ZWJ, and regional-indicator context.
Display width is deterministic rather than locale-dependent: East-Asian W/F
and emoji-presentation forms are wide, ambiguous and text-default pictographs
are narrow, and controls or zero-width-only clusters cannot own a cell.

`core/tui.Surface` owns one fixed row-major cell allocation plus one compact
byte arena for multi-scalar graphemes. A leading cell records one or two
terminal columns; a wide glyph's second column is an explicit continuation.
Single scalars remain allocation-free, while a multi-scalar spelling is copied
exactly into Surface storage and survives caller-buffer reuse. Replacing either
half of a wide glyph clears the complete old glyph. Repeated incremental edits
use live-byte accounting and deterministic row-major arena compaction; a full
surface clear reuses the allocation with zero live bytes.

Public ASCII byte/string operations retain their all-or-nothing contract.
`put_rune` accepts one standalone printable scalar, and `write_utf8` plus its
mutable-byte-view counterpart `write_utf8_bytes` first validate and measure
every extended grapheme before changing any cell. The
renderer emits complete stored graphemes and advances by terminal columns, so a
diff never splits a combining sequence or addresses the middle of a wide glyph.
The zero style maps to terminal defaults. A color is either default, one ANSI
256-color index, or explicit 8-bit sRGB. The renderer emits indexed colors as
`38;5`/`48;5` SGR forms and RGB colors as `38;2`/`48;2`, retaining one
deterministic cell model while letting an application choose between a
terminal-owned palette and profile-independent truecolor.

`core/tui.Renderer` owns desired and last-published Surfaces of equal size plus
one reusable output byte array. Rendering scans in row-major order, groups each
maximal contiguous changed run behind one absolute cursor position, and emits
style transitions only when cell style changes. Every nonempty update is
enclosed by DEC synchronized-update markers and ends in default style. A
supporting terminal therefore reveals a moved window and its erased old shadow
as one frame rather than painting individual cursor runs; an unsupported
terminal ignores the optional private mode and receives the same correct diff.
The first frame, a resize, explicit invalidation, or a failed possibly-partial
terminal write forces the next update to reset, home, clear, and repaint the
complete desired surface. Previous cells advance only after
`terminal.write_screen` completes, so an I/O error never publishes speculative
diff state.

Renderer captures one allocator across both Surfaces and its output array. It
borrows `terminal.Screen` for a present call and retains no application or
terminal pointer. Applications repaint the desired Surface directly and call
`invalidate` after every successful or possibly-partial failed screen resume,
or after output performed outside the renderer. `present` rejects a non-active
Screen even for an empty diff and invalidates its correspondence. A
dimension-changing resize discards both old frames and returns a blank desired
surface; equal dimensions are an exact no-op. Focus, input dispatch, clipping,
text wrapping, widgets, layout, and event loops remain outside this core cell
surface. The ordinary `lib/turbo_ui` package builds those policies over a
borrowed Surface without changing `core/tui`. Unicode normalization, shaping,
bidi, locale tailoring, and line breaking deliberately remain outside this
cell surface.

The virtual-memory seam uses target-qualified source with fixed signatures.
POSIX calls `mmap`, `mprotect`, and `munmap`; Windows calls `VirtualAlloc`,
`VirtualProtect`, and `VirtualFree`. Reserve creates inaccessible address space,
commit/protect change whole-region permissions, and release clears the move-by-
convention handle. Darwin selects `MAP_ANON = 0x1000`; Linux selects
`MAP_ANONYMOUS = 0x20`; Windows owns its allocation/protection constants. These
are versioned core/target facts, not values inferred from host headers.

## Hosted directory enumeration

Status: ordinary target-selected Draft implementation on every hosted target.

`core/filesystem` owns one deliberately small directory-iteration seam for
interactive tools. `open` acquires one move-by-convention `Directory`; `read`
copies one native name into caller storage and reports its best-effort kind;
`close` releases the handle. Names are UTF-8 on every target, but native order
is explicitly unspecified and callers sort after copying. The package supplies
no path object, recursive traversal, metadata, mutation, or semantic identity.

Darwin and glibc retain a `DIR *` and copy the short-lived `dirent` name before
the next `readdir` call. Windows retains a `FindFirstFileW` handle plus one
inline `WIN32_FIND_DATAW`, converts the UTF-8 search path to a temporary UTF-16
`path\\*` spelling, and converts each returned filename back to UTF-8. This is
ordinary core code, not a runtime or compiler intrinsic.

## Minimal child-process execution

Status: ordinary target-selected Draft implementation on every current hosted
target; no shell, runtime helper, or compiler intrinsic.

`core/process.run` launches one exact zero-terminated executable path with no
extra arguments. `run_with_options` adds an exact argument tail, last-wins
`NAME=value` overrides over the inherited environment, and an optional working
directory; nil or an empty working-directory cstring means inheritance. Both
inherit standard handles, wait synchronously, and return an explicit `Result`
separating invalid options or parent-side failure from exit/signal completion.
A nonzero child exit is therefore a successful process operation. No operation
performs path search, shell parsing, pipe/redirection, detachment, cancellation,
or background lifetime. All cstrings and slices are borrowed for the complete
synchronous call.

Darwin and Linux share one ordinary Draft procedure which materializes
terminated `argv` and optional complete `envp` tables before `fork`, optionally
calls `chdir`, then uses `execv` or `execve` and a retrying `waitpid`. Small
compile-time branches retain the Darwin/glibc foreign groups and errno access
symbols. The child does no Draft work after `fork`; directory failure exits 126
and exec failure exits 127. The parent interprets the POSIX status as either an
eight-bit exit code or terminating signal. Windows converts
the application, CRT-quoted command line, optional directory, and sorted
double-NUL environment block from UTF-8 to owned UTF-16. It calls
`CreateProcessW`, waits, reads the DWORD exit code, and closes both returned
handles. Windows environment names are currently restricted to ASCII so the
common byte sorter exactly implements its case-insensitive order; values remain
UTF-8. A replacement launch reports `.unavailable` if an inherited name is
non-ASCII; an empty override list asks CreateProcessW to inherit the native
block directly and needs no such restriction. This is the foreground IDE
Build/Run boundary; redirected and asynchronous execution remain intentionally
absent.

## Hosted process views and core threads

Status: target-selected AArch64 Darwin, AArch64/x86-64 GNU, and x86-64 Windows
source contracts.

The hosted C entry receives the platform process vectors. Before Draft `main`,
the runtime materializes arguments and environment as stable `{pointer,length}`
string records;
`core/os` returns non-owning slices over those records. Normal return frees the
record arrays after all Draft defers finish. Environment entries preserve their
exact `name=value` bytes and ordering. The initial file API wraps already-open
fixed descriptors. Pathname opening now uses a target-qualified true C variadic
`open` declaration. Body checking promotes the mode value and LLVM applies the
selected target's unnamed-argument ABI, so `core/os` needs no package-assembly
adapter on POSIX. Windows synchronously converts UTF-8 paths to UTF-16 and calls
fixed-signature UCRT wide pathname operations. Its `wmain` vectors are likewise
converted once to owned UTF-8 before the common record materializer runs.

`core/thread` uses a target-owned native-operation seam. Spawn state owns a copy
of the active Context. The C trampoline installs that copy as the child TLS
default before entering the ordinary Draft callback, replacing temp_allocator
with a provider whose state belongs to that OS thread, so ordinary calls,
defers, and `runtime.default_context` agree. Join clears the owning handle.
Mutex and condition storage is accessed only through pthread operations. Darwin
uses 64-byte mutex and 48-byte condition storage, including their signatures.
glibc 2.39 AArch64 uses 48 bytes with eight-byte alignment for both. On x86-64,
the mutex is 40 bytes and the condition remains 48 bytes, both aligned to eight.
Both GNU profiles define `pthread_t` as `unsigned long` rather than Darwin's
opaque pointer. Windows owns a `CreateThread` HANDLE through successful join,
uses a 32-bit callback result, and maps stable one-pointer mutex/condition
records to exclusive SRW locks and condition variables. Their destroy
operations are explicit Draft lifetime boundaries even though Windows requires
no native destruction call.

## Initial compiler-backed atomic interface

Status: shared native core surface; C11 memory semantics are normative.

`core/atomic.Value[T]` is an ordinary, naturally aligned nominal wrapper whose
storage may be initialized non-atomically only before publication. After
publication, all access uses the package operations. The first target accepts
one-, two-, four-, and eight-byte integer objects plus pointer objects; read,
write, exchange, integer read/modify/write, compare-exchange, and thread fences
lower to dedicated target-independent MIR instructions. LLVM emission uses
`load atomic`, `store atomic`, `atomicrmw`, `cmpxchg`, and `fence` rather than
foreign calls or pthread locks.

Every order argument is a compile-time `atomic.Order` value. Semantic checking
rejects release loads, acquire stores, releasing failure orders, and a
compare-exchange failure order stronger than its success order. A relaxed fence
is valid under the adopted C11 rules but has no synchronization effect; MIR
retains it and LLVM text emits an explicit no-op comment because LLVM has no
`fence monotonic` instruction.

The initial operations must be called directly through the imported package (an
alias is fine). Taking one as a procedure value or explicitly specializing it
is diagnosed because its order and storage facts belong in atomic HIR/MIR, not
in the ordinary calling convention. The Draft source bodies are interface
records and defensive traps only; no valid compiled call reaches them.
