# Agent acceptance fixture

This workspace is the small end-to-end acceptance case for compiler-owned agent
operations. It combines features that are most useful when exercised together:

- dependency-ordered declaration synthesis at package scope;
- opaque-round member synthesis that rebuilds a public aggregate interface;
- typed expression and statement synthesis inside `main`;
- an imported package used by both the program and its test-only graph;
- independent native test execution and evidence;
- an artifact-backed judgment whose claim depends on the imported body;
- provider-free checking and native building from saved expansions; and
- generated-source correlation in the native debug sidecar.

The committed `.draft` files are content-addressed acceptance inputs, not build
caches. They contain four checked generated Draft fragments and a v5 resolution
manifest. Validation and judgment evidence is stored independently and is not
selected by that manifest. `.draft/build` is ignored and may be deleted at any
time.

## Qualification flow

Resolve with an explicit Codex distribution:

```sh
build/draftc resolve examples/agent-acceptance/app \
  --codex-distribution-root /absolute/codex-distribution \
  --codex-executable /absolute/codex-distribution/codex \
  --codex-model model-name
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

Finally, reproduce the program without either provider:

```sh
build/draftc check examples/agent-acceptance/app
build/draftc build examples/agent-acceptance/app \
  -o /tmp/draft-agent-acceptance
/tmp/draft-agent-acceptance
```

`build` loads the saved generated source, checks the complete program, and uses
the host native toolchain. It neither contacts Codex nor requires test or
judgment evidence. To recheck every saved expansion without provider access,
run:

```sh
build/draftc resolve examples/agent-acceptance/app --revalidate
```

The original Codex/model qualification and the former native-distribution
experiment remain available in the
[historical first-compiler qualification](../history/releases/first-compiler-qualification.md).
