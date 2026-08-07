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

That narrowness is enforced at the *front* of a command only. Once a scan is generated, source
words are folded in from every later invocation without asking what ran between them, so a
command whose second invocation changes directory can still contribute a source to a scan that
will not find it — issue #356. `REQ-SCAN-FLAG-SOURCE` below records that folding as it is
today, not as it should be.

Two classes escape the report. A compile-and-link command with no `-c` produces an executable
rather than an object file, so the output-shaped trigger cannot see it (the `HOSTCC` generator
rules in `examples/bsp/gcc/gmp/Tupfile` are the in-tree instance). And the suppression for a
command that writes its own depfile tests the whole command text for a depfile flag, so an
unrelated word spelling one silences the report for that rule.

---

## Group: recognition

Which commands a scan is generated for.

### REQ-SCAN-UNREPRODUCIBLE-PREFIX

- conformance: putup-only
- discharge: test "GccScanner rejects compound shell commands"
- discharge: test "GccScanner compiler wrapper handling"

Where any word other than a recognized compiler wrapper precedes a command's compiler, such as
a loop, a directory change or an environment assignment, putup shall generate no dependency
scan for that command, because the scan runs without that word and would preprocess in a state
the compile never had.

### REQ-SCAN-FLAG-SOURCE

- conformance: putup-only
- discharge: test "GccScanner takes flags from the invocation it scans, sources from all of them"

Where a command runs more than one invocation, putup shall build the scan from the flags of the
first invocation only, and from the source-file words of every invocation, whether or not the
later ones run a compiler.

## Group: reporting

What putup says about a rule it did not scan.

### REQ-SCAN-REPORT-UNSCANNED

- conformance: putup-only
- discharge: test "Scenario: A compile-shaped rule with no dependency scan is reported"

When a rule's declared outputs include an object file, no scan is generated for its command,
and its command carries no depfile flag anywhere in its text, putup shall name that object and
the rule's Tupfile under `parse`, and report how many such rules exist under a build.
