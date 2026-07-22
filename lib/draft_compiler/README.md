# draft_compiler

`draft_compiler` is the thin Draft binding to the bootstrap
`draft_compiler_service` shared library. It translates a Draft configuration
into the fixed C ABI and constructs the provider-free Host table consumed by
`turbo_editor_app`.

The caller explicitly owns `create`/`session_destroy`. The native service copies
configuration paths and borrows all later byte buffers synchronously. Compiler,
filesystem, C++, and LLVM representations never cross this package boundary.
