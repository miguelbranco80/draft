# Judgment tour contract

This example keeps one deliberately small runtime design so its judgment claims
can be checked by reading the package:

- `Counter` is a value record containing only one `i64` field named `value`.
- `counter_add` is the only procedure that mutates a `Counter` through a pointer.
- `counter_add` adds its `amount` once and does not retain the pointer.
- The `if`, loop, and `switch` examples do not mutate their borrowed inputs.
- `main` checks every demonstrated runtime operation with ordinary assertions.

The attachment is evidence for the package-level judgment. It is not compiled
into the program, and the package remains checkable and buildable without
running `draftc judge`.
