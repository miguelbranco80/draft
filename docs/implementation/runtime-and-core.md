# Hosted runtime and core packages

This document records the hosted runtime, Context implementation,
allocator/core facilities, process and thread support, and compiler-backed
atomic surface. Portable language behavior remains in the specification;
exact machine and OS facts live in the selected target profile.

## Initial hosted runtime context layout

Status: bootstrap runtime ABI; synchronized with `core/runtime` by tests.

Core content identity `draft-core-bootstrap-v4` names the current target-
qualified Darwin/Linux OS, memory, thread, time, package-assembly, formatting,
and console distribution. It includes the typed immutable-string write path:
core code may pass existing string storage to a synchronous nonmutating native
write without manufacturing a mutable byte slice or copying through a bounded
buffer.

Both current AArch64 profiles use a root Context of 96 bytes with 8-byte
alignment. Its fields
begin at offsets 0, 16, 32, 40, 56, 72, 80, and 88, in the source order declared
by `core/runtime.Context`. Allocator, logger, and random-generator provider
records each contain a procedure pointer and a provider-state pointer. The
assertion callback is an ordinary Draft procedure pointer, so its physical call
prepends the active Context pointer.

Only the executable root module defines runtime failure helpers and root process
state. Dependency modules reference those hidden link-unit symbols. This gives
all ordinary calls one coherent Context and prevents per-package runtime state
from emerging as a bootstrap artifact. Changing this layout or helper contract
requires a new runtime ABI and core distribution identity.

`context` is a predeclared, addressable value in every ordinary Draft procedure.
When `core/runtime` is imported, its type is exactly the public
`runtime.Context`; otherwise the compiler uses a private ABI-identical nominal
type. A lexical block that assigns a Context field or takes the address of one
starts with a complete copy of the surrounding Context. Calls in that block use
the copy, and leaving the block restores the surrounding pointer. A `c proc`
has no implicit Context and may not name `context`.

Two compiler-owned bridges cover that C boundary. `runtime.default_context`
lazily initializes Draft TLS from the process-default Context and returns the
calling thread's snapshot through the selected AArch64 indirect aggregate-result
convention. `runtime.call_with_context` statically checks a non-nil `^Context`,
an ordinary Draft callback, and the callback's exact arguments, initializes
Draft TLS when entered from a foreign-created thread, then lowers directly to
that callback with the explicit hidden Context pointer. Named callbacks retain
their ordinary effect summaries; indirect callbacks remain unknown edges.
These are narrow versioned runtime exceptions, not permission to use Context in
arbitrary C signatures. The supplied pointer remains dynamic-call state and is
not installed as the thread default.

The hosted default allocator implements the three `core/runtime` operations
against the selected libc heap. Fresh storage is zeroed, alignments through 16
use the ordinary allocator, larger alignments use `posix_memalign`, and aligned
resize allocates/copies/releases while preserving the old allocation on failure. The
root and each lazy thread Context use this provider for general allocation. The
temporary provider instead owns a pthread-key state containing a direct list of
separately aligned allocations. Individual free is a no-op, resize allocates and
preserves the live prefix, explicit reset releases the whole list, pthread key
destruction releases it on thread return, and hosted main releases its state
before process-view teardown. `core/memory` exposes temporary byte/typed helpers
and explicit reset without hiding a call-boundary reset. The runtime also
installs a stderr logger and `arc4random_buf` random provider rather than empty
records.

The LLVM runtime bridge uses the selected libc's physical pthread typedefs.
Darwin's `pthread_once_t` is a 16-byte signature record initialized with its
fixed `PTHREAD_ONCE_INIT` value and `pthread_key_t` is 64 bits. Under the
selected glibc contract, `pthread_once_t` is one zero-initialized 32-bit word
and `pthread_key_t` is 32 bits. Their declarations, globals, loads, and calls
all use the matching type and alignment; these target facts never enter
Draft's source-visible type system.

## Initial core memory facilities

Status: ordinary Draft library surface over the allocator and target-selected
Darwin/GNU ABIs.

`core/memory.Arena` is a direct linked list of backing blocks with an absolute
address-aligned bump cursor. Its allocator performs allocate and preserving
resize, treats individual free as a logical no-op, and releases complete blocks
on explicit reset/destroy. Block metadata and bytes use the caller-selected
backing allocator, so no compiler ownership mechanism is hidden behind the
handle.

`memory.Buffer[T]` owns one fixed-length typed allocation. `Owned_String` owns a
zero-terminated byte copy and exposes a mutable bounded byte view plus `cstring`;
Draft's built-in `string` remains an immutable non-owning view and the library
does not fabricate one through an undocumented cast. Both handles store their
allocator and require explicit destruction.

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

The virtual-memory seam uses target-qualified source with fixed signatures for
`mmap`, `mprotect`, and `munmap`. Reserve creates inaccessible private anonymous
address space, commit/protect change whole-region permissions, and release
clears the move-by-convention handle. Darwin selects `MAP_ANON = 0x1000`; Linux
selects `MAP_ANONYMOUS = 0x20`. These constants are versioned core/target facts,
not values inferred from host headers.

## Hosted process views and core threads

Status: target-selected AArch64 Darwin/GNU source contract; ELF runtime/link
qualification follows in the native backend slice.

The hosted C entry receives the platform `envp` vector. Before Draft `main`, the
runtime materializes argv and envp as stable `{pointer,length}` string records;
`core/os` returns non-owning slices over those records. Normal return frees the
record arrays after all Draft defers finish. Environment entries preserve their
exact `name=value` bytes and ordering. The initial file API wraps already-open
fixed descriptors. Pathname opening uses a target-qualified fixed-signature
assembly wrapper because Draft 1 deliberately rejects variadic C imports:
Darwin spills the variadic mode to its stack slot, while GNU AAPCS64 can tail
branch with the existing x0/w1/w2 argument registers.

`core/thread` uses pthreads through fixed C signatures. Spawn state owns a copy
of the active Context. The C trampoline installs that copy as the child TLS
default before entering the ordinary Draft callback, replacing temp_allocator
with a provider whose state belongs to that OS thread, so ordinary calls,
defers, and `runtime.default_context` agree. Join clears the owning handle.
Mutex and condition storage is accessed only through pthread operations. Darwin
uses 64-byte mutex and 48-byte condition storage, including their signatures.
glibc 2.39 AArch64 uses 48 bytes with eight-byte alignment for both;
`pthread_t` is `unsigned long` rather than Darwin's opaque pointer.

## Initial compiler-backed atomic interface

Status: shared AArch64 core surface; C11 memory semantics are normative.

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
