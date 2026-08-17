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
- discharge: test "matches_gcc_compile refuses a command whose first invocation is not a compile"
- discharge: test "A command whose first invocation is not a compile is scanned nowhere"
- discharge: test "ClangClScanner scans the prefix before an invocation that is not a compile"

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

When a rule declares an object file that no generated scan covers — every object it declares,
where no scan at all is generated — and no invocation a scan would have been built from carries a
depfile flag, putup shall name that object and the rule's Tupfile under `parse`, and report how
many such objects exist under a build.
