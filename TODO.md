# TODO

- [ ] Add named and default procedure arguments.
- [ ] Add packed structs and bit fields.
- [ ] Add Raylib bindings, native linking, ABI tests, and examples.
- [ ] Add an x86-64 Linux native target.
- [ ] Add a Windows native target, starting with x86-64.
- [ ] Add explicit C variadic interoperation when fixed wrappers are insufficient.
- [x] Unify current declaration syntax around `import`, `pub`, `export <name> as "<symbol>"`, `foreign <provider> {}`, `c proc`/`c struct`/`c enum`/`c raw union`, `align(N)`, and `asm <architecture> {}`; remove annotations.
- [ ] Add GPU compute with `gpu proc`.
- [ ] Extend `deny` coverage to more built-ins and effects.
- [ ] Add remaining layout forms: bit sets, transparent wrappers, niche unions, and broader SIMD.
- [ ] Broaden assembly support, including an explicit raw escape hatch.
- [ ] Finalize postfix `^`/XOR newline syntax.
- [ ] Add freestanding and embedded target support.
- [ ] Complete cross-target sanitizer, race, and coverage profiles.
- [ ] Add terminal resize/signal handling and Unicode display support.
