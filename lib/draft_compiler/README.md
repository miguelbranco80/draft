# draft_compiler

`draft_compiler` is the thin Draft binding to the bootstrap
`draft_compiler_service` shared library. It translates a Draft configuration
into the fixed C ABI and constructs the Host table consumed by
`turbo_editor_app`. The table keeps Check and Build provider-free and exposes
source-generating Resolve, evidence-producing Judge, and ephemeral `//?`
editor expansion as distinct explicit entries.

The caller explicitly owns `create`/`session_destroy`. The native service copies
configuration paths and borrows all later byte buffers synchronously. Compiler,
filesystem, C++, and LLVM representations never cross this package boundary.
Operation timing, Resolve/Judge counters, comment-expansion slot offsets/bytes
and errors, and structured diagnostic rows cross only as fixed scalars plus
caller-owned byte copies.
