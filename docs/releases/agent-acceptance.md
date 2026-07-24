# Agent acceptance fixture

This workspace is the small end-to-end acceptance case for compiler-owned agent
operations. It combines features that are most useful when exercised together:

- independent declaration and member synthesis in one opaque interface wave;
- typed expression and statement synthesis inside `main`;
- an imported package used by the program, test graph, and benchmark graph;
- independent native test and benchmark execution and evidence;
- an artifact-backed judgment whose claim depends on the imported body;
- provider-free checking and native building from saved expansions; and
- native debug information for a program containing generated source.

The committed `.draft` files are content-addressed acceptance inputs, not build
caches. They contain four checked generated-source objects and separate v7
resolution manifests for AArch64 macOS, AArch64 GNU/Linux, x86-64 GNU/Linux,
and x86-64 Windows. Each selected program has four sites, and the checked
expansions are shared across the four targets. The
input digests remain target-specific where the typed obligation contains target
facts. Validation and judgment evidence is stored independently in the
workspace-level `.draft/evidence` store and is not selected by either manifest.
`.draft/build` and `.draft/staging` are ignored and may be deleted at any time.

## Reproduce the committed program

The fixture already contains fresh checked expansions, so ordinary consumer
commands are deliberately provider-free:

```sh
build/draftc check examples/agent-acceptance/app
build/draftc build examples/agent-acceptance/app \
  -o /tmp/draft-agent-acceptance
/tmp/draft-agent-acceptance
build/draftc test examples/agent-acceptance/app
build/draftc bench examples/agent-acceptance/app --verify
```

These commands use the macOS compatibility default. Pass
`--target aarch64-linux` or `--target x86_64-linux` to select the corresponding
independently committed Linux manifest.

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

## 2026-07-20 live adapter and matrix requalification

On AArch64 macOS 26.5.2, `codex-cli 0.144.6` using its configured default model
resolved this fixture from zero `.draft` state through provider identity
`openai-codex-cli-v28`. Separate macOS and Linux commands each synthesized all
four sites with zero reused pins. After the PR1 language and coding-skill inputs
changed, both target manifests were explicitly regenerated through the same
provider boundary; all four regenerated expansions typechecked, and both
targets accepted the same four source objects. Both saved manifests then passed
provider-free frontend checks, while the macOS program passed native
build/execution, Draft test and verified benchmark execution, workspace evidence
publication, and expanded-source inspection with linked LLVM 22.1.8.

The completed raw string-data integration, exact compile-time type-value
semantics, lexical statement selection, and target-scalar materialization use
compiler content identity `draft-bootstrap-cpp-v140` and core distribution
identity `draft-core-bootstrap-v4`. Provider-free `resolve --revalidate`
commands for macOS and Linux each reused and rechecked all four saved
expansions; no generated source object changed. Both final manifests then
passed frontend checks. On macOS, the final program built and launched without
an LLVM debug-information warning; the Linux target emitted an AArch64 ELF
object. Fresh attempt-one test and verified-benchmark evidence passed under
linked LLVM 22.1.8. The committed evidence objects are
`f076bbfae10007e1888147530e54e81ff1cd8bcb318c64072fad6ee64c4f9100`
for the test and
`4f2157fbd8fe050ddb2658f47174fca9c753783af4f4a5df4de61daeaab4c0f7`
for the benchmark. The complete normal, release, and ASan/UBSan CTest matrices
each passed 71 of 71 tests on the final tree.

The semantic work-graph repair was requalified on 2026-07-20 with compiler
content identity `draft-bootstrap-cpp-v140`. The compiler now retains immutable
declaration baselines separately from generation-owned body semantics and HIR,
and keys retained body work by the exact canonical concrete-instance demand
set. Provider-free `resolve --revalidate` reused and rechecked all four sites
for each target with zero synthesis calls. The resulting committed program
digests are
`8393122f7ffed79faa0322779db944e26c0976a80d6ab991762748fc940dd50e`
for AArch64 macOS and
`92ccb29aa3f8718aad0763cf433636c2ef9c92ed22603d97715cd98d8e6f7091`
for AArch64 GNU/Linux; the four generated source objects did not change.

Selective packed struct fields were requalified on 2026-07-22 with compiler
content identity `draft-bootstrap-cpp-v141`. Provider-free
`resolve --revalidate` reused and rechecked all four saved expansions for each
target with zero synthesis calls. The resulting committed program digests are
`a1a044e1429b205e4004932756d1541efcb16b0fa0cbbbe9757ac3eb5f6574de`
for AArch64 macOS and
`c7b77a0196c348dd0e2be9133f8dda20a7c33d6b79164c02e95b4a9f27f0f9fa`
for AArch64 GNU/Linux; the generated source objects did not change. The full
CTest matrix passed 74 of 74 tests on the qualified tree.

Field-level `bits(N)` layout was requalified on 2026-07-22 with compiler
content identity `draft-bootstrap-cpp-v142`. The embedded Draft coding skill
changed with the language contract, so provider-free `resolve --revalidate`
rechecked and reused all four saved expansions for each target with zero
synthesis calls. The resulting committed program digests are
`b6f7abb5992dfd0e7f89158a5fb18514b26bdfa9878a285eeb0e5559901f2193`
for AArch64 macOS and
`e170d9ff2b75a664c64ed4dfb46726ddafceb1192cc37121f19a28d7f81f6734`
for AArch64 GNU/Linux; the generated source objects did not change. The full
normal and ASan/UBSan CTest matrices each passed 74 of 74 tests.

