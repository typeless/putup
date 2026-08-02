# Display text — requirements

- area: display
- required-legs: none

A rule may carry a `^ text ^` annotation ahead of its command. The subject here is what that
annotation names in output and how it resolves: where it stands in for the command, where it
deliberately does not, how it composes with a `!macro`, and what it must not touch. Both branches
belong here — a rule *without* an annotation still has to be named, and stating only the branch
that has one is what let four call sites report nothing at all (#229). See `README.md` for the
format and the rules that apply to every area.

This area requires no legs. A display is a function of the rule that declares it and carries no
state across builds, so its requirements are invariants of that function and carry no `leg` field.

The grammar of the caret head belongs here too. Upstream keys flags against display on whether a
space follows the opening `^`, and rejects an unterminated caret at parse time; putup implements
neither flag, so it refuses the flag form rather than rendering it as a label.

---

## Group: substitution

Where the annotation stands in for the command, and where it must not.

### REQ-DISP-SUBST

- conformance: tup-conformant
- reference: upstream substitutes the display for the entry's name when not verbose (tup/src/tup/entry.c:257)
- discharge: test "Scenario: A rule's display text stands in for the command in build output"

Where a rule carries a display annotation, putup shall report that rule by its annotation rather
than by its command text.

### REQ-DISP-ABSENT

- conformance: tup-conformant
- reference: upstream substitutes the display for the entry's name only where one exists (tup/src/tup/entry.c:257); with no annotation the entry's name — the command itself — is what gets reported, so a rule without one is never nameless
- discharge: test "Scenario: A rebuild reason names a command that carries no display annotation"
- discharge: test "Scenario: Removing a group member re-runs the commands that consume the group"

Where a rule carries no display annotation, putup shall report that rule by its command text.

### REQ-DISP-FAIL

- conformance: deliberate-deviation
- reference: upstream reports a failing command by its display unless --verbose (tup/src/tup/entry.c:257); putup's default output echoes no per-command line, so the failure line is the only record of what ran and an annotation there would name the step rather than the fault
- discharge: test "Scenario: A failing command is reported by its command line, not its display"
- discharge: test "Scenario: A failing config rule is reported by its command line, not its display"

When a command fails, putup shall report it by its command line rather than by its display
annotation.

### REQ-DISP-VERBOSE

- conformance: deliberate-deviation
- reference: upstream's --verbose switches from the display back to the real command (tup/src/tup/entry.c:257); putup's -v is its only per-command echo, so switching it would leave an annotation unobservable in build output, and `show script` reports the commands instead
- discharge: test "Scenario: A rule's display text stands in for the command in build output"

While reporting each command it runs, putup shall report a rule's annotation rather than the
command that verbose output would otherwise reveal.

---

## Group: resolution

What the annotation's text resolves to before it is reported.

### REQ-DISP-FLAGS

- conformance: tup-conformant
- reference: upstream expands the display through the same tup_printf percent expansion as the command (tup/src/tup/parser.c:3563-3567)
- discharge: test "Scenario: Percent flags inside display text expand against the rule"

putup shall expand percent flags in a display annotation against the rule that declares it.

### REQ-DISP-EXPAND-FAIL

- conformance: tup-conformant
- reference: upstream returns -1 when display expansion fails rather than dropping the display (tup/src/tup/parser.c:3564-3566)
- discharge: test "Scenario: Percent flags inside display text expand against the rule"

If expanding a display annotation fails, then putup shall report the failure rather than build the
command without its annotation.

### REQ-DISP-MACRO

- conformance: putup-only
- discharge: test "Scenario: A bang macro's display wins over one written on the rule"

Where a rule applies a `!macro` that carries its own display annotation, putup shall report the
macro's annotation and discard the rule's.

### REQ-DISP-UNTERMINATED

- conformance: tup-conformant
- reference: upstream makes a missing closing caret a parse-time error, "Missing ending `'^'` flag" (tup/src/tup/parser.c:3431-3472)
- discharge: test "Scenario: An unterminated caret is a parse error, not a shell error"

If a display annotation has no closing `^`, then putup shall reject the Tupfile while parsing it.

### REQ-DISP-HEAD

- conformance: deliberate-deviation
- reference: upstream reads the non-space run after `^` as flags — `t` marks outputs transient, `o` requests output comparison (tup/src/tup/parser.c:3431-3472); putup implements neither, and treating the run as display text would render a flag as a label, honouring nothing and refusing nothing
- discharge: test "Scenario: An upstream caret flag is rejected, not rendered as a label"

Where the characters after `^` are not preceded by a space, putup shall reject the Tupfile rather
than treat them as a display annotation.

---

## Group: identity

What the annotation must not reach.

### REQ-DISP-IDENTITY

- conformance: putup-only
- discharge: test "Display text stays out of command identity"

putup shall exclude a display annotation from a command's identity.
