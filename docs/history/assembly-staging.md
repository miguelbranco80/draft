# Parsed assembly implementation staging

This historical record explains the transition from syntax preservation to the completed first-target assembly path. Current language and target behavior is owned by the specification and target profile.

## Parsed assembly staging

Status: implementation sequence; Draft 1 scope is unchanged.

The bootstrap front end preserves assembly statements, expressions, typed
operands, synthesis sites, source ranges, effects, and denial interactions from
the beginning. Target-independent MIR originally rejected an assembly region
until ordinary Draft MIR had a working AArch64 macOS emission path. That staging
boundary has now been crossed: language-owned directives are structural syntax,
the `draft-aarch64-apple-v2` analyzer validates fixed general, scalar FP, and
fixed-vector registers; scaled, unscaled, paired, narrow, and ordered memory
operations; the closed integer, selection, conversion, scalar FP, baseline NEON,
and barrier vocabulary; and register/flags/memory declarations. It also treats
condition flags as local dataflow so a select cannot consume ambient flags. MIR
lowers accepted regions as volatile assembly. Instructions outside that profile,
lane selection, address writeback, labels, branches, calls, stack changes, and
unwinding remain external-file features rather than implicit assembler syntax.

Package assembly follows the separate file contract in section 3. The compiler
copies every selected file's exact bytes into the compiled package snapshot;
the later native adapter never rereads the workspace. It writes those bytes to
the isolated build directory, passes `-x assembler` for all three extensions,
and appends each resulting object after its package LLVM object in canonical
package and filename order. The explicit language selection is essential for
`.S`: Draft's target profile specifies no preprocessing, independently of
Clang's conventional filename behavior. Symbols cross this boundary only
through `foreign` and `export` C-ABI declarations.
