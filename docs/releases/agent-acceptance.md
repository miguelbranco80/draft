# Agent acceptance fixture

This workspace is the small end-to-end acceptance case for compiler-owned agent
operations. It combines features that are most useful when exercised together:

- independent declaration and member synthesis in one opaque interface wave;
- typed expression and statement synthesis inside `main`;
- an imported package used by the program, test graph, and benchmark graph;
- independent native test and benchmark execution and evidence;
- an artifact-backed judgment whose claim depends on the imported body;
- provider-free checking and native building from saved expansions; and
- generated-source correlation in the native debug sidecar.

The committed `.draft` files are content-addressed acceptance inputs, not build
caches. They contain four checked generated Draft fragments and a v5 resolution
manifest. Validation and judgment evidence is stored independently and is not
selected by that manifest. `.draft/build` is ignored and may be deleted at any
time.

## Reproduce the committed program

The fixture already contains fresh checked expansions, so ordinary consumer
commands are deliberately provider-free:

```sh
build/draftc check examples/agent-acceptance/app
build/draftc build examples/agent-acceptance/app \
  -o /tmp/draft-agent-acceptance
/tmp/draft-agent-acceptance
build/draftc test examples/agent-acceptance/app
build/draftc bench examples/agent-acceptance/app
```

All four commands load the committed generated source without contacting
Codex. `build` uses the host native toolchain and does not require test,
benchmark, or judgment evidence. To recheck every saved expansion without
provider access, run:

```sh
build/draftc resolve examples/agent-acceptance/app --revalidate
```

## Exercise live synthesis

An ordinary `resolve` of this checkout reuses the fresh pins; it does not call
Codex. To exercise all four live sites without modifying the committed fixture,
copy only its Draft sources into a disposable workspace:

```sh
rm -rf /tmp/draft-agent-acceptance-live
mkdir -p /tmp/draft-agent-acceptance-live/app \
  /tmp/draft-agent-acceptance-live/lib
cp examples/agent-acceptance/app/*.draft \
  /tmp/draft-agent-acceptance-live/app/
cp examples/agent-acceptance/lib/*.draft \
  /tmp/draft-agent-acceptance-live/lib/

build/draftc resolve /tmp/draft-agent-acceptance-live/app \
  --build -o /tmp/draft-agent-acceptance-live/program \
  --model model-name
/tmp/draft-agent-acceptance-live/program
```

Omit `--model` to use the Codex-configured default. The resolver runs the
independent declaration/member sites in one provider wave, then the independent
expression/statement sites in another. It checks and commits the complete
program before native emission. Use `--regenerate` on the repository fixture
only when intentionally refreshing its committed expansions.

The ordinary judgment intentionally cannot prove the imported function's
return value from its interface alone. Supply the exact library source as the
claim artifact:

```sh
build/draftc judge examples/agent-acceptance/app \
  --judge-artifact library-source:examples/agent-acceptance/lib/package.draft \
  --model model-name
```

The original Codex/model qualification and the former native-distribution
experiment remain available in the
[historical first-compiler qualification](../history/releases/first-compiler-qualification.md).

## 2026-07-20 live adapter requalification

On AArch64 macOS, `codex-cli 0.144.6` using its configured default model resolved
a clean temporary copy of this fixture through provider identity
`openai-codex-cli-v28`. All four sites were accepted in two ready waves with
four actual provider calls: declaration/member first, then expression/statement.
`resolve --build` emitted and ran the native executable; the saved manifest and
generated objects then passed provider-free `check`, `build`, execution, and
expanded-source inspection.

The corrected `agent-pending/app` walkthrough was independently resolved to the
checked expression `42`, built and executed, rebuilt provider-free, expanded,
and judged successfully. That smaller run also verified that its resolution
store lands under `agent-pending/.draft/`, rather than the shared parent of the
examples collection.
