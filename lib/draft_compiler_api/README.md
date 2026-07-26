# draft_compiler_api

`draft_compiler_api` contains the fixed Draft records and procedure types used
between `turbo_editor_app` and an optional compiler host. It declares no foreign
symbols and owns no compiler session. Check and Build are provider-free.
Resolve, Judge, and ephemeral `//?` comment expansion are separate, explicitly
named optional procedures.

The table is deliberately synchronous and fixed-layout. The application owns
source/output buffers; the host owns its opaque `user` state; neither side
retains the other's borrowed storage.

Semantic-operation results include monotonic elapsed nanoseconds. Diagnostics
are available both as deterministic rendered text and as structured rows whose
indexed label, path, exact source bytes, half-open range, severity, and
editability can be copied synchronously by an IDE.
Resolve additionally reports commit and synthesis/reuse counts. Judge separates
command completion from the all-pass verdict and reports selected/evidence
counts, so the application never has to infer either operation from diagnostic
text.
Comment expansion instead reports elapsed time plus active-source offsets and
byte counts for import, package-declaration, and local slots. Its three result
strings and error string have a separate copy lifetime, so a failed convenience
request cannot overwrite the latest semantic Diagnostics.
