# Agent acceptance fixture

This workspace is the small end-to-end acceptance case for compiler-owned agent
operations. It deliberately combines features that are easier to validate
together than as isolated mocks:

- dependency-ordered declaration synthesis at package scope;
- typed expression and statement synthesis inside `main`;
- an imported package used by both the program and its test-only graph;
- native pre-commit test execution;
- an artifact-backed judgment whose claim depends on the imported body;
- provider-free offline checking and locked native rebuilding; and
- generated-source correlation in the native debug sidecar.

The committed `.draft` files are content-addressed acceptance inputs, not build
caches. They contain the three checked generated Draft fragments, the v127
resolution manifest, and its selected test and judgment evidence. `.draft/build`
remains ignored and may be deleted at any time.

## Qualification flow

Resolve with an explicit Codex distribution and exact native inputs:

```sh
build/draftc resolve examples/agent-acceptance/app \
  --codex-distribution-root /absolute/codex-distribution \
  --codex-executable /absolute/codex-distribution/codex \
  --codex-model model-name \
  --toolchain-root /absolute/llvm-root \
  --sdk-root /absolute/MacOSX.sdk
```

The ordinary judgment intentionally cannot prove the imported function's
return value from its interface alone. Supply the exact library source as the
claim artifact:

```sh
build/draftc judge examples/agent-acceptance/app \
  --judge-artifact library-source:examples/agent-acceptance/lib/package.draft \
  --codex-distribution-root /absolute/codex-distribution \
  --codex-executable /absolute/codex-distribution/codex \
  --codex-model model-name
```

Finally, reproduce without either provider. The digest is the SHA-256 of
`lib/package.draft` and is also recorded in the selected judgment evidence:

```sh
build/draftc build examples/agent-acceptance/app --locked \
  --toolchain-root /absolute/llvm-root \
  --sdk-root /absolute/MacOSX.sdk \
  --require-test-evidence \
  --require-judgment-evidence \
  --judge-artifact \
    library-source:117d920b2cba9a52b050d7149325f704b1990b149760136c93689a9ea74a3f54 \
  -o /tmp/draft-agent-acceptance
```

The checked snapshot was qualified on AArch64 macOS with the Codex executable
bundled in the ChatGPT application, model `gpt-5.6-sol`, the selected
self-contained LLVM 22.1.8 plus Apple ld/ld-classic toolchain, and the minimal
content-pinned `libSystem.tbd` SDK. The toolchain and SDK layouts and identities
are recorded in the repository's `TOOLCHAIN_DISTRIBUTION.md`.
