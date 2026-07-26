# draft_compiler_api

`draft_compiler_api` contains only the provider-free Draft records and procedure
types used between `turbo_editor_app` and an optional compiler host. It declares
no foreign symbols and owns no compiler session.

The table is deliberately synchronous and fixed-layout. The application owns
source/output buffers; the host owns its opaque `user` state; neither side
retains the other's borrowed storage.

Semantic-operation results include monotonic elapsed nanoseconds. Diagnostics
are available both as deterministic rendered text and as structured rows whose
indexed label, path, exact source bytes, half-open range, severity, and
editability can be copied synchronously by an IDE.
