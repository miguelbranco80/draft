# Explicit UTF-8

This example shows how Draft keeps byte storage and Unicode interpretation
separate. [`package.draft`](package.draft) validates and counts a mixed-width
string, walks it by explicit byte offset, and encodes one `rune` into a fixed
caller-owned buffer. Nothing allocates, and no compiler intrinsic is involved.

[`utf8_test.draft`](utf8_test.draft) qualifies all one- through four-byte width
boundaries, forward and reverse decoding, embedded NUL, an undersized output
buffer, and the main malformed-sequence classes. The decoder is strict: an
invalid sequence reports `.invalid_encoding`, returns U+FFFD, and recommends
one byte of progress rather than silently accepting or normalizing input.

On a supported native host:

```sh
build/draftc check examples/core-utf8
build/draftc test examples/core-utf8
build/draftc run examples/core-utf8
```

The example is deliberately explicit. A future `for rune in text` syntax can
lower to the same byte-offset decoding operation without replacing this API.
