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

### REQ-EVAL-INCLUDE-CONTEXT-BY-STATEMENT

- conformance: deliberate-deviation
- reference: upstream's `parser_include_file` (`src/tup/parser.c`) holds no include-once set, and it does not reach an `include` in a branch it does not take. putup parses both branches of a condition on a config or environment variable, so the conditional context of REQ-EVAL-INCLUDE-CONTEXT has to repeat for a chain of includes to end. putup identified a condition by the node it made each time it evaluated the statement, so a file that included itself under such a condition was a new context at every level and putup read it until it crashed, in either branch. A condition is now the conditional statement, by file and line, with the branch; a context is the set of those. Under its own condition a file is read once more, as upstream reads it when the branch is taken, and upstream reads it again after that without end unless the file's text stops it. Where the file's text stops it after more than one further reading, putup lacks the later readings. A rule in the further reading declares the output of the first reading again, which putup reports as a conflict (COMPATIBILITY.md, "Includes and Conditionals"). A file with k such statements is read once for each set of them, so the readings grow as 2^k. All of this holds for a branch the configuration takes; REQ-EVAL-INCLUDE-RETURN-UNTAKEN covers the other branch
- discharge: test "Scenario: A chain of includes that returns to a file being read ends"

Where an `include` sits under conditions that differ from those of an earlier `include` of the
same file only by a repeat of a conditional statement and branch, putup shall skip the file as
already included.

### REQ-EVAL-INCLUDE-RETURN-UNTAKEN

- conformance: deliberate-deviation
- reference: upstream does not reach an `include` in a branch it does not take (`src/tup/parser.c`, the `if` handling in `parse_tupfile`). putup parses that branch to declare its rules, and by REQ-EVAL-INCLUDE-CONTEXT-BY-STATEMENT alone it read a file once for each set of its conditional includes of itself (2^k readings for k statements), and it followed an include through a symlink to the file's own directory until the OS refused the path. An include that returns to a file being read is one that upstream can repeat without end if the configuration takes the branch, and putup lets one reading stand for it. The file is identified by its real directory and its spelled name: a symlinked directory names the same reading, a symlinked file in another directory does not, because the directory sets `TUP_CWD`. The cost: later statements in that branch expand with the variables of one reading, where a taken branch has those of the further readings too, so the eager conflict check on that branch (COMPATIBILITY.md, "Includes and Conditionals") can miss a conflict or report one that no configuration has. Chains through several files still grow with the number of distinct paths among them
- discharge: test "Scenario: An include of a file being read is not followed in a branch not taken"

Where an `include` sits in a branch the configuration does not take and names a file that is being
read, putup shall not read the file.

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
- discharge: test "An ifeq left-hand side ends at the first comma on its line"
- discharge: test "An ifeq right-hand side runs to the last close paren on its line"
- discharge: test "An ifeq missing its comma or close paren is refused on its own line"

When a Tupfile writes `ifeq` or `ifneq`, putup shall end the left-hand side at the first `,` on
that line, end the right-hand side at the last `)` on that line, read a `#` or a quote on that
line as text, and reject a line that has no `,` or no `)` with an error on that line.

## Group: macros

What defining a bang macro in terms of another one binds.

### REQ-EVAL-BANG-ALIAS-COPIES

- conformance: tup-conformant
- reference: upstream reads `!a = !b` as an alias when the value starts with `!`: it copies `!b`'s foreach, order-only inputs, command and outputs into a new `!a` at that line, replacing any earlier `!a`, and refuses with "tup error: Unable to find !-macro '%s'" when `!b` is not defined (`parse_bang_definition`, the `value[0] == '!'` branch). A later redefinition of `!b` leaves `!a` as copied. Upstream looks the whole value up, so `!a = !b extra` is refused as an unknown macro; putup refuses it as a parse error
- discharge: test "Scenario: A bang macro defined as another macro takes a copy of its definition"

When a Tupfile defines a bang macro as `!name = !target`, putup shall bind `!name` to a copy of
`!target`'s definition as it stands at that line, and reject the Tupfile if `!target` is not
defined.

### REQ-EVAL-BANG-WORD-ERROR-SITE

- conformance: deliberate-deviation
- reference: upstream reports a failing rule as "Error parsing %s line %i" with the file being parsed and the line of the rule (`parse_tupfile`, `src/tup/parser.c`), so a malformed output word a macro defines in Tuprules.tup is reported on the calling Tupfile's line. The word is wrong as the macro wrote it, whichever rule calls the macro, so putup names the line that holds the text to change. putup reported the calling Tupfile's name with the definition's line number, a position in neither file
- discharge: test "Scenario: A refused output word from a bang macro is reported where the macro wrote it"

When an output word a bang macro defines is refused, putup shall report the file and line of the
macro's definition.

### REQ-EVAL-INPUT-NOT-ROOT

- conformance: tup-conformant
- reference: upstream's `path_list_fill_dt_pel` (`src/tup/parser.c`) refuses an input whose path has no final element after resolution: `.` as "Not expecting '.' path here", and `./` or `sub/..` as "Final pel missing for path". putup crashed on such an input in an in-tree build, reading the node type of the source root, which had no node, and refused it in a variant build as "Invalid source node ID"
- discharge: test "Scenario: An input naming the project root is refused"

When a rule's input or order-only input names the project root directory, putup shall reject the
Tupfile with an error on the rule's line.

### REQ-EVAL-TUP-CWD-FOLLOWS-FILE

