# Judgment tour

This provider-free example shows where Draft 1 permits `judge` and what each
placement means. It includes package, struct-member, procedure, branch, loop,
switch-case, and compile-time `when` judgments. The package also demonstrates
the current equivalent of a method: `counter_add(counter: ^Counter, amount:
i64)` is an ordinary procedure operating directly on a caller-owned struct.
Draft 1 has no method-declaration or method-call syntax.

The claims are review requests, not executable assertions and not formal
proofs. They emit no machine code. The example uses ordinary `assert` calls for
the objective runtime results, so checking, building, and running it never
contacts Codex:

```sh
build/draftc check examples/judgment-tour/app
build/draftc build examples/judgment-tour/app \
  -o /tmp/draft-judgment-tour
/tmp/draft-judgment-tour
```

## Inspect and run judgments

Listing sites is provider-free and prints each stable judgment identity, its
package, its enclosing declaration or type, source file, and occurrence:

```sh
build/draftc judge examples/judgment-tour/app --list
```

Run all selected judgments with the configured Codex CLI:

```sh
build/draftc judge examples/judgment-tour/app
```

Pass either a package-and-declaration selector such as `app:counter_add`, or one
exact `site-...` identity from the list, to run a subset. A successful
invocation records evidence under this example's `.draft/` area; that evidence
is separate from source resolution and is never required by `check` or
`build`.

For `--target aarch64-macos`, the `when` else-branch judgment is present. For
`--target aarch64-linux`, the Linux judgment is present instead. Runtime branch
and switch judgments are different: both sites remain independently eligible,
regardless of which branch a particular execution would take. A loop judgment
also produces one review request per command invocation, not one request per
runtime iteration.
