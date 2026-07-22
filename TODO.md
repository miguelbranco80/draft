# TODO

- [x] Add named and default procedure arguments.
- [x] Add field-level `packed` and `bits(N)` layout.
- [x] Add Raylib bindings, native linking, ABI tests, and examples.
- [x] Add an x86-64 Linux native target.
- [x] Add a Windows native target, starting with x86-64.
- [x] Add explicit C variadic interoperation when fixed wrappers are insufficient.
- [x] Unify current declaration syntax around `import`, `pub`, `export <name> as "<symbol>"`, `foreign <provider> {}`, `c proc`/`c struct`/`c enum`/`c union`, `align(N)`, and `asm <architecture> {}`; remove annotations.
- [ ] Add GPU compute with `gpu proc`.
- [ ] Extend `deny` coverage to more built-ins and effects.
- [ ] Add remaining layout forms: bit sets, transparent wrappers, niche variants, and broader SIMD.
- [ ] Broaden assembly support, including an explicit raw escape hatch.
- [x] Reserve `^` for pointers and use binary `~`/`~=` for XOR.
- [ ] Add freestanding and embedded target support.
- [ ] Complete cross-target sanitizer, race, and coverage profiles.
- [ ] Add terminal resize notifications/signal handling and Unicode display support.
