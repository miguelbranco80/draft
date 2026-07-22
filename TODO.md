# TODO

- [ ] Add named and default procedure arguments.
- [ ] Add packed structs and bit fields.
- [ ] Add Raylib bindings, native linking, ABI tests, and examples.
- [ ] Add an x86-64 Linux native target.
- [ ] Add a Windows native target, starting with x86-64.
- [ ] Add explicit C variadic interoperation when fixed wrappers are insufficient.
- [ ] Define a coherent declaration grammar: one owner for visibility, linkage/provider, ABI/layout, implementation, and execution domain; avoid a general annotation bag.
- [ ] Audit `import`, `pub`, `export`, `foreign`, `c`, `@repr`/`@align`, `asm`, and future `gpu` syntax; consider `c struct`/`c enum`/`c raw union`, `align(N)`, and an explicit external-symbol spelling.
