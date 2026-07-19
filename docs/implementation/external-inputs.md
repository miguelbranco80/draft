# Resolved external program inputs

This document describes the filesystem content that is part of a resolved
Draft program. It does not describe host compiler installation or release
packaging. The normative resolution rule is in
[agent synthesis section 10](../specification/03-agent-synthesis.md).

## What is pinned

A resolution manifest can contain exact content-tree identities for:

- foreign object files, static archives, and shared libraries selected for a
  logical provider;
- foreign-provider summaries consumed during semantic checking;
- runtime data files or directories that the embedding application deploys.

These bytes can change program behavior or the semantic claims checked by the
compiler. `draft resolve` therefore records their canonical SHA-256 content-tree
digest. Later provider-free commands require the caller to supply the complete
relocated set and re-hash it before native tools run.

Physical paths are deliberately absent from the manifest. A regular file's
bytes and executable permission participate in its identity. A directory is
walked in canonical relative-name order; unsafe or escaping symlinks and special
filesystem entries are rejected. Relocating an unchanged tree preserves its
identity, while changing bytes, names, permissions, or link targets makes the
resolved input stale.

## What is not pinned

Clang, lld, Apple ld, `llvm-ar`, `libtool`, `dsymutil`, platform SDKs, system
headers, and system libraries are host build configuration. They are neither
Draft source nor elaborator output and do not appear in resolution manifests.
The native adapter selects them through ordinary command lookup or explicit API
paths and records the Clang version in validation evidence for auditability.

This separation gives `build` one clear promise: it never contacts a synthesis
or judgment provider and never changes the resolved program. It does not promise
that two unrelated host toolchain installations produce byte-identical native
artifacts. CI checks deterministic output under each declared host environment,
and release records identify the environment they exercised.

## Complete-set verification

Manifest-bearing compilation treats each external role as a complete set. An
extra configured provider or runtime asset is rejected because it would affect
execution without affecting resolved-program identity. A missing row is also
rejected. Provider summaries must be consumed by semantic compilation, and
object/archive/shared-library mappings must correspond to providers required by
the checked package graph.

Handwritten programs without a resolution manifest may still receive explicit
foreign provider mappings. Those files are validated and canonicalized, but no
manifest comparison is possible because the program has no saved resolution
transaction.
