# Open and specification-facing language decisions

These records concern source acceptance or observable language behavior. A
provisional rule may be mirrored in the specification so current bootstrap
behavior is unambiguous without becoming an accepted permanent Draft rule. The
complete chronological record is preserved in
[the bootstrap decision archive](../history/bootstrap-implementation-decisions.md).

## End-of-line `^`

Status: provisional; needs specification confirmation.

Draft uses `^` both as binary XOR and as postfix pointer dereference. The
semicolon rule inserts after postfix `^` but not after an operator that requires
a following operand. The token stream alone cannot distinguish these programs:

```draft
pointer^
next_statement()
```

```draft
left ^
right
```

The bootstrap lexer treats `^` at a semicolon-eligible newline or EOF as postfix,
following the specification's explicit statement-ending list. During parsing,
`^` is binary when the next token directly begins a primary operand, synthesis,
assembly, denial expression, or unary-only `!`/`~` operand. It is postfix before
a delimiter, postfix continuation, or another binary operator. Thus `left ^
right` works inside parentheses (where no semicolon is inserted), `pointer^ + 1`
dereferences before addition, and an ambiguous unary operand is made explicit as
`left ^ (-right)`. This rule is deterministic and whitespace-independent.
The specification mirrors it provisionally; design confirmation must either
adopt it or replace it directly.
