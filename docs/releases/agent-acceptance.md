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
caches. They contain five checked generated-source objects and separate v6
resolution manifests for AArch64 macOS and GNU/Linux. Each selected program has
four sites; three expansions are shared across targets, while the accepted
expression has one target-specific spelling. Validation and judgment evidence
is stored independently in the workspace-level `.draft/evidence` store and is
not selected by either manifest. `.draft/build` and `.draft/staging` are ignored
and may be deleted at any time.

## Reproduce the committed program

The fixture already contains fresh checked expansions, so ordinary consumer
commands are deliberately provider-free:

```sh
build/draftc check examples/agent-acceptance --root app
build/draftc build examples/agent-acceptance --root app \
  -o /tmp/draft-agent-acceptance
/tmp/draft-agent-acceptance
build/draftc test examples/agent-acceptance --root app
build/draftc bench examples/agent-acceptance --root app
```

These commands use the macOS compatibility default. Pass
`--target aarch64-linux` to select the independently committed Linux manifest.

All four commands load the committed generated source without contacting
Codex. `build` uses the host native toolchain and does not require test,
benchmark, or judgment evidence. To recheck every saved expansion without
provider access, run:

```sh
build/draftc resolve examples/agent-acceptance --root app --revalidate
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

build/draftc resolve /tmp/draft-agent-acceptance-live --root app \
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
build/draftc judge examples/agent-acceptance --root app \
  --judge-artifact library-source:examples/agent-acceptance/lib/package.draft \
  --model model-name
```

The original Codex/model qualification and the former native-distribution
experiment remain available in the
[historical first-compiler qualification](../history/releases/first-compiler-qualification.md).

## 2026-07-20 live adapter and matrix requalification

On AArch64 macOS 26.5.2, `codex-cli 0.144.6` using its configured default model
resolved this fixture from zero `.draft` state through provider identity
`openai-codex-cli-v28`. Separate macOS and Linux commands each synthesized all
four sites with zero reused pins. Both saved manifests then passed provider-free
frontend checks, while the macOS program passed native build/execution, Draft
test and verified benchmark execution, workspace evidence publication, and
expanded-source inspection with linked LLVM 22.1.8.

The repository example matrix classified all 36 tracked Draft/assembly package
directories. Both-target frontend qualification passed for every declared
positive or intentionally unresolved row. On the native macOS host, all 25
ordinary executable rows built and launched, the C client consumed the Draft
library, the explicitly mapped C object supplied the foreign-provider example,
and every classified example test and benchmark passed from an isolated
workspace copy. The Linux manifest's native execution remains the responsibility
of the required AArch64 Linux CI row; this local run does not claim Linux-host
execution.

The corrected `agent-pending/app` walkthrough was independently resolved to the
checked expression `42`, built and executed, rebuilt provider-free, expanded,
and judged successfully. That smaller run also verified that its resolution
store lands in the `app` root/target namespace under
`agent-pending/.draft/resolutions/`, rather than the shared parent of the
examples collection or a sibling executable's manifest.
