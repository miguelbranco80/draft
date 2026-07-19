# Live `...` synthesis

This is the smallest intentionally unresolved Draft workspace. It demonstrates
one typed expression synthesis site, the saved generated-source transaction, a
provider-free rebuild, and a separate optional judgment. The example commits no
`.draft` directory because its purpose is to start with a real Codex call.

The package lives in `app/` because the Draft CLI treats the requested package's
parent as its workspace root. Consequently, resolution writes the manifest and
generated source under this example's `.draft/` directory rather than under the
shared top-level `examples/` directory.

## Run it without changing the checkout

Build `draftc` and make sure the ordinary Codex CLI is installed, authenticated,
and discoverable through `PATH`:

```sh
codex --version
```

From the repository root, copy the unresolved workspace and resolve that copy:

```sh
rm -rf /tmp/draft-agent-pending
cp -R examples/agent-pending /tmp/draft-agent-pending

build/draftc resolve /tmp/draft-agent-pending/app \
  --build -o /tmp/draft-agent-pending-program \
  --timings

/tmp/draft-agent-pending-program
```

`--model` is optional. When omitted, the adapter uses the Codex-configured
default. The executable prints nothing and returns success; its runtime
assertion checks that the generated `i64` expression produced `42`.

The successful resolve creates:

```text
/tmp/draft-agent-pending/.draft/resolution.json
/tmp/draft-agent-pending/.draft/generated/<content-digest>.draft
```

Those are ordinary project source inputs, not a compiler cache. Inspect the
complete provider-free source view with:

```sh
rm -rf /tmp/draft-agent-pending-expanded
build/draftc expand /tmp/draft-agent-pending/app \
  --out /tmp/draft-agent-pending-expanded
```

After resolution, these commands do not contact Codex:

```sh
build/draftc check /tmp/draft-agent-pending/app
build/draftc build /tmp/draft-agent-pending/app \
  -o /tmp/draft-agent-pending-rebuild
/tmp/draft-agent-pending-rebuild
```

Judgment is deliberately separate from synthesis and building. To evaluate the
surface claim after the program is resolved, run:

```sh
build/draftc judge /tmp/draft-agent-pending/app
```

A real project should commit `.draft/resolution.json` together with every
referenced `.draft/generated` object. Teammates and clean builds can then check,
test, expand, and build the exact accepted source without Codex access.
