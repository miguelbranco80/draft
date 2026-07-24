# ThinLTO cross-package benchmark

This example makes the O2 package-boundary benefit visible without involving
allocation, I/O, or a large application. `lib/kernel.mix` is an ordinary hidden
Draft definition which ThinLTO may import into the app loop. The app's local
reference loop performs identical arithmetic without crossing a package
boundary.

Build and run the correctness smoke program:

```sh
build/draftc run examples/thinlto-benchmark
```

Run the isolated benchmark samples:

```sh
build/draftc bench examples/thinlto-benchmark/app -O2
```

`bench_hidden_draft_boundary` is the ThinLTO-importable path;
`bench_local_reference` is the equivalent package-local path. At O2 their
medians should be close because the package call has been imported. At O0 the
hidden path retains a call on every iteration, so running the same command with
`-O0` makes the boundary cost visible. Exact ratios are machine-dependent, and
O0 versus O2 changes more than LTO, so this is a practical demonstration rather
than a laboratory-isolated optimizer measurement. The command prints its
evidence digest; the matching `.draft/evidence/<digest>.json` contains the ten
`durations_ns` samples for each procedure.
