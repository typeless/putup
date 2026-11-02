# Dependency scanning — requirements

- area: dep-scan
- required-legs: none

The subject here is which commands putup scans for implicit dependencies, and what the scan it
builds carries. A scan is a generated command derived from a rule's own command text: it runs
the same compiler with `-M` so it preprocesses in the state the compile does. Whether a scan is
generated is therefore a question about the command's words, not about the build's state, so
this area declares no legs. See `README.md` for the format and the rules that apply to every
area.

Upstream tup performs no dependency scanning at all — it observes the compile's real file
accesses instead — so every requirement here is `putup-only` and no citation is possible.

Recognition is deliberately narrow. putup runs a scan from the rule's directory without the
rest of the command, so it can only scan a command whose compile it can reproduce in that
state; a prefix that changes the working directory or the environment makes the reproduction
false rather than incomplete. What precedes a compile is therefore either reproduced or proven
inert, and never silently dropped: a recognized compiler wrapper and the compile's own leading
environment assignments are reproduced, because the scan keeps them; an invocation that runs
nothing, or only announces, is inert. Anything else is opaque. Declining is therefore correct —
but declining in silence leaves a rule whose headers are never recorded, which is why the last
requirement here exists.

The unit that narrowness is measured in is the invocation: a command's control operators divide it
into invocations, each gets its own scan, and a scan may draw a word only from the invocation it
reproduces. A redirection is not such a divider — it hands its target to the same program — though
the scan still carries no flag from beyond it.

One class escapes the report. A compile-and-link command with no `-c` produces an executable
rather than an object file, so the output-shaped trigger cannot see it (the `HOSTCC` generator
rules in `examples/bsp/gcc/gmp/Tupfile` are the in-tree instance). The suppression for a command
that writes its own depfile reads only the invocations a scan would have been built from, so a
depfile flag elsewhere — in a later invocation, or behind a prefix that makes the compile
unreproducible — silences nothing.

---

## Group: recognition

Which commands a scan is generated for.

### REQ-SCAN-UNREPRODUCIBLE-INVOCATION

- conformance: putup-only
- discharge: test "GccScanner rejects compound shell commands"
- discharge: test "GccScanner compiler wrapper handling"
- discharge: test "GccScanner scans the prefix before a directory change"
- discharge: test "GccScanner scans the prefix before an invocation that is not a compile"
- discharge: test "GccScanner refuses a command whose first invocation is not a compile"
- discharge: test "A command whose first invocation is not a compile is scanned nowhere"
- discharge: test "ClangClScanner scans the prefix before an invocation that is not a compile"
- discharge: test "Scenario: The object of a compile that runs elsewhere is reported instead of scanned wrongly"

Where a command runs an invocation that is neither a compile putup recognizes nor one it proves
inert, whether a loop, a directory change, a standalone environment assignment, a link or any
other program, putup shall generate no dependency scan for that invocation or for any that
follows it, because the scan runs from the rule's directory with the rest of the command
stripped and past such an invocation would preprocess in a state the compile never had.

### REQ-SCAN-KEEPS-ASSIGNMENT-PREFIX

- conformance: putup-only
- discharge: test "GccScanner keeps a leading environment assignment in the scan"
- discharge: test "ClangClScanner reads the same prefix its sibling does"

Where a compile's own invocation begins with `NAME=VALUE` words carrying no shell syntax putup's
word split left unresolved, putup shall keep those words in front of the compiler in the scan it builds, because the scan then preprocesses
under the environment the compile did, including a variable such as `CPATH` that moves the header
search and that a dropped word would resolve against a path the compile never read; an assignment
standing as its own invocation scopes to the rest of the command line rather than to one
invocation, so it is opaque instead.

### REQ-SCAN-TRANSPARENT-INVOCATION

- conformance: putup-only
- discharge: test "GccScanner scans past an invocation that changes nothing"
- discharge: test "ClangClScanner reads the same prefix its sibling does"
- discharge: test "A separator with nothing after it begins no invocation"

Where an invocation before a compile runs nothing, or runs only a program that writes to its
output stream — `echo`, `true`, `:` — with no word that redirects and no shell syntax putup's word
split left unresolved, putup shall generate the scans for the compiles after it as if it were
absent, because such an invocation changes neither the directory, nor the environment, nor any
file the compile reads; where it redirects, the file it writes may be the very header the compile
includes, so it is opaque even though its program is one of these.

### REQ-SCAN-PER-INVOCATION

- conformance: putup-only
- discharge: test "GccScanner scans each compile of an all-compile command with its own flags"

Where a command's leading invocations are compiles putup recognizes, putup shall build one scan per
such invocation, each carrying that invocation's own flags and its own source-file words, because
each one preprocesses a different translation unit and the object it writes is covered only by a
scan derived from it.

### REQ-SCAN-UNSPACED-OPERATOR

- conformance: putup-only
- discharge: test "tokenize_shell_command splits a control operator from the word it touches"
- discharge: test "GccScanner ends a word at an operator written against it"

When an unquoted `;`, `&`, `&&`, `|` or `||` is written against a word, putup shall end the
invocation at that operator, as the shell does.

### REQ-SCAN-UNSPACED-REDIRECTION

- conformance: putup-only
- discharge: test "tokenize_shell_command splits a redirection from the word it touches"
- discharge: test "GccScanner ends a word at an operator written against it"

When an unquoted redirection is written against a word, putup shall read that word apart from
the redirection, as the shell does.

