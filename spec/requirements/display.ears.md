# Display text — requirements

- area: display
- required-legs: none

A rule may carry a `^ text ^` annotation ahead of its command. The subject here is what that
annotation names in output and how it resolves: where it stands in for the command, where it
deliberately does not, how it composes with a `!macro`, and what it must not touch. See
`README.md` for the format and the rules that apply to every area.

This area requires no legs. A display is a function of the rule that declares it and carries no
state across builds, so its requirements are invariants of that function and carry no `leg` field.

The grammar of the caret head is out of scope here and tracked by #217: upstream keys flags against
display on whether a space follows the opening `^`, and rejects an unterminated caret at parse
time, where putup reads every head as display and lets an unterminated one reach the shell.

---

## Group: substitution

Where the annotation stands in for the command, and where it must not.

### REQ-DISP-SUBST

- conformance: tup-conformant
- reference: upstream substitutes the display for the entry's name when not verbose (tup/src/tup/entry.c:257)
- discharge: test "Scenario: A rule's display text stands in for the command in build output"

Where a rule carries a display annotation, putup shall report that rule by its annotation rather
than by its command text.

### REQ-DISP-FAIL

- conformance: deliberate-deviation
- reference: upstream reports a failing command by its display unless --verbose (tup/src/tup/entry.c:257); putup's default output echoes no per-command line, so the failure line is the only record of what ran and an annotation there would name the step rather than the fault
- discharge: test "Scenario: A failing command is reported by its command line, not its display"

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

---

## Group: identity

What the annotation must not reach.

### REQ-DISP-IDENTITY

- conformance: putup-only
- discharge: test "Display text stays out of command identity"

putup shall exclude a display annotation from a command's identity.
