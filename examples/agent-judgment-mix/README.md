# Judgments guiding `...`

This intentionally unresolved workspace combines judgments with all five Draft
1 synthesis categories:

| Surface position | Generated fragment | Guiding judgment |
| --- | --- | --- |
| Package declaration list | `Generated_Offset` | Package claim and `CONTRACT.md` |
| `Generated_Record` member list | `value: i64` | Package claim plus the earlier type-member claim |
| `compute` return expression | One `i64` expression | Package claim plus the dominating procedure claim |
| `emit_barrier` assembly instruction list | One `dmb ish` row | Package claim plus the dominating procedure claim |
| `main` statement list | One assertion | Package claim plus the dominating procedure claim |

The claims guide synthesis through fixed source-position rules; `resolve` does
not execute them. Generated fragments may not add another `judge`. After the
program is resolved, the separate `judge` command reviews the complete checked
program, including relevant generated source.

## Run it without changing the checkout

The example begins without a `.draft` manifest so it makes real Codex requests.
Copy it to a disposable workspace, then resolve and build the copy:

```sh
rm -rf /tmp/draft-agent-judgment-mix
cp -R examples/agent-judgment-mix /tmp/draft-agent-judgment-mix

build/draftc resolve /tmp/draft-agent-judgment-mix/app \
  --build -o /tmp/draft-agent-judgment-mix-program \
  --timings
/tmp/draft-agent-judgment-mix-program
```

Resolution writes the accepted fragments and manifest below
`/tmp/draft-agent-judgment-mix/.draft/`. Inspect the complete provider-free
projection and rebuild it without Codex:

```sh
build/draftc expand /tmp/draft-agent-judgment-mix/app \
  --out /tmp/draft-agent-judgment-mix-expanded
build/draftc check /tmp/draft-agent-judgment-mix/app
build/draftc build /tmp/draft-agent-judgment-mix/app \
  -o /tmp/draft-agent-judgment-mix-rebuild
```

Listing the five surface judgments is also provider-free. Running them is a
separate explicit Codex-backed action:

```sh
build/draftc judge /tmp/draft-agent-judgment-mix/app --list
build/draftc judge /tmp/draft-agent-judgment-mix/app
```

A real project commits the manifest and every referenced generated object after
review. This example intentionally does not, because its purpose is to exercise
the first live synthesis transaction and make the guidance visible.