- conformance: tup-conformant
- reference: upstream expands `TUP_CWD` to `get_relative_dir(tf->tent, tf->curtent)` and `TUP_VARIANT_OUTPUTDIR` to `get_relative_dir(tf->srctent, tf->tent)` (`eval`, `src/tup/parser.c`); `tup.1` gives `..` as the value of `TUP_CWD` inside a file read by `include ../foo.tup` and says `TUP_VARIANT_OUTPUTDIR` does not change when another file is included. putup gave the Tupfile's root-relative directory in the Tupfile and changed `TUP_CWD` only inside a `Tuprules.tup`
- discharge: test "Scenario: TUP_CWD and TUP_VARIANTDIR name the directory of the file being parsed"

While a Tupfile or a file it includes is being parsed, putup shall expand `$(TUP_CWD)` to the
relative path from the Tupfile's directory to the directory of the file being parsed, and
`$(TUP_VARIANT_OUTPUTDIR)` to the same value in every file.

### REQ-EVAL-TUP-VARIANTDIR-FOLLOWS-FILE

- conformance: deliberate-deviation
- reference: upstream expands `TUP_VARIANTDIR` to a reference to the node of the directory being parsed, keeps the reference through assignments, and turns it into a path where a rule uses it, relative to the directory of the file that holds the rule (`eval` and `expand_node_strings`, `src/tup/parser.c`). The two agree wherever the rule is in the Tupfile. For a rule in an included file upstream's path starts from that file's directory, not from the directory the command runs in, and without a variant it is `.`, where `tup.1` says the value equals `TUP_CWD`. putup expands the variable where it is written, to the path `tup.1` describes, from the directory the command runs in. putup gave `TUP_VARIANT_OUTPUTDIR`'s value in every file
- discharge: test "Scenario: TUP_CWD and TUP_VARIANTDIR name the directory of the file being parsed"

While a Tupfile or a file it includes is being parsed, putup shall expand `$(TUP_VARIANTDIR)` to
`$(TUP_CWD)` taken from the Tupfile's variant output directory.

### REQ-EVAL-INCLUDE-RELATIVE-TO-FILE

- conformance: tup-conformant
- reference: upstream's `parser_include_file` (`src/tup/parser.c`) resolves the path of an `include` against `tf->curtent`, the directory of the file that contains the statement. putup resolved it against the Tupfile's directory, so an included file could not include a file beside itself by a relative path
- discharge: test "Scenario: TUP_CWD and TUP_VARIANTDIR name the directory of the file being parsed"
- discharge: test "Scenario: A file named by includes in two directories is included once"

When an `include` statement names a relative path, putup shall resolve the path against the
directory of the file that contains the statement.

### REQ-EVAL-INCLUDE-RULES-FROM-FILE

- conformance: tup-conformant
- reference: upstream's `include_rules` counts the directories to climb from `tf->curtent`, the directory of the file being parsed, and reads each `Tuprules.tup` relative to it (`src/tup/parser.c`). putup walked up from the Tupfile's directory whichever file held the statement. Which of those files putup skips as already read is REQ-EVAL-INCLUDE-CONTEXT.
- discharge: test "Scenario: include_rules in an included file reads the Tuprules.tup files above that file"

When a file below the project root contains `include_rules`, putup shall read the `Tuprules.tup`
files from the project root down to the directory of that file.

### REQ-EVAL-INCLUDE-RULES-OUTSIDE-TREE

- conformance: deliberate-deviation
- reference: upstream's `parser_include_file` (`src/tup/parser.c`) looks the include's directory up in its database of the project tree, so it cannot include a file from outside the tree and has no case for `include_rules` there. putup includes such a file: in a three-tree build the source and build trees are outside the tree that holds the Tupfiles. No directory lies between the project root and a file outside it, so there is no `Tuprules.tup` to read. putup read the `Tuprules.tup` files above the Tupfile for it
- discharge: test "Scenario: include_rules in a file outside the project reads no Tuprules.tup"

When a file outside the tree that holds the Tupfiles contains `include_rules`, putup shall read
no `Tuprules.tup` for it.

### REQ-EVAL-INCLUDE-RULES-NOT-NESTED

- conformance: tup-conformant
- reference: upstream's `include_rules` fails with "Unable to call include_rules from within a Tuprules.tup context" while `tf->including_rules` is set, which holds for every file an `include_rules` reaches, a file a `Tuprules.tup` includes among them (`src/tup/parser.c`). putup accepted the statement and read the `Tuprules.tup` files above the file that held it, so a `Tuprules.tup` could bring another directory's rules into every Tupfile below it. A file putup skips as already read (REQ-EVAL-INCLUDE-CONTEXT) is not parsed, so a statement in it is not reached; upstream reads the file again and refuses it
- discharge: test "Scenario: include_rules in a file that include_rules reads is refused"
- discharge: test "Scenario: include_rules in a branch of a Tuprules.tup that no configuration takes is not read"

If putup parses an `include_rules` statement while it reads a file for another `include_rules`,
then putup shall reject the Tupfile with an error on the statement's line.

### REQ-EVAL-INCLUDE-RULES-NOT-NESTED-EITHER-BRANCH

- conformance: deliberate-deviation
- reference: upstream executes only the branch a condition takes, so it accepts a nested `include_rules` in the other branch. putup parses both branches of a condition on a config or environment variable, and no configuration could take the branch without the error of REQ-EVAL-INCLUDE-RULES-NOT-NESTED, so putup refuses the statement in both. putup crashed on the untaken branch: the include-once key holds the condition stack, so the `Tuprules.tup` was read again inside its own branch without end
- discharge: test "Scenario: include_rules in a file that include_rules reads is refused"

If putup parses an `include_rules` statement, in a branch that the configuration does not take,
while it reads a file for another `include_rules`, then putup shall reject the Tupfile with an
error on the statement's line.