The repository example matrix classified all 36 tracked Draft/assembly package
directories. Both-target frontend qualification passed for every declared
positive or intentionally unresolved row. On the native macOS host, all 25
ordinary executable rows built and launched, the C client consumed the Draft
library, the explicitly mapped C object supplied the foreign-provider example,
and every classified example test and benchmark passed from an isolated
workspace copy. The Linux manifest's native execution remains the responsibility
of the required AArch64 Linux CI row; this local run does not claim Linux-host
execution.

## x86-64 Linux manifest qualification

On 2026-07-22, the new `draft-x86_64-linux-gnu-v1` manifest was created by
provider-free `resolve --revalidate`. The resolver rechecked and reused all four
existing generated source objects and made zero synthesis calls. The committed
x86-64 resolved-program digest is
`4c81303f74cfcfe552864e25250034383b3dcd9babc7d30b1e82192644189294`.
The exhaustive example frontend matrix then passed every classified package on
AArch64 macOS, AArch64 GNU/Linux, and x86-64 GNU/Linux. This is source and
target-identity evidence; native x86-64 execution remains the responsibility of
the required Linux CI row and is not claimed by the macOS-hosted run.

The corrected `agent-pending/app` walkthrough was independently resolved to the
checked expression `42`, built and executed, rebuilt provider-free, expanded,
and judged successfully. That smaller run also verified that its resolution
store lands in the `app` root/target namespace under
`agent-pending/.draft/resolutions/`, rather than the shared parent of the
examples collection or a sibling executable's manifest.

## Embedded-core and workspace-command requalification

On 2026-07-24, provider-free `resolve --revalidate` reused and rechecked all
four saved expansions with zero synthesis calls after core source became an
immutable compiler-distributed bundle. The bundle identity was
`draft-core:a65d9a05146ae42d44dc7cd1e4396fd20eef7e7b1d83e5dc5c7ac05fa399c0bb`.
The resulting committed resolved-program digests are
`f22d3b1e6b11845739b7be0d36a0225026f2660c71c59e1a1a26a133055f27ef`
for AArch64 macOS,
`4b5d723c652b6e864573ec60afb86e5e026d0aec6c88e68ded8d494a72d9dc79`
for AArch64 GNU/Linux,
`9941c17fa05a07e8f232f45939f064ad937afc8afc16c3dc8d49ecba72b85ef0`
for x86-64 GNU/Linux, and
`81e2e7de0dc2ab14a27918eceb40a526fb046d1797382fbcd21041938ef03b1c`
for x86-64 Windows. No generated source object changed. The normal CTest
matrix passed all 93 tests, including provider-free frontend, expansion,
validation, native run, and relocated-compiler core checks.

## Configured-process target-profile revalidation

On 2026-07-24, the closed system-provider summaries gained the exact
`core/process` argument/environment/working-directory symbols and advanced all
four target identities. Provider-free `resolve --revalidate` reused and
rechecked all four saved expansions with zero synthesis calls on each target;
no generated source object changed. The resulting committed resolved-program
digests are
`1d11757dd656114d98cd7500d31dd9d80c178fbab47d263665eb833184a86abb`
for `draft-aarch64-macos-v6`,
`c86d794b7228fef6493a483f856ed08ecff9abf1e67d273f5414c1a8ed7dd0e9`
for `draft-aarch64-linux-gnu-v2`,
`def8903f032fd34d8833a3636cc44589b2761e815e6cdbdbc812b5471cd6f260`
for `draft-x86_64-linux-gnu-v2`, and
`ae06c1223d19fc49601f98bf94bcccb625f9dced7088bbd5af83f17f5b226745`
for `draft-x86_64-windows-msvc-v2`.

## Shared POSIX process implementation revalidation

On 2026-07-24, the three byte-identical Darwin/AArch64-Linux/x86-64-Linux
fork/exec/wait procedures were replaced by one compile-time-selected POSIX
implementation. Darwin and glibc retain separate foreign groups and errno
symbols; the public `core/process` behavior and generated source did not
change. The resulting embedded core identity is
`draft-core:c9afdc7a187dae44c381462e9e5f16f5315717de0122c4231194ab3f7efb3459`.
Provider-free `resolve --revalidate` reused and rechecked all four expansions
with zero synthesis calls for each target. The committed resolved-program
digests are
`7bfa92ab29bee84aa39d1c79e7040f09ac27361a1bad13f3e6732415f9d67cff`
for `draft-aarch64-macos-v6`,
`e64ed84b9dcd2f32f35184f1b7ae00fd0bbdd9c889c3add85d38afda146735d7`
for `draft-aarch64-linux-gnu-v2`,
`7d90a79b2a8aacc4e2969f1898ef8821ce7841cc13132f02280b8df548736f2f`
for `draft-x86_64-linux-gnu-v2`, and
`22186fba98f4f600a0f6856512e7a07f176528ae6a460a5865b9195522025828`
for `draft-x86_64-windows-msvc-v2`. The native `core/process` integration test
and all ten example/integration tests passed on the AArch64 macOS host.
