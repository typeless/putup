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
false rather than incomplete. A recognized compiler wrapper is the one exception, because it
changes neither and the scan keeps it. Declining is therefore correct — but declining in
silence leaves a rule whose headers are never recorded, which is why the last requirement here
exists.

The unit that narrowness is measured in is the invocation: a command's control operators divide it
into invocations, and a scan may draw a word only from one it can reproduce. A redirection is not
such a divider — it hands its target to the same program — though the scan still carries no flag
from beyond it.

Two classes escape the report. A compile-and-link command with no `-c` produces an executable
rather than an object file, so the output-shaped trigger cannot see it (the `HOSTCC` generator
rules in `examples/bsp/gcc/gmp/Tupfile` are the in-tree instance). And the suppression for a
command that writes its own depfile tests the whole command text for a depfile flag, so an
unrelated word spelling one silences the report for that rule.

---

## Group: recognition

Which commands a scan is generated for.

### REQ-SCAN-UNREPRODUCIBLE-INVOCATION

- conformance: putup-only
- discharge: test "GccScanner rejects compound shell commands"
- discharge: test "GccScanner compiler wrapper handling"
- discharge: test "GccScanner refuses a command whose later invocation changes directory"
- discharge: test "GccScanner refuses a command whose later invocation is not a compile"
- discharge: test "matches_gcc_compile refuses a link whose later invocation compiles"
- discharge: test "ClangClScanner refuses a later invocation that is not a compile"

Where a command runs any invocation that is not a compile putup recognizes, whether a loop, a
directory change, an environment assignment, a link or any other program, putup shall generate no
dependency scan for that command, because the scan runs from the rule's directory with the rest of
the command stripped and would preprocess in a state the compile never had.

### REQ-SCAN-FLAG-SOURCE

- conformance: putup-only
- discharge: test "GccScanner takes flags from the invocation it scans, sources from all of them"

Where a command runs more than one invocation and every one of them is a compile putup recognizes,
putup shall build one scan carrying the source-file words of all of them; that scan takes its flags
from the first invocation alone, which issue #355 records as a limitation rather than a behaviour a
later invocation may rely on.

## Group: reporting

What putup says about a rule it did not scan. Per rule the reporting is binary — scanned, or
reported unscanned — so no scan decision may create a state this group cannot express, such as a
rule scanned in part; widening what a scan may cover means widening this group first.

### REQ-SCAN-REPORT-UNSCANNED

- conformance: putup-only
- discharge: test "Scenario: A compile-shaped rule with no dependency scan is reported"

When a rule's declared outputs include an object file, no scan is generated for its command,
and its command carries no depfile flag anywhere in its text, putup shall name that object and
the rule's Tupfile under `parse`, and report how many such rules exist under a build.