### REQ-SCAN-QUOTED-OPERATOR

- conformance: putup-only
- discharge: test "A quoted operator is a word and not a separator"
- discharge: test "tokenize_shell_command tells an operator from a word spelled the same"
- discharge: test "Scenario: A quoted word and a bare operator tokenize back to themselves"

If a control operator or a redirection is quoted or escaped, then putup shall read it as part of
a word of the invocation, as the shell does.

### REQ-SCAN-NEWLINE-SEPARATOR

- conformance: putup-only
- reference: no Tupfile text can carry a newline into a command (REQ-CONT-NO-NEWLINE); an imported environment value can (#347); putup scans the next line as sh runs it on every host, so on Windows, where cmd.exe drops it, the invocations putup scans past a newline never run
- discharge: test "tokenize_shell_command ends a command at an unquoted newline"
- discharge: test "A newline in the command text ends an invocation as the shell's does"
- discharge: test "Scenario: A compile behind an imported leading newline still tracks its headers"
- discharge: test "Scenario: The host shell runs no word after a newline as an argument of the command before it"

When a command's text holds an unquoted newline, putup shall end the invocation at that newline,
because no host shell passes a word after it to the program before it: sh runs the next line as a
command of its own, and cmd.exe drops the rest of the line.

### REQ-SCAN-EMPTY-WORD

- conformance: putup-only
- discharge: test "tokenize_shell_command keeps a quoted empty word"
- discharge: test "Scenario: A quoted word and a bare operator tokenize back to themselves"

If a command spells an empty word with quotes, then putup shall keep it as a word of the
invocation, as the shell does.

### REQ-SCAN-CARRIES-COMPILE-WORDS

- conformance: putup-only
- discharge: test "Scenario: A header the compile reads only under -O2 is tracked"
- discharge: test "Scenario: Implicit deps survive a flag whose path is a separate word"

Where putup builds a scan from a compile's invocation, putup shall carry the compile's flags into
the scan, a flag whose path stands as a word of its own together with that word, rather than
preprocessing the translation unit under a reduced flag set that resolves the other arm of an
include the compile gated on a flag such as `-O2`.

## Group: reporting

What putup says about an object it did not scan. The unit is the object, not the rule: per object
the reporting is binary — covered by a scan derived from the compile that writes it, or reported
unscanned — so a rule scanned in part is not a state this group must express, only what a reader
sees when some of a rule's objects are named. The report's sentences carry that unit too: each
speaks about the object it names, not about the command that declares it.

### REQ-SCAN-REPORT-UNSCANNED

- conformance: putup-only
- discharge: test "Scenario: A compile-shaped rule with no dependency scan is reported"
- discharge: test "Scenario: An object no scanned invocation writes is reported beside its scanned sibling"
- discharge: test "Scenario: A depfile flag the compile never carried hides no unscanned object"
- discharge: test "A depfile flag outside the scannable prefix suppresses nothing"
- discharge: test "A depfile flag inside the scannable prefix still suppresses"
- discharge: test "Scenario: The object of a compile that runs elsewhere is reported instead of scanned wrongly"

When a rule declares an object file that no generated scan covers — every object it declares,
where no scan at all is generated — and no invocation a scan would have been built from carries a
depfile flag, putup shall name that object and the rule's Tupfile under `parse` and under a
verbose build, and report how many such objects exist under any build.

## Group: scan-results

What putup does with what a scan printed. A scan is a command putup wrote itself, so its output is
a contract rather than a report: putup knows the rule it asked for, and anything else the driver
printed is not a dependency. The one dependency that looks unusable is the one outside the source
tree; it is recorded, not dropped, and #305 was filed on the assumption of the opposite.

### REQ-SCAN-REJECTS-FOREIGN-OUTPUT

- conformance: putup-only
- discharge: test "Scenario: A dep scan that prints anything but its rule fails the build"

If a scan writes anything ahead of the make rule it was generated to produce, then putup shall
fail the build and name the scan's command rather than record what it read, because a note line
taken for a dependency path names a file that does not exist, and a dependency that never stats
leaves the compile it feeds out of date at every build from then on.

### REQ-SCAN-FAILURE-NAMES-RULE-COMMAND

- conformance: putup-only
- discharge: test "Scenario: A failing dep scan names the command it scans for"

When a scan fails, putup shall name the rule command the scan was derived from beside the scan's
own command, because the scan is text putup wrote rather than the user, and an error naming only
it gives no way back to the rule that needs changing (#417).

### REQ-SCAN-OUTSIDE-TREE-ABSOLUTE

- conformance: putup-only
- discharge: test "Scenario: A dependency outside the source tree is recorded rather than dropped"

Where a dependency a scan reports resolves outside both the source tree and the build tree, putup
shall record it under its absolute path rather than skip it, because a header under a sysroot or a
toolchain prefix is one the compile read like any other, and dropping it — the arm a
relativize-or-skip reading of the path would take — leaves the compile silently stale after a
toolchain change.

### REQ-SCAN-BUILD-TREE-OUTPUT

- conformance: putup-only
- discharge: test "Scenario: A 3-tree build whose compile includes a generated header is up to date afterwards"

Where a dependency a scan reports resolves inside a build tree that lies outside the source tree,
putup shall record it under the build tree's name for that path, the one the command producing
the file declared, because recorded under its absolute path it is a second node for the same file,
typed as a source, and a source written during the build is recorded as unknown, so every build
reruns the commands that include it.
