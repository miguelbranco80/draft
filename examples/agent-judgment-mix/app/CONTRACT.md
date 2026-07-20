# Generated program contract

The resolved program has five generated fragments:

1. `Generated_Offset` is an `i64` constant whose value is 40.
2. `Generated_Record` has exactly one field, `value: i64`.
3. `compute` returns `record.value + Generated_Offset`.
4. `emit_barrier` contains one `dmb ish` instruction inside an assembly block
   that declares its memory clobber.
5. `main` asserts that the computed result is 42.

No generated fragment needs foreign calls, unchecked access, dynamic
allocation, or additional declarations beyond the requested package constant.
The contract is attached to the package judgment, making it common guidance for
every synthesis site while remaining ordinary inspectable project text.
