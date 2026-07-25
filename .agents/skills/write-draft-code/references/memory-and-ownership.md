# Memory and Ownership in Draft

Use this guide whenever Draft code owns storage, an operating-system resource,
or mutable state shared through a pointer. The normative rules are in
[`docs/specification/02-types-memory-runtime.md`](../../../../docs/specification/02-types-memory-runtime.md),
especially sections 5–6. The concrete ownership contracts live in the imported
`core/` package sources.

## Contents

- [The ownership model](#the-ownership-model)
- [Storage and initialization](#storage-and-initialization)
- [Pointers, slices, and strings](#pointers-slices-and-strings)
- [Explicit resource lifetime](#explicit-resource-lifetime)
- [Allocators and context](#allocators-and-context)
- [Temporary storage and arenas](#temporary-storage-and-arenas)
- [Owning buffers and strings](#owning-buffers-and-strings)
- [Growable containers](#growable-containers)
- [Files, mappings, threads, and synchronization](#files-mappings-threads-and-synchronization)
- [Concurrency and atomics](#concurrency-and-atomics)
- [Error and invariant policy](#error-and-invariant-policy)
- [Ownership review checklist](#ownership-review-checklist)

## The ownership model

Draft values copy according to their declared layout. Draft 1 has no hidden
move, destructor, reference count, borrow checker, or scope-exit cleanup. This
has direct consequences:

- A copied pointer, slice, `string`, `cstring`, `io.Reader`, or `io.Writer`
  remains a non-owning view or handle. It does not extend the provider's
  lifetime.
- A copied owning struct duplicates its pointer and cleanup metadata, not the
  allocation or native resource. Destroying both copies double-frees or closes
  one resource twice.
- A generic container copies elements structurally. It does not clone or
  destroy resources owned by an element.
- Assignment does not invalidate the old source value. “Move” is a convention
  that code must make explicit by clearing the source or never using it again.

Treat these live values as non-copyable by convention:

- `array.Dynamic`, `map.Map`, `memory.Buffer`, `memory.Owned_String`, and
  `memory.Arena`;
- `memory.Virtual_Region` and an opened `os.File`;
- a non-inactive `terminal.Session` or `terminal.Screen`;
- a live `tui.Surface` or `tui.Renderer`;
- `thread.Thread`, `thread.Mutex`, and `thread.Condition`.

Pass a pointer to one stable owner. If an intentional transfer is necessary,
copy the value to the destination and immediately replace the source with its
zero value before either path can fail or clean up. A local may instead end its
lifetime immediately after the structural copy—for example, an `append` as its
final use—when it has no deferred cleanup and no later branch can observe or
destroy it. In both forms, the destination becomes the one owner; do not read,
destroy, or otherwise use the transferred source.

## Storage and initialization

Ordinary locals are zero-initialized. Pointers become `nil`, aggregate fields
become their zero values, and a variant selects its first alternative.
This makes a declared-but-uninitialized-looking owner an empty owner:

```draft
values: array.Dynamic[u32]
array.init(&values)
defer array.destroy(&values)
```

`---` is a special initializer for automatic locals only. It permits omitted
zeroing; reading any byte before it is initialized is undefined behavior:

```draft
native_result: Native_Record = ---
fill_native_record(&native_result)
```

Use `---` only when a procedure, foreign call, or assembly region provably
writes the complete value before the first read. It is not an optimization to
sprinkle through ordinary application code.

Package variables and `thread_local` package variables accept only zero or
compile-time constant initialization. Draft runs no hidden package initializer.
Runtime setup is an explicit procedure called by `main` or the owning host.

Package storage lives for the linked image. Thread-local storage lives for its
thread. Automatic storage lives only for the lexical activation. Taking an
address never extends any of those lifetimes.

## Pointers, slices, and strings

`^T`, `[^]T`, `rawptr`, `[]T`, `string`, and `cstring` do not own storage.

- `^T` is a nullable pointer to one value.
- `[^]T` is a C-like multi-pointer with unchecked indexing and no length.
- `[]T` is a mutable borrowed `{data, len}` view with checked indexing.
- `string` is an immutable borrowed byte view. Indexing returns `byte`.
- `cstring` promises a zero-terminated byte sequence suitable for C.
- `rawptr` carries an address without a pointed-to type or ownership contract.

Member selection automatically dereferences one `^T`: `pointer.field` and
`pointer^.field` access the same storage, and `pointer.0` does the same for a
tuple pointee. Prefer the concise spelling for members. Use postfix `pointer^`
when the complete pointee is the value or assignment target. Automatic member
dereference does not check nil, borrow storage, establish an owner, or extend a
lifetime. It does not apply to `[^]T`, `rawptr`, or `cstring`; a multi-pointer
must select an element before selecting a member.

Do not cast an immutable `string` into mutable bytes or a `cstring`. The source
may not be writable or terminated. `raw_data(text)` is the explicit no-copy
escape for a native read-only pointer-plus-length contract: it returns `[^]u8`,
inherits the string's backing lifetime, and does not make the bytes writable.
Writing through that pointer is undefined unless the backing storage is
independently known to be writable. Prefer typed core operations; copy into
`memory.Owned_String` when a C path or API needs a terminator.
`os.write_text` and `os.write_text_all` are the standard synchronous no-copy
consumers for file output.

A pointer or view is invalid after its storage is freed, unmapped, reallocated,
reset, destroyed, or leaves scope. Common invalidation points include:

- `array.reserve` or `array.append`, when growth reallocates;
- `array.destroy`, `memory.buffer_destroy`, or
  `memory.owned_string_destroy`;
- `memory.arena_reset`, `memory.arena_destroy`, or
  `memory.reset_temporary`;
- successful `memory.virtual_release`;
- return from a procedure that owned the referenced automatic local.

`random.Generator` is intentionally a plain copyable value: copying it forks a
deterministic stream at that exact state. `random.fill` borrows its mutable slice
only for the provider call and retains nothing; a provider failure may have
modified a prefix, so discard the bytes unless it returns true.

Use `ptr_offset` and `ptr_sub` for pointer operations. Ordinary `+` and `-` are
not pointer arithmetic. A typed access requires live, sufficiently aligned
storage containing a valid representation. Draft has no strict-aliasing rule,
but that does not make invalid representations or dead storage safe.

## Explicit resource lifetime

Acquire an owned resource into a named local and register its matching cleanup
as soon as acquisition succeeds:

```draft
(handle, error) := os.open_for_reading(path)
if error != .none {
    return .open_failed
}
defer os.close(&handle)
```

`defer` accepts a call statement. It evaluates and saves the callee and
arguments immediately, then invokes calls in LIFO order on normal scope exit,
`return`, `break`, and `continue`. A trap does not run deferred calls.

Because `defer os.close(&handle)` saves the pointer immediately, `handle` must
stay at that address. Do not copy a new owner over the variable or transfer it
after registering cleanup. When cleanup can fail and the failure matters to
program semantics, perform it explicitly on the success path instead of hiding
the result in `defer`.

There is no automatic cleanup at a closing brace. A bare block is still useful
for making ownership and its deferred cleanup local and visible.

## Allocators and context

`runtime.Allocator` is a borrowed `{procedure, user}` provider record. The
provider owns `user`; callers must keep that state alive while allocations or
containers can call it.

The ordinary `memory.allocate`, `memory.new`, container `init`, and arena
initializers capture or use `context.allocator`. Their explicit
`*_with_allocator` variants use the supplied provider. Free or resize with the
same allocator, exact old size, and exact alignment used to allocate.

Nonzero allocation failures currently assert in the ordinary memory layer.
This is not a recoverable allocation API. Application code that requires
fallible allocation needs an explicit allocator or library seam with a
documented result; it must not pretend an assertion is an ordinary error.

Fresh storage from the hosted default allocator is zeroed, but the public
allocator contract does not require every custom allocator to zero storage.
Initialize values explicitly after `new_with_allocator` when the provider is
not known to return zeroed bytes.

Every ordinary procedure receives the active `runtime.Context` implicitly.
Calls propagate it. A runtime lexical scope that assigns a `context` field or
takes a field address gets a compiler-managed copy; calls in that scope see the
copy, and leaving the scope restores the surrounding context:

```draft
{
    context.allocator = memory.arena_allocator(&arena)
    build_transient_index()
}
```

Keep `arena` at a stable address for the entire scope because the allocator's
`user` pointer borrows it. `c proc` has no implicit context and cannot name
`context`; use the documented runtime bridge at that boundary.

## Temporary storage and arenas

`memory.temporary_allocate` and `memory.temporary_new` allocate from the
calling thread's resettable temporary allocator. All returned pointers remain
valid together until `memory.reset_temporary()` or thread exit. A reset
invalidates every outstanding temporary pointer; do not retain one in global,
heap, container, callback, or returned state.

`memory.Arena` owns a linked group of backing allocations:

1. Zero-initialize it and call `arena_init` or `arena_init_with_allocator`.
2. Keep it at a stable address after calling `arena_allocator`.
3. `arena_reset` releases all blocks and invalidates all arena allocations but
   leaves the initialized arena reusable.
4. `arena_destroy` also clears the provider policy and ends use of allocator
   records borrowed from the arena.

An individual free through an arena allocator is intentionally a no-op.
Resizing allocates another range and leaves the old range live until group
reset. Do not infer ordinary heap behavior from that provider.

## Owning buffers and strings

`memory.Buffer[T]` owns one fixed-length allocation. Initialize it once with
`buffer_init`, `buffer_init_with_allocator`, `buffer_copy`, or its allocator
variant; release it once with `buffer_destroy`.

`buffer_view` borrows the allocation until destruction. `buffer_copy` performs
a structural element copy, not a deep clone. A `Buffer` of owning `T` therefore
does not define the elements' cleanup policy.

`memory.Owned_String` owns copied bytes plus one trailing zero. Initialize it
with `owned_string_copy`, borrow its bounded mutable bytes with
`owned_string_bytes`, borrow its C view with `owned_string_cstring`, and end
both views with `owned_string_destroy`. Embedded zero bytes are preserved, but
C observes the first zero as the end of the string.

## Growable containers

`array.Dynamic[T]` and `map.Map[K,V]` capture one allocator during explicit
initialization. Destroy them explicitly. Never copy a live container.

`Dynamic` uses contiguous storage. `reserve` and `append` can invalidate all
element pointers and the slice from `view`. `remove` preserves order by
shifting values; `clear` zeroes live slots but retains capacity.

`Map` uses open addressing. `set` structurally replaces a stored value; `get`
returns `(zero V, false)` for absence; `remove` zeroes a slot. Rebuilds copy
keys and values. None of these operations call element destructors.

Consequently, putting an owning handle in either generic container requires
application-defined lifecycle logic. Clean up live elements before `clear` or
`destroy`; make an intentional append/set transfer the source's final use or
zero that source immediately; and decide what replacement and removal mean.
Container rebuilds must leave exactly one live owner for each allocation.
Prefer stable IDs or non-owning records when that policy would otherwise be
subtle.

`map.Map[string,V]` stores shallow string views. The key bytes must outlive map
membership. Copy keys into stable owned storage if input buffers are transient.

## Files, mappings, threads, and synchronization

`os.File` is a descriptor handle. Copies alias one native descriptor, and
`close(^File)` clears only the passed copy after successful close. Pick one
owner; pass `File` by value only to operations that borrow the descriptor.

`process.run_with_options` synchronously borrows the executable cstring, every
argument/environment cstring, both slice tables, and the optional working
directory. Keep every backing owner alive and unmoved until it returns. The
package copies only the complete environment it must merge; it retains no
caller pointer after the child has been created and waited for.

`terminal.Session` borrows its input `os.File` and owns the exact native mode
that must be restored. Keep the Session at one stable address, restore before
closing the descriptor, and do not copy an active or suspended value. Suspend
temporarily installs the saved mode without releasing the obligation; resume
reapplies raw mode. Failed transitions retain their source state so the same
owner may report, retry, or restore. Runtime traps and external process
termination do not run deferred restoration.

`terminal.Screen` likewise borrows its output `os.File` and owns the obligation
to leave the alternate screen and show the cursor. `begin_screen` records that
obligation before writing because a failed complete write may already have
published a state-changing prefix. Therefore call `restore_screen` whenever a
begin attempt leaves the Screen active—even when begin returned an error—and
keep the descriptor open until restoration succeeds. A failed restore preserves
the active owner for reporting or retry. A suspended Screen remains an owner
even though the primary screen is currently visible; failed resume deliberately
marks it active because a control-sequence prefix may require cleanup.

`terminal.Resize_Watcher` borrows one terminal descriptor and owns the process
resize-observation slot. On POSIX that includes the exact prior SIGWINCH action;
on Windows it remains an explicit comparison lifetime. Keep an active watcher
at one address, do not copy it, and call `end_resize_watch`. Failed POSIX
restoration retains active state and the process slot for retry.

When an application owns all three terminal resources, acquire Session,
Resize_Watcher, then Screen; finally restore Screen, end Resize_Watcher, and
restore Session. Suspend Screen then Session and resume Session then Screen;
the resize watcher remains active through that temporary job-control boundary.
Always attempt every final restoration even when an earlier one fails. This
ordering exposes the primary screen and saved input mode to external work and
avoids re-entering the alternate screen when raw input could not be reacquired.

`tui.Surface` owns one fixed cell buffer and one compact byte arena containing
multi-scalar grapheme spellings. `cell_at` returns only a value description;
the private arena is never borrowed. `tui.Renderer` owns two nested Surfaces
plus one reusable output buffer, so do not separately destroy or copy its
fields. A pointer returned by `tui.surface(&renderer)` expires at a
dimension-changing resize or destroy; equal-size resize is an exact no-op.
Renderer borrows `terminal.Screen` only during `present`; failed output
invalidates display history but retains every allocation for retry. Invalidate
after successful resume and after a failed resume that may have written an
unknown control-sequence prefix.

`memory.Virtual_Region` is move-by-convention. Reserve returns inaccessible
address space; commit/protect applies to the complete region; release unmaps
and clears it only after success. Never use an old region pointer after release.

`thread.Thread` carries one join right. Keep it in one owner and join it.
`thread.Mutex` and `thread.Condition` must stay at stable addresses after
initialization and must not be copied. A spawned thread borrows its `user`
pointer, so that storage and everything it references must outlive the child.
Synchronize shared mutation and join before cleanup.

Wait on a condition in a predicate loop because wakes may be spurious:

```draft
thread.mutex_lock(&state.mutex)
defer thread.mutex_unlock(&state.mutex)
for !state.ready {
    thread.condition_wait(&state.condition, &state.mutex)
}
```

## Concurrency and atomics

Unsynchronized conflicting access from multiple threads is a data race and
undefined behavior. An assembly `memory` clobber constrains compiler motion; it
does not make an ordinary access atomic or establish synchronization.

After publishing `atomic.Value[T]`, access its storage only through
`core/atomic`. Current atomics support naturally aligned 1-, 2-, 4-, or 8-byte
integer and pointer objects. Order arguments must be compile-time constants.
Current bootstrap lowering also requires direct package calls: do not take an
atomic operation as a procedure value or explicitly specialize it.

Use release/acquire or stronger ordering only when it matches an explained
happens-before relationship. Do not select sequential consistency as a
substitute for understanding which data the atomic publishes.

## Error and invariant policy

Use explicit results for expected failure:

- `(count, io.Error)` for I/O;
- `(value, bool)` for lookup or creation;
- `bool` for lifecycle operations;
- an application enum or `result.Result` when the caller needs richer policy.

Check partial I/O counts. `os.read`, `os.write`, and `os.write_text` perform one
native call; successful operations can process fewer bytes than requested. Use
the corresponding complete-write operation when a partial prefix is not an
application result. EOF is the `io.Error` case `.end_of_input`, not a zero-count
success.

Use `assert` for a programmer invariant whose violation means the program is
internally wrong. Assertions may be disabled and traps skip `defer`; never use
them for malformed user data, expected OS errors, or a recoverable resource
condition.

## Ownership review checklist

- Identify exactly one owner for every allocation and native resource.
- Pair every successful acquisition with one explicit cleanup path.
- Confirm cleanup uses the original allocator, size, alignment, or handle.
- Keep provider state and callback user data alive and at stable addresses.
- Mark every returned pointer/view with its invalidation boundary.
- Reject accidental copies of owning structs and initialized synchronization
  objects.
- Define element cleanup before storing owning values in generic containers.
- Check partial counts and every recoverable result.
- Prove each `---` value is completely initialized before read.
- Prove shared mutable data has a synchronization relationship, not merely an
  atomic-looking field or assembly barrier.
