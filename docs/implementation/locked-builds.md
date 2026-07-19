# Locked builds and external inputs

This document records the bootstrap security and reproducibility contract for native toolchains, SDKs, runtime assets, foreign providers, manifests, and canonical workspace roots. Concrete selected release trees are documented under releases.

## Initial locked native input contract

Status: bootstrap build contract; versioned by the resolution and content-tree
formats.

The locked compiler-owned artifact seam accepts exactly two external inputs for
the selected target. AArch64 macOS uses `llvm-aarch64-macos` plus `macos-sdk`
and requires executable `bin/clang`, `bin/ld`, `bin/ld-classic`,
`bin/llvm-ar`, and `bin/dsymutil`. AArch64 Linux uses
`llvm-aarch64-linux` plus `aarch64-linux-sysroot` and requires `bin/clang`,
`bin/ld.lld`, and `bin/llvm-ar`. Clang is the manifest entry point in both
profiles; every other entry is fixed by the adapter rather than discovered by
name. Apple ld's classic helper and dSYM linker have no ELF analogue.
Both complete trees are hashed as sorted relative path records including file
kind, permission bits, exact regular-file bytes, and exact internal symlink
spelling. Physical root paths are excluded. Absolute or escaping symlinks,
root symlinks, and special files are rejected.

Hashing alone cannot close a macOS-hosted toolchain whose executable retains an
ambient Homebrew or developer-directory dylib path. Before the tree is hashed, a direct
load-command parser requires every tool and recursively loaded dylib to be a
thin AArch64 Mach-O image. Non-system loads must resolve exactly once through a
relocatable runpath to a regular file inside the selected root. Dylib IDs and
runpaths obey the same rule. Only explicit `/usr/lib/...` and
`/System/Library/...` dependencies may leave the tree.

Locked invocation uses the verified absolute Clang and linker paths,
`--no-default-config`, and an explicit system root. macOS uses `-isysroot`, its
target deployment floor, and the linker's content-derived Mach-O UUID. Linux
uses `--sysroot`, an absolute `ld.lld`, deterministic `llvm-ar`, and a
content-derived GNU build ID. Current macOS requires the
UUID load command for executable launch; a real integration gate proves two
complete links to the same explicit output identity are byte-for-byte equal.
The child environment contains fixed `LANG`, `LC_ALL`, `HOME`, and `TMPDIR`
plus an empty `PATH`; it inherits no SDK, compiler, header, library, deployment,
or package search variables. Other manifest external-input roles fail closed
until their
artifact-to-command mapping exists. Ordinary development builds retain the
separate explicit host-toolchain escape hatch.

The adapter deliberately does not pass Apple's private `--no-xcselect` driver
option: upstream LLVM 22.1 rejects it. An absolute `-isysroot`, absolute
`--ld-path`, empty `PATH`, and scrubbed SDK/deployment environment close the
same discovery paths while remaining compatible with the pinned upstream
driver. The initial Linux tool distribution is a macOS-hosted cross toolchain,
so the same Mach-O load-command closure protects its Clang, lld, and archiver.
A Linux-hosted bootstrap remains unqualified until an equivalent ELF
host-tool dependency-closure check is implemented.

Runtime assets use a separate complete-set mapping from a nonempty logical name
to one absolute real file or directory root. Resolution records a
`runtime-asset` row with the existing portable content-tree digest; later native
build, test, and benchmark invocations must supply every row and may relocate
unchanged content. A runtime asset is not a link input, so the adapter never
passes it to Clang or guesses a location beside the produced artifact. Instead,
the reusable native API returns the verified canonical roots to its embedding
build/deployment layer. Root symlinks, escaping internal symlinks, special files,
duplicates, missing rows, extra mappings, and content changes all fail before a
compiler process starts.

Resolution manifests accept raw non-ASCII JSON string contents only as valid,
shortest-form UTF-8 scalar encodings. Continuation leads, overlong sequences,
surrogates, and values above U+10FFFF fail before any identity is installed.
The deterministic malformed corpus truncates at every byte boundary, replaces
every byte with NUL and `0xff`, and tries every possible trailing byte under the
ordinary and sanitizer suites.

The target profile maps the logical `darwin` and `libc` providers to the SDK's
explicit `System` library. `draft_runtime` is owned by the root LLVM module and
`package_assembly` is owned by captured package assembly; none can be remapped.
The current v5 target carries the closed, symbol-level denial-summary table for the
System calls used by the first core distribution. Most rows have no callback;
`pthread_create` identifies its start routine as flow-through parameter two.
Provider names alone never confer trust: an unlisted System symbol remains an
unknown edge, while every package-assembly call contributes the `asm` effect.
Every other foreign provider requires one command-line object, archive, or
shared-library mapping. Resolution stores the provider name, artifact role, and
content-tree digest without its physical path. Offline builds require the same
complete mapping, re-hash relocated bytes, reject unused mappings, and reject
unknown providers before invoking Clang. A locked link first copies each
verified artifact into the isolated build directory and re-hashes that snapshot,
so the linker never consumes a mutable workspace path after verification.

## Canonical CLI workspace roots

Status: implemented for every package command.

The public driver canonicalizes the requested existing package directory before
deriving its workspace parent. This resolves symlinked parent spellings such as
macOS `/tmp` before they reach the resolution store. The store retains its
strict no-follow traversal after accepting that canonical root; weakening the
store would turn a presentation-path issue into a transaction security issue.

Only package/workspace ownership uses this helper. Requested output paths,
relocatable provider artifacts, runtime assets, and locked toolchain/SDK roots
keep their separate explicit policies. A public `resolve` regression reaches a
copied package through a symlinked parent, pins dummy native roots, and proves
that the manifest is published under the canonical real workspace.
