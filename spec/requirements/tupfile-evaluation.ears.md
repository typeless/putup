# Tupfile evaluation — requirements

- area: tupfile-evaluation
- required-legs: none

The subject here is what a Tupfile's text means as it is evaluated: which included files
contribute, and what an assignment operator does to a variable that already has a value. It
stops where a rule is emitted — the rule's own operands, its globs and its outputs each have
their own area.

This area requires no legs. Evaluation carries no state across builds: the same text, the same
configuration and the same tree evaluate to the same result, so its requirements are invariants
of that function and carry no `leg` field.

See `README.md` for the format and the rules that apply to every area.

---

## Group: includes

What a repeated `include` of one file contributes.

An `include` is deduplicated so that a file reached twice along the same path does not emit its
rules twice. The unit of that deduplication is the file *together with the conditional context
it sits in*, because a conditionally-guarded include and an unguarded one are two different
contributions of the same text: the guarded copy's rules carry the guard, the unguarded copy's
do not, and skipping the second because the first was seen drops everything the second would
have added.

### REQ-EVAL-INCLUDE-CONTEXT

- conformance: deliberate-deviation
- reference: upstream `parser_include_file` holds no include-once set at all and reparses the named file on every `include`, so upstream never skips a repeat; putup deduplicates repeats instead, and this requirement bounds that deduplication to a single conditional context rather than lifting it to the file
- discharge: test "Scenario: Include first seen in a dead branch still applies when included actively"
- discharge: test "Scenario: Include first seen in a config-inactive branch is reprocessed when included actively"

Where a Tupfile includes a file that was first included under a conditional context other than
the one this include sits in, putup shall process that file again rather than skip it as already
included.

---

## Group: assignment

What an assignment operator does to a variable that is already defined.

### REQ-EVAL-SOFT-ASSIGN

- conformance: putup-only
- reference: upstream `set_variable` recognises `+=`, `:=` and plain `=` only, and tup.1's variable section documents no `?=`, so there is no upstream counterpart to defer to
- discharge: test "Scenario: ?= soft assignment - = takes precedence"

Where a Tupfile assigns a variable with `?=` and that variable is already defined, putup shall
keep the defined value rather than replace it with the assignment's own value.

## Group: variable references

What `$`, `@` and `&` mean when no variable reference follows them.

### REQ-EVAL-SIGIL-LITERAL

- conformance: tup-conformant
- reference: upstream `eval` (`src/tup/parser.c`) expands `$(`, `@(` and `&(` only when the sigil is immediately followed by `(`; any other `$`, `@` or `&` is appended to the result unchanged, which is what lets an output exclusion such as `^\.d$` end in a regex anchor. The parse keeps `$$` as two literal characters; evaluation then rewrites `$$` to `$`, a deviation of its own that upstream does not make, pinned by "Dollar-dollar escapes to literal dollar in shell commands"
- discharge: test "A variable sigil not followed by an open paren is literal text"
- discharge: test "An output exclusion regex may end in a dollar anchor"

Where a `$`, `@` or `&` in a Tupfile is not immediately followed by `(`, putup shall read it as
a literal character rather than reject the Tupfile.

## Group: conditionals

Where the two sides of an `ifeq` or `ifneq` begin and end.

### REQ-EVAL-IFEQ-LAST-PAREN

- conformance: tup-conformant
- reference: upstream `eval_eq` (`src/tup/parser.c`) takes the left side from after the first `(` to the first `,`, and the right side from after that comma to the last `)` on the line, found by scanning back from the end of the line, so a `)` inside the right side is part of it. A `#` after the start of the line is not a comment there (upstream strips comments only at the start of a line), so it belongs to the side it falls in; a line with no `,` or no `)` is a syntax error on that line
- discharge: test "An ifeq right-hand side runs to the last close paren on its line"
- discharge: test "An ifeq missing its comma or close paren is refused on its own line"

When a Tupfile writes `ifeq` or `ifneq`, putup shall end the right-hand side at the last `)` on
that line, read a `#` on that line as text, and reject a line that has no `,` or no `)` with an
error on that line.

## Group: macros

What defining a bang macro in terms of another one binds.

### REQ-EVAL-BANG-ALIAS-COPIES

- conformance: tup-conformant
- reference: upstream reads `!a = !b` as an alias when the value starts with `!`: it copies `!b`'s foreach, order-only inputs, command and outputs into a new `!a` at that line, replacing any earlier `!a`, and refuses with "tup error: Unable to find !-macro '%s'" when `!b` is not defined (`parse_bang_definition`, the `value[0] == '!'` branch). A later redefinition of `!b` leaves `!a` as copied. Upstream looks the whole value up, so `!a = !b extra` is refused as an unknown macro; putup refuses it as a parse error
- discharge: test "Scenario: A bang macro defined as another macro takes a copy of its definition"

When a Tupfile defines a bang macro as `!name = !target`, putup shall bind `!name` to a copy of
`!target`'s definition as it stands at that line, and reject the Tupfile if `!target` is not
defined.
