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
